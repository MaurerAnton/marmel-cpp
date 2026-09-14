// Rust origin: src/widget.rs
#include "marmel/widget.hpp"

#include <cctype>

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
bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-';
}
bool valid_name(const std::string& s) {
    if (s.empty() || !is_name_start(s[0])) return false;
    for (char c : s)
        if (!is_name_char(c)) return false;
    return true;
}
std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Strip `//` comments, respecting double quotes.
std::string strip_comment(const std::string& line) {
    bool in_str = false;
    for (std::size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (in_str) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }
    return line;
}

std::vector<std::string> split_words(const std::string& s) {
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
bool Widget::get_bool(const std::string& key, bool fallback) const {
    auto v = get(key);
    if (!v) return fallback;
    std::string t = to_lower(trim_copy(*v));
    if (t == "true" || t == "yes" || t == "1") return true;
    if (t == "false" || t == "no" || t == "0") return false;
    return fallback;
}
unsigned long long Widget::get_u64(const std::string& key, unsigned long long fallback) const {
    auto v = get(key);
    if (!v) return fallback;
    try {
        std::size_t n = 0;
        unsigned long long u = std::stoull(trim_copy(*v), &n);
        if (n == 0) return fallback;
        return u;
    } catch (...) {
        return fallback;
    }
}

std::vector<Widget> parse(const std::string& source) {
    // Split into logical lines (keep 1-based numbering).
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
    std::vector<Widget> out;
    std::size_t idx = 0;
    auto fail = [&](std::size_t line, const std::string& msg) -> std::vector<Widget> {
        throw ParseError{line + 1, msg};
    };
    while (idx < lines.size()) {
        std::string code = trim_copy(strip_comment(lines[idx]));
        if (code.empty()) {
            idx++;
            continue;
        }
        // Header: widget NAME KIND
        auto words = split_words(code);
        if (words.empty() || words[0] != "widget")
            return fail(idx, "expected `widget` definition");
        if (words.size() < 2 || !valid_name(words[1]))
            return fail(idx, "invalid widget name");
        if (words.size() < 3) return fail(idx, "missing widget kind");
        auto kind = parse_kind(words[2]);
        if (!kind) return fail(idx, "unknown widget kind `" + words[2] + "`");
        bool braced = false;
        if (words.size() > 3) {
            if (words.size() == 4 && words[3] == "{")
                braced = true; // `widget NAME KIND {` on one line
            else
                return fail(idx, "extra token after widget kind");
        }
        Widget w;
        w.name = words[1];
        w.kind = *kind;
        idx++;
        if (!braced) {
            // `{` on the next non-blank line.
            bool found = false;
            while (idx < lines.size()) {
                std::string l = trim_copy(strip_comment(lines[idx]));
                if (l.empty()) {
                    idx++;
                    continue;
                }
                if (l != "{") return fail(idx, "missing `{` after widget header");
                found = true;
                idx++;
                break;
            }
            if (!found) return fail(lines.size() - 1, "missing `{` after widget header");
        }
        // Props until `}`.
        bool closed = false;
        while (idx < lines.size()) {
            std::string raw = strip_comment(lines[idx]);
            std::string l = trim_copy(raw);
            if (l.empty()) {
                idx++;
                continue;
            }
            if (l == "}") {
                closed = true;
                idx++;
                break;
            }
            auto eq = l.find('=');
            if (eq == std::string::npos) return fail(idx, "expected `key = value`");
            std::string key = trim_copy(l.substr(0, eq));
            std::string val = trim_copy(l.substr(eq + 1));
            if (!valid_name(key)) return fail(idx, "invalid property name `" + key + "`");
            if (w.props.count(key)) return fail(idx, "duplicate property `" + key + "`");
            if (val.empty()) return fail(idx, "missing value for property `" + key + "`");
            std::string value;
            if (val.front() == '"') {
                // Quoted string with \n \t \" \\ escapes.
                std::string acc;
                bool closed_str = false;
                for (std::size_t i = 1; i < val.size(); i++) {
                    char c = val[i];
                    if (c == '\\' && i + 1 < val.size()) {
                        char e = val[++i];
                        if (e == 'n') acc += '\n';
                        else if (e == 't') acc += '\t';
                        else if (e == '"') acc += '"';
                        else if (e == '\\') acc += '\\';
                        else return fail(idx, "bad escape in string");
                    } else if (c == '"') {
                        closed_str = true;
                        std::string rest = trim_copy(val.substr(i + 1));
                        if (!rest.empty()) return fail(idx, "trailing characters after string");
                        break;
                    } else {
                        acc += c;
                    }
                }
                if (!closed_str) return fail(idx, "unterminated string");
                value = acc;
            } else {
                // Bare token: no whitespace, quotes or braces.
                for (char c : val) {
                    if (std::isspace(static_cast<unsigned char>(c)))
                        return fail(idx, "whitespace in bare value");
                    if (c == '"' || c == '{' || c == '}')
                        return fail(idx, "quote/brace in bare value");
                }
                value = val;
            }
            w.props.emplace(key, value);
            idx++;
        }
        if (!closed) return fail(lines.size() - 1, "missing closing `}`");
        out.push_back(std::move(w));
    }
    return out;
}

} // namespace marmel::widget
