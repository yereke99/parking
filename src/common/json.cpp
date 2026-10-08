// RFC 8259 reader and compact writer for the files this project writes itself (camera status).
// Numbers are converted with the C locale explicitly: GStreamer or a host library may call
// setlocale(), and a status file written as "25,0" in one process must not break another.
#include "anpr/common/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <sstream>

#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace anpr::json {
namespace {

/// Deeper documents are rejected instead of exhausting the stack on hostile input.
constexpr int kMaxDepth = 256;

locale_t cLocale() {
    static const locale_t locale = ::newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    return locale;
}

/// strtod in the C locale. `complete` is false unless all of `text` was a number.
double strtodC(const std::string& text, bool& complete) {
    if (cLocale() != static_cast<locale_t>(nullptr)) {
        char* end = nullptr;
        const double value = ::strtod_l(text.c_str(), &end, cLocale());
        complete = !text.empty() && end == text.c_str() + text.size();
        return value;
    }
    // newlocale() failed (out of memory): the classic stream locale converts just as exactly.
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    double value = 0.0;
    in >> value;
    complete = !in.fail() && in.peek() == std::char_traits<char>::eof();
    return value;
}

std::string formatNumber(double value) {
    if (!std::isfinite(value)) {
        return "null";  // JSON has no NaN or infinity.
    }
    if (std::floor(value) == value && std::fabs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    // The fewest significant digits (15 to 17) that read back as exactly the same double.
    for (const int precision : {15, 16, 17}) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out.precision(precision);
        out << value;
        const std::string text = out.str();
        bool complete = false;
        if (strtodC(text, complete) == value || precision == 17) {
            return text;
        }
    }
    return "null";
}

void appendUtf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6U)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3FU)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12U)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18U)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3FU)));
    }
}

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    ParseResult run() {
        ParseResult result;
        skipWhitespace();
        if (!parseValue(result.value, 0)) {
            result.value = Value();
            result.error = error_;
            return result;
        }
        skipWhitespace();
        if (position_ != text_.size()) {
            fail("unexpected data after the document");
            result.value = Value();
            result.error = error_;
            return result;
        }
        result.ok = true;
        return result;
    }

private:
    const std::string& text_;
    std::size_t position_{0};
    std::string error_;

    bool fail(const std::string& message) {
        if (error_.empty()) {
            error_ = message + " at offset " + std::to_string(position_);
        }
        return false;
    }

    [[nodiscard]] bool atEnd() const { return position_ >= text_.size(); }
    [[nodiscard]] char peek() const { return atEnd() ? '\0' : text_[position_]; }

    void skipWhitespace() {
        while (!atEnd()) {
            const char ch = text_[position_];
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                return;
            }
            ++position_;
        }
    }

    bool expectWord(const char* word) {
        const std::string expected(word);
        if (text_.compare(position_, expected.size(), expected) != 0) {
            return fail("invalid literal");
        }
        position_ += expected.size();
        return true;
    }

    bool parseValue(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("document nested deeper than " + std::to_string(kMaxDepth) + " levels");
        }
        if (atEnd()) {
            return fail("unexpected end of input");
        }
        switch (peek()) {
            case '{':
                return parseObject(out, depth + 1);
            case '[':
                return parseArray(out, depth + 1);
            case '"': {
                std::string text;
                if (!parseString(text)) {
                    return false;
                }
                out = Value::string(std::move(text));
                return true;
            }
            case 't':
                out = Value::boolean(true);
                return expectWord("true");
            case 'f':
                out = Value::boolean(false);
                return expectWord("false");
            case 'n':
                out = Value();
                return expectWord("null");
            default:
                return parseNumber(out);
        }
    }

    bool parseObject(Value& out, int depth) {
        out = Value::object();
        ++position_;  // '{'
        skipWhitespace();
        if (peek() == '}') {
            ++position_;
            return true;
        }
        for (;;) {
            skipWhitespace();
            if (peek() != '"') {
                return fail("expected a member name");
            }
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (peek() != ':') {
                return fail("expected ':'");
            }
            ++position_;
            skipWhitespace();
            Value member;
            if (!parseValue(member, depth)) {
                return false;
            }
            out.set(std::move(key), std::move(member));  // A repeated name keeps the last value.
            skipWhitespace();
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == '}') {
                ++position_;
                return true;
            }
            return fail(atEnd() ? "unterminated object" : "expected ',' or '}'");
        }
    }

    bool parseArray(Value& out, int depth) {
        out = Value::array();
        ++position_;  // '['
        skipWhitespace();
        if (peek() == ']') {
            ++position_;
            return true;
        }
        for (;;) {
            skipWhitespace();
            Value item;
            if (!parseValue(item, depth)) {
                return false;
            }
            out.push(std::move(item));
            skipWhitespace();
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == ']') {
                ++position_;
                return true;
            }
            return fail(atEnd() ? "unterminated array" : "expected ',' or ']'");
        }
    }

    bool parseHex4(std::uint32_t& value) {
        if (position_ + 4 > text_.size()) {
            return fail("truncated \\u escape");
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = text_[position_++];
            value <<= 4U;
            if (ch >= '0' && ch <= '9') {
                value |= static_cast<std::uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value |= static_cast<std::uint32_t>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                value |= static_cast<std::uint32_t>(ch - 'A' + 10);
            } else {
                --position_;
                return fail("invalid \\u escape");
            }
        }
        return true;
    }

    bool parseString(std::string& out) {
        ++position_;  // opening quote
        for (;;) {
            if (atEnd()) {
                return fail("unterminated string");
            }
            const char ch = text_[position_];
            if (ch == '"') {
                ++position_;
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                return fail("unescaped control character in string");
            }
            ++position_;
            if (ch != '\\') {
                out.push_back(ch);  // UTF-8 passes through byte by byte.
                continue;
            }
            if (atEnd()) {
                return fail("unterminated string");
            }
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(escaped);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t unit = 0;
                    if (!parseHex4(unit)) {
                        return false;
                    }
                    if (unit >= 0xD800 && unit <= 0xDBFF) {
                        // A high surrogate needs a following low surrogate escape.
                        std::uint32_t low = 0;
                        const std::size_t saved = position_;
                        if (text_.compare(position_, 2, "\\u") == 0) {
                            position_ += 2;
                            if (!parseHex4(low)) {
                                return false;
                            }
                        }
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10U) + (low - 0xDC00));
                        } else {
                            position_ = saved;
                            appendUtf8(out, 0xFFFD);  // Lone surrogate: not representable.
                        }
                    } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
                        appendUtf8(out, 0xFFFD);
                    } else {
                        appendUtf8(out, unit);
                    }
                    break;
                }
                default:
                    --position_;
                    return fail("invalid escape");
            }
        }
    }

    bool parseNumber(Value& out) {
        // Validate the RFC 8259 grammar first; strtod alone would accept "0x1p3", "inf", " 1".
        const std::size_t start = position_;
        std::size_t cursor = position_;
        const auto digitAt = [this](std::size_t index) {
            return index < text_.size() && text_[index] >= '0' && text_[index] <= '9';
        };
        if (cursor < text_.size() && text_[cursor] == '-') {
            ++cursor;
        }
        if (!digitAt(cursor)) {
            return fail("unexpected character");
        }
        if (text_[cursor] == '0') {
            ++cursor;
            if (digitAt(cursor)) {
                return fail("leading zero in number");
            }
        } else {
            while (digitAt(cursor)) {
                ++cursor;
            }
        }
        if (cursor < text_.size() && text_[cursor] == '.') {
            ++cursor;
            if (!digitAt(cursor)) {
                position_ = cursor;
                return fail("expected digits after the decimal point");
            }
            while (digitAt(cursor)) {
                ++cursor;
            }
        }
        if (cursor < text_.size() && (text_[cursor] == 'e' || text_[cursor] == 'E')) {
            ++cursor;
            if (cursor < text_.size() && (text_[cursor] == '+' || text_[cursor] == '-')) {
                ++cursor;
            }
            if (!digitAt(cursor)) {
                position_ = cursor;
                return fail("expected exponent digits");
            }
            while (digitAt(cursor)) {
                ++cursor;
            }
        }
        bool complete = false;
        const double value = strtodC(text_.substr(start, cursor - start), complete);
        if (!complete) {
            return fail("invalid number");
        }
        if (!std::isfinite(value)) {
            return fail("number out of range");
        }
        position_ = cursor;
        out = Value::number(value);
        return true;
    }
};

void dumpTo(const Value& value, std::string& out) {
    switch (value.kind()) {
        case Value::Kind::kNull:
            out += "null";
            return;
        case Value::Kind::kBool:
            out += *value.asBool() ? "true" : "false";
            return;
        case Value::Kind::kNumber:
            out += formatNumber(*value.asNumber());
            return;
        case Value::Kind::kString:
            out += quote(*value.asString());
            return;
        case Value::Kind::kArray: {
            out.push_back('[');
            bool first = true;
            for (const Value& item : value.items()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                dumpTo(item, out);
            }
            out.push_back(']');
            return;
        }
        case Value::Kind::kObject: {
            out.push_back('{');
            bool first = true;
            for (const auto& member : value.members()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                out += quote(member.first);
                out.push_back(':');
                dumpTo(member.second, out);
            }
            out.push_back('}');
            return;
        }
    }
}

}  // namespace

Value Value::boolean(bool value) {
    Value result;
    result.kind_ = Kind::kBool;
    result.bool_ = value;
    return result;
}

Value Value::number(double value) {
    Value result;
    result.kind_ = Kind::kNumber;
    result.number_ = value;
    return result;
}

Value Value::string(std::string value) {
    Value result;
    result.kind_ = Kind::kString;
    result.string_ = std::move(value);
    return result;
}

Value Value::array() {
    Value result;
    result.kind_ = Kind::kArray;
    return result;
}

Value Value::object() {
    Value result;
    result.kind_ = Kind::kObject;
    return result;
}

std::optional<bool> Value::asBool() const {
    if (kind_ != Kind::kBool) {
        return std::nullopt;
    }
    return bool_;
}

std::optional<double> Value::asNumber() const {
    if (kind_ != Kind::kNumber) {
        return std::nullopt;
    }
    return number_;
}

std::optional<std::int64_t> Value::asInt() const {
    // Only exact integers within the int64 range; 2.5 is not silently truncated to 2.
    if (kind_ != Kind::kNumber || !std::isfinite(number_) || std::floor(number_) != number_ ||
        number_ < -9223372036854775808.0 || number_ >= 9223372036854775808.0) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(number_);
}

std::optional<std::string> Value::asString() const {
    if (kind_ != Kind::kString) {
        return std::nullopt;
    }
    return string_;
}

const Value* Value::find(const std::string& key) const {
    if (kind_ != Kind::kObject) {
        return nullptr;
    }
    for (const auto& member : members_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

std::string Value::getString(const std::string& key, const std::string& fallback) const {
    const Value* member = find(key);
    if (member == nullptr) {
        return fallback;
    }
    return member->asString().value_or(fallback);
}

double Value::getNumber(const std::string& key, double fallback) const {
    const Value* member = find(key);
    if (member == nullptr) {
        return fallback;
    }
    return member->asNumber().value_or(fallback);
}

std::int64_t Value::getInt(const std::string& key, std::int64_t fallback) const {
    const Value* member = find(key);
    if (member == nullptr) {
        return fallback;
    }
    return member->asInt().value_or(fallback);
}

bool Value::getBool(const std::string& key, bool fallback) const {
    const Value* member = find(key);
    if (member == nullptr) {
        return fallback;
    }
    return member->asBool().value_or(fallback);
}

void Value::push(Value value) {
    // Pushing onto anything but an array turns the value into one (a null is the common case).
    if (kind_ != Kind::kArray) {
        *this = array();
    }
    items_.push_back(std::move(value));
}

void Value::set(std::string key, Value value) {
    if (kind_ != Kind::kObject) {
        *this = object();
    }
    for (auto& member : members_) {
        if (member.first == key) {
            member.second = std::move(value);
            return;
        }
    }
    members_.emplace_back(std::move(key), std::move(value));
}

std::string Value::dump() const {
    std::string out;
    dumpTo(*this, out);
    return out;
}

ParseResult parse(const std::string& text) {
    return Parser(text).run();
}

std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char code[7];
                    std::snprintf(code, sizeof(code), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(ch)));
                    out += code;
                } else {
                    out.push_back(ch);
                }
        }
    }
    return out;
}

std::string quote(const std::string& text) {
    return "\"" + escape(text) + "\"";
}

}  // namespace anpr::json
