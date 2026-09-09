#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace anpr::yaml {

/// Minimal YAML subset used for configuration files.
///
/// Supported: nested block mappings, block sequences of scalars and of mappings, inline flow
/// sequences of scalars (`[a, b, c]`), quoted and bare scalars, `#` comments, blank lines.
/// Not supported: anchors, aliases, tags, multi-document files, block scalars, flow mappings.
/// Tab characters are rejected because they make indentation ambiguous.
class Node {
public:
    enum class Kind { kNull, kScalar, kMap, kSequence };

    Node() = default;
    explicit Node(Kind kind) : kind_(kind) {}

    [[nodiscard]] Kind kind() const { return kind_; }
    [[nodiscard]] bool isNull() const { return kind_ == Kind::kNull; }
    [[nodiscard]] bool isScalar() const { return kind_ == Kind::kScalar; }
    [[nodiscard]] bool isMap() const { return kind_ == Kind::kMap; }
    [[nodiscard]] bool isSequence() const { return kind_ == Kind::kSequence; }

    [[nodiscard]] const std::string& scalar() const { return scalar_; }
    [[nodiscard]] const std::vector<Node>& sequence() const { return sequence_; }
    [[nodiscard]] const std::vector<std::pair<std::string, Node>>& map() const { return map_; }

    /// Looks up a child by key. Returns nullptr when absent or when this node is not a map.
    [[nodiscard]] const Node* find(const std::string& key) const;

    /// Looks up a dotted path such as `detector.intervals.idle_ms`.
    [[nodiscard]] const Node* path(const std::string& dotted_key) const;

    void setScalar(std::string value);
    Node& addMapEntry(std::string key);
    Node& addSequenceItem(Node node);

private:
    Kind kind_{Kind::kNull};
    std::string scalar_;
    std::vector<Node> sequence_;
    std::vector<std::pair<std::string, Node>> map_;
};

struct ParseResult {
    Node root;
    bool ok{true};
    std::string error;
};

/// Parses YAML text. On failure `ok` is false and `error` names the line.
ParseResult parse(const std::string& text);

/// Reads and parses a file. Missing files report an error rather than throwing.
ParseResult parseFile(const std::string& path);

/// Scalar conversions. Each returns nullopt when the node is missing or malformed.
std::optional<std::string> asString(const Node* node);
std::optional<double> asDouble(const Node* node);
std::optional<long long> asInt(const Node* node);
std::optional<bool> asBool(const Node* node);

}  // namespace anpr::yaml
