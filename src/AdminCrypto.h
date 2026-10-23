// The admin channel's cryptographic primitives, from OpenSSL's libcrypto (linked dynamically).
// Engine-free and socket-free.

#ifndef SERVEREXT_ADMINCRYPTO_H
#define SERVEREXT_ADMINCRYPTO_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace serverext {
namespace admin {

// HMAC-SHA256 of data under key. @return False only if OpenSSL fails.
bool hmacSha256(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data, uint8_t out[32]);
// Fills out with n bytes from OpenSSL's CSPRNG. @return False when the generator is not seeded.
bool randomBytes(uint8_t* out, size_t n);
// Constant-time comparison (CRYPTO_memcmp).
bool equalConstantTime(const uint8_t* a, const uint8_t* b, size_t n);
// SHA-256 of data.
void sha256(const uint8_t* data, size_t n, uint8_t out[32]);

} // namespace admin
} // namespace serverext

#endif // SERVEREXT_ADMINCRYPTO_H
