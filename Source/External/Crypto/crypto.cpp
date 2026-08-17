#include "crypto/crypto.h"
#include "openssl_runtime.h"

#include <mutex>

#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace
{
std::once_flag initialization_flag;
bool initialized{};

void initialize_crypto()
{
    if (!OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr))
        return;

    string256 random_seed;
    xr_sprintf(random_seed, "%I64d_%s", CPU::QPC(), "S.T.A.L.K.E.R. 4ever Rulezz !!!");
    RAND_add(random_seed, xr_strlen(random_seed), 0.0);
    initialized = true;
}
}

namespace crypto::detail
{
bool ensure_crypto_initialized()
{
    std::call_once(initialization_flag, initialize_crypto);
    return initialized;
}
}

namespace crypto
{
void xr_crypto_init()
{
    VERIFY(detail::ensure_crypto_initialized());
}
}
