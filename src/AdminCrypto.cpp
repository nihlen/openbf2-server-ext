#include "AdminCrypto.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace serverext {
namespace admin {

bool hmacSha256(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data, uint8_t out[32])
{
    unsigned int len = 0;
    static const uint8_t empty = 0;
    const unsigned char* result =
        HMAC(EVP_sha256(), key.empty() ? &empty : &key[0], static_cast<int>(key.size()),
             data.empty() ? &empty : &data[0], data.size(), out, &len);
    return result && len == 32;
}

bool randomBytes(uint8_t* out, size_t n)
{
    return RAND_bytes(out, static_cast<int>(n)) == 1;
}

bool equalConstantTime(const uint8_t* a, const uint8_t* b, size_t n)
{
    return CRYPTO_memcmp(a, b, n) == 0;
}

void sha256(const uint8_t* data, size_t n, uint8_t out[32])
{
    SHA256(data, n, out);
}

} // namespace admin
} // namespace serverext
