#include "auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <sstream>
#include <stdexcept>

namespace falconer {

namespace {
constexpr size_t kSecretBytes = 32;

std::optional<std::string> from_hex(const std::string& hex)
{
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; ++j) {
            const char c = hex[i + j];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        out.push_back(static_cast<char>(value));
    }
    return out;
}
} // namespace

std::optional<SessionSecrets> parse_secrets_line(const std::string& line)
{
    std::istringstream in(line);
    std::string client_hex;
    std::string server_hex;
    if (!(in >> client_hex >> server_hex)) {
        return std::nullopt;
    }
    auto client = from_hex(client_hex);
    auto server = from_hex(server_hex);
    if (!client || !server || client->size() != kSecretBytes || server->size() != kSecretBytes) {
        return std::nullopt;
    }
    return SessionSecrets{*client, *server};
}

std::string random_bytes(size_t count)
{
    std::string out(count, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(count)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return out;
}

std::string to_hex(const std::string& bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

std::string hmac_sha256(const std::string& key, const std::string& data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(
        EVP_sha256(),
        key.data(),
        static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char*>(data.data()),
        data.size(),
        digest,
        &digest_len);
    return std::string(reinterpret_cast<char*>(digest), digest_len);
}

bool constant_time_equal(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace falconer
