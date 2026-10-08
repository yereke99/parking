// Minimal XML scanning for camera protocols. One forward tokenizer drives every query, so a
// malformed or truncated document costs one linear pass and ends the query early instead of
// throwing or looping.
#include "anpr/hikvision/xml_lite.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace anpr::xml {
namespace {

constexpr std::size_t kNpos = std::string_view::npos;

enum class TokenKind { kStartTag, kEmptyTag, kEndTag, kText, kCdata, kSkipped };

struct Token {
    TokenKind kind{TokenKind::kSkipped};
    std::size_t begin{0};
    std::size_t end{0};
    /// Local name for tags.
    std::string_view name;
    /// Raw payload of text and CDATA tokens.
    std::string_view content;
};

bool isXmlSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

bool isNameStart(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return std::isalpha(byte) != 0 || ch == '_' || ch == ':' || byte >= 0x80;
}

std::string_view localName(std::string_view qualified) {
    const std::size_t colon = qualified.rfind(':');
    return colon == kNpos ? qualified : qualified.substr(colon + 1);
}

/// Splits a document into tags, text, CDATA and skipped constructs (comments, processing
/// instructions, DOCTYPE). `next` returns false at the end of the input and at a construct that
/// is cut off, so a truncated document simply ends early.
class Tokenizer {
public:
    explicit Tokenizer(std::string_view text) : text_(text) {}

    bool next(Token& token) {
        if (pos_ >= text_.size()) {
            return false;
        }
        token = Token{};
        token.begin = pos_;
        if (text_[pos_] != '<') {
            std::size_t lt = text_.find('<', pos_);
            if (lt == kNpos) {
                lt = text_.size();
            }
            return emit(token, TokenKind::kText, lt, text_.substr(pos_, lt - pos_));
        }
        const std::string_view rest = text_.substr(pos_);
        if (startsWith(rest, "<!--")) {
            return skipTo(token, "-->", 4);
        }
        if (startsWith(rest, "<![CDATA[")) {
            const std::size_t close = text_.find("]]>", pos_ + 9);
            if (close == kNpos) {
                return stop();
            }
            return emit(token, TokenKind::kCdata, close + 3,
                        text_.substr(pos_ + 9, close - pos_ - 9));
        }
        if (startsWith(rest, "<?")) {
            return skipTo(token, "?>", 2);
        }
        if (startsWith(rest, "<!")) {
            return declaration(token);
        }
        if (startsWith(rest, "</")) {
            return endTag(token);
        }
        if (rest.size() < 2) {
            return stop();
        }
        if (!isNameStart(rest[1])) {
            // A stray '<' (invalid XML): keep it as text so the scan always moves forward.
            return emit(token, TokenKind::kText, pos_ + 1, text_.substr(pos_, 1));
        }
        return startTag(token);
    }

private:
    std::string_view text_;
    std::size_t pos_{0};

    static bool startsWith(std::string_view text, std::string_view prefix) {
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    bool stop() {
        pos_ = text_.size();
        return false;
    }

    bool emit(Token& token, TokenKind kind, std::size_t end, std::string_view content) {
        token.kind = kind;
        token.end = end;
        token.content = content;
        pos_ = end;
        return true;
    }

    bool skipTo(Token& token, std::string_view terminator, std::size_t opener_size) {
        const std::size_t close = text_.find(terminator, pos_ + opener_size);
        if (close == kNpos) {
            return stop();
        }
        return emit(token, TokenKind::kSkipped, close + terminator.size(), {});
    }

    /// <!DOCTYPE ...> and other declarations; an internal subset in [...] may contain '>'.
    bool declaration(Token& token) {
        int brackets = 0;
        char quote = 0;
        for (std::size_t i = pos_ + 2; i < text_.size(); ++i) {
            const char ch = text_[i];
            if (quote != 0) {
                if (ch == quote) {
                    quote = 0;
                }
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == '[') {
                ++brackets;
            } else if (ch == ']') {
                brackets = std::max(0, brackets - 1);
            } else if (ch == '>' && brackets == 0) {
                return emit(token, TokenKind::kSkipped, i + 1, {});
            }
        }
        return stop();
    }

    std::size_t nameEnd(std::size_t from) const {
        std::size_t i = from;
        while (i < text_.size() && !isXmlSpace(text_[i]) && text_[i] != '>' && text_[i] != '/') {
            ++i;
        }
        return i;
    }

    bool endTag(Token& token) {
        const std::size_t name_begin = pos_ + 2;
        const std::size_t name_end = nameEnd(name_begin);
        const std::size_t close = text_.find('>', name_end);
        if (close == kNpos) {
            return stop();
        }
        token.name = localName(text_.substr(name_begin, name_end - name_begin));
        return emit(token, TokenKind::kEndTag, close + 1, {});
    }

    bool startTag(Token& token) {
        const std::size_t name_begin = pos_ + 1;
        const std::size_t name_end = nameEnd(name_begin);
        // Attributes are skipped, but a quoted value may contain '>' or '/'.
        char quote = 0;
        char last = 0;
        for (std::size_t i = name_end; i < text_.size(); ++i) {
            const char ch = text_[i];
            if (quote != 0) {
                if (ch == quote) {
                    quote = 0;
                }
                continue;
            }
            if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == '>') {
                token.name = localName(text_.substr(name_begin, name_end - name_begin));
                return emit(token, last == '/' ? TokenKind::kEmptyTag : TokenKind::kStartTag,
                            i + 1, {});
            }
            if (!isXmlSpace(ch)) {
                last = ch;
            }
        }
        return stop();
    }
};

bool isTag(TokenKind kind) {
    return kind == TokenKind::kStartTag || kind == TokenKind::kEmptyTag ||
           kind == TokenKind::kEndTag;
}

void appendUtf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

/// "#65" or "#x41" to a code point. Rejects NUL, surrogates and values beyond Unicode.
bool parseCharacterReference(std::string_view entity, std::uint32_t& code_point) {
    const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
    const std::size_t first = hex ? 2 : 1;
    if (entity.size() <= first) {
        return false;
    }
    std::uint32_t value = 0;
    for (std::size_t i = first; i < entity.size(); ++i) {
        const auto ch = static_cast<unsigned char>(entity[i]);
        std::uint32_t digit = 0;
        if (std::isdigit(ch) != 0) {
            digit = ch - '0';
        } else if (hex && std::isxdigit(ch) != 0) {
            digit = static_cast<std::uint32_t>(std::tolower(ch) - 'a' + 10);
        } else {
            return false;
        }
        value = value * (hex ? 16U : 10U) + digit;
        if (value > 0x10FFFF) {
            return false;
        }
    }
    if (value == 0 || (value >= 0xD800 && value <= 0xDFFF)) {
        return false;
    }
    code_point = value;
    return true;
}

bool decodeEntity(std::string_view entity, std::string& out) {
    if (entity == "amp") {
        out.push_back('&');
    } else if (entity == "lt") {
        out.push_back('<');
    } else if (entity == "gt") {
        out.push_back('>');
    } else if (entity == "quot") {
        out.push_back('"');
    } else if (entity == "apos") {
        out.push_back('\'');
    } else if (!entity.empty() && entity[0] == '#') {
        std::uint32_t code_point = 0;
        if (!parseCharacterReference(entity, code_point)) {
            return false;
        }
        appendUtf8(out, code_point);
    } else {
        return false;
    }
    return true;
}

/// Unknown or malformed references are kept as written: losing a character of a plate or a
/// model name would be worse than showing the raw reference.
void appendDecoded(std::string& out, std::string_view text) {
    // "&#x0000010FFFF;" is the longest reference worth decoding; bounding the ';' search keeps a
    // text full of bare '&' linear.
    constexpr std::size_t kMaxEntity = 16;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t amp = text.find('&', i);
        if (amp == kNpos) {
            out.append(text.substr(i));
            return;
        }
        out.append(text.substr(i, amp - i));
        const std::size_t limit = std::min(text.size(), amp + 1 + kMaxEntity);
        std::size_t semicolon = kNpos;
        for (std::size_t j = amp + 1; j < limit; ++j) {
            if (text[j] == ';') {
                semicolon = j;
                break;
            }
            if (text[j] == '&') {
                break;
            }
        }
        if (semicolon != kNpos && decodeEntity(text.substr(amp + 1, semicolon - amp - 1), out)) {
            i = semicolon + 1;
        } else {
            out.push_back('&');
            i = amp + 1;
        }
    }
}

std::string textContent(std::string_view inner) {
    std::string out;
    Tokenizer tokenizer(inner);
    Token token;
    while (tokenizer.next(token)) {
        if (token.kind == TokenKind::kText) {
            appendDecoded(out, token.content);
        } else if (token.kind == TokenKind::kCdata) {
            out.append(token.content);
        }
    }
    return out;
}

/// Inner XML of up to `limit` elements named `name`, outermost first. Nested elements of the
/// same name only move the depth counter, which pairs every start tag with its own close tag.
std::vector<std::string_view> findElements(std::string_view xml, const std::string& name,
                                           std::size_t limit) {
    std::vector<std::string_view> found;
    const std::string_view wanted = localName(name);
    if (wanted.empty()) {
        return found;
    }
    Tokenizer tokenizer(xml);
    Token token;
    std::size_t depth = 0;
    std::size_t inner_begin = 0;
    while (found.size() < limit && tokenizer.next(token)) {
        if (!isTag(token.kind) || token.name != wanted) {
            continue;
        }
        if (token.kind == TokenKind::kStartTag) {
            if (depth == 0) {
                inner_begin = token.end;
            }
            ++depth;
        } else if (token.kind == TokenKind::kEmptyTag) {
            if (depth == 0) {
                found.push_back(xml.substr(token.end, 0));
            }
        } else if (depth > 0) {
            --depth;
            if (depth == 0) {
                found.push_back(xml.substr(inner_begin, token.begin - inner_begin));
            }
        }
    }
    return found;
}

}  // namespace

std::optional<std::string> firstText(const std::string& xml, const std::string& local_name) {
    const auto found = findElements(xml, local_name, 1);
    if (found.empty()) {
        return std::nullopt;
    }
    return textContent(found.front());
}

std::vector<std::string> allTexts(const std::string& xml, const std::string& local_name) {
    std::vector<std::string> texts;
    for (const std::string_view inner : findElements(xml, local_name, kNpos)) {
        texts.push_back(textContent(inner));
    }
    return texts;
}

std::optional<std::string> firstElement(const std::string& xml, const std::string& local_name) {
    const auto found = findElements(xml, local_name, 1);
    if (found.empty()) {
        return std::nullopt;
    }
    return std::string(found.front());
}

std::vector<std::string> allElements(const std::string& xml, const std::string& local_name) {
    std::vector<std::string> blocks;
    for (const std::string_view inner : findElements(xml, local_name, kNpos)) {
        blocks.emplace_back(inner);
    }
    return blocks;
}

std::vector<ElementInfo> elements(const std::string& xml) {
    // A close tag that does not match the innermost open element is paired with an enclosing one
    // within this many levels (the ones in between were never closed); further out it is ignored,
    // so a flood of stray close tags cannot make the walk quadratic.
    constexpr std::size_t kMaxRecoveryDepth = 16;
    std::vector<ElementInfo> result;
    std::vector<std::size_t> open;
    Tokenizer tokenizer(xml);
    Token token;
    while (tokenizer.next(token)) {
        switch (token.kind) {
            case TokenKind::kStartTag:
            case TokenKind::kEmptyTag: {
                if (!open.empty()) {
                    result[open.back()].has_children = true;
                }
                ElementInfo info;
                info.name = std::string(token.name);
                info.depth = static_cast<int>(open.size());
                info.closed = token.kind == TokenKind::kEmptyTag;
                result.push_back(std::move(info));
                if (token.kind == TokenKind::kStartTag) {
                    open.push_back(result.size() - 1);
                }
                break;
            }
            case TokenKind::kEndTag: {
                const std::size_t lowest =
                    open.size() > kMaxRecoveryDepth ? open.size() - kMaxRecoveryDepth : 0;
                for (std::size_t i = open.size(); i > lowest; --i) {
                    if (result[open[i - 1]].name == token.name) {
                        result[open[i - 1]].closed = true;
                        open.resize(i - 1);
                        break;
                    }
                }
                break;
            }
            case TokenKind::kText:
                if (!open.empty()) {
                    appendDecoded(result[open.back()].text, token.content);
                }
                break;
            case TokenKind::kCdata:
                if (!open.empty()) {
                    result[open.back()].text.append(token.content);
                }
                break;
            case TokenKind::kSkipped:
                break;
        }
    }
    return result;
}

std::string rootName(const std::string& xml) {
    Tokenizer tokenizer(xml);
    Token token;
    while (tokenizer.next(token)) {
        if (token.kind == TokenKind::kStartTag || token.kind == TokenKind::kEmptyTag) {
            return std::string(token.name);
        }
    }
    return {};
}

std::string decodeEntities(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    appendDecoded(out, text);
    return out;
}

std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

std::string trim(const std::string& text) {
    const auto is_space = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace anpr::xml
