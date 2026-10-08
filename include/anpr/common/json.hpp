#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace anpr::json {

/// A minimal JSON document model for the files this project writes itself (the camera status
/// file). RFC 8259 syntax, UTF-8 passed through, \uXXXX escapes decoded to UTF-8.
class Value {
public:
    enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };

    Value() = default;
    static Value boolean(bool value);
    static Value number(double value);
    static Value string(std::string value);
    static Value array();
    static Value object();

    [[nodiscard]] Kind kind() const { return kind_; }
    [[nodiscard]] bool isNull() const { return kind_ == Kind::kNull; }
    [[nodiscard]] bool isObject() const { return kind_ == Kind::kObject; }
    [[nodiscard]] bool isArray() const { return kind_ == Kind::kArray; }

    [[nodiscard]] std::optional<bool> asBool() const;
    [[nodiscard]] std::optional<double> asNumber() const;
    [[nodiscard]] std::optional<std::int64_t> asInt() const;
    [[nodiscard]] std::optional<std::string> asString() const;

    [[nodiscard]] const std::vector<Value>& items() const { return items_; }
    /// Object members in document order.
    [[nodiscard]] const std::vector<std::pair<std::string, Value>>& members() const {
        return members_;
    }
    /// Member lookup; nullptr when absent or not an object.
    [[nodiscard]] const Value* find(const std::string& key) const;

    /// Convenience accessors with defaults, for reading optional fields.
    [[nodiscard]] std::string getString(const std::string& key, const std::string& fallback = {}) const;
    [[nodiscard]] double getNumber(const std::string& key, double fallback = 0.0) const;
    [[nodiscard]] std::int64_t getInt(const std::string& key, std::int64_t fallback = 0) const;
    [[nodiscard]] bool getBool(const std::string& key, bool fallback = false) const;

    void push(Value value);
    void set(std::string key, Value value);

    /// Compact serialisation.
    [[nodiscard]] std::string dump() const;

private:
    Kind kind_{Kind::kNull};
    bool bool_{false};
    double number_{0.0};
    std::string string_;
    std::vector<Value> items_;
    std::vector<std::pair<std::string, Value>> members_;
};

struct ParseResult {
    Value value;
    bool ok{false};
    std::string error;
};

ParseResult parse(const std::string& text);

/// Escapes `text` for use inside a JSON string literal (no surrounding quotes).
std::string escape(const std::string& text);
/// `"text"` with escaping.
std::string quote(const std::string& text);

}  // namespace anpr::json
