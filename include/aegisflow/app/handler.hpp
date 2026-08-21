#pragma once

#include "aegisflow/app/blacklist_cache_bootstrap.hpp"
#include "aegisflow/app/blacklist_candidate_queue.hpp"
#include "aegisflow/app/blacklist_maintenance.hpp"
#include "aegisflow/app/feature_state_maintenance.hpp"
#include "aegisflow/net/acceptor_loop.hpp"
#include "aegisflow/net/event_loop_group.hpp"
#include "aegisflow/net/limits.hpp"
#include "aegisflow/risk/login_policy.hpp"
#include "aegisflow/runtime/bounded_worker_pool.hpp"
#include "aegisflow/storage/mysql_dao.hpp"
#include "aegisflow/storage/redis_connection.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace aegisflow::app {

enum class HandlerState : std::uint8_t {
    Constructed,
    Initialized,
    Running,
    Stopping,
    Stopped,
    Failed,
};

enum class HandlerStatus : std::uint8_t {
    Ok,
    InvalidConfig,
    ConfigConflict,
    InvalidState,
    DependencyFailed,
    StartFailed,
    JoinFailed,
};

struct HandlerConfig {
    risk::LoginPolicyConfig policy;
    storage::MysqlConfig mysql;
    storage::RedisConfig redis;
    BlacklistCacheBootstrapConfig blacklist_cache;
    runtime::BoundedWorkerPoolConfig worker_pool;
    runtime::BoundedWorkerPoolConfig maintenance_pool = [] {
        runtime::BoundedWorkerPoolConfig config;
        config.thread_count = 1;
        config.queue_capacity = 64;
        return config;
    }();
    feature::FeatureStateReclamationConfig feature_reclamation;
    FeatureStateMaintenanceConfig feature_state_maintenance;
    BlacklistMaintenanceConfig blacklist_maintenance;
    std::size_t candidate_queue_capacity = 4096;
    net::LimitsConfig limits;
    net::EventLoopGroupConfig event_loops;
    net::AcceptorLoopConfig acceptor;
    net::SessionDeadlineConfig deadlines = [] {
        net::SessionDeadlineConfig config;
        config.enabled = true;
        config.idle_timeout = std::chrono::seconds(30);
        config.read_timeout = std::chrono::seconds(5);
        config.write_timeout = std::chrono::seconds(10);
        config.business_timeout = std::chrono::seconds(2);
        return config;
    }();
    std::chrono::milliseconds shutdown_grace_timeout{
        std::chrono::seconds(5)
    };

    bool operator==(const HandlerConfig& other) const {
        return policy == other.policy &&
               mysql == other.mysql &&
               redis == other.redis &&
               blacklist_cache == other.blacklist_cache &&
               worker_pool == other.worker_pool &&
               maintenance_pool == other.maintenance_pool &&
               feature_reclamation == other.feature_reclamation &&
               feature_state_maintenance ==
                   other.feature_state_maintenance &&
               blacklist_maintenance == other.blacklist_maintenance &&
               candidate_queue_capacity == other.candidate_queue_capacity &&
               limits == other.limits &&
               event_loops == other.event_loops &&
               acceptor == other.acceptor &&
               deadlines == other.deadlines &&
               shutdown_grace_timeout == other.shutdown_grace_timeout;
    }
};

class Handler final {
public:
    static Handler& instance();

    [[nodiscard]] HandlerStatus init(const HandlerConfig& config) noexcept;
    [[nodiscard]] HandlerStatus start() noexcept;
    void stop() noexcept;
    [[nodiscard]] HandlerStatus join() noexcept;
    [[nodiscard]] HandlerState state() const noexcept;

    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;
    Handler(Handler&&) = delete;
    Handler& operator=(Handler&&) = delete;

private:
    Handler();
    ~Handler();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // 命名空间 aegisflow::app
