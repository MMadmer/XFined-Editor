#include "crypto/xr_sha.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace
{
struct sha0_context
{
    std::array<u32, 5> state{};
    std::array<u8, 64> block{};
    u64 total_bytes{};
    size_t buffered{};
};

// Preserve the historical public pointer layout while keeping SHA-0 private.
sha0_context& get_context(SHAstate_st* opaque_context)
{
    return *reinterpret_cast<sha0_context*>(opaque_context);
}

constexpr u32 rotate_left(u32 value, unsigned shift)
{
    return (value << shift) | (value >> (32u - shift));
}

u32 load_big_endian(u8 const* source)
{
    return static_cast<u32>(source[0]) << 24u
        | static_cast<u32>(source[1]) << 16u
        | static_cast<u32>(source[2]) << 8u
        | static_cast<u32>(source[3]);
}

void store_big_endian(u32 value, u8* destination)
{
    destination[0] = static_cast<u8>(value >> 24u);
    destination[1] = static_cast<u8>(value >> 16u);
    destination[2] = static_cast<u8>(value >> 8u);
    destination[3] = static_cast<u8>(value);
}

void transform(sha0_context& context, u8 const* block)
{
    std::array<u32, 80> words{};
    for (size_t i = 0; i < 16; ++i)
        words[i] = load_big_endian(block + i * 4u);

    // SHA-0 omits the one-bit schedule rotation added by SHA-1.
    for (size_t i = 16; i < words.size(); ++i)
        words[i] = words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16];

    auto a = context.state[0];
    auto b = context.state[1];
    auto c = context.state[2];
    auto d = context.state[3];
    auto e = context.state[4];
    for (size_t i = 0; i < words.size(); ++i)
    {
        u32 function;
        u32 constant;
        if (i < 20)
        {
            function = (b & c) | (~b & d);
            constant = 0x5a827999u;
        }
        else if (i < 40)
        {
            function = b ^ c ^ d;
            constant = 0x6ed9eba1u;
        }
        else if (i < 60)
        {
            function = (b & c) | (b & d) | (c & d);
            constant = 0x8f1bbcdcu;
        }
        else
        {
            function = b ^ c ^ d;
            constant = 0xca62c1d6u;
        }

        auto const next = rotate_left(a, 5) + function + e + constant + words[i];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = next;
    }

    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
    context.state[4] += e;
}

void initialize(sha0_context& context)
{
    context = {};
    context.state = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
}

void update(sha0_context& context, u8 const* data, size_t size)
{
    context.total_bytes += size;
    if (context.buffered)
    {
        auto const count = std::min(size, context.block.size() - context.buffered);
        std::memcpy(context.block.data() + context.buffered, data, count);
        context.buffered += count;
        data += count;
        size -= count;
        if (context.buffered == context.block.size())
        {
            transform(context, context.block.data());
            context.buffered = 0;
        }
    }

    while (size >= context.block.size())
    {
        transform(context, data);
        data += context.block.size();
        size -= context.block.size();
    }

    if (size)
    {
        std::memcpy(context.block.data(), data, size);
        context.buffered = size;
    }
}

void finalize(sha0_context& context, u8* result)
{
    auto const bit_count = context.total_bytes * 8u;
    std::array<u8, 128> padding{};
    padding[0] = 0x80u;
    auto const padding_size = context.buffered < 56 ? 56u - context.buffered : 120u - context.buffered;
    update(context, padding.data(), padding_size);

    std::array<u8, 8> encoded_size{};
    for (size_t i = 0; i < encoded_size.size(); ++i)
        encoded_size[encoded_size.size() - 1u - i] = static_cast<u8>(bit_count >> (i * 8u));
    update(context, encoded_size.data(), encoded_size.size());

    for (size_t i = 0; i < context.state.size(); ++i)
        store_big_endian(context.state[i], result + i * 4u);
}
}

namespace crypto
{
xr_sha256::xr_sha256()
{
    m_sha_ctx = reinterpret_cast<SHAstate_st*>(xr_new<sha0_context>());
}

xr_sha256::~xr_sha256()
{
    auto* context = reinterpret_cast<sha0_context*>(m_sha_ctx);
    xr_delete(context);
}

void xr_sha256::start_calculate(u8 const* data, u32 data_size)
{
    initialize(get_context(m_sha_ctx));
    ZeroMemory(m_result, sizeof(m_result));
    VERIFY(data_size);
    m_data_src = data;
    m_data_size = data_size;
}

bool xr_sha256::continue_calculate()
{
    auto const to_calculate = m_data_size >= calc_chunk_size ? calc_chunk_size : m_data_size;
    auto& context = get_context(m_sha_ctx);
    update(context, m_data_src, to_calculate);
    m_data_src += to_calculate;
    m_data_size -= to_calculate;

    if (m_data_size)
        return false;

    finalize(context, m_result);
    return true;
}
}
