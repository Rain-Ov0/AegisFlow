#include "aegisflow/app/blacklist_cache_bootstrap.hpp"

#include "aegisflow/log/logger.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace aegisflow::app {
namespace {

storage::SnapshotReadResult makeSnapshotReadResult(
    const storage::StoreStatus status
) {
    storage::SnapshotReadResult result;
    result.status = status;
    return result;
}

BlacklistBootstrapResult makeBootstrapResult(
    const storage::StoreStatus status
) {
    BlacklistBootstrapResult result;
    result.status = status;
    return result;
}

BlacklistBootstrapResult makeBootstrapResult(
    const storage::StoreStatus status,
    storage::SnapshotReadResult snapshot
) {
    BlacklistBootstrapResult result;
    result.status = status;
    result.snapshot = std::move(snapshot);
    return result;
}

storage::SnapshotReadResult readStableUntilDeadline(
    storage::BlacklistRedisStore& redis,
    const std::size_t scan_count,
    const storage::RedisDeadline deadline
) {
    while (std::chrono::steady_clock::now() < deadline) {
        auto snapshot = redis.loadStableSnapshot(scan_count, deadline);
        if (snapshot.status != storage::StoreStatus::WatchConflict) {
            return snapshot;
        }
    }
    return makeSnapshotReadResult(storage::StoreStatus::DeadlineExceeded);
}

}  // 命名空间

BlacklistBootstrapResult initializeBlacklistCache(
    storage::BlacklistRedisStore& redis,
    storage::MysqlDao& mysql,
    const std::size_t batch_size,
    const std::size_t scan_count,
    const storage::RedisDeadline deadline
) {
    if (batch_size == 0 || scan_count == 0) {
        return makeBootstrapResult(storage::StoreStatus::ProtocolError);
    }

    auto ready = redis.readReadyRevision(deadline);
    if (ready.status == storage::StoreStatus::Ok) {
        auto snapshot = readStableUntilDeadline(redis, scan_count, deadline);
        return makeBootstrapResult(snapshot.status, std::move(snapshot));
    }
    if (ready.status != storage::StoreStatus::NotReady) {
        return makeBootstrapResult(ready.status);
    }

    // ready 不存在时，Stream 是 MySQL 尚未确认的写后缓冲。
    // 每批严格按 Stream 顺序入库，COMMIT 成功后才 XDEL；因此
    // 中途断线最多导致幂等重放，不会丢失变更。
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return makeBootstrapResult(
                storage::StoreStatus::DeadlineExceeded);
        }
        auto pending = redis.readPending(batch_size, deadline);
        if (pending.status != storage::StoreStatus::Ok) {
            return makeBootstrapResult(pending.status);
        }
        if (pending.entries.empty()) {
            break;
        }

        std::vector<std::string> invalid_ids;
        std::vector<std::string> valid_ids;
        std::vector<risk::BlacklistMutation> mutations;
        invalid_ids.reserve(pending.entries.size());
        valid_ids.reserve(pending.entries.size());
        mutations.reserve(pending.entries.size());
        for (auto& entry : pending.entries) {
            if (!entry.mutation.has_value()) {
                AEGISFLOW_LOG_ERROR(
                    "invalid blacklist pending entry id=" + entry.stream_id +
                    " error=" + entry.error);
                invalid_ids.push_back(std::move(entry.stream_id));
                continue;
            }
            valid_ids.push_back(std::move(entry.stream_id));
            mutations.push_back(std::move(*entry.mutation));
        }

        if (!invalid_ids.empty()) {
            const auto deleted = redis.deletePending(invalid_ids, deadline);
            if (deleted != storage::StoreStatus::Ok) {
                return makeBootstrapResult(deleted);
            }
        }
        if (!mutations.empty()) {
            if (!mysql.applyBlacklistMutations(mutations, deadline)) {
                return makeBootstrapResult(
                    std::chrono::steady_clock::now() >= deadline
                        ? storage::StoreStatus::DeadlineExceeded
                        : storage::StoreStatus::IoError);
            }
            const auto deleted = redis.deletePending(valid_ids, deadline);
            if (deleted != storage::StoreStatus::Ok) {
                return makeBootstrapResult(deleted);
            }
        }
    }

    auto entries = mysql.loadEnabledBlacklists(deadline);
    if (!mysql.lastBlacklistLoadSucceeded()) {
        return makeBootstrapResult(
            std::chrono::steady_clock::now() >= deadline
                ? storage::StoreStatus::DeadlineExceeded
                : storage::StoreStatus::IoError);
    }

    // 多个启动者只有一个能提交 ready。失败者必须丢弃
    // 自己的 MySQL 候选快照，改读获胜者已完整发布的 Redis。
    while (std::chrono::steady_clock::now() < deadline) {
        const auto rebuilt = redis.rebuildFromMysql(entries, deadline);
        if (rebuilt.status == storage::StoreStatus::Ok) {
            auto snapshot = readStableUntilDeadline(redis, scan_count, deadline);
            return makeBootstrapResult(snapshot.status, std::move(snapshot));
        }
        if (rebuilt.status != storage::StoreStatus::WatchConflict) {
            return makeBootstrapResult(rebuilt.status);
        }
        ready = redis.readReadyRevision(deadline);
        if (ready.status == storage::StoreStatus::Ok) {
            auto snapshot = readStableUntilDeadline(redis, scan_count, deadline);
            return makeBootstrapResult(snapshot.status, std::move(snapshot));
        }
        if (ready.status != storage::StoreStatus::NotReady) {
            return makeBootstrapResult(ready.status);
        }
    }
    return makeBootstrapResult(storage::StoreStatus::DeadlineExceeded);
}

}  // 命名空间 aegisflow::app
