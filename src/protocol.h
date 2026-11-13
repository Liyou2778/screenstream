#pragma once

#include "common.h"

#include <cstddef>
#include <vector>

namespace ss {

// ---------------------------------------------------------------------------
// ScreenStream frame protocol (V1)
//
// Every WebSocket binary message carries exactly one chunk of one frame.
// Each chunk repeats the full frame metadata so a receiver can reassemble
// chunks independently, even if messages are interleaved or lost.
//
//  offset size field
//  0      4    magic        "SSF1" (0x53 0x53 0x46 0x31)
//  4      4    seq          uint32 LE, monotonically increasing frame id
//  8      8    ts_us        uint64 LE, Unix epoch microseconds (server clock)
//  16     2    width        uint16 LE, frame width in pixels
//  18     2    height       uint16 LE, frame height in pixels
//  20     1    fmt          1 = JPEG
//  21     4    total_len    uint32 LE, total payload bytes of the whole frame
//  25     2    chunk_index  uint16 LE, 0 .. chunk_count-1
//  27     2    chunk_count  uint16 LE, ceil(total_len / 32768)
//  29     4    chunk_len    uint32 LE, payload bytes carried by this chunk
//  33     N    payload      JPEG bytes slice
// ---------------------------------------------------------------------------
constexpr size_t kChunkHeaderSize = 33;
constexpr size_t kMaxChunkPayload = 32 * 1024;
constexpr uint8_t kFrameFormatJpeg = 1;
constexpr uint32_t kFrameMagic = 0x31465353u; // "SSF1" little endian

struct FrameMeta {
    uint32_t seq = 0;
    int64_t ts_us = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t format = kFrameFormatJpeg;
};

// Splits one encoded frame into wire messages (each beginning with the 33 byte header).
std::vector<std::vector<uint8_t>> ChunkFrame(const FrameMeta& meta, const uint8_t* payload, size_t len);

// Builds an unmasked server->client WebSocket frame.
std::vector<uint8_t> MakeWebSocketFrame(uint8_t opcode, const uint8_t* payload, size_t len);

std::string Base64Encode(const uint8_t* data, size_t len);

// SHA1 digest of `data`, base64 encoded (needed for the WebSocket handshake).
std::string Sha1Base64(const std::string& data);

} // namespace ss
