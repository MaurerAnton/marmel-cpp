#pragma once
// Declarative widget DSL (offline, I/O-free).
// Rust origin: src/widget.rs (507 lines).
//
// Grammar:
//   document  = { widget_def }
//   widget_def= "widget" NAME KIND ["{"] { prop } "}"  (`{` may trail the
//               header or sit on the next non-blank line; props each on
//               their own logical line)
//   prop      = NAME "=" (STRING | BARE)
//   NAME      = [A-Za-z_][A-Za-z0-9_-]*
//   KIND      = paragraph|block|gauge|list|table|chart|sparkline|canvas
//   STRING    = "…" with \\n \\t \\" \\\\ escapes
//   BARE      = single [^\s{}"]+ token (quotes/braces/whitespace forbidden)
//   // comments respected only outside quotes; blank lines ignored.
// Errors carry 1-based line numbers ("line L: msg").

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace marmel::widget {

enum class WidgetKind { Paragraph, Block, Gauge, List, Table, Chart, Sparkline, Canvas };

std::optional<WidgetKind> parse_kind(const std::string& text);
const char* kind_as_str(WidgetKind k);
std::string to_string(WidgetKind k);

struct Widget {
    std::string name;
    WidgetKind kind = WidgetKind::Paragraph;
    std::map<std::string, std::string> props;
    std::optional<std::string> get(const std::string& key) const;
    /// Some(true/false) when present (any non-true/yes/1 text is false);
    /// nullopt when missing. Mirrors Rust Option<bool>.
    std::optional<bool> get_bool(const std::string& key) const;
    /// Parsed integer or nullopt when missing/unparsable.
    std::optional<unsigned long long> get_u64(const std::string& key) const;
};

struct ParseError {
    std::size_t line = 1; // 1-based
    std::string message;
    std::string to_string() const { return "line " + std::to_string(line) + ": " + message; }
};

/// Throws ParseError on invalid input (Rust returns Result; the message
/// format is identical so tests translate 1:1 with try/catch).
std::vector<Widget> parse(const std::string& source);

} // namespace marmel::widget
