// Rust origin: src/widget.rs
#include "marmel/widget.hpp"

#include <cctype>
#include <stdexcept>

namespace marmel::widget {
namespace {

std::string trim_copy(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}
bool is_name_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
// NOTE: Rust is_name uses ASCII alphanumeric (not Unicode alnum).
bool valid_name(const std::string& s) {
    if (s.empty()) return false;
    unsigned char f = s[0];
    if (!((f >= 'A' && f <= 'Z') || (f >= 'a' && f <= 'z') || f == '_')) return false;
    for (unsigned char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}
std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Strip a trailing `//` comment, toggling on EVERY `"` (no escape handling,
// exactly as in Rust — `//` after an escaped quote IS stripped there too).
std::string strip_comment(const std::string& line) {
    bool in_string = false;
    for (std::size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '"') {
            in_string = !in_string;
        } else if (c == '/' && !in_string && i + 1 < line.size() && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }
    return line;
}

std::vector<std::string> split_whitespace(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
        if (i >= s.size()) break;
        std::size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) j++;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

struct Cursor {
    const std::vector<std::string>& lines;
    std::size_t idx = 0;
    ParseError fail(std::size_t line, const std::string& message) const {
        throw ParseError{line + 1, message};
    }
    // Skip blank and comment-only lines (comment stripped, then trimmed).
    void skip_blank_and_comments() {
        while (idx < lines.size() && trim_copy(strip_comment(lines[idx])).empty()) idx++;
    }
};

std::pair<std::string, std::string> parse_prop(const std::string& line) {
    auto eq = line.find('=');
    if (eq == std::string::npos)
        throw std::runtime_error("expected `key = value`, got `" + line + "`");
    std::string key = trim_copy(line.substr(0, eq));
    if (!valid_name(key)) throw std::runtime_error("invalid property name `" + key + "`");
    std::string raw_value = trim_copy(line.substr(eq + 1));
    if (raw_value.empty()) throw std::runtime_error("property `" + key + "` is missing a value");
    std::string value;
    if (raw_value.front() == '"') {
        std::string rest = raw_value.substr(1);
        std::string out;
        bool closed = false;
        for (std::size_t i = 0; i < rest.size();) {
            char c = rest[i];
            if (c == '"') {
                std::string tail = trim_copy(rest.substr(i + 1));
                if (!tail.empty())
                    throw std::runtime_error("unexpected characters after closing quote");
                closed = true;
                i++;
                break;
            } else if (c == '\\') {
                if (i + 1 >= rest.size())
                    throw std::runtime_error("unterminated escape sequence");
                char e = rest[i + 1];
                if (e == 'n') out += '\n';
                else if (e == 't') out += '\t';
                else if (e == '"') out += '"';
                else if (e == '\\') out += '\\';
                else
                    throw std::runtime_error(std::string("invalid escape `\\") + e + "`");
                i += 2;
            } else {
                out += c;
                i++;
            }
        }
        if (!closed) throw std::runtime_error("unterminated string literal");
        value = out;
    } else {
        if (raw_value.find('"') != std::string::npos)
            throw std::runtime_error("unexpected quote in bare value");
        if (raw_value.find('{') != std::string::npos || raw_value.find('}') != std::string::npos)
            throw std::runtime_error("unexpected brace in bare value");
        if (split_whitespace(raw_value).size() != 1)
            throw std::runtime_error("bare value must not contain whitespace");
        value = raw_value;
    }
    return {key, value};
}

} // namespace

std::optional<WidgetKind> parse_kind(const std::string& text) {
    std::string t = to_lower(trim_copy(text));
    if (t == "paragraph") return WidgetKind::Paragraph;
    if (t == "block") return WidgetKind::Block;
    if (t == "gauge") return WidgetKind::Gauge;
    if (t == "list") return WidgetKind::List;
    if (t == "table") return WidgetKind::Table;
    if (t == "chart") return WidgetKind::Chart;
    if (t == "sparkline") return WidgetKind::Sparkline;
    if (t == "canvas") return WidgetKind::Canvas;
    return std::nullopt;
}
const char* kind_as_str(WidgetKind k) {
    switch (k) {
        case WidgetKind::Paragraph: return "paragraph";
        case WidgetKind::Block: return "block";
        case WidgetKind::Gauge: return "gauge";
        case WidgetKind::List: return "list";
        case WidgetKind::Table: return "table";
        case WidgetKind::Chart: return "chart";
        case WidgetKind::Sparkline: return "sparkline";
        case WidgetKind::Canvas: return "canvas";
    }
    return "paragraph";
}
std::string to_string(WidgetKind k) { return kind_as_str(k); }

std::optional<std::string> Widget::get(const std::string& key) const {
    auto it = props.find(key);
    return it == props.end() ? std::nullopt : std::optional<std::string>{it->second};
}
std::optional<bool> Widget::get_bool(const std::string& key) const {
    auto v = get(key);
    if (!v) return std::nullopt;
    std::string t = to_lower(trim_copy(*v));
    return t == "true" || t == "yes" || t == "1";
}
std::optional<unsigned long long> Widget::get_u64(const std::string& key) const {
    auto v = get(key);
    if (!v) return std::nullopt;
    try {
        std::size_t n = 0;
        unsigned long long u = std::stoull(trim_copy(*v), &n);
        if (n == 0) return std::nullopt;
        return u;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Widget> parse(const std::string& source) {
    std::vector<std::string> lines;
    {
        std::size_t i = 0;
        while (i <= source.size()) {
            auto nl = source.find('\n', i);
            if (nl == std::string::npos) {
                lines.push_back(source.substr(i));
                break;
            }
            lines.push_back(source.substr(i, nl - i));
            i = nl + 1;
        }
    }
    Cursor cur{lines, 0};
    std::vector<Widget> out;
    while (true) {
        cur.skip_blank_and_comments();
        if (cur.idx >= lines.size()) break;
        std::size_t header_line = cur.idx;
        std::string header = strip_comment(lines[cur.idx]);
        auto tokens = split_whitespace(header);
        if (tokens.size() < 2) cur.fail(header_line, "expected `widget <name> <kind>`");
        if (to_lower(tokens[0]) != "widget") cur.fail(header_line, "expected `widget` keyword");
        std::string name = tokens[1];
        if (!valid_name(name)) cur.fail(header_line, "invalid widget name `" + name + "`");
        if (tokens.size() < 3)
            cur.fail(header_line, "widget `" + name + "` is missing a kind");
        auto kind = parse_kind(tokens[2]);
        if (!kind) cur.fail(header_line, "unknown widget kind `" + tokens[2] + "`");
        if (tokens.size() > 3)
            cur.fail(header_line, "unexpected token `" + tokens[3] + "` after kind");
        cur.idx++;
        // Expect an opening brace on the next non-blank line.
        cur.skip_blank_and_comments();
        if (cur.idx >= lines.size())
            cur.fail(header_line, "widget `" + name + "` is missing `{`");
        std::size_t open_line = cur.idx;
        if (trim_copy(strip_comment(lines[cur.idx])) != "{")
            cur.fail(open_line, "expected `{` to open the widget body");
        cur.idx++;
        std::map<std::string, std::string> props;
        while (true) {
            cur.skip_blank_and_comments();
            if (cur.idx >= lines.size())
                cur.fail(header_line, "widget `" + name + "` is missing closing `}`");
            std::size_t line_no = cur.idx;
            std::string line = trim_copy(strip_comment(lines[cur.idx]));
            if (line == "}") {
                cur.idx++;
                break;
            }
            std::pair<std::string, std::string> kv;
            try {
                kv = parse_prop(line);
            } catch (const std::runtime_error& e) {
                cur.fail(line_no, e.what());
            }
            if (!props.emplace(kv.first, kv.second).second)
                cur.fail(line_no, "duplicate property `" + kv.first + "` in widget `" + name + "`");
            cur.idx++;
        }
        Widget w;
        w.name = name;
        w.kind = *kind;
        w.props = std::move(props);
        out.push_back(std::move(w));
    }
    return out;
}

} // namespace marmel::widget
