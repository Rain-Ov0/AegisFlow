#pragma once

#include "aegisflow/base/array_view.hpp"
#include "aegisflow/risk/blacklist_mutation.hpp"
#include "aegisflow/risk/blacklist_types.hpp"
#include "aegisflow/storage/redis_connection.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegisflow::storage {

enum class StoreStatus {
    Ok,
    WatchConflict,
    NotReady,
    InvalidRemoteState,
    DeadlineExceeded,
    IoError,
    CommitUnknown,
    TransactionError,
    ProtocolError,
};

struct StoreResult {
    StoreStatus status = StoreStatus::IoError;
    std::optional<std::uint64_t> revision;
};

struct RedisKeySet {
    std::string users;
    std::string ips;
    std::string devices;
    std::string pending;
    std::string revision;
    std::string cache_ready;
    std::string published_revision;
    std::string reset_barrier;

    [[nodiscard]] static RedisKeySet fromPrefix(std::string prefix);
    [[nodiscard]] std::vector<std::string> all() const;
    bool operator==(const RedisKeySet& other) const {
        return users == other.users &&
               ips == other.ips &&
               devices == other.devices &&
               pending == other.pending &&
               revision == other.revision &&
               cache_ready == other.cache_ready &&
               published_revision == other.published_revision &&
               reset_barrier == other.reset_barrier;
    }
};

struct PendingBlacklistEntry {
    std::string stream_id;
    std::optional<risk::BlacklistMutation> mutation;
    std::string error;
};

struct PendingReadResult {
    StoreStatus status = StoreStatus::IoError;
    std::vector<PendingBlacklistEntry> entries;
};

struct SnapshotReadResult {
    StoreStatus status = StoreStatus::IoError;
    std::uint64_t revision = 0;
    std::vector<risk::BlacklistEntry> entries;
};

struct ResetBarrierState {
    std::uint64_t requested = 0;
    std::uint64_t completed = 0;
};

struct ResetReadResult {
    StoreStatus status = StoreStatus::IoError;
    ResetBarrierState state;
};

struct ResetConvergence {
    StoreStatus status = StoreStatus::IoError;
    std::uint64_t user_count = 0;
    std::uint64_t ip_count = 0;
    std::uint64_t device_count = 0;
    std::uint64_t pending_count = 0;
    std::optional<std::uint64_t> revision;
    std::optional<std::uint64_t> published_revision;

    [[nodiscard]] bool redisAndPublicationEmpty() const noexcept;
};

class BlacklistRedisStore final {
public:
    BlacklistRedisStore(
        IRedisCommandExecutor& executor,
        RedisKeySet keys
    );

    [[nodiscard]] const RedisKeySet& keys() const noexcept { return keys_; }

    [[nodiscard]] StoreResult applyMutations(
        base::ArrayView<const risk::BlacklistMutation> mutations,
        RedisDeadline deadline
    );
    [[nodiscard]] PendingReadResult readPending(
        std::size_t count,
        RedisDeadline deadline
    );
    [[nodiscard]] StoreStatus deletePending(
        base::ArrayView<const std::string> stream_ids,
        RedisDeadline deadline
    );
    [[nodiscard]] StoreResult rebuildFromMysql(
        base::ArrayView<const risk::BlacklistEntry> entries,
        RedisDeadline deadline
    );
    [[nodiscard]] StoreResult readReadyRevision(RedisDeadline deadline);
    [[nodiscard]] StoreResult readRevision(RedisDeadline deadline);
    [[nodiscard]] SnapshotReadResult loadStableSnapshot(
        std::size_t scan_count,
        RedisDeadline deadline
    );
    [[nodiscard]] StoreResult removeExpiredFields(
        base::ArrayView<const risk::BlacklistEntry> expired_entries,
        std::uint64_t expected_revision,
        std::uint64_t now_ms,
        RedisDeadline deadline
    );
    [[nodiscard]] StoreStatus setPublishedRevision(
        std::uint64_t revision,
        RedisDeadline deadline
    );

    [[nodiscard]] StoreResult requestResetBarrier(RedisDeadline deadline);
    [[nodiscard]] ResetReadResult readResetBarrier(RedisDeadline deadline);
    [[nodiscard]] StoreStatus completeResetBarrier(
        std::uint64_t token,
        RedisDeadline deadline
    );
    [[nodiscard]] ResetConvergence readResetConvergence(
        RedisDeadline deadline
    );

private:
    IRedisCommandExecutor* executor_;
    RedisKeySet keys_;
};

}  // 命名空间 aegisflow::storage
