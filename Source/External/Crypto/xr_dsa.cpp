#include "crypto/xr_dsa.h"
#include "openssl_runtime.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace
{
template <typename Type, void (*Deleter)(Type*)>
struct openssl_deleter
{
    void operator()(Type* value) const
    {
        Deleter(value);
    }
};

template <typename Type, void (*Deleter)(Type*)>
using openssl_ptr = std::unique_ptr<Type, openssl_deleter<Type, Deleter>>;

using bn_ptr = openssl_ptr<BIGNUM, BN_free>;
using bn_context_ptr = openssl_ptr<BN_CTX, BN_CTX_free>;
using parameter_builder_ptr = openssl_ptr<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free>;
using parameters_ptr = openssl_ptr<OSSL_PARAM, OSSL_PARAM_free>;
using key_ptr = openssl_ptr<EVP_PKEY, EVP_PKEY_free>;
using key_context_ptr = openssl_ptr<EVP_PKEY_CTX, EVP_PKEY_CTX_free>;
constexpr size_t max_der_signature_size = 2u + 2u * (2u + crypto::xr_dsa::private_key_length + 1u);

struct dsa_context
{
    std::array<u8, crypto::xr_dsa::public_key_length> p{};
    std::array<u8, crypto::xr_dsa::private_key_length> q{};
    std::array<u8, crypto::xr_dsa::public_key_length> g{};
    std::array<u8, crypto::xr_dsa::private_key_length> signing_private{};
    std::array<u8, crypto::xr_dsa::public_key_length> verification_public{};
    key_ptr signing_key;
    key_ptr verification_key;
};

// Preserve the historical public pointer layout without defining OpenSSL internals.
dsa_context& get_context(dsa_st* opaque_context)
{
    return *reinterpret_cast<dsa_context*>(opaque_context);
}

bn_ptr make_number(u8 const* bytes, size_t size)
{
    return bn_ptr(BN_bin2bn(bytes, static_cast<int>(size), nullptr));
}

key_ptr make_key(dsa_context const& domain, BIGNUM const* private_key, BIGNUM const* public_key, int selection)
{
    auto p = make_number(domain.p.data(), domain.p.size());
    auto q = make_number(domain.q.data(), domain.q.size());
    auto g = make_number(domain.g.data(), domain.g.size());
    auto builder = parameter_builder_ptr(OSSL_PARAM_BLD_new());
    if (!p || !q || !g || !builder)
        return {};

    if (!OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_FFC_P, p.get())
        || !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_FFC_Q, q.get())
        || !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_FFC_G, g.get())
        || (private_key && !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_PRIV_KEY, private_key))
        || (public_key && !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_PUB_KEY, public_key)))
        return {};

    auto parameters = parameters_ptr(OSSL_PARAM_BLD_to_param(builder.get()));
    auto context = key_context_ptr(EVP_PKEY_CTX_new_from_name(nullptr, "DSA", nullptr));
    if (!parameters || !context || EVP_PKEY_fromdata_init(context.get()) <= 0)
        return {};

    EVP_PKEY* raw_key{};
    if (EVP_PKEY_fromdata(context.get(), &raw_key, selection, parameters.get()) <= 0)
        return {};
    return key_ptr(raw_key);
}

key_ptr make_signing_key(dsa_context const& domain, u8 const* private_bytes, size_t private_size)
{
    auto private_key = make_number(private_bytes, private_size);
    auto p = make_number(domain.p.data(), domain.p.size());
    auto g = make_number(domain.g.data(), domain.g.size());
    auto public_key = bn_ptr(BN_new());
    auto context = bn_context_ptr(BN_CTX_new());
    if (!private_key || !p || !g || !public_key || !context
        || !BN_mod_exp(public_key.get(), g.get(), private_key.get(), p.get(), context.get()))
        return {};

    return make_key(domain, private_key.get(), public_key.get(), EVP_PKEY_KEYPAIR);
}

key_ptr make_verification_key(dsa_context const& domain, u8 const* public_bytes, size_t public_size)
{
    auto public_key = make_number(public_bytes, public_size);
    if (!public_key)
        return {};
    return make_key(domain, nullptr, public_key.get(), EVP_PKEY_PUBLIC_KEY);
}

std::string encode_hex(std::vector<u8> const& bytes)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result(bytes.size() * 2u, '\0');
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        result[i * 2u] = digits[bytes[i] >> 4u];
        result[i * 2u + 1u] = digits[bytes[i] & 0x0fu];
    }
    return result;
}

int decode_nibble(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}

bool decode_hex(char const* text, std::vector<u8>& result)
{
    if (!text || !*text)
        return false;

    auto const length = xr_strlen(text);
    if (length % 2u || length / 2u > max_der_signature_size)
        return false;

    result.resize(length / 2u);
    for (size_t i = 0; i < result.size(); ++i)
    {
        auto const high = decode_nibble(text[i * 2u]);
        auto const low = decode_nibble(text[i * 2u + 1u]);
        if (high < 0 || low < 0)
            return false;
        result[i] = static_cast<u8>((high << 4) | low);
    }
    return true;
}

#ifdef DEBUG
bn_ptr get_number(EVP_PKEY* key, char const* name)
{
    BIGNUM* value{};
    if (!EVP_PKEY_get_bn_param(key, name, &value))
        return {};
    return bn_ptr(value);
}

template <size_t Size>
std::array<u8, Size> export_number(BIGNUM const* number)
{
    std::array<u8, Size> result{};
    VERIFY(BN_bn2binpad(number, result.data(), static_cast<int>(result.size())) == result.size());
    return result;
}

template <size_t Size>
void print_big_number(std::array<u8, Size> const& bytes, u32 max_columns = 8)
{
    string4096 result_buffer;
    string16 byte_buffer;
    result_buffer[0] = 0;
    xr_strcat(result_buffer, "\t");
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        if (!(i % max_columns) && i)
            xr_strcat(result_buffer, "\n\t");
        xr_sprintf(byte_buffer, "0x%02x, ", bytes[i]);
        xr_strcat(result_buffer, byte_buffer);
    }
    Msg(result_buffer);
}
#endif
}
namespace crypto
{
xr_dsa::xr_dsa(u8 const p[public_key_length], u8 const q[private_key_length], u8 const g[public_key_length])
{
    auto* context = xr_new<dsa_context>();
    std::copy_n(p, context->p.size(), context->p.begin());
    std::copy_n(q, context->q.size(), context->q.begin());
    std::copy_n(g, context->g.size(), context->g.begin());
    m_dsa = reinterpret_cast<dsa_st*>(context);
}

xr_dsa::~xr_dsa()
{
    auto* context = reinterpret_cast<dsa_context*>(m_dsa);
    xr_delete(context);
}

shared_str const xr_dsa::sign(private_key_t const& private_key, u8 const* data, u32 data_size)
{
    if (!detail::ensure_crypto_initialized())
        return {};

    auto& domain = get_context(m_dsa);
    std::array<u8, private_key_length> requested_private{};
    std::copy_n(private_key.m_value, requested_private.size(), requested_private.begin());
    if (!domain.signing_key || domain.signing_private != requested_private)
    {
        auto key = make_signing_key(domain, requested_private.data(), requested_private.size());
        if (!key)
            return {};
        domain.signing_key = std::move(key);
        domain.signing_private = requested_private;
    }

    auto context = key_context_ptr(EVP_PKEY_CTX_new_from_pkey(nullptr, domain.signing_key.get(), nullptr));
    if (!context || EVP_PKEY_sign_init(context.get()) <= 0)
        return {};

    size_t signature_size{};
    if (EVP_PKEY_sign(context.get(), nullptr, &signature_size, data, data_size) <= 0)
        return {};

    std::vector<u8> signature(signature_size);
    if (EVP_PKEY_sign(context.get(), signature.data(), &signature_size, data, data_size) <= 0)
        return {};
    signature.resize(signature_size);
    auto const hex = encode_hex(signature);
    return shared_str(hex.c_str());
}

bool xr_dsa::verify(public_key_t const& public_key, u8 const* data, u32 data_size, shared_str const& signature_text)
{
    if (!detail::ensure_crypto_initialized())
        return false;

    std::vector<u8> signature;
    if (!decode_hex(signature_text.c_str(), signature))
        return false;

    auto& domain = get_context(m_dsa);
    std::array<u8, public_key_length> requested_public{};
    std::copy_n(public_key.m_value, requested_public.size(), requested_public.begin());
    if (!domain.verification_key || domain.verification_public != requested_public)
    {
        auto key = make_verification_key(domain, requested_public.data(), requested_public.size());
        if (!key)
            return false;
        domain.verification_key = std::move(key);
        domain.verification_public = requested_public;
    }

    auto context = key_context_ptr(EVP_PKEY_CTX_new_from_pkey(nullptr, domain.verification_key.get(), nullptr));
    if (!context || EVP_PKEY_verify_init(context.get()) <= 0)
        return false;
    return EVP_PKEY_verify(context.get(), signature.data(), signature.size(), data, data_size) == 1;
}

#ifdef DEBUG
void xr_dsa::generate_params()
{
    if (!detail::ensure_crypto_initialized())
        return;

    auto context = key_context_ptr(EVP_PKEY_CTX_new_from_name(nullptr, "DSA", nullptr));
    unsigned p_bits = key_bit_length;
    unsigned q_bits = private_key_length * 8u;
    char generation_type[] = "fips186_2";
    char digest_name[] = "SHA1";
    OSSL_PARAM generation_parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_FFC_TYPE, generation_type, 0),
        OSSL_PARAM_construct_uint(OSSL_PKEY_PARAM_FFC_PBITS, &p_bits),
        OSSL_PARAM_construct_uint(OSSL_PKEY_PARAM_FFC_QBITS, &q_bits),
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_FFC_DIGEST, digest_name, 0),
        OSSL_PARAM_construct_end(),
    };
    if (!context || EVP_PKEY_paramgen_init(context.get()) <= 0
        || EVP_PKEY_CTX_set_params(context.get(), generation_parameters) <= 0)
        return;

    EVP_PKEY* raw_parameters{};
    if (EVP_PKEY_generate(context.get(), &raw_parameters) <= 0)
        return;
    auto parameters = key_ptr(raw_parameters);

    auto keygen_context = key_context_ptr(EVP_PKEY_CTX_new_from_pkey(nullptr, parameters.get(), nullptr));
    if (!keygen_context || EVP_PKEY_keygen_init(keygen_context.get()) <= 0)
        return;

    EVP_PKEY* raw_key{};
    if (EVP_PKEY_generate(keygen_context.get(), &raw_key) <= 0)
        return;
    auto key = key_ptr(raw_key);

    auto p = get_number(key.get(), OSSL_PKEY_PARAM_FFC_P);
    auto q = get_number(key.get(), OSSL_PKEY_PARAM_FFC_Q);
    auto g = get_number(key.get(), OSSL_PKEY_PARAM_FFC_G);
    auto public_key_number = get_number(key.get(), OSSL_PKEY_PARAM_PUB_KEY);
    auto private_key_number = get_number(key.get(), OSSL_PKEY_PARAM_PRIV_KEY);
    if (!p || !q || !g || !public_key_number || !private_key_number)
        return;

    auto const p_bytes = export_number<public_key_length>(p.get());
    auto const q_bytes = export_number<private_key_length>(q.get());
    auto const g_bytes = export_number<public_key_length>(g.get());
    auto const public_key_bytes = export_number<public_key_length>(public_key_number.get());
    auto const private_key_bytes = export_number<private_key_length>(private_key_number.get());

    Msg("// DSA params");
    Msg("u8 const p_number[crypto::xr_dsa::public_key_length] = {");
    print_big_number(p_bytes);
    Msg("};//p_number");
    Msg("u8 const q_number[crypto::xr_dsa::private_key_length] = {");
    print_big_number(q_bytes);
    Msg("};//q_number");
    Msg("u8 const g_number[crypto::xr_dsa::public_key_length] = {");
    print_big_number(g_bytes);
    Msg("};//g_number");
    Msg("u8 const public_key[crypto::xr_dsa::public_key_length] = {");
    print_big_number(public_key_bytes);
    Msg("};//public_key");
    Msg("// Private key:");
    for (size_t i = 0; i < private_key_bytes.size(); ++i)
        Msg("\tm_private_key.m_value[%d]\t= 0x%02x;", static_cast<int>(i), private_key_bytes[i]);

    xr_dsa generated(p_bytes.data(), q_bytes.data(), g_bytes.data());
    private_key_t private_key{};
    public_key_t public_key{};
    std::copy(private_key_bytes.begin(), private_key_bytes.end(), private_key.m_value);
    std::copy(public_key_bytes.begin(), public_key_bytes.end(), public_key.m_value);
    u8 test_digest[] = "this is a test";
    u8 bad_digest[] = "this as a test";
    auto const signature = generated.sign(private_key, test_digest, sizeof(test_digest));
    VERIFY(generated.verify(public_key, test_digest, sizeof(test_digest), signature));
    VERIFY(!generated.verify(public_key, bad_digest, sizeof(bad_digest), signature));
}
#endif
}
