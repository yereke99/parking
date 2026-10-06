#include "anpr/common/yaml.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <fstream>
#include <sstream>

namespace anpr::yaml {
namespace {

struct Line {
    int indent{0};
    std::string content;
    int number{0};
};

std::string trimRight(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::string trim(std::string value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    value.erase(0, begin);
    return trimRight(std::move(value));
}

/// Removes an unquoted trailing comment. A `#` inside quotes is kept.
std::string stripComment(const std::string& line) {
    bool in_single = false;
    bool in_double = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '\'' && !in_double) {
            in_single = !in_single;
        } else if (ch == '"' && !in_single) {
            in_double = !in_double;
        } else if (ch == '#' && !in_single && !in_double) {
            const bool at_start = i == 0;
            const bool after_space = i > 0 && std::isspace(static_cast<unsigned char>(line[i - 1])) != 0;
            if (at_start || after_space) {
                return line.substr(0, i);
            }
        }
    }
    return line;
}

std::string unquote(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

/// Splits `key: value` while ignoring colons inside quotes and inside flow sequences.
/// Returns npos when the line has no mapping colon.
std::size_t findMappingColon(const std::string& line) {
    bool in_single = false;
    bool in_double = false;
    int bracket_depth = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '\'' && !in_double) {
            in_single = !in_single;
        } else if (ch == '"' && !in_single) {
            in_double = !in_double;
        } else if (!in_single && !in_double) {
            if (ch == '[') {
                ++bracket_depth;
            } else if (ch == ']') {
                --bracket_depth;
            } else if (ch == ':' && bracket_depth == 0) {
                const bool ends_here = i + 1 == line.size();
                const bool followed_by_space =
                    !ends_here && std::isspace(static_cast<unsigned char>(line[i + 1])) != 0;
                if (ends_here || followed_by_space) {
                    return i;
                }
            }
        }
    }
    return std::string::npos;
}

Node makeFlowSequence(const std::string& value) {
    Node node(Node::Kind::kSequence);
    const std::string body = value.substr(1, value.size() - 2);
    std::string item;
    bool in_single = false;
    bool in_double = false;
    auto flush = [&]() {
        const std::string trimmed = trim(item);
        if (!trimmed.empty()) {
            Node scalar(Node::Kind::kScalar);
            scalar.setScalar(unquote(trimmed));
            node.addSequenceItem(std::move(scalar));
        }
        item.clear();
    };
    for (const char ch : body) {
        if (ch == '\'' && !in_double) {
            in_single = !in_single;
            item.push_back(ch);
        } else if (ch == '"' && !in_single) {
            in_double = !in_double;
            item.push_back(ch);
        } else if (ch == ',' && !in_single && !in_double) {
            flush();
        } else {
            item.push_back(ch);
        }
    }
    flush();
    return node;
}

Node makeScalar(const std::string& raw) {
    const std::string value = trim(raw);
    if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
        return makeFlowSequence(value);
    }
    Node node(Node::Kind::kScalar);
    node.setScalar(unquote(value));
    return node;
}

class Parser {
public:
    explicit Parser(std::vector<Line> lines) : lines_(std::move(lines)) {}

    ParseResult run() {
        ParseResult result;
        if (lines_.empty()) {
            result.root = Node(Node::Kind::kMap);
            return result;
        }
        result.root = parseBlock(lines_.front().indent, result);
        if (!result.ok) {
            return result;
        }
        if (index_ < lines_.size()) {
            fail(result, lines_[index_].number, "unexpected indentation");
        }
        return result;
    }

private:
    std::vector<Line> lines_;
    std::size_t index_{0};

    static void fail(ParseResult& result, int line_number, const std::string& message) {
        if (result.ok) {
            result.ok = false;
            result.error = "line " + std::to_string(line_number) + ": " + message;
        }
    }

    Node parseBlock(int indent, ParseResult& result) {
        if (index_ >= lines_.size() || lines_[index_].indent != indent) {
            return Node(Node::Kind::kNull);
        }
        if (lines_[index_].content.rfind("- ", 0) == 0 || lines_[index_].content == "-") {
            return parseSequence(indent, result);
        }
        return parseMap(indent, result);
    }

    Node parseMap(int indent, ParseResult& result) {
        Node node(Node::Kind::kMap);
        while (result.ok && index_ < lines_.size() && lines_[index_].indent == indent) {
            const Line line = lines_[index_];
            if (line.content.rfind("- ", 0) == 0 || line.content == "-") {
                fail(result, line.number, "sequence item inside a mapping block");
                return node;
            }
            const std::size_t colon = findMappingColon(line.content);
            if (colon == std::string::npos) {
                fail(result, line.number, "expected 'key: value'");
                return node;
            }
            const std::string key = unquote(line.content.substr(0, colon));
            if (key.empty()) {
                fail(result, line.number, "empty mapping key");
                return node;
            }
            const std::string inline_value = trim(line.content.substr(colon + 1));
            ++index_;

            Node& child = node.addMapEntry(key);
            if (!inline_value.empty()) {
                child = makeScalar(inline_value);
                continue;
            }
            if (index_ < lines_.size() && lines_[index_].indent > indent) {
                child = parseBlock(lines_[index_].indent, result);
            } else {
                child = Node(Node::Kind::kNull);
            }
        }
        return node;
    }

    Node parseSequence(int indent, ParseResult& result) {
        Node node(Node::Kind::kSequence);
        while (result.ok && index_ < lines_.size() && lines_[index_].indent == indent &&
               (lines_[index_].content.rfind("- ", 0) == 0 || lines_[index_].content == "-")) {
            const Line line = lines_[index_];
            const std::string item = line.content == "-" ? std::string{} : trim(line.content.substr(2));
            ++index_;

            if (item.empty()) {
                if (index_ < lines_.size() && lines_[index_].indent > indent) {
                    node.addSequenceItem(parseBlock(lines_[index_].indent, result));
                } else {
                    node.addSequenceItem(Node(Node::Kind::kNull));
                }
                continue;
            }

            const std::size_t colon = findMappingColon(item);
            if (colon == std::string::npos) {
                node.addSequenceItem(makeScalar(item));
                continue;
            }

            // `- key: value` starts a mapping whose remaining keys are indented to the position
            // of the key text, two columns past the dash.
            Node entry(Node::Kind::kMap);
            const std::string key = unquote(item.substr(0, colon));
            const std::string inline_value = trim(item.substr(colon + 1));
            Node& child = entry.addMapEntry(key);
            const int nested_indent = indent + 2;
            if (!inline_value.empty()) {
                child = makeScalar(inline_value);
            } else if (index_ < lines_.size() && lines_[index_].indent > nested_indent) {
                child = parseBlock(lines_[index_].indent, result);
            } else {
                child = Node(Node::Kind::kNull);
            }
            while (result.ok && index_ < lines_.size() && lines_[index_].indent == nested_indent &&
                   lines_[index_].content.rfind("- ", 0) != 0) {
                Node rest = parseMap(nested_indent, result);
                for (const auto& [rest_key, rest_value] : rest.map()) {
                    entry.addMapEntry(rest_key) = rest_value;
                }
                break;
            }
            node.addSequenceItem(std::move(entry));
        }
        return node;
    }
};

}  // namespace

const Node* Node::find(const std::string& key) const {
    if (kind_ != Kind::kMap) {
        return nullptr;
    }
    for (const auto& entry : map_) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

const Node* Node::path(const std::string& dotted_key) const {
    const Node* current = this;
    std::size_t begin = 0;
    while (current != nullptr && begin <= dotted_key.size()) {
        const std::size_t dot = dotted_key.find('.', begin);
        const std::string key = dotted_key.substr(
            begin, dot == std::string::npos ? std::string::npos : dot - begin);
        current = current->find(key);
        if (dot == std::string::npos) {
            break;
        }
        begin = dot + 1;
    }
    return current;
}

void Node::setScalar(std::string value) {
    kind_ = Kind::kScalar;
    scalar_ = std::move(value);
}

Node& Node::addMapEntry(std::string key) {
    kind_ = Kind::kMap;
    for (auto& entry : map_) {
        if (entry.first == key) {
            return entry.second;
        }
    }
    map_.emplace_back(std::move(key), Node{});
    return map_.back().second;
}

Node& Node::addSequenceItem(Node node) {
    kind_ = Kind::kSequence;
    sequence_.push_back(std::move(node));
    return sequence_.back();
}

namespace {

/// Net bracket depth added by a line, ignoring brackets inside quotes.
int bracketDelta(const std::string& line) {
    bool in_single = false;
    bool in_double = false;
    int depth = 0;
    for (const char ch : line) {
        if (ch == '\'' && !in_double) {
            in_single = !in_single;
        } else if (ch == '"' && !in_single) {
            in_double = !in_double;
        } else if (!in_single && !in_double) {
            if (ch == '[') {
                ++depth;
            } else if (ch == ']') {
                --depth;
            }
        }
    }
    return depth;
}

}  // namespace

ParseResult parse(const std::string& text) {
    ParseResult result;
    std::vector<Line> lines;
    std::istringstream stream(text);
    std::string raw;
    int number = 0;

    // A flow sequence may span several physical lines. Those are joined back into one logical
    // line before indentation is interpreted, which is how the Fast Plate OCR config writes its
    // region list.
    std::string pending;
    int pending_indent = 0;
    int pending_number = 0;
    int open_brackets = 0;

    while (std::getline(stream, raw)) {
        ++number;
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        if (raw.find('\t') != std::string::npos) {
            result.ok = false;
            result.error = "line " + std::to_string(number) + ": tab indentation is not supported";
            return result;
        }
        const std::string without_comment = trimRight(stripComment(raw));
        if (trim(without_comment).empty() && open_brackets == 0) {
            continue;
        }

        if (open_brackets > 0) {
            pending += ' ';
            pending += trim(without_comment);
        } else {
            int indent = 0;
            while (indent < static_cast<int>(without_comment.size()) &&
                   without_comment[static_cast<std::size_t>(indent)] == ' ') {
                ++indent;
            }
            pending_indent = indent;
            pending_number = number;
            pending = without_comment.substr(static_cast<std::size_t>(indent));
        }

        open_brackets += bracketDelta(without_comment);
        if (open_brackets > 0) {
            continue;
        }
        open_brackets = 0;
        lines.push_back(Line{pending_indent, pending, pending_number});
        pending.clear();
    }

    if (open_brackets > 0) {
        result.ok = false;
        result.error = "line " + std::to_string(pending_number) + ": unterminated flow sequence";
        return result;
    }

    Parser parser(std::move(lines));
    return parser.run();
}

ParseResult parseFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        ParseResult result;
        result.ok = false;
        result.error = "cannot open " + path;
        return result;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return parse(buffer.str());
}

std::optional<std::string> asString(const Node* node) {
    if (node == nullptr || !node->isScalar()) {
        return std::nullopt;
    }
    return node->scalar();
}

std::optional<double> asDouble(const Node* node) {
    const auto text = asString(node);
    if (!text || text->empty()) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const double value = std::stod(*text, &consumed);
        if (consumed != text->size()) {
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<long long> asInt(const Node* node) {
    const auto text = asString(node);
    if (!text || text->empty()) {
        return std::nullopt;
    }
    std::size_t digit = text->front() == '-' ? 1 : 0;
    if (digit == text->size()) {
        return std::nullopt;
    }
    for (; digit < text->size(); ++digit) {
        if (std::isdigit(static_cast<unsigned char>((*text)[digit])) == 0) {
            return std::nullopt;
        }
    }
    try {
        std::size_t consumed = 0;
        const long long value = std::stoll(*text, &consumed, 10);
        if (consumed != text->size()) {
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<bool> asBool(const Node* node) {
    auto text = asString(node);
    if (!text) {
        return std::nullopt;
    }
    std::transform(text->begin(), text->end(), text->begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (*text == "true" || *text == "yes" || *text == "on" || *text == "1") {
        return true;
    }
    if (*text == "false" || *text == "no" || *text == "off" || *text == "0") {
        return false;
    }
    return std::nullopt;
}

}  // namespace anpr::yaml
