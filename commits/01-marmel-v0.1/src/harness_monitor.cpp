// Rust origin: src/harness/monitor.rs
#include "marmel/harness.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace marmel::harness {
namespace {

bool is_pagination_tool(const std::string& name) {
    // Exact base names only (no terminal__ stripping — matches Rust).
    return name == "read_file" || name == "grep_search";
}

// Recursively drop `offset`/`page` keys (all objects, incl. nested/arrays).
// Used by the ToolCallRecord semantic comparisons (strip_pagination).
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
// Mirrors monitor.rs find_next_tool_call_block / parse_tool_call_block /
// try_embedded_json / try_function_attr / try_legacy_function_block /
// extract_inner_text / extract_attribute / make_rescued_call exactly
// (including literal `<function=`/`<parameter=` matching and Null→"null").
struct ParsedBlock {
    std::string name;
    std::string args_str; // JSON dump or raw string
};

std::string extract_inner_text(const std::string& block) {
    auto gt = block.find('>');
    std::size_t open_end = (gt == std::string::npos) ? 0 : gt + 1;
    auto close = block.find("</tool_call>");
    if (close == std::string::npos || close < open_end) return block.substr(open_end);
    return block.substr(open_end, close - open_end);
}

std::optional<std::string> extract_attribute(const std::string& block, const std::string& attr) {
    std::string needle = attr + "=";
    auto idx = block.find(needle);
    if (idx == std::string::npos) return std::nullopt;
    std::string after = trim_ws(block.substr(idx + needle.size()));
    if (!after.empty() && after.front() == '"') {
        auto end = after.find('"', 1);
        if (end == std::string::npos) return std::nullopt;
        return after.substr(1, end - 1);
    }
    std::size_t end = 0;
    while (end < after.size() && after[end] != '>' && !std::isspace(static_cast<unsigned char>(after[end])))
        end++;
    if (end == 0) return std::nullopt;
    return after.substr(0, end);
}

std::optional<ParsedBlock> try_embedded_json(const std::string& block) {
    std::string t = trim_ws(extract_inner_text(block));
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
        args_str = "null";
    else
        args_str = args.dump();
    return ParsedBlock{name, args_str};
}

std::optional<ParsedBlock> try_function_attr(const std::string& block) {
    auto name = extract_attribute(block, "function");
    if (!name) return std::nullopt;
    std::string body = trim_ws(extract_inner_text(block));
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
    return ParsedBlock{*name, args_str};
}

std::optional<ParsedBlock> try_legacy_function_block(const std::string& block) {
    static const std::string kFuncOpen = "<function=";
    auto fs = block.find(kFuncOpen);
    if (fs == std::string::npos) return std::nullopt;
    std::size_t name_start = fs + kFuncOpen.size();
    auto gt_rel = block.substr(name_start).find('>');
    if (gt_rel == std::string::npos) return std::nullopt;
    std::string name = trim_ws(block.substr(name_start, gt_rel));
    std::size_t body_start = name_start + gt_rel + 1;
    static const std::string kClose = "</function>";
    auto be_rel = block.substr(body_start).find(kClose);
    if (be_rel == std::string::npos) return std::nullopt;
    std::string body = block.substr(body_start, be_rel);
    Json::Object obj;
    std::size_t cursor = 0;
    static const std::string kParam = "<parameter=";
    static const std::string kParamClose = "</parameter>";
    while (true) {
        auto rel = body.substr(cursor).find(kParam);
        if (rel == std::string::npos) break;
        std::size_t p_start = cursor + rel;
        std::size_t key_start = p_start + kParam.size();
        auto ke_rel = body.substr(key_start).find('>');
        if (ke_rel == std::string::npos) break;
        std::string key = trim_ws(body.substr(key_start, ke_rel));
        std::size_t val_start = key_start + ke_rel + 1;
        auto ve_rel = body.substr(val_start).find(kParamClose);
        if (ve_rel != std::string::npos) {
            obj.emplace(key, Json(trim_ws(body.substr(val_start, ve_rel))));
        }
        cursor = val_start;
    }
    if (obj.empty()) return ParsedBlock{name, trim_ws(body)};
    return ParsedBlock{name, Json(std::move(obj)).dump()};
}

std::optional<ParsedBlock> parse_tool_call_block(const std::string& block) {
    if (auto p = try_embedded_json(block)) return p;
    if (auto p = try_function_attr(block)) return p;
    return try_legacy_function_block(block);
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
    // Legacy helper: drops offset/page at the TOP level only.
    auto norm = [](const std::string& s) {
        try {
            Json v = Json::parse(s);
            if (v.is_object()) {
                v.as_object().erase("offset");
                v.as_object().erase("page");
            }
            return v;
        } catch (...) {
            return Json(s);
        }
    };
    return norm(a) == norm(b);
}

// --- XmlToolRescue -----------------------------------------------------------------
XmlToolRescue::XmlToolRescue() = default;
XmlToolRescue::XmlToolRescue(std::shared_ptr<HarnessStats> stats) : stats_(std::move(stats)) {}
void XmlToolRescue::set_stats(std::shared_ptr<HarnessStats> stats) { stats_ = std::move(stats); }

std::vector<types::ToolCall> XmlToolRescue::rescue(const std::string& text) const {
    std::vector<types::ToolCall> out;
    std::size_t from = 0;
    static const std::string kCloseTag = "</tool_call>";
    static const std::string kFuncClose = "</function>";
    while (from < text.size()) {
        std::optional<std::pair<std::size_t, std::size_t>> range;
        // Angle-bracket style first (no early break when malformed).
        {
            std::size_t open = text.find("<tool_call", from);
            if (open != std::string::npos) {
                std::size_t close = text.find(kCloseTag, open);
                if (close != std::string::npos)
                    range = std::make_pair(open, close + kCloseTag.size());
            }
        }
        // Legacy SPEC style: `tool_call` + optional whitespace + `<function=`.
        if (!range) {
            std::size_t search = from;
            while (true) {
                std::size_t tc = text.find("tool_call", search);
                if (tc == std::string::npos) break;
                std::size_t after = tc + 9;
                std::size_t ws = after;
                while (ws < text.size() &&
                       std::isspace(static_cast<unsigned char>(text[ws])))
                    ws++;
                if (text.compare(ws, 10, "<function=") == 0) {
                    std::size_t close = text.find(kFuncClose, ws);
                    if (close == std::string::npos) break;
                    range = std::make_pair(tc, close + kFuncClose.size());
                    break;
                }
                search = tc + 9;
            }
        }
        if (!range) break;
        std::string block = text.substr(range->first, range->second - range->first);
        // Shared parse order for both block kinds: embedded > attr > legacy.
        if (auto parsed = parse_tool_call_block(block)) {
            if (!parsed->name.empty()) {
                types::ToolCall tc;
                tc.id = "call_text_" + types::generate_uuid_v4();
                tc.name = parsed->name;
                tc.arguments = parsed->args_str.empty() ? "{}" : parsed->args_str;
                out.push_back(std::move(tc));
            }
        }
        from = range->second;
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
    // Decode UTF-8 into scalar values (invalid bytes → U+FFFD, one each).
    for (std::size_t i = 0; i < text.size();) {
        unsigned char c = text[i];
        char32_t cp = 0xFFFD;
        std::size_t len = 1;
        if ((c & 0x80) == 0) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        }
        bool ok = (c & 0x80) == 0;
        if (!ok && i + len <= text.size()) {
            ok = true;
            cp = (len == 2)   ? (c & 0x1F)
                 : (len == 3) ? (c & 0x0F)
                              : (c & 0x07);
            for (std::size_t k = 1; k < len; k++) {
                unsigned char d = text[i + k];
                if ((d & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
                cp = (cp << 6) | (d & 0x3F);
            }
        }
        if (!ok) {
            cp = 0xFFFD;
            len = 1;
        }
        buffer_.push_back(cp);
        while (buffer_.size() > kTextBufferCapacity) buffer_.pop_front();
        i += len;
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
    // Only re-arms the flag (the text buffer is kept, as in Rust).
    repetition_fired_ = false;
}
std::size_t HarnessMonitor::tool_buffer_len() const { return tool_rep_.len(); }

} // namespace marmel::harness
