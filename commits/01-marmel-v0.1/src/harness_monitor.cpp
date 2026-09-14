// Rust origin: src/harness/monitor.rs
#include "marmel/harness.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace marmel::harness {
namespace {

std::string bare_name(const std::string& name) {
    constexpr char kPrefix[] = "terminal__";
    if (name.rfind(kPrefix, 0) == 0) return name.substr(sizeof(kPrefix) - 1);
    return name;
}

bool is_pagination_tool(const std::string& name) {
    std::string b = bare_name(name);
    return b == "read_file" || b == "grep_search";
}

// Recursively drop `offset`/`page` keys (all objects, incl. nested/arrays).
Json strip_pagination(const Json& v) {
    if (v.is_object()) {
        Json::Object o;
        for (auto& kv : v.as_object()) {
            if (kv.first == "offset" || kv.first == "page") continue;
            o.emplace(kv.first, strip_pagination(kv.second));
        }
        return Json(std::move(o));
    }
    if (v.is_array()) {
        Json::Array a;
        for (auto& e : v.as_array()) a.push_back(strip_pagination(e));
        return Json(std::move(a));
    }
    return v;
}

bool semantic_value_eq(const Json& a, const Json& b) {
    return strip_pagination(a) == strip_pagination(b);
}

bool pagination_only_differs(const std::string& name, const Json& a, const Json& b) {
    if (!is_pagination_tool(name)) return false;
    if (a == b) return false;
    return semantic_value_eq(a, b);
}

bool is_consecutive_repeat(const ToolCallRecord& prev, const ToolCallRecord& rec) {
    if (prev.name != rec.name) return false;
    if (pagination_only_differs(prev.name, prev.arguments, rec.arguments)) return false;
    return prev.arguments == rec.arguments;
}

std::string trim_ws(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

// --- XML rescue internals ------------------------------------------------------
struct ParsedBlock {
    std::string name;
    std::string args_str; // JSON dump or raw string
};

std::optional<ParsedBlock> try_embedded_json(const std::string& inner) {
    std::string t = trim_ws(inner);
    if (t.empty() || t.front() != '{') return std::nullopt;
    Json v;
    try {
        v = Json::parse(t);
    } catch (...) {
        return std::nullopt;
    }
    if (!v.is_object()) return std::nullopt;
    std::string name;
    if (v.contains("function") && v.at("function").is_string())
        name = v.at("function").as_string();
    else if (v.contains("name") && v.at("name").is_string())
        name = v.at("name").as_string();
    else
        return std::nullopt;
    Json args = v.contains("arguments") ? v.at("arguments") : Json();
    std::string args_str;
    if (args.is_string())
        args_str = args.as_string();
    else if (args.is_null())
        args_str = "{}";
    else
        args_str = args.dump();
    return ParsedBlock{name, args_str};
}

std::optional<ParsedBlock> try_function_attr(const std::string& open_tag, const std::string& inner) {
    static const std::regex re(R"re(function\s*=\s*"?([^"\s>]+)"?)re");
    std::smatch m;
    if (!std::regex_search(open_tag, m, re)) return std::nullopt;
    std::string name = m[1].str();
    std::string body = trim_ws(inner);
    std::string args_str;
    try {
        Json v = Json::parse(body);
        if (v.is_string())
            args_str = v.as_string();
        else
            args_str = v.dump();
    } catch (...) {
        args_str = body;
    }
    return ParsedBlock{name, args_str};
}

std::optional<ParsedBlock> try_legacy_function_block(const std::string& inner) {
    static const std::regex fn_re(R"(<function\s*=\s*([^>]+)>)");
    std::smatch m;
    if (!std::regex_search(inner, m, fn_re)) return std::nullopt;
    std::string name = trim_ws(m[1].str());
    static const std::regex param_re(
        R"re(<parameter(?:\s+name)?\s*=\s*"?([^">]+)"?\s*>(.*?)</parameter>)re");
    Json::Object obj;
    auto begin = std::sregex_iterator(inner.begin(), inner.end(), param_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        obj.emplace(trim_ws((*it)[1].str()), Json((*it)[2].str()));
    }
    if (!obj.empty()) return ParsedBlock{name, Json(std::move(obj)).dump()};
    // Raw body string: text between the first '>' and </function>.
    auto gt = inner.find('>');
    auto close = inner.rfind("</function>");
    std::string body;
    if (gt != std::string::npos && close != std::string::npos && close > gt)
        body = trim_ws(inner.substr(gt + 1, close - gt - 1));
    else
        body = trim_ws(inner);
    return ParsedBlock{name, body};
}

} // namespace

// --- ToolCallRecord --------------------------------------------------------------
ToolCallRecord ToolCallRecord::from_tool_call(const types::ToolCall& tc) {
    ToolCallRecord r;
    r.name = tc.name;
    r.arguments = Json::parse_or_string(tc.arguments);
    return r;
}
bool ToolCallRecord::semantically_eq(const ToolCallRecord& o) const {
    return name == o.name && semantic_value_eq(arguments, o.arguments);
}
bool ToolCallRecord::same_operation(const ToolCallRecord& o) const {
    if (name != o.name) return false;
    if (pagination_only_differs(name, arguments, o.arguments)) return false;
    return arguments == o.arguments;
}
bool semantic_json_eq(const std::string& a, const std::string& b) {
    try {
        return semantic_value_eq(Json::parse(a), Json::parse(b));
    } catch (...) {
        return a == b;
    }
}

// --- XmlToolRescue -----------------------------------------------------------------
XmlToolRescue::XmlToolRescue() = default;
XmlToolRescue::XmlToolRescue(std::shared_ptr<HarnessStats> stats) : stats_(std::move(stats)) {}
void XmlToolRescue::set_stats(std::shared_ptr<HarnessStats> stats) { stats_ = std::move(stats); }

std::vector<types::ToolCall> XmlToolRescue::rescue(const std::string& text) const {
    std::vector<types::ToolCall> out;
    std::size_t from = 0;
    while (from < text.size()) {
        std::size_t open = text.find("<tool_call", from);
        std::string open_tag, inner;
        bool legacy = false;
        std::size_t next_from = std::string::npos;
        if (open != std::string::npos) {
            std::size_t gt = text.find('>', open);
            std::size_t close = text.find("</tool_call>", open);
            if (gt != std::string::npos && close != std::string::npos && close > gt) {
                open_tag = text.substr(open, gt - open + 1);
                inner = text.substr(gt + 1, close - gt - 1);
                next_from = close + std::string("</tool_call>").size();
            } else {
                break;
            }
        } else {
            // Legacy: `tool_call <function=...>...</function>`.
            std::size_t tc = text.find("tool_call", from);
            if (tc == std::string::npos) break;
            std::size_t fn = text.find("<function=", tc);
            if (fn == std::string::npos) break;
            // Require only whitespace between "tool_call" and "<function=".
            bool ws_only = true;
            for (std::size_t i = tc + 9; i < fn; i++)
                if (!std::isspace(static_cast<unsigned char>(text[i]))) ws_only = false;
            if (!ws_only) {
                from = fn + 1;
                continue;
            }
            std::size_t close = text.find("</function>", fn);
            if (close == std::string::npos) break;
            open_tag = "";
            inner = text.substr(fn, close - fn + std::string("</function>").size());
            next_from = close + std::string("</function>").size();
            legacy = true;
        }
        std::optional<ParsedBlock> parsed;
        if (!legacy) {
            parsed = try_embedded_json(inner);
            if (!parsed) parsed = try_function_attr(open_tag, inner);
            if (!parsed) parsed = try_legacy_function_block(inner);
        } else {
            parsed = try_legacy_function_block(inner);
            if (!parsed) parsed = try_embedded_json(inner);
        }
        if (parsed && !parsed->name.empty()) {
            types::ToolCall tc;
            tc.id = "call_text_" + types::generate_uuid_v4();
            tc.name = parsed->name;
            tc.arguments = parsed->args_str.empty() ? "{}" : parsed->args_str;
            out.push_back(std::move(tc));
        }
        if (next_from == std::string::npos) break;
        from = next_from;
    }
    if (!out.empty() && stats_) stats_->record_xml_rescue();
    return out;
}

// --- ToolRepetitionDetector -----------------------------------------------------------
ToolRepetitionDetector::ToolRepetitionDetector(std::size_t threshold)
    : threshold_(std::max<std::size_t>(2, threshold)) {}

Intervention ToolRepetitionDetector::evaluate(const ToolCallRecord& rec) {
    Intervention iv = Intervention::None;
    // classify BEFORE recording (buffer holds history only).
    {
        // consecutive
        std::size_t count = 1;
        for (auto it = buffer_.rbegin(); it != buffer_.rend(); ++it) {
            if (is_consecutive_repeat(*it, rec))
                count++;
            else
                break;
        }
        if (count >= threshold_) iv = Intervention::Block;
    }
    if (iv == Intervention::None) {
        if (detect_cycle(rec)) iv = Intervention::Cut;
    }
    record(rec);
    return iv;
}
void ToolRepetitionDetector::record(ToolCallRecord rec) {
    buffer_.push_back(std::move(rec));
    while (buffer_.size() > kToolBufferCapacity) buffer_.pop_front();
}
bool ToolRepetitionDetector::detect_consecutive(const ToolCallRecord& rec) const {
    std::size_t count = 1;
    for (auto it = buffer_.rbegin(); it != buffer_.rend(); ++it) {
        if (is_consecutive_repeat(*it, rec))
            count++;
        else
            break;
    }
    return count >= threshold_;
}
bool ToolRepetitionDetector::detect_cycle(const ToolCallRecord& rec) const {
    std::vector<const ToolCallRecord*> all;
    all.reserve(buffer_.size() + 1);
    for (auto& r : buffer_) all.push_back(&r);
    all.push_back(&rec);
    std::size_t n = all.size();
    std::size_t needed = threshold_ * 2;
    if (n < needed) return false;
    const ToolCallRecord& a = *all[n - 1];
    const ToolCallRecord& b = *all[n - 2];
    if (a.semantically_eq(b)) return false;
    for (std::size_t i = n - 2; i-- > 0;) {
        std::size_t dist = (n - 1) - i; // distance from tail
        const ToolCallRecord& expected = (dist % 2 == 0) ? a : b;
        if (!all[i]->semantically_eq(expected)) {
            // First break at distance dist: matched prefix length = dist.
            return (dist / 2) >= threshold_;
        }
        if (i == 0) break;
    }
    return (n / 2) >= threshold_;
}

// --- RepetitionDetector ------------------------------------------------------------------
RepetitionDetector::RepetitionDetector(std::size_t threshold, std::size_t min_len)
    : threshold_(std::max<std::size_t>(2, threshold)), min_len_(std::max<std::size_t>(1, min_len)) {}
void RepetitionDetector::push(const std::string& text) {
    for (char c : text) {
        buffer_.push_back(c);
        while (buffer_.size() > kTextBufferCapacity) buffer_.pop_front();
    }
}
bool RepetitionDetector::is_repeating() const {
    std::size_t n = buffer_.size();
    if (n < threshold_ || threshold_ == 0) return false;
    std::size_t max_pattern = n / threshold_;
    if (max_pattern < min_len_) return false;
    for (std::size_t pat = max_pattern; pat >= min_len_; pat--) {
        bool ok = true;
        for (std::size_t k = 1; k < threshold_; k++) {
            for (std::size_t j = 0; j < pat; j++) {
                if (buffer_[n - (k + 1) * pat + j] != buffer_[n - pat + j]) {
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
        }
        if (ok) return true;
        if (pat == min_len_) break; // size_t underflow guard
    }
    return false;
}

// --- prune (re-export lives in agent.cpp; identical algorithm here) -------------------------
std::vector<types::Message> prune_orphan_tool_messages(std::vector<types::Message> messages) {
    std::map<std::string, bool> valid;
    for (auto& m : messages)
        if (m.role == types::MsgRole::Assistant)
            for (auto& tc : m.tool_calls) valid[tc.id] = true;
    std::vector<types::Message> out;
    for (auto& m : messages) {
        if (m.role == types::MsgRole::Tool && !valid.count(m.tool_call_id)) continue;
        out.push_back(std::move(m));
    }
    return out;
}

// --- HarnessMonitor ----------------------------------------------------------------------------
HarnessMonitor::HarnessMonitor() : stats_(std::make_shared<HarnessStats>()) { reset_text_break(); }
HarnessMonitor::HarnessMonitor(std::shared_ptr<HarnessStats> stats) : stats_(std::move(stats)) {
    xml_.set_stats(stats_);
}
HarnessMonitor HarnessMonitor::new_with_config(std::shared_ptr<HarnessStats> stats,
                                               const config::MonitoringConfig& cfg) {
    HarnessMonitor m;
    m.stats_ = std::move(stats);
    m.threshold_ = std::max<std::size_t>(2, cfg.repetition_threshold);
    m.min_len_ = std::max<std::size_t>(1, cfg.min_pattern_len);
    m.tool_rep_ = ToolRepetitionDetector(m.threshold_);
    m.text_rep_ = RepetitionDetector(m.threshold_, m.min_len_);
    m.xml_.set_stats(m.stats_);
    m.repetition_fired_ = false;
    return m;
}
HarnessMonitor HarnessMonitor::with_new_stats() {
    return HarnessMonitor(std::make_shared<HarnessStats>());
}
void HarnessMonitor::attach_stats(std::shared_ptr<HarnessStats> stats) {
    stats_ = std::move(stats);
    xml_.set_stats(stats_);
}
std::vector<types::ToolCall> HarnessMonitor::rescue_xml(const std::string& text) {
    return xml_.rescue(text);
}
Intervention HarnessMonitor::observe_tool(const std::string& name, const Json& args) {
    ToolCallRecord rec{name, args};
    return tool_rep_.evaluate(rec);
}
std::optional<std::string> HarnessMonitor::intervention_error(Intervention iv) const {
    if (iv == Intervention::Block)
        return "TOOL REPETITION DETECTED: You have called this tool with identical arguments " +
               std::to_string(threshold_) +
               " times in a row. Stop looping and try an alternative approach.";
    if (iv == Intervention::Cut)
        return "TOOL CYCLE DETECTED: You are repeating a loop of tool calls. Step back and "
               "re-evaluate your plan.";
    return std::nullopt;
}
bool HarnessMonitor::feed_text(const std::string& chunk) {
    text_rep_.push(chunk);
    if (!repetition_fired_ && text_rep_.is_repeating()) {
        repetition_fired_ = true;
        if (stats_) stats_->record_repetition_break();
        text_rep_ = RepetitionDetector(threshold_, min_len_);
        return true;
    }
    return false;
}
void HarnessMonitor::reset_text_break() {
    repetition_fired_ = false;
    if (threshold_ == 0) threshold_ = kDefaultRepetitionThreshold;
    if (min_len_ == 0) min_len_ = kDefaultMinPatternLen;
    text_rep_ = RepetitionDetector(threshold_, min_len_);
}
std::size_t HarnessMonitor::tool_buffer_len() const { return tool_rep_.len(); }

} // namespace marmel::harness
