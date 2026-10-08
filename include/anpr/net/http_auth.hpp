#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace anpr::net {

/// A username and password for one camera. Never logged, never written to disk by this project:
/// they come from the environment (see cameras/camera_mode_config.hpp).
struct Credentials {
    std::string username;
    std::string password;

    [[nodiscard]] bool empty() const { return username.empty(); }
};

/// One challenge from a `WWW-Authenticate` header (HTTP and RTSP use the same syntax).
struct AuthChallenge {
    /// Lower case: "digest" or "basic" (anything else is kept but never answered).
    std::string scheme;
    std::string realm;
    std::string nonce;
    std::string opaque;
    /// Empty means MD5. "MD5-sess" is supported; anything else is not answered.
    std::string algorithm;
    /// The raw qop list, for example "auth" or "auth,auth-int". Empty for RFC 2069 digests,
    /// which is what Hikvision's RTSP server sends.
    std::string qop;
    bool stale{false};
};

/// Parses every challenge in the given header values. A single header may carry several
/// comma-separated challenges; quoted strings may contain commas.
std::vector<AuthChallenge> parseAuthChallenges(const std::vector<std::string>& header_values);

/// The challenge to answer: Digest (MD5 / MD5-sess) before Basic. nullptr when none is usable.
const AuthChallenge* preferredChallenge(const std::vector<AuthChallenge>& challenges);

/// Builds the `Authorization` header value answering `challenge` for one request.
/// `nonce_count` starts at 1 for a fresh nonce; `cnonce` is used only when the challenge has qop.
/// Implements RFC 2617 (qop=auth) and its RFC 2069 fallback (no qop), and Basic.
std::string buildAuthorization(const AuthChallenge& challenge, const Credentials& credentials,
                               const std::string& method, const std::string& uri,
                               std::uint32_t nonce_count, const std::string& cnonce);

}  // namespace anpr::net
