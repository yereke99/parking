// WWW-Authenticate parsing and Authorization building for HTTP and RTSP (RFC 2617 / RFC 2069
// digest, Basic). Building a header is pure: whether credentials are sent, and how often, is
// decided by the clients, which send them at most once per probe.
#include "anpr/net/http_auth.hpp"

#include <cctype>
#include <cstdio>

#include "anpr/net/crypto.hpp"

namespace anpr::net {
namespace {

std::string toLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool equalsIgnoreCase(const std::string& lhs, const std::string& rhs) {
    return toLower(lhs) == toLower(rhs);
}

/// RFC 7230 tchar.
bool isTokenChar(char ch) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
        return true;
    }
    switch (ch) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

/// A cursor over one header value.
class Cursor {
public:
    explicit Cursor(const std::string& text) : text_(text) {}

    [[nodiscard]] bool atEnd() const { return position_ >= text_.size(); }
    [[nodiscard]] char peek() const { return atEnd() ? '\0' : text_[position_]; }
    [[nodiscard]] std::size_t position() const { return position_; }
    void rewind(std::size_t position) { position_ = position; }
    void advance() { ++position_; }

    void skipSpaces() {
        while (!atEnd() && (peek() == ' ' || peek() == '\t')) {
            ++position_;
        }
    }

    void skipSpacesAndCommas() {
        while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == ',')) {
            ++position_;
        }
    }

    std::string token() {
        const std::size_t begin = position_;
        while (!atEnd() && isTokenChar(peek())) {
            ++position_;
        }
        return text_.substr(begin, position_ - begin);
    }

    /// A quoted-string with backslash escapes; the cursor is on the opening quote. An unterminated
    /// string takes the rest of the value, which is what lenient servers mean.
    std::string quoted() {
        std::string value;
        ++position_;
        while (!atEnd()) {
            const char ch = text_[position_++];
            if (ch == '"') {
                return value;
            }
            if (ch == '\\' && !atEnd()) {
                value.push_back(text_[position_++]);
            } else {
                value.push_back(ch);
            }
        }
        return value;
    }

    /// An unquoted parameter value: everything up to the next comma, trimmed. More permissive
    /// than a token so that values such as `algorithm=MD5-sess` or odd firmware output survive.
    std::string bareValue() {
        const std::size_t begin = position_;
        while (!atEnd() && peek() != ',') {
            ++position_;
        }
        std::string value = text_.substr(begin, position_ - begin);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.pop_back();
        }
        return value;
    }

private:
    const std::string& text_;
    std::size_t position_{0};
};

void applyParameter(AuthChallenge& challenge, const std::string& name, const std::string& value) {
    if (name == "realm") {
        challenge.realm = value;
    } else if (name == "nonce") {
        challenge.nonce = value;
    } else if (name == "opaque") {
        challenge.opaque = value;
    } else if (name == "algorithm") {
        challenge.algorithm = value;
    } else if (name == "qop") {
        challenge.qop = value;
    } else if (name == "stale") {
        challenge.stale = equalsIgnoreCase(value, "true");
    }
}

/// Parses the parameters after a scheme until the next challenge or the end of the value.
void parseParameters(Cursor& cursor, AuthChallenge& challenge) {
    for (;;) {
        cursor.skipSpacesAndCommas();
        if (cursor.atEnd()) {
            return;
        }
        const std::size_t start = cursor.position();
        const std::string name = cursor.token();
        if (name.empty()) {
            cursor.advance();  // Stray character: skip it rather than loop forever.
            continue;
        }
        cursor.skipSpaces();
        if (cursor.peek() != '=') {
            // `token` not followed by '=' starts the next challenge ("..., Basic realm=...").
            cursor.rewind(start);
            return;
        }
        cursor.advance();
        cursor.skipSpaces();
        if (cursor.atEnd() || cursor.peek() == ',' || cursor.peek() == '=') {
            // token68 such as "Negotiate abc==": consume the padding, keep nothing.
            while (cursor.peek() == '=') {
                cursor.advance();
            }
            continue;
        }
        const std::string value = cursor.peek() == '"' ? cursor.quoted() : cursor.bareValue();
        applyParameter(challenge, toLower(name), value);
    }
}

bool qopOffersAuth(const std::string& qop) {
    std::size_t begin = 0;
    while (begin <= qop.size()) {
        std::size_t end = qop.find(',', begin);
        if (end == std::string::npos) {
            end = qop.size();
        }
        std::string item = qop.substr(begin, end - begin);
        const std::size_t first = item.find_first_not_of(" \t");
        const std::size_t last = item.find_last_not_of(" \t");
        item = first == std::string::npos ? std::string() : item.substr(first, last - first + 1);
        if (equalsIgnoreCase(item, "auth")) {
            return true;
        }
        begin = end + 1;
    }
    return false;
}

bool isMd5Sess(const AuthChallenge& challenge) {
    return equalsIgnoreCase(challenge.algorithm, "md5-sess");
}

bool digestUsable(const AuthChallenge& challenge) {
    if (challenge.scheme != "digest" || challenge.nonce.empty()) {
        return false;
    }
    const bool md5 = challenge.algorithm.empty() || equalsIgnoreCase(challenge.algorithm, "md5");
    if (!md5 && !isMd5Sess(challenge)) {
        return false;  // SHA-256 and friends: not implemented, never answered.
    }
    // A qop list without "auth" (only auth-int) cannot be answered; MD5-sess needs a cnonce,
    // which only exists with qop.
    if (!challenge.qop.empty() && !qopOffersAuth(challenge.qop)) {
        return false;
    }
    return !(isMd5Sess(challenge) && challenge.qop.empty());
}

/// Escapes `"` and `\` for a quoted-string parameter value.
std::string quote(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

}  // namespace

std::vector<AuthChallenge> parseAuthChallenges(const std::vector<std::string>& header_values) {
    std::vector<AuthChallenge> challenges;
    for (const std::string& value : header_values) {
        Cursor cursor(value);
        for (;;) {
            cursor.skipSpacesAndCommas();
            if (cursor.atEnd()) {
                break;
            }
            const std::string scheme = cursor.token();
            if (scheme.empty()) {
                cursor.advance();
                continue;
            }
            AuthChallenge challenge;
            challenge.scheme = toLower(scheme);
            parseParameters(cursor, challenge);
            challenges.push_back(std::move(challenge));
        }
    }
    return challenges;
}

const AuthChallenge* preferredChallenge(const std::vector<AuthChallenge>& challenges) {
    for (const AuthChallenge& challenge : challenges) {
        if (digestUsable(challenge)) {
            return &challenge;
        }
    }
    for (const AuthChallenge& challenge : challenges) {
        if (challenge.scheme == "basic") {
            return &challenge;
        }
    }
    return nullptr;
}

std::string buildAuthorization(const AuthChallenge& challenge, const Credentials& credentials,
                               const std::string& method, const std::string& uri,
                               std::uint32_t nonce_count, const std::string& cnonce) {
    if (challenge.scheme == "basic") {
        return "Basic " + base64Encode(credentials.username + ":" + credentials.password);
    }
    if (challenge.scheme != "digest") {
        return {};
    }

    const bool with_qop = qopOffersAuth(challenge.qop);
    const std::string client_nonce = cnonce.empty() ? randomHex(8) : cnonce;
    char count_text[9];
    std::snprintf(count_text, sizeof(count_text), "%08x", static_cast<unsigned>(nonce_count));

    std::string ha1 = md5Hex(credentials.username + ":" + challenge.realm + ":" +
                             credentials.password);
    if (isMd5Sess(challenge)) {
        ha1 = md5Hex(ha1 + ":" + challenge.nonce + ":" + client_nonce);
    }
    const std::string ha2 = md5Hex(method + ":" + uri);
    const std::string response =
        with_qop ? md5Hex(ha1 + ":" + challenge.nonce + ":" + count_text + ":" + client_nonce +
                          ":auth:" + ha2)
                 : md5Hex(ha1 + ":" + challenge.nonce + ":" + ha2);

    std::string header = "Digest username=" + quote(credentials.username) +
                         ", realm=" + quote(challenge.realm) + ", nonce=" + quote(challenge.nonce) +
                         ", uri=" + quote(uri);
    if (!challenge.algorithm.empty()) {
        header += ", algorithm=" + challenge.algorithm;
    }
    header += ", response=" + quote(response);
    if (!challenge.opaque.empty()) {
        header += ", opaque=" + quote(challenge.opaque);
    }
    if (with_qop) {
        header += ", qop=auth, nc=" + std::string(count_text) + ", cnonce=" + quote(client_nonce);
    } else if (isMd5Sess(challenge)) {
        header += ", cnonce=" + quote(client_nonce);
    }
    return header;
}

}  // namespace anpr::net
