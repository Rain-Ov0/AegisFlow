#include "aegisflow/app/handler.hpp"

#include "aegisflow/app/blacklist_cache_bootstrap.hpp"
#include "aegisflow/log/logger.hpp"
#include "aegisflow/net/login_business_handler.hpp"
#include "aegisflow/risk/blacklist_manager.hpp"
#include "aegisflow/storage/blacklist_redis_store.hpp"
#include "aegisflow/storage/redis_connection.hpp"
#include "aegisflow/timer/timer.hpp"

#include <arpa/inet.h>

#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace aegisflow::app {
namespace {

[[nodiscard]] HandlerConfig normalizeConfig(HandlerConfig config) {
    config.worker_pool.queue_capacity =
        config.limits.business_queue_capacity;
    config.event_loops.max_connections = config.limits.max_connections;
    config.event_loops.max_connections_per_loop =
        config.limits.max_connections_per_loop;
    config.event_loops.connection_queue_capacity =
        config.limits.connection_queue_capacity;
    auto& loop = config.event_loops.event_loop;
    loop.max_connections = config.limits.max_connections_per_loop;
    loop.completion_capacity = config.limits.completion_queue_capacity;
    loop.completion_byte_capacity =
        config.limits.completion_queue_byte_capacity;
    loop.max_completion_bytes = config.limits.max_output_buffer_bytes;
    loop.overload_response_frame = net::buildLoginOverloadResponseFrame();
    loop.business_timeout_response_frame =
        net::buildLoginBusinessTimeoutResponseFrame();
    loop.deadlines = config.deadlines;
    loop.session.max_frame_payload_bytes =
        config.limits.max_frame_payload_bytes;
    loop.session.input_soft_watermark_bytes =
        config.limits.input_soft_watermark_bytes;
    loop.session.max_input_buffer_bytes =
        config.limits.max_input_buffer_bytes;
    loop.session.output_soft_watermark_bytes =
        config.limits.output_soft_watermark_bytes;
    loop.session.max_output_buffer_bytes =
        config.limits.max_output_buffer_bytes;
    config.acceptor.max_connections = config.limits.max_connections;
    config.blacklist_maintenance.batch_size =
        config.blacklist_cache.batch_size;
    return config;
}

[[nodiscard]] bool positiveTimeout(
    const std::chrono::milliseconds value
) noexcept {
    return value.count() > 0 && value <= std::chrono::hours(24);
}

[[nodiscard]] bool validConfig(const HandlerConfig& config) noexcept {
    if (config.worker_pool.thread_count == 0 ||
        config.worker_pool.queue_capacity == 0 ||
        config.maintenance_pool.thread_count != 1 ||
        config.maintenance_pool.queue_capacity == 0 ||
        !feature::LoginFeatureStore::isValidReclamationConfig(
            config.feature_reclamation
        ) ||
        !positiveTimeout(config.feature_state_maintenance.cleanup_interval) ||
        !positiveTimeout(
            config.blacklist_maintenance.maintenance_interval) ||
        !positiveTimeout(
            config.blacklist_maintenance.maintenance_timeout) ||
        !positiveTimeout(
            config.blacklist_maintenance.expire_cleanup_interval) ||
        config.blacklist_maintenance.batch_size == 0 ||
        config.blacklist_maintenance.candidate_batch_size == 0 ||
        config.candidate_queue_capacity == 0 ||
        config.blacklist_maintenance.candidate_batch_size >
            config.candidate_queue_capacity ||
        !positiveTimeout(config.blacklist_cache.startup_timeout) ||
        config.blacklist_cache.batch_size == 0 ||
        !positiveTimeout(config.blacklist_cache.reset_timeout) ||
        config.redis.host.empty() || config.redis.port == 0 ||
        config.redis.database > 15 || config.redis.key_prefix.empty() ||
        config.redis.connect_timeout.count() <= 0 ||
        config.redis.command_timeout.count() <= 0 ||
        (!config.redis.username.empty() && config.redis.password.empty()) ||
        config.event_loops.loop_count == 0 ||
        config.event_loops.loop_count >
            std::numeric_limits<std::uint32_t>::max() ||
        config.event_loops.event_loop.max_events == 0 ||
        config.event_loops.event_loop.read_scratch_bytes == 0 ||
        config.event_loops.event_loop.completion_capacity == 0 ||
        config.event_loops.event_loop.completion_byte_capacity == 0 ||
        config.acceptor.backlog <= 0 ||
        !config.deadlines.enabled ||
        !positiveTimeout(config.deadlines.idle_timeout) ||
        !positiveTimeout(config.deadlines.read_timeout) ||
        !positiveTimeout(config.deadlines.write_timeout) ||
        !positiveTimeout(config.deadlines.business_timeout) ||
        !positiveTimeout(config.shutdown_grace_timeout) ||
        !net::validLimits(
            config.limits,
            config.event_loops.loop_count,
            config.worker_pool.thread_count
        )) {
        return false;
    }

    in_addr address{};
    if (::inet_pton(AF_INET, config.acceptor.bind_address.c_str(),
                    &address) != 1) {
        return false;
    }
    try {
        risk::validateLoginPolicyConfig(config.policy);
    } catch (...) {
        return false;
    }
    return true;
}

struct InitialBlacklistState {
    std::uint64_t revision = 0;
    bool publication_dirty = false;
};

[[nodiscard]] std::optional<InitialBlacklistState> loadInitialBlacklist(
    const HandlerConfig& config,
    risk::BlacklistManager& manager
) noexcept {
    try {
        const auto deadline = std::chrono::steady_clock::now() +
                              config.blacklist_cache.startup_timeout;
        auto redis_connection = storage::RedisConnection::connect(
            config.redis, deadline);
        if (redis_connection == nullptr) {
            return std::nullopt;
        }
        storage::MysqlDao mysql(config.mysql);
        if (!mysql.connect(deadline)) {
            return std::nullopt;
        }
        storage::BlacklistRedisStore redis(
            *redis_connection,
            storage::RedisKeySet::fromPrefix(config.redis.key_prefix));
        auto initialized = initializeBlacklistCache(
            redis,
            mysql,
            config.blacklist_cache.batch_size,
            config.blacklist_cache.batch_size,
            deadline);
        if (initialized.status != storage::StoreStatus::Ok ||
            !manager.publish(initialized.snapshot.entries)) {
            return std::nullopt;
        }
        // 内存快照已完整发布时，published_revision 只是
        // 外部收敛指示。SET 失败不阻止 Acceptor，运行期
        // maintenance 会携带 dirty 版本持续重试。
        InitialBlacklistState result;
        result.revision = initialized.snapshot.revision;
        result.publication_dirty = redis.setPublishedRevision(
            initialized.snapshot.revision, deadline) !=
            storage::StoreStatus::Ok;
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

}  // 命名空间

class Handler::Impl final {
public:
    ~Impl() {
        stop();
        (void)join();
    }

    [[nodiscard]] HandlerStatus init(
        const HandlerConfig& requested
    ) noexcept {
        std::lock_guard lock(mutex);
        HandlerConfig effective;
        try {
            effective = normalizeConfig(requested);
        } catch (...) {
            return HandlerStatus::InvalidConfig;
        }
        if (config.has_value()) {
            return *config == effective
                       ? HandlerStatus::Ok
                       : HandlerStatus::ConfigConflict;
        }
        if (state != HandlerState::Constructed || !validConfig(effective)) {
            return state == HandlerState::Constructed
                       ? HandlerStatus::InvalidConfig
                       : HandlerStatus::InvalidState;
        }

        try {
            auto manager = std::make_unique<risk::BlacklistManager>();
            const auto initial = loadInitialBlacklist(effective, *manager);
            if (!initial.has_value()) {
                return HandlerStatus::DependencyFailed;
            }
            auto candidates = std::make_shared<BlacklistCandidateQueue>(
                effective.candidate_queue_capacity);
            auto service = std::make_shared<RiskService>(
                effective.policy,
                manager.get(),
                effective.feature_reclamation
            );
            business_handler = std::make_shared<net::LoginBusinessHandler>(
                service, candidates);
            blacklist_manager = std::move(manager);
            candidate_queue = std::move(candidates);
            risk_service = std::move(service);
            initial_blacklist_revision = initial->revision;
            initial_publication_dirty = initial->publication_dirty;
            config = std::move(effective);
            state = HandlerState::Initialized;
            return HandlerStatus::Ok;
        } catch (...) {
            return HandlerStatus::DependencyFailed;
        }
    }

    [[nodiscard]] HandlerStatus start() noexcept {
        std::lock_guard lock(mutex);
        if (state == HandlerState::Running) {
            return HandlerStatus::Ok;
        }
        if (state != HandlerState::Initialized || !config.has_value() ||
            business_handler == nullptr || blacklist_manager == nullptr ||
            risk_service == nullptr || candidate_queue == nullptr) {
            return HandlerStatus::InvalidState;
        }

        try {
            worker_pool = std::make_unique<runtime::BoundedWorkerPool>(
                config->worker_pool
            );
            maintenance_pool =
                std::make_unique<runtime::BoundedWorkerPool>(
                    config->maintenance_pool
                );
            feature_maintenance = FeatureStateMaintenance::create(
                config->feature_state_maintenance,
                *maintenance_pool,
                timer::Timer::instance(),
                risk_service->featureStore()
            );
            if (!feature_maintenance->start()) {
                return failStart();
            }
            blacklist_maintenance = BlacklistMaintenance::create(
                config->blacklist_maintenance,
                makeBlacklistMaintenanceBackendFactory(
                    config->redis, config->mysql),
                *candidate_queue,
                *worker_pool,
                *maintenance_pool,
                timer::Timer::instance(),
                *blacklist_manager,
                initial_blacklist_revision,
                initial_publication_dirty
            );
            if (!blacklist_maintenance->start()) {
                return failStart();
            }
            event_loops = std::make_unique<net::EventLoopGroup>(
                config->event_loops,
                *worker_pool,
                business_handler,
                timer::Timer::instance()
            );
            if (event_loops->start() != net::EventLoopGroupStatus::Ok) {
                return failStart();
            }
            acceptor = std::make_unique<net::AcceptorLoop>(
                config->acceptor,
                *event_loops
            );
            if (acceptor->start() != net::AcceptorLoopStatus::Ok) {
                return failStart();
            }
        } catch (...) {
            return failStart();
        }

        join_status = HandlerStatus::Ok;
        state = HandlerState::Running;
        return HandlerStatus::Ok;
    }

    void stop() noexcept {
        std::lock_guard lock(mutex);
        if (state == HandlerState::Constructed ||
            state == HandlerState::Initialized) {
            state = HandlerState::Stopped;
            return;
        }
        if (state != HandlerState::Running) {
            return;
        }

        state = HandlerState::Stopping;
        graceful_deadline = std::chrono::steady_clock::now() +
                            config->shutdown_grace_timeout;
        // stop 只截断新请求并关闭业务提交。Timer 和
        // candidate queue 必须等 join 确认所有已接受业务终态后再停。
        acceptor->stop();
        drain_started =
            event_loops->beginDrain() == net::EventLoopGroupStatus::Ok;
        worker_pool->close();
    }

    [[nodiscard]] HandlerStatus join() noexcept {
        std::unique_lock lock(mutex);
        if (state == HandlerState::Stopped ||
            (state == HandlerState::Failed && acceptor == nullptr)) {
            return join_status;
        }
        if (state != HandlerState::Stopping) {
            return HandlerStatus::InvalidState;
        }
        if (join_in_progress) {
            state_changed.wait(lock, [this] { return !join_in_progress; });
            return join_status;
        }
        join_in_progress = true;
        const auto deadline = graceful_deadline;
        lock.unlock();

        HandlerStatus result = HandlerStatus::Ok;
        if (acceptor->join() != net::AcceptorLoopStatus::Ok) {
            result = HandlerStatus::JoinFailed;
        }
        (void)worker_pool->drainUntil(deadline);
        if (drain_started) {
            const auto status = event_loops->drainUntil(deadline);
            if (status != net::EventLoopGroupStatus::Ok &&
                status !=
                    net::EventLoopGroupStatus::DrainDeadlineExceeded) {
                result = HandlerStatus::JoinFailed;
            }
        } else {
            result = HandlerStatus::JoinFailed;
        }
        // 业务任务先进入终态，EventLoop 再消费已投递的
        // completion 并停止。在此之前关 candidate queue 会丢掉最后一批候选。
        event_loops->stop();
        if (event_loops->join() != net::EventLoopGroupStatus::Ok ||
            worker_pool->join() != runtime::WorkerPoolStatus::Ok) {
            result = HandlerStatus::JoinFailed;
        }

        candidate_queue->close();
        feature_maintenance->stop();
        blacklist_maintenance->stop();
        if (!blacklist_maintenance->finalDrainUntil(deadline)) {
            result = HandlerStatus::JoinFailed;
        }
        maintenance_pool->close();
        (void)maintenance_pool->drainUntil(deadline);
        if (maintenance_pool->join() != runtime::WorkerPoolStatus::Ok) {
            result = HandlerStatus::JoinFailed;
        }
        const auto queue_full_dropped =
            candidate_queue->takeDroppedSinceLastReport();
        if (queue_full_dropped != 0) {
            AEGISFLOW_LOG_WARN(
                "blacklist candidate queue full; dropped=" +
                std::to_string(queue_full_dropped));
        }
        const auto dropped = candidate_queue->discardRemainingOnShutdown();
        if (dropped != 0) {
            AEGISFLOW_LOG_ERROR(
                "blacklist candidates dropped on shutdown=" +
                std::to_string(dropped));
            result = HandlerStatus::JoinFailed;
        }

        lock.lock();
        resetStartedComponents();
        join_status = result;
        state = result == HandlerStatus::Ok
                    ? HandlerState::Stopped
                    : HandlerState::Failed;
        join_in_progress = false;
        state_changed.notify_all();
        return result;
    }

    [[nodiscard]] HandlerState currentState() const noexcept {
        std::lock_guard lock(mutex);
        return state;
    }

private:
    [[nodiscard]] HandlerStatus failStart() noexcept {
        stopStartedComponents();
        resetStartedComponents();
        state = HandlerState::Failed;
        join_status = HandlerStatus::StartFailed;
        return HandlerStatus::StartFailed;
    }

    void stopStartedComponents() noexcept {
        const auto deadline = std::chrono::steady_clock::now() +
                              config->shutdown_grace_timeout;
        if (acceptor != nullptr) {
            acceptor->stop();
            (void)acceptor->join();
        }
        if (event_loops != nullptr) {
            (void)event_loops->beginDrain();
        }
        if (worker_pool != nullptr) {
            worker_pool->close();
            (void)worker_pool->drainUntil(deadline);
        }
        if (event_loops != nullptr) {
            (void)event_loops->drainUntil(deadline);
            event_loops->stop();
            (void)event_loops->join();
        }
        if (worker_pool != nullptr) {
            (void)worker_pool->join();
        }
        if (candidate_queue != nullptr) {
            candidate_queue->close();
        }
        if (feature_maintenance != nullptr) {
            feature_maintenance->stop();
        }
        if (blacklist_maintenance != nullptr) {
            blacklist_maintenance->stop();
            (void)blacklist_maintenance->finalDrainUntil(deadline);
        }
        if (maintenance_pool != nullptr) {
            maintenance_pool->close();
            (void)maintenance_pool->drainUntil(deadline);
            (void)maintenance_pool->join();
        }
        if (candidate_queue != nullptr) {
            const auto queue_full_dropped =
                candidate_queue->takeDroppedSinceLastReport();
            if (queue_full_dropped != 0) {
                AEGISFLOW_LOG_WARN(
                    "blacklist candidate queue full; dropped=" +
                    std::to_string(queue_full_dropped));
            }
            const auto dropped =
                candidate_queue->discardRemainingOnShutdown();
            if (dropped != 0) {
                AEGISFLOW_LOG_ERROR(
                    "blacklist candidates dropped on failed start=" +
                    std::to_string(dropped));
            }
        }
    }

    void resetStartedComponents() noexcept {
        acceptor.reset();
        event_loops.reset();
        blacklist_maintenance.reset();
        feature_maintenance.reset();
        maintenance_pool.reset();
        worker_pool.reset();
    }

    mutable std::mutex mutex;
    std::condition_variable state_changed;
    std::optional<HandlerConfig> config;
    std::unique_ptr<risk::BlacklistManager> blacklist_manager;
    std::shared_ptr<BlacklistCandidateQueue> candidate_queue;
    std::shared_ptr<RiskService> risk_service;
    std::shared_ptr<net::IFrameBusinessHandler> business_handler;
    std::unique_ptr<runtime::BoundedWorkerPool> worker_pool;
    std::unique_ptr<runtime::BoundedWorkerPool> maintenance_pool;
    std::shared_ptr<FeatureStateMaintenance> feature_maintenance;
    std::shared_ptr<BlacklistMaintenance> blacklist_maintenance;
    std::unique_ptr<net::EventLoopGroup> event_loops;
    std::unique_ptr<net::AcceptorLoop> acceptor;
    std::chrono::steady_clock::time_point graceful_deadline{};
    HandlerState state = HandlerState::Constructed;
    HandlerStatus join_status = HandlerStatus::Ok;
    bool drain_started = false;
    bool join_in_progress = false;
    std::uint64_t initial_blacklist_revision = 0;
    bool initial_publication_dirty = false;
};

Handler& Handler::instance() {
    static Handler singleton;
    return singleton;
}

Handler::Handler() : impl_(std::make_unique<Impl>()) {}

Handler::~Handler() = default;

HandlerStatus Handler::init(const HandlerConfig& config) noexcept {
    return impl_->init(config);
}

HandlerStatus Handler::start() noexcept {
    return impl_->start();
}

void Handler::stop() noexcept {
    impl_->stop();
}

HandlerStatus Handler::join() noexcept {
    return impl_->join();
}

HandlerState Handler::state() const noexcept {
    return impl_->currentState();
}

}  // 命名空间 aegisflow::app
