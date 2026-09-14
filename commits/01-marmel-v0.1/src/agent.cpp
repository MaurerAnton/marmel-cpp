// Rust origin: src/agent/{context,phase,loop}.rs
#include "marmel/agent_context.hpp"
#include "marmel/agent_loop.hpp"
#include "marmel/agent_phase.hpp"
#include "marmel/harness.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <thread>

namespace marmel::agent {
namespace {

// --- UTF-8 helpers ------------------------------------------------------------
bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }
std::size_t next_boundary(const std::string& s, std::size_t i) {
    while (i < s.size() && is_continuation(static_cast<unsigned char>(s[i]))) i++;
    return i;
}
std::size_t prev_boundary(const std::string& s, std::size_t i) {
    if (i > s.size()) i = s.size();
    while (i > 0 && is_continuation(static_cast<unsigned char>(s[i - 1])) == false && i < s.size() &&
           is_continuation(static_cast<unsigned char>(s[i])))
        break;
    while (i > 0 && is_continuation(static_cast<unsigned char>(s[i > 0 ? i - 1 : 0])) && i != 0) {
        // step back over continuation bytes to the lead byte
        if (i > 0 && is_continuation(static_cast<unsigned char>(s[i - 1]))) i--;
        else break;
    }
    // Simpler correct version: walk back while previous byte is continuation.
    std::size_t j = i > s.size() ? s.size() : i;
    while (j > 0 && is_continuation(static_cast<unsigned char>(s[j - 1]))) j--;
    // If we landed mid-sequence start, j is the lead byte — valid boundary.
    // If s[j] itself is a continuation (shouldn't happen), step forward.
    while (j < s.size() && is_continuation(static_cast<unsigned char>(s[j]))) j++;
    return j;
}

std::size_t codepoints(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s)
        if (!is_continuation(c)) n++;
    return n;
}
std::size_t approx_tokens(const std::string& s) { return (codepoints(s) + 3) / 4; }

std::size_t message_tokens(const types::Message& m) {
    std::size_t n = 3; // framing (OpenAI cookbook approx, as in Rust)
    if (m.role == types::MsgRole::Assistant) {
        if (m.has_content) n += approx_tokens(m.content);
        if (m.reasoning_content) n += approx_tokens(*m.reasoning_content);
        for (auto& tc : m.tool_calls) n += 1 + approx_tokens(tc.name) + approx_tokens(tc.arguments);
    } else if (m.role == types::MsgRole::Tool) {
        n += approx_tokens(m.content);
    } else {
        n += approx_tokens(m.content);
    }
    return n;
}

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string trim_copy(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        b--;
    return s.substr(a, b - a);
}
std::vector<std::string> collect_task_ids(const std::string& markdown, const std::regex& re) {
    std::vector<std::string> out;
    std::istringstream in(markdown);
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re)) out.push_back(m[1].str());
    }
    return out;
}
bool contains_ci(const std::string& hay, const std::string& needle_upper) {
    return to_upper(hay).find(needle_upper) != std::string::npos;
}

std::optional<std::string> find_task_id(const std::string& text) {
    static const std::regex re(R"(\(?\[?(t-[A-Za-z0-9_-]+)\]?\)?)");
    std::smatch m;
    if (std::regex_search(text, m, re)) return m[1].str();
    return std::nullopt;
}

/// UTC `%Y%m%d_%H%M%S` stamp (chrono::Utc, as in Rust — not localtime).
std::string utc_stamp() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

} // namespace

// --- context -------------------------------------------------------------------

std::string utf8_safe_slice(const std::string& s, std::size_t start, std::size_t end) {
    std::size_t b = start > s.size() ? s.size() : next_boundary(s, start);
    std::size_t e = end > s.size() ? s.size() : prev_boundary(s, end);
    if (b >= e) return "";
    return s.substr(b, e - b);
}

std::size_t count_tokens(const std::vector<types::Message>& messages) {
    std::size_t n = 0;
    for (auto& m : messages) n += message_tokens(m);
    return n;
}
std::size_t compaction_threshold(std::size_t max_tokens) {
    return static_cast<std::size_t>(std::llround(max_tokens * kCompactTriggerRatio));
}
std::size_t compaction_target(std::size_t max_tokens) {
    return static_cast<std::size_t>(std::llround(max_tokens * kCompactTargetRatio));
}

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

bool SlowPrefillTracker::record_prefill(std::chrono::milliseconds duration) {
    if (turns_since_rebirth_ < std::numeric_limits<std::uint64_t>::max()) turns_since_rebirth_++;
    // Rust compares duration.as_secs() (truncated whole seconds) >= 300.
    auto secs = static_cast<unsigned long long>(duration.count() / 1000);
    if (secs >= kSlowPrefillThresholdSecs) {
        if (consecutive_slow_ < std::numeric_limits<unsigned>::max()) consecutive_slow_++;
        return consecutive_slow_ >= 2 && turns_since_rebirth_ >= kMinTurnsAfterRebirth;
    }
    consecutive_slow_ = 0;
    return false;
}
void SlowPrefillTracker::note_rebirth() {
    turns_since_rebirth_ = 0;
    consecutive_slow_ = 0;
}

ContextEngine::ContextEngine(std::size_t max_context_tokens) : max_context_tokens_(max_context_tokens) {}
void ContextEngine::set_stats(std::shared_ptr<harness::HarnessStats> stats) { stats_ = std::move(stats); }

void ContextEngine::set_system_prompt(std::string prompt) {
    if (messages_.empty())
        messages_.push_back(types::Message::system(std::move(prompt)));
    else
        messages_[0] = types::Message::system(std::move(prompt));
}
void ContextEngine::set_goal(std::string goal) {
    if (messages_.empty()) messages_.push_back(types::Message::system(""));
    if (messages_.size() == 1)
        messages_.push_back(types::Message::user(std::move(goal)));
    else
        messages_[1] = types::Message::user(std::move(goal));
}
void ContextEngine::append(types::Message msg) { messages_.push_back(std::move(msg)); }
std::size_t ContextEngine::token_count() const { return count_tokens(messages_); }
bool ContextEngine::should_compact() const { return token_count() > compaction_threshold(max_context_tokens_); }
void ContextEngine::compact() {
    std::size_t target = compaction_target(max_context_tokens_);
    // Always pin the first two messages (no early return).
    std::vector<types::Message> kept;
    for (std::size_t i = 0; i < messages_.size() && i < 2; i++) kept.push_back(messages_[i]);
    std::size_t total = count_tokens(kept);
    std::vector<types::Message> tail;
    if (messages_.size() > 2) {
        for (std::size_t i = messages_.size(); i-- > 2;) {
            std::size_t cost = message_tokens(messages_[i]);
            if (total + cost > target && !tail.empty()) break;
            total += cost;
            tail.push_back(messages_[i]);
        }
        std::reverse(tail.begin(), tail.end());
    }
    for (auto& m : tail) kept.push_back(std::move(m));
    messages_ = prune_orphan_tool_messages(std::move(kept));
    if (stats_) stats_->record_compaction();
}
void ContextEngine::reset_compaction_retry_count() { compaction_retry_count_ = 0; }

bool ContextEngine::compact_to_target(std::size_t target, bool force) {
    std::size_t cur = token_count();
    if (!force && cur <= target) return false;
    std::size_t before = messages_.size();
    std::vector<types::Message> kept;
    for (std::size_t i = 0; i < messages_.size() && i < 2; i++) kept.push_back(messages_[i]);
    std::size_t total = count_tokens(kept);
    std::vector<types::Message> tail;
    if (messages_.size() > 2) {
        for (std::size_t i = messages_.size(); i-- > 2;) {
            std::size_t cost = message_tokens(messages_[i]);
            if (total + cost > target && !tail.empty()) break;
            total += cost;
            tail.push_back(messages_[i]);
        }
        std::reverse(tail.begin(), tail.end());
    }
    // Return value is pre-prune (Rust: removed = len - (2 + kept_tail)).
    std::size_t removed = before >= 2 + tail.size() ? before - (2 + tail.size()) : 0;
    for (auto& m : tail) kept.push_back(std::move(m));
    messages_ = prune_orphan_tool_messages(std::move(kept));
    return removed > 0;
}

bool ContextEngine::compact_context(std::size_t max_tokens) {
    if (token_count() <= max_tokens) return false;
    std::size_t target = static_cast<std::size_t>(std::llround(max_tokens * kCompactOverLimitRatio));
    return compact_to_target(target, false);
}
bool ContextEngine::force_compact_context(std::size_t target_tokens) {
    return compact_to_target(target_tokens, true);
}

bool ContextEngine::compact_with_retry(std::size_t limit) {
    if (compaction_retry_count_ >= kCompactionRetryCap) return false;
    compaction_retry_count_++;
    bool ok;
    if (compaction_retry_count_ == 1 && token_count() > limit) {
        ok = compact_context(limit);
    } else {
        double ratio = compaction_retry_count_ == 1 ? kCompactRetry1Ratio : kCompactRetry2Ratio;
        std::size_t cur = token_count();
        std::size_t target =
            static_cast<std::size_t>(std::llround(std::min(cur, limit) * ratio));
        ok = force_compact_context(target);
    }
    if (ok) {
        if (stats_) stats_->record_compaction();
        inject_context_limit_exceeded();
    }
    return ok;
}

void ContextEngine::inject_context_limit_exceeded() {
    messages_.push_back(types::Message::user(kContextLimitExceededMessage));
}

void ContextEngine::perform_rebirth(const std::string& summary) {
    std::string sys = (!messages_.empty() && messages_[0].role == types::MsgRole::System)
                          ? messages_[0].content
                          : "";
    std::string goal = (messages_.size() > 1 && messages_[1].role == types::MsgRole::User)
                           ? messages_[1].content
                           : "";
    std::string slot = goal;
    for (std::size_t i = messages_.size(); i-- > 2;) {
        if (messages_[i].role == types::MsgRole::User && messages_[i].content != goal) {
            slot = messages_[i].content;
            break;
        }
    }
    messages_.clear();
    messages_.push_back(types::Message::system(sys));
    messages_.push_back(types::Message::user(goal));
    messages_.push_back(types::Message::user(slot));
    messages_.push_back(
        types::Message::system(std::string(kRebirthCheckpointPrefix) + summary + ")"));
    if (stats_) stats_->record_rebirth();
    prefill_.note_rebirth();
}

ContextEngineFactory::ContextEngineFactory(std::size_t max_context_tokens)
    : max_context_tokens_(max_context_tokens) {}
ContextEngine ContextEngineFactory::manager_context(std::string system_prompt,
                                                    std::string goal) const {
    ContextEngine e(max_context_tokens_);
    e.set_system_prompt(std::move(system_prompt));
    e.set_goal(std::move(goal));
    return e;
}
ContextEngine ContextEngineFactory::specialist_context(std::string role_prompt,
                                                       std::string brief) const {
    ContextEngine e(max_context_tokens_);
    e.set_system_prompt(std::move(role_prompt));
    e.set_goal(std::move(brief));
    return e;
}

// --- phase ---------------------------------------------------------------------

std::optional<MissionPhase> parse_phase(const std::string& text) {
    std::string t = trim_copy(text);
    std::string u = to_upper(t);
    if (u == "CONVERSATIONAL") return MissionPhase::Conversational;
    if (u == "EXECUTING") return MissionPhase::Executing;
    return std::nullopt;
}

bool output_is_success(const std::string& output) {
    std::string u = to_upper(output);
    return u.find("ERROR") == std::string::npos && u.find("FAILED") == std::string::npos &&
           u.find("REPLAN REQUIRED") == std::string::npos;
}

std::vector<std::string> parse_unchecked_tasks(const std::string& markdown) {
    static const std::regex re(R"(^\s*[-*]\s*\[\s*\]\s*\*{0,2}\[?(t-[A-Za-z0-9_-]+)\]?\*{0,2})",
                               std::regex::icase);
    return collect_task_ids(markdown, re);
}
std::vector<std::string> parse_all_tasks(const std::string& markdown) {
    static const std::regex re(
        R"(^\s*[-*]\s*\[\s*[ xX]?\s*\]\s*\*{0,2}\[?(t-[A-Za-z0-9_-]+)\]?\*{0,2})",
        std::regex::icase);
    return collect_task_ids(markdown, re);
}

std::optional<MissionMarker> MissionMarker::parse(const std::string& text) {
    std::string u = to_upper(text);
    if (u.find("REPLAN REQUIRED") != std::string::npos)
        return MissionMarker{Kind::Replan, std::nullopt, text};
    if (u.find("MISSION COMPLETE") != std::string::npos)
        return MissionMarker{Kind::Complete, find_task_id(text), {}};
    if (u.find("FAILED") != std::string::npos) return MissionMarker{Kind::Failed, std::nullopt, text};
    return std::nullopt;
}

std::mutex& Plan::plan_mutex() {
    static std::mutex m;
    return m;
}

Plan::Plan(std::string dir) : dir_(std::move(dir)) {}
std::string Plan::plan_path() const { return dir_ + "/" + kPlanFile; }
std::string Plan::forced_phase_path() const { return dir_ + "/" + kForcedPhaseFile; }

void Plan::create(const std::string& plan_markdown) const {
    std::lock_guard<std::mutex> lock(plan_mutex());
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    std::ofstream f(plan_path(), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write plan file: " + plan_path());
    f << plan_markdown; // verbatim (no trailing-newline normalization)
}

std::optional<std::string> Plan::read() const {
    std::lock_guard<std::mutex> lock(plan_mutex());
    std::ifstream f(plan_path(), std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool Plan::exists() const {
    std::lock_guard<std::mutex> lock(plan_mutex());
    std::error_code ec;
    return std::filesystem::exists(plan_path(), ec);
}
void Plan::clear() const {
    // Deletes the active plan + top-level archive snapshot + forced phase;
    // the archive/ directory itself is kept (as in Rust).
    std::lock_guard<std::mutex> lock(plan_mutex());
    std::error_code ec;
    std::filesystem::remove(plan_path(), ec);
    std::filesystem::remove(dir_ + "/execution_plan_archive.md", ec);
    std::filesystem::remove(forced_phase_path(), ec);
}
std::vector<std::string> Plan::pending_tasks() const {
    auto c = read();
    if (!c) return {};
    return parse_unchecked_tasks(*c);
}
std::vector<std::string> Plan::all_tasks() const {
    auto c = read();
    if (!c) return {};
    return parse_all_tasks(*c);
}
bool Plan::is_complete() const {
    auto c = read();
    if (!c) return false;
    static const std::regex unchecked(R"(\[\s*\]|\(\s*\))");
    static const std::regex checked(R"(\[[xX]\]|\([xX]\))");
    if (std::regex_search(*c, unchecked)) return false;
    return std::regex_search(*c, checked);
}

std::optional<std::string> Plan::archive() const {
    std::lock_guard<std::mutex> lock(plan_mutex());
    std::ifstream f(plan_path(), std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    static const std::regex unchecked(R"(\[\s*\]|\(\s*\))");
    static const std::regex checked(R"(\[[xX]\]|\([xX]\))");
    if (std::regex_search(content, unchecked)) return std::nullopt;
    if (!std::regex_search(content, checked)) return std::nullopt;
    std::error_code ec;
    std::filesystem::create_directories(dir_ + "/archive", ec);
    std::string dest = dir_ + "/archive/execution_plan_" + utc_stamp() + ".md";
    {
        std::ofstream o(dest, std::ios::binary | std::ios::trunc);
        o << content;
    }
    // Latest snapshot lives at the TOP level (dir/execution_plan_archive.md).
    {
        std::ofstream o(dir_ + "/execution_plan_archive.md", std::ios::binary | std::ios::trunc);
        o << content;
    }
    std::filesystem::remove(plan_path(), ec);
    return dest;
}

bool Plan::check_off(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(plan_mutex());
    std::ifstream f(plan_path(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    std::string needle = to_upper(task_id);
    static const std::regex box(R"(\[\s*\])");
    // Rust lines()/join("\n") semantics: no forced trailing newline.
    std::vector<std::string> lines;
    {
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    bool flipped = false;
    for (auto& line : lines) {
        if (!flipped && contains_ci(line, needle) && std::regex_search(line, box)) {
            line = std::regex_replace(line, box, "[x]", std::regex_constants::format_first_only);
            flipped = true;
        }
    }
    if (!flipped) return false;
    std::string out;
    for (std::size_t i = 0; i < lines.size(); i++) {
        if (i) out += '\n';
        out += lines[i];
    }
    {
        std::ofstream o(plan_path(), std::ios::binary | std::ios::trunc);
        o << out;
    }
    static const std::regex unchecked(R"(\[\s*\]|\(\s*\))");
    static const std::regex checked(R"(\[[xX]\]|\([xX]\))");
    if (!std::regex_search(out, unchecked) && std::regex_search(out, checked)) {
        // Best-effort completion snapshot (keep the active file, as in Rust).
        std::error_code ec;
        std::filesystem::create_directories(dir_ + "/archive", ec);
        std::ofstream o(dir_ + "/archive/execution_plan_" + utc_stamp() + ".md",
                        std::ios::binary | std::ios::trunc);
        o << out;
        std::ofstream o2(dir_ + "/execution_plan_archive.md", std::ios::binary | std::ios::trunc);
        o2 << out;
    }
    return true;
}

bool Plan::check_off_on_success(const std::string& task_id, const std::string& output) const {
    if (!output_is_success(output)) return false;
    return check_off(task_id);
}
bool Plan::check_plan_on_marker(const std::optional<std::string>& task_id,
                                const std::string& deliverable) const {
    auto marker = MissionMarker::parse(deliverable);
    if (!marker || !marker->is_complete()) return false;
    // Explicit override wins, else the marker's own (t-xxx) token. NOTE: an
    // explicit Some("") flows into check_off("") like upstream (matches the
    // first unchecked line via contains("")).
    std::optional<std::string> tid = task_id;
    if (!tid && marker->task_id) tid = marker->task_id;
    if (!tid) return false;
    return check_off(*tid);
}

std::optional<MissionPhase> Plan::forced_phase() const {
    std::lock_guard<std::mutex> lock(plan_mutex());
    std::ifstream f(forced_phase_path(), std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_phase(ss.str());
}
MissionPhase Plan::determine_phase() const {
    if (auto fp = forced_phase()) return *fp;
    return exists() ? MissionPhase::Executing : MissionPhase::Conversational;
}

// --- loop ----------------------------------------------------------------------

TurnPhase next_phase(TurnPhase p) {
    switch (p) {
        case TurnPhase::PrepareTurn: return TurnPhase::CallBackend;
        case TurnPhase::CallBackend: return TurnPhase::StreamResponse;
        case TurnPhase::StreamResponse: return TurnPhase::ProcessResponse;
        case TurnPhase::ProcessResponse: return TurnPhase::ExecuteTools;
        case TurnPhase::ExecuteTools: return TurnPhase::CheckFinish;
        case TurnPhase::CheckFinish: return TurnPhase::PrepareTurn;
    }
    return TurnPhase::PrepareTurn;
}
const char* phase_name(TurnPhase p) {
    switch (p) {
        case TurnPhase::PrepareTurn: return "PrepareTurn";
        case TurnPhase::CallBackend: return "CallBackend";
        case TurnPhase::StreamResponse: return "StreamResponse";
        case TurnPhase::ProcessResponse: return "ProcessResponse";
        case TurnPhase::ExecuteTools: return "ExecuteTools";
        case TurnPhase::CheckFinish: return "CheckFinish";
    }
    return "?";
}

bool is_read_tool(const std::string& name) {
    // Base names only (matches TOOL_* constants): terminal__* aliases match
    // neither class and are dropped silently, exactly as in Rust.
    return name == "read_file" || name == "grep_search" || name == "glob";
}
bool is_write_tool(const std::string& name) {
    return name == "write_file" || name == "replace" || name == "run_command" ||
           name == "delegate_task";
}
std::optional<std::string> extract_task_id(const std::string& name, const Json& args) {
    (void)name;
    if (args.is_object() && args.contains("task_id") && args.at("task_id").is_string())
        return args.at("task_id").as_string();
    static const std::regex re(R"(\[(t-[A-Za-z0-9_-]+)\])");
    std::string dump = args.dump();
    std::smatch m;
    if (std::regex_search(dump, m, re)) return m[1].str();
    return std::nullopt;
}

AgentLoop::AgentLoop() : AgentLoop(Plan(kMarmelDir)) {}
AgentLoop::AgentLoop(Plan plan)
    : plan_(std::move(plan)),
      abort_flag_(std::make_shared<std::atomic<bool>>(false)),
      monitor_(std::make_unique<harness::HarnessMonitor>()),
      stats_(std::make_shared<harness::HarnessStats>()) {
    monitor_->attach_stats(stats_);
}
AgentLoop::AgentLoop(Plan plan, std::shared_ptr<harness::HarnessStats> stats)
    : plan_(std::move(plan)),
      abort_flag_(std::make_shared<std::atomic<bool>>(false)),
      monitor_(std::make_unique<harness::HarnessMonitor>()),
      stats_(std::move(stats)) {
    monitor_->attach_stats(stats_);
}
AgentLoop::~AgentLoop() = default;
harness::HarnessMonitor& AgentLoop::monitor() { return *monitor_; }

bool AgentLoop::feed_stream_text(const std::string& chunk) { return monitor_->feed_text(chunk); }
std::vector<types::ToolCall> AgentLoop::rescue_xml_calls(const std::string& text) {
    auto calls = monitor_->rescue_xml(text);
    if (!calls.empty()) rescued_this_turn_ = true;
    return calls;
}
void AgentLoop::signal(Signal s) {
    if (s.kind == Signal::Kind::Abort) abort_flag_->store(true);
    pending_signals_.push_back(std::move(s));
}
void AgentLoop::track_pty_pid(std::int32_t pid) { active_pty_pids_.push_back(pid); }

bool AgentLoop::drain_signals() {
    abort_flag_->store(false); // clear stale flag; re-armed below on Abort
    std::vector<Signal> pending;
    pending.swap(pending_signals_);
    bool aborted = false;
    for (auto& s : pending) {
        if (s.kind == Signal::Kind::Abort) {
            aborted = true;
        } else {
            transcript_.push_back(types::Message::user(s.text));
        }
    }
    if (aborted) abort_flag_->store(true);
    return aborted;
}

void AgentLoop::abort_pty_process_groups() {
#ifdef __unix__
    for (auto pid : active_pty_pids_) harness::kill_process_group(pid);
#else
    (void)0;
#endif
}

void AgentLoop::enqueue_tools(const std::vector<Json>& tools) {
    for (auto& t : tools) {
        PendingTool p;
        if (t.is_object()) {
            p.name = t.str_or("name", "");
            p.arguments = t.contains("arguments") ? t.at("arguments") : Json::object();
        }
        p.task_id = extract_task_id(p.name, p.arguments);
        pending_tools_.push_back(std::move(p));
    }
}

bool AgentLoop::check_off_tool(const PendingTool& tool, const std::string& output_content) {
    if (!tool.task_id || tool.task_id->empty()) return false;
    if (tool.name == "delegate_task")
        return plan_.check_plan_on_marker(tool.task_id, output_content);
    return plan_.check_off_on_success(*tool.task_id, "ok");
}

TurnOutcome AgentLoop::run_turn() {
    turn_count_++;
    if (turn_count_ > kMaxTurns) return TurnOutcome::complete();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTurnWatchdogSecs);
    TurnPhase phase = TurnPhase::PrepareTurn;
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline)
            return TurnOutcome::error("turn watchdog exceeded");
        switch (phase) {
            case TurnPhase::PrepareTurn: {
                if (drain_signals()) {
                    abort_pty_process_groups();
                    return TurnOutcome::aborted();
                }
                phase = TurnPhase::CallBackend;
                break;
            }
            case TurnPhase::CallBackend:
                phase = TurnPhase::StreamResponse;
                break;
            case TurnPhase::StreamResponse:
                phase = TurnPhase::ProcessResponse;
                break;
            case TurnPhase::ProcessResponse:
                phase = TurnPhase::ExecuteTools;
                break;
            case TurnPhase::ExecuteTools: {
                std::vector<PendingTool> tools;
                tools.swap(pending_tools_);
                if (tools.empty()) {
                    phase = TurnPhase::CheckFinish;
                    break;
                }
                // (a) semantic gate BEFORE dispatch
                std::vector<std::string> blocked;
                for (auto& t : tools) {
                    auto iv = monitor_->observe_tool(t.name, t.arguments);
                    if (auto err = monitor_->intervention_error(iv)) blocked.push_back(*err);
                }
                if (!blocked.empty()) {
                    std::string joined;
                    for (std::size_t i = 0; i < blocked.size(); i++) {
                        if (i) joined += '\n';
                        joined += blocked[i];
                    }
                    return TurnOutcome::tool_error(joined);
                }
                // (b) partition (neither-class tools dropped silently)
                std::vector<PendingTool> reads, writes;
                for (auto& t : tools) {
                    if (is_read_tool(t.name))
                        reads.push_back(t);
                    else if (is_write_tool(t.name))
                        writes.push_back(t);
                }
                auto run_one = [&](const PendingTool& t) -> std::pair<bool, std::string> {
                    if (dispatcher_) return dispatcher_(t, caller_);
                    return {false, "no tool dispatcher installed"};
                };
                // (c) parallel reads, completion order (FuturesUnordered):
                // abort is checked after each await, before check-off.
                std::optional<std::string> error;
                {
                    using Fut = std::future<std::pair<PendingTool, std::pair<bool, std::string>>>;
                    std::vector<Fut> futs;
                    for (auto& t : reads)
                        futs.push_back(std::async(std::launch::async, [&, t] {
                            return std::make_pair(t, run_one(t));
                        }));
                    std::vector<bool> done(futs.size(), false);
                    std::size_t remaining = futs.size();
                    while (remaining > 0) {
                        bool progressed = false;
                        for (std::size_t i = 0; i < futs.size(); i++) {
                            if (done[i]) continue;
                            if (futs[i].wait_for(std::chrono::milliseconds(1)) !=
                                std::future_status::ready)
                                continue;
                            done[i] = true;
                            remaining--;
                            progressed = true;
                            auto [tool, res] = futs[i].get();
                            if (abort_flag_->load()) {
                                abort_pty_process_groups();
                                return TurnOutcome::aborted();
                            }
                            if (!res.first) {
                                if (!error) error = res.second;
                            } else {
                                check_off_tool(tool, res.second);
                            }
                        }
                        if (!progressed && remaining > 0) {
                            if (abort_flag_->load()) {
                                abort_pty_process_groups();
                                return TurnOutcome::aborted();
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    }
                }
                // (d) sequential writes
                for (auto& t : writes) {
                    if (abort_flag_->load()) {
                        abort_pty_process_groups();
                        return TurnOutcome::aborted();
                    }
                    if (error) break;
                    auto res = run_one(t);
                    if (!res.first) {
                        error = res.second;
                    } else {
                        check_off_tool(t, res.second);
                    }
                    if (abort_flag_->load()) {
                        abort_pty_process_groups();
                        return TurnOutcome::aborted();
                    }
                }
                if (error) return TurnOutcome::tool_error(*error);
                phase = TurnPhase::CheckFinish;
                break;
            }
            case TurnPhase::CheckFinish:
                return plan_.is_complete() ? TurnOutcome::complete() : TurnOutcome::cont();
        }
    }
}

} // namespace marmel::agent
