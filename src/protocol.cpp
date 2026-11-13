#include "protocol.h"

#include <algorithm>
#include <cstring>

namespace ss {

namespace {

inline void WriteU16LE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void WriteU32LE(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

inline void WriteU64LE(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

inline uint32_t RotateLeft(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

struct Sha1Context {
    uint32_t state[5];
    uint64_t total_bits;
    uint8_t buffer[64];
    size_t buffer_size;
};

void Sha1Init(Sha1Context& ctx) {
    ctx.state[0] = 0x67452301;
    ctx.state[1] = 0xEFCDAB89;
    ctx.state[2] = 0x98BADCFE;
    ctx.state[3] = 0x10325476;
    ctx.state[4] = 0xC3D2E1F0;
    ctx.total_bits = 0;
    ctx.buffer_size = 0;
}

void Sha1ProcessBlock(Sha1Context& ctx, const uint8_t* block) {
    uint32_t words[80];
    for (int i = 0; i < 16; ++i) {
        words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        words[i] = RotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    }

    uint32_t a = ctx.state[0];
    uint32_t b = ctx.state[1];
    uint32_t c = ctx.state[2];
    uint32_t d = ctx.state[3];
    uint32_t e = ctx.state[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32_t temp = RotateLeft(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = RotateLeft(b, 30);
        b = a;
        a = temp;
    }

    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
    ctx.state[4] += e;
}

void Sha1Update(Sha1Context& ctx, const uint8_t* data, size_t len) {
    ctx.total_bits += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        const size_t space = 64 - ctx.buffer_size;
        const size_t take = std::min(space, len);
        std::memcpy(ctx.buffer + ctx.buffer_size, data, take);
        ctx.buffer_size += take;
        data += take;
        len -= take;
        if (ctx.buffer_size == 64) {
            Sha1ProcessBlock(ctx, ctx.buffer);
            ctx.buffer_size = 0;
        }
    }
}

void Sha1Final(Sha1Context& ctx, uint8_t digest[20]) {
    uint8_t padding[128] = {0x80};
    const uint64_t total_bits = ctx.total_bits;
    size_t pad_len = (ctx.buffer_size < 56) ? (56 - ctx.buffer_size) : (120 - ctx.buffer_size);
    uint8_t length_bytes[8];
    for (int i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<uint8_t>((total_bits >> (8 * (7 - i))) & 0xFF);
    }
    Sha1Update(ctx, padding, pad_len);
    Sha1Update(ctx, length_bytes, 8);
    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<uint8_t>((ctx.state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((ctx.state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((ctx.state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i] & 0xFF);
    }
}

} // namespace

std::string Base64Encode(const uint8_t* data, size_t len) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                                (static_cast<uint32_t>(data[i + 1]) << 8) |
                                static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back(kTable[(triple >> 6) & 0x3F]);
        out.push_back(kTable[triple & 0x3F]);
        i += 3;
    }
    const size_t remaining = len - i;
    if (remaining == 1) {
        const uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                                (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back(kTable[(triple >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string Sha1Base64(const std::string& data) {
    Sha1Context ctx;
    Sha1Init(ctx);
    Sha1Update(ctx, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    uint8_t digest[20] = {};
    Sha1Final(ctx, digest);
    return Base64Encode(digest, sizeof(digest));
}

std::vector<uint8_t> MakeWebSocketFrame(uint8_t opcode, const uint8_t* payload, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(len + 10);
    out.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F)));
    if (len < 126) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len < 65536) {
        out.push_back(126);
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(len) >> (8 * i)) & 0xFF));
        }
    }
    if (len > 0) {
        out.insert(out.end(), payload, payload + len);
    }
    return out;
}

std::vector<std::vector<uint8_t>> ChunkFrame(const FrameMeta& meta, const uint8_t* payload, size_t len) {
    std::vector<std::vector<uint8_t>> chunks;
    size_t chunk_count = (len == 0) ? 1 : ((len + kMaxChunkPayload - 1) / kMaxChunkPayload);
    if (chunk_count > 0xFFFF) {
        chunk_count = 0xFFFF;
    }
    chunks.reserve(chunk_count);

    size_t offset = 0;
    for (size_t index = 0; index < chunk_count; ++index) {
        const size_t remaining = (offset < len) ? (len - offset) : 0;
        const size_t chunk_len = std::min(kMaxChunkPayload, remaining);
        std::vector<uint8_t> buffer(kChunkHeaderSize + chunk_len);
        uint8_t* p = buffer.data();
        p[0] = 'S';
        p[1] = 'S';
        p[2] = 'F';
        p[3] = '1';
        WriteU32LE(p + 4, meta.seq);
        WriteU64LE(p + 8, static_cast<uint64_t>(meta.ts_us));
        WriteU16LE(p + 16, meta.width);
        WriteU16LE(p + 18, meta.height);
        p[20] = meta.format;
        WriteU32LE(p + 21, static_cast<uint32_t>(len));
        WriteU16LE(p + 25, static_cast<uint16_t>(index));
        WriteU16LE(p + 27, static_cast<uint16_t>(chunk_count));
        WriteU32LE(p + 29, static_cast<uint32_t>(chunk_len));
        if (chunk_len > 0) {
            std::memcpy(p + kChunkHeaderSize, payload + offset, chunk_len);
        }
        offset += chunk_len;
        chunks.push_back(std::move(buffer));
    }
    return chunks;
}

} // namespace ss
