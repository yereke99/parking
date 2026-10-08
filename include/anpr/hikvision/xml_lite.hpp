#pragma once

#include <optional>
#include <string>
#include <vector>

namespace anpr::xml {

/// Just enough XML for camera protocols (SADP, WS-Discovery/ONVIF SOAP, Hikvision ISAPI). Not a
/// general parser: elements are matched by local name with any namespace prefix ignored
/// ("<tds:Model>" matches "Model"), attributes are skipped, CDATA and the five predefined
/// entities plus numeric character references are decoded. Malformed input yields nullopt or
/// empty results, never an exception, and every query is one linear pass over the input.

/// Decoded text content of the first element named `local_name`: text of child elements
/// included, whitespace kept (use trim for values). nullopt when absent or never closed.
std::optional<std::string> firstText(const std::string& xml, const std::string& local_name);

/// Decoded text content of every element named `local_name`, in document order. An element
/// nested inside an earlier match of the same name is part of that match, not a separate result,
/// so the output is never larger than the input.
std::vector<std::string> allTexts(const std::string& xml, const std::string& local_name);

/// Raw inner XML of the first element named `local_name` (for nested blocks such as <ANPR>).
/// A nested element of the same name is paired with its own close tag.
std::optional<std::string> firstElement(const std::string& xml, const std::string& local_name);
/// Raw inner XML of every element named `local_name`, outermost matches only (see allTexts).
std::vector<std::string> allElements(const std::string& xml, const std::string& local_name);

/// One element as reported by `elements()`.
struct ElementInfo {
    std::string name;  ///< local name
    /// Decoded text directly inside the element (text of child elements excluded), untrimmed.
    std::string text;
    int depth{0};  ///< 0 for the root
    bool has_children{false};
    /// False when the input ends (or a mismatched close tag interrupts it) before its close tag.
    bool closed{false};
};

/// Every element in document order from one linear pass, for documents whose element names are
/// not known in advance (SADP answers, capability documents).
std::vector<ElementInfo> elements(const std::string& xml);

/// Local name of the document's root element ("EventNotificationAlert", "Envelope"), or empty.
std::string rootName(const std::string& xml);

std::string decodeEntities(const std::string& text);
/// Escapes &, <, >, " and ' for element text or attribute values.
std::string escape(const std::string& text);

/// Trims ASCII whitespace from both ends.
std::string trim(const std::string& text);

}  // namespace anpr::xml
