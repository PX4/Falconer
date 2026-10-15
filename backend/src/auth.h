#pragma once

#include <optional>
#include <string>

namespace falconer {

// Per-launch secrets handed over by the launcher (Tauri shell or fctl).
struct SessionSecrets {
    std::string client; // raw bytes
    std::string server; // raw bytes
};

// Parses "<client_hex> <server_hex>", each 32 bytes.
std::optional<SessionSecrets> parse_secrets_line(const std::string& line);

std::string random_bytes(size_t count);
std::string to_hex(const std::string& bytes);
std::string hmac_sha256(const std::string& key, const std::string& data);
bool constant_time_equal(const std::string& a, const std::string& b);

} // namespace falconer
