#pragma once

#include "aegisflow/net/protocol_contract.hpp"

#include <cstddef>
#include <limits>

namespace aegisflow::net {

struct LimitsConfig {
    std::size_t max_connections = 65536;
    std::size_t max_connections_per_loop = 65536;
    std::size_t connection_queue_capacity = 1024;
    std::size_t business_queue_capacity = 1024;
    std::size_t completion_queue_capacity = 2048;
    std::size_t completion_queue_byte_capacity = 16U * 1024U * 1024U;
    std::size_t max_frame_payload_bytes = protocol::kMaxPayloadSize;
    std::size_t input_soft_watermark_bytes = 64U * 1024U;
    std::size_t max_input_buffer_bytes =
        protocol::kFrameHeaderSize + protocol::kMaxPayloadSize;
    std::size_t output_soft_watermark_bytes = 64U * 1024U;
    std::size_t max_output_buffer_bytes =
        protocol::kFrameHeaderSize + protocol::kMaxPayloadSize;

    [[nodiscard]] constexpr bool operator==(
        const LimitsConfig& other
    ) const noexcept {
        return max_connections == other.max_connections &&
               max_connections_per_loop ==
                   other.max_connections_per_loop &&
               connection_queue_capacity ==
                   other.connection_queue_capacity &&
               business_queue_capacity == other.business_queue_capacity &&
               completion_queue_capacity ==
                   other.completion_queue_capacity &&
               completion_queue_byte_capacity ==
                   other.completion_queue_byte_capacity &&
               max_frame_payload_bytes == other.max_frame_payload_bytes &&
               input_soft_watermark_bytes ==
                   other.input_soft_watermark_bytes &&
               max_input_buffer_bytes == other.max_input_buffer_bytes &&
               output_soft_watermark_bytes ==
                   other.output_soft_watermark_bytes &&
               max_output_buffer_bytes == other.max_output_buffer_bytes;
    }
};

[[nodiscard]] inline bool validLimits(
    const LimitsConfig& limits,
    const std::size_t loop_count,
    const std::size_t worker_threads
) noexcept {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (loop_count == 0 || worker_threads == 0 ||
        limits.max_connections == 0 ||
        limits.max_connections_per_loop == 0 ||
        limits.connection_queue_capacity == 0 ||
        limits.business_queue_capacity == 0 ||
        limits.completion_queue_capacity == 0 ||
        limits.completion_queue_byte_capacity == 0 ||
        limits.max_frame_payload_bytes == 0 ||
        limits.input_soft_watermark_bytes == 0 ||
        limits.max_input_buffer_bytes == 0 ||
        limits.output_soft_watermark_bytes == 0 ||
        limits.max_output_buffer_bytes == 0) {
        return false;
    }
    if (limits.max_frame_payload_bytes > protocol::kMaxPayloadSize ||
        limits.max_frame_payload_bytes >
            maximum - protocol::kFrameHeaderSize ||
        limits.max_input_buffer_bytes <
            protocol::kFrameHeaderSize + limits.max_frame_payload_bytes ||
        limits.max_input_buffer_bytes >
            protocol::kFrameHeaderSize + protocol::kMaxPayloadSize ||
        limits.input_soft_watermark_bytes >
            limits.max_input_buffer_bytes ||
        limits.max_output_buffer_bytes < protocol::kFrameHeaderSize + 1U ||
        limits.max_output_buffer_bytes >
            protocol::kFrameHeaderSize + protocol::kMaxPayloadSize ||
        limits.output_soft_watermark_bytes >
            limits.max_output_buffer_bytes) {
        return false;
    }
    if (limits.max_connections_per_loop > maximum / loop_count ||
        limits.max_connections >
            loop_count * limits.max_connections_per_loop ||
        limits.connection_queue_capacity >
            limits.max_connections_per_loop ||
        worker_threads > maximum - limits.business_queue_capacity ||
        limits.completion_queue_capacity <
            limits.business_queue_capacity + worker_threads ||
        limits.completion_queue_byte_capacity <
            limits.max_output_buffer_bytes) {
        return false;
    }
    return true;
}

}  // 命名空间 aegisflow::net
