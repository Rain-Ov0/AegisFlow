#include "aegisflow/app/blacklist_maintenance.hpp"

#include "aegisflow/storage/redis_connection.hpp"

#include <memory>
#include <utility>

namespace aegisflow::app {
namespace {

bool dropsRedisConnection(const storage::StoreStatus status) noexcept {
    return status == storage::StoreStatus::IoError ||
           status == storage::StoreStatus::CommitUnknown ||
           status == storage::StoreStatus::DeadlineExceeded ||
           status == storage::StoreStatus::ProtocolError;
}

storage::StoreResult storeIoError() {
    storage::StoreResult result;
    result.status = storage::StoreStatus::IoError;
    return result;
}

storage::SnapshotReadResult snapshotIoError() {
    storage::SnapshotReadResult result;
    result.status = storage::StoreStatus::IoError;
    return result;
}

storage::PendingReadResult pendingIoError() {
    storage::PendingReadResult result;
    result.status = storage::StoreStatus::IoError;
    return result;
}

storage::ResetReadResult resetIoError() {
    storage::ResetReadResult result;
    result.status = storage::StoreStatus::IoError;
    return result;
}

class ProductionMaintenanceBackend final
    : public IBlacklistMaintenanceBackend {
public:
    ProductionMaintenanceBackend(
        storage::RedisConfig redis_config,
        storage::MysqlConfig mysql_config
    ) : redis_config_(std::move(redis_config)),
        mysql_config_(std::move(mysql_config)),
        keys_(storage::RedisKeySet::fromPrefix(redis_config_.key_prefix)) {}

    storage::StoreResult applyCandidates(
        const base::ArrayView<const risk::BlacklistMutation> mutations,
        const storage::RedisDeadline deadline
    ) override {
        if (!ensureRedis(deadline)) {
            return storeIoError();
        }
        auto result = redis_store_->applyMutations(mutations, deadline);
        observe(result.status);
        return result;
    }

    storage::StoreResult readRevision(
        const storage::RedisDeadline deadline
    ) override {
        if (!ensureRedis(deadline)) {
            return storeIoError();
        }
        auto result = redis_store_->readRevision(deadline);
        observe(result.status);
        return result;
    }

    storage::SnapshotReadResult loadStableSnapshot(
        const std::size_t scan_count,
        const storage::RedisDeadline deadline
    ) override {
        if (!ensureRedis(deadline)) {
            return snapshotIoError();
        }
        auto result = redis_store_->loadStableSnapshot(scan_count, deadline);
        observe(result.status);
        return result;
    }

    storage::StoreResult removeExpiredFields(
        const base::ArrayView<const risk::BlacklistEntry> entries,
        const std::uint64_t expected_revision,
        const std::uint64_t now_ms,
        const storage::RedisDeadline deadline
    ) override {
        if (!ensureRedis(deadline)) {
            return storeIoError();
        }
        auto result = redis_store_->removeExpiredFields(
            entries, expected_revision, now_ms, deadline);
        observe(result.status);
        return result;
    }

    storage::StoreStatus setPublishedRevision(
        const std::uint64_t revision,
        const storage::RedisDeadline deadline
    ) override {
        if (!ensureRedis(deadline)) {
            return storage::StoreStatus::IoError;
        }
        const auto status = redis_store_->setPublishedRevision(
            revision, deadline);
        observe(status);
        return status;
    }

    storage::PendingReadResult readPending(
        const std::size_t count,
        const storage::RedisDeadline deadline
    ) override {
        if (!ensureRedis(deadline)) {
            return pendingIoError();
        }
        auto result = redis_store_->readPending(count, deadline);
        observe(result.status);
        return result;
    }

    storage::StoreStatus deletePending(
        const base::ArrayView<const std::string> stream_ids,
        const storage::RedisDeadline deadline
    ) override {
        if (!ensureRedis(deadline)) {
            return storage::StoreStatus::IoError;
        }
        const auto status = redis_store_->deletePending(stream_ids, deadline);
        observe(status);
        return status;
    }

    bool applyMysqlMutations(
        const base::ArrayView<const risk::BlacklistMutation> mutations,
        const storage::MysqlDeadline deadline
    ) override {
        if (!ensureMysql(deadline)) {
            return false;
        }
        if (mysql_->applyBlacklistMutations(mutations, deadline)) {
            return true;
        }
        // mysqlclient 无法证明失败连接仍可用，因此不在
        // 后续 tick 复用；Stream 记录未 XDEL，恢复后可幂等重放。
        mysql_.reset();
        return false;
    }

    storage::ResetReadResult readResetBarrier(
        const storage::RedisDeadline deadline
    ) override {
        if (!ensureRedis(deadline)) {
            return resetIoError();
        }
        auto result = redis_store_->readResetBarrier(deadline);
        observe(result.status);
        return result;
    }

    storage::StoreStatus completeResetBarrier(
        const std::uint64_t token,
        const storage::RedisDeadline deadline
    ) override {
        if (!ensureRedis(deadline)) {
            return storage::StoreStatus::IoError;
        }
        const auto status = redis_store_->completeResetBarrier(token, deadline);
        observe(status);
        return status;
    }

private:
    bool ensureRedis(const storage::RedisDeadline deadline) {
        if (redis_store_ != nullptr) {
            return true;
        }
        redis_connection_ = storage::RedisConnection::connect(
            redis_config_, deadline);
        if (redis_connection_ == nullptr) {
            return false;
        }
        redis_store_ = std::make_unique<storage::BlacklistRedisStore>(
            *redis_connection_, keys_);
        return true;
    }

    bool ensureMysql(const storage::MysqlDeadline deadline) {
        if (mysql_ != nullptr) {
            return true;
        }
        auto candidate = std::make_unique<storage::MysqlDao>(mysql_config_);
        if (!candidate->connect(deadline)) {
            return false;
        }
        mysql_ = std::move(candidate);
        return true;
    }

    void observe(const storage::StoreStatus status) noexcept {
        if (!dropsRedisConnection(status)) {
            return;
        }
        // Store 持有 executor 指针，必须先销毁 Store 再销毁
        // connection；下一命令由同一 maintenance worker 懒重连。
        redis_store_.reset();
        redis_connection_.reset();
    }

    storage::RedisConfig redis_config_;
    storage::MysqlConfig mysql_config_;
    storage::RedisKeySet keys_;
    std::unique_ptr<storage::RedisConnection> redis_connection_;
    std::unique_ptr<storage::BlacklistRedisStore> redis_store_;
    std::unique_ptr<storage::MysqlDao> mysql_;
};

class ProductionMaintenanceBackendFactory final
    : public IBlacklistMaintenanceBackendFactory {
public:
    ProductionMaintenanceBackendFactory(
        storage::RedisConfig redis_config,
        storage::MysqlConfig mysql_config
    ) : redis_config_(std::move(redis_config)),
        mysql_config_(std::move(mysql_config)) {}

    std::unique_ptr<IBlacklistMaintenanceBackend> create() override {
        return std::make_unique<ProductionMaintenanceBackend>(
            redis_config_, mysql_config_);
    }

private:
    storage::RedisConfig redis_config_;
    storage::MysqlConfig mysql_config_;
};

}  // 命名空间

std::shared_ptr<IBlacklistMaintenanceBackendFactory>
makeBlacklistMaintenanceBackendFactory(
    storage::RedisConfig redis_config,
    storage::MysqlConfig mysql_config
) {
    return std::make_shared<ProductionMaintenanceBackendFactory>(
        std::move(redis_config), std::move(mysql_config));
}

}  // 命名空间 aegisflow::app
