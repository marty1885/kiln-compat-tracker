#pragma once

#include <drogon/HttpRequest.h>
#include <botan/bcrypt.h>
#include <botan/auto_rng.h>
#include <botan/hex.h>
#include <string>

namespace kiln {

inline std::string hash_password(const std::string &password) {
    Botan::AutoSeeded_RNG rng;
    return Botan::generate_bcrypt(password, rng, 12);
}

inline bool verify_password(const std::string &password,
                            const std::string &hash) {
    return Botan::check_bcrypt(password, hash);
}

inline std::string generate_token() {
    Botan::AutoSeeded_RNG rng;
    std::vector<uint8_t> buf(32);
    rng.randomize(buf.data(), buf.size());
    return Botan::hex_encode(buf, false);
}

inline bool is_admin(const drogon::HttpRequestPtr &req) {
    auto session = req->session();
    if (!session) return false;
    return session->find("admin_id");
}

} // namespace kiln
