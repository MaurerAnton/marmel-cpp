// Rust origin: src/orchestrator/{mod,registry(already in agents.cpp),
//              steer,freeze}.rs (+ ManagerLoop from src/agent/loop.rs)
#include "marmel/orchestrator.hpp"

#include "marmel/agent_loop.hpp"
#include "marmel/harness.hpp"
#include "marmel/llm.hpp"
#include "marmel/prompts.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>

namespace marmel::orchestrator {
namespace {

namespace fs = std::filesystem;

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim_copy(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}
std::string utc_now_rfc3339() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// --- active workers + status ------------------------------------------------------
std::mutex& workers_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, ActiveWorkerInfo>& workers_map() {
    static std::map<std::string, ActiveWorkerInfo> m;
    return m;
}
std::atomic<unsigned long long>& worker_counter() {
    static std::atomic<unsigned long long> c{0};
    return c;
}
StatusCallback& status_cb() {
    static StatusCallback cb;
    return cb;
}
std::mutex& status_queue_mutex() {
    static std::mutex m;
    return m;
}
std::vector<std::string>& status_queue() {
    static std::vector<std::string> q;
    return q;
}

} // namespace

void set_status_callback(StatusCallback cb) { status_cb() = std::move(cb); }
void emit_status(const std::string& msg) {
    {
        std::lock_guard<std::mutex> l(status_queue_mutex());
        status_queue().push_back(msg);
    }
    if (status_cb()) status_cb()(msg);
}
std::vector<std::string> drain_status_queue() {
    std::lock_guard<std::mutex> l(status_queue_mutex());
    std::vector<std::string> q;
    q.swap(status_queue());
    return q;
}

ActiveWorkerGuard::ActiveWorkerGuard(ActiveWorkerGuard&& o) noexcept
    : key_(std::move(o.key_)), armed_(o.armed_) {
    o.armed_ = false;
}
ActiveWorkerGuard& ActiveWorkerGuard::operator=(ActiveWorkerGuard&& o) noexcept {
    if (this != &o) {
        if (armed_) {
            std::lock_guard<std::mutex> l(workers_mutex());
            workers_map().erase(key_);
        }
        key_ = std::move(o.key_);
        armed_ = o.armed_;
        o.armed_ = false;
    }
    return *this;
}
ActiveWorkerGuard::~ActiveWorkerGuard() {
    if (armed_) {
        std::lock_guard<std::mutex> l(workers_mutex());
        workers_map().erase(key_);
    }
}

ActiveWorkerGuard register_active_worker(std::optional<std::string> task_id, std::string agent_name,
                                         std::string prompt) {
    std::string key = task_id && !task_id->empty()
                          ? *task_id
                          : agent_name + "-" + std::to_string(++worker_counter());
    ActiveWorkerInfo info{std::move(task_id), std::move(agent_name), std::move(prompt),
                          std::chrono::steady_clock::now()};
    std::lock_guard<std::mutex> l(workers_mutex());
    workers_map()[key] = std::move(info);
    return ActiveWorkerGuard(key);
}
bool has_active_workers() {
    std::lock_guard<std::mutex> l(workers_mutex());
    return !workers_map().empty();
}
std::string get_active_subtasks_str() {
    std::lock_guard<std::mutex> l(workers_mutex());
    auto& m = workers_map();
    if (m.empty()) return "No active subtasks.";
    std::string out;
    for (auto& [k, v] : m) {
        if (!out.empty()) out += "\n";
        out += "- " + v.agent_name;
        if (v.task_id) out += " on " + *v.task_id;
        out += ": " + v.prompt.substr(0, 120);
    }
    return out;
}
std::map<std::string, ActiveWorkerInfo> snapshot_active_workers() {
    std::lock_guard<std::mutex> l(workers_mutex());
    return workers_map();
}

std::string generate_plan_progress_summary(const std::string& plan_content) {
    std::string t = trim_copy(plan_content);
    if (t.empty() || t == "None") return "No active execution plan on disk.";
    std::vector<std::string> completed, in_progress, pending;
    std::regex re_id(R"(\b(t-[a-zA-Z0-9_\-]+)\b)");
    std::regex re_done(R"(\[[xX]\])");
    std::regex re_pending(R"(\[\s*\]|\(\s*\))");
    auto active = snapshot_active_workers();
    std::istringstream in(plan_content);
    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim_copy(line);
        if (trimmed.empty()) continue;
        bool is_done = std::regex_search(trimmed, re_done);
        std::smatch idm;
        bool has_id = std::regex_search(trimmed, idm, re_id);
        bool is_pending = std::regex_search(trimmed, re_pending) ||
                          (!is_done && has_id && trimmed[0] != '#');
        auto clean = [](std::string s) {
            std::size_t i = 0;
            while (i < s.size() && (s[i] == '-' || s[i] == '*' || s[i] == '+' || s[i] == '>' ||
                                    s[i] == '|' || s[i] == ' ' || s[i] == '.' ||
                                    std::isdigit(static_cast<unsigned char>(s[i]))))
                i++;
            return trim_copy(s.substr(i));
        };
        if (is_done) {
            completed.push_back(clean(trimmed));
        } else if (is_pending) {
            std::string cl = clean(trimmed);
            const ActiveWorkerInfo* match = nullptr;
            if (has_id) {
                std::string tid = to_lower(idm[1].str());
                for (auto& [k, v] : active) {
                    std::string ktid = v.task_id ? to_lower(*v.task_id) : "";
                    if (ktid == tid || to_lower(k).find(tid) != std::string::npos ||
                        to_lower(v.prompt).find(tid) != std::string::npos) {
                        match = &v;
                        break;
                    }
                }
            }
            if (match) {
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - match->started_at)
                                .count();
                in_progress.push_back(cl + " (Assigned to: " + match->agent_name +
                                      ", Running: " + std::to_string(secs) + "s)");
            } else {
                pending.push_back(cl);
            }
        }
    }
    std::size_t total = completed.size() + in_progress.size() + pending.size();
    if (total == 0) return "Execution plan contains no checklist items ([ ] or [x]).";
    unsigned pct =
        static_cast<unsigned>(std::llround(completed.size() * 100.0 / static_cast<double>(total)));
    std::string summary = "Overall Progress: " + std::to_string(completed.size()) + "/" +
                          std::to_string(total) + " tasks completed (" + std::to_string(pct) +
                          "%)\n\n";
    if (!completed.empty()) {
        summary += "### Completed Steps (" + std::to_string(completed.size()) + "/" +
                   std::to_string(total) + "):\n";
        for (auto& s : completed) summary += "- " + s + "\n";
        summary += "\n";
    }
    if (!in_progress.empty()) {
        summary += "### Currently In Progress (" + std::to_string(in_progress.size()) + "/" +
                   std::to_string(total) + "):\n";
        for (auto& s : in_progress) summary += "- " + s + "\n";
        summary += "\n";
    }
    if (!pending.empty()) {
        summary += "### Pending Steps (" + std::to_string(pending.size()) + "/" +
                   std::to_string(total) + "):\n";
        for (auto& s : pending) summary += "- " + s + "\n";
    }
    return trim_copy(summary);
}

std::string brief_for_task(const agent::Plan& plan, const std::string& task_id) {
    if (auto content = plan.read()) {
        std::regex re(R"(^\s*-\s*\[\s*[ xX]?\s*\]\s*\[(t-[A-Za-z0-9_-]+)\]\s*(.*)$)");
        std::istringstream in(*content);
        std::string line;
        while (std::getline(in, line)) {
            std::smatch m;
            if (std::regex_match(line, m, re) && m[1].str() == task_id) {
                std::string desc = trim_copy(m[2].str());
                if (!desc.empty())
                    return desc + "\n\nExecute this delegated task to completion and return your "
                                 "deliverable, ending with MISSION COMPLETE (" +
                           task_id + ").";
            }
        }
    }
    return "Execute the delegated task described by the plan line, producing the deliverable and "
           "ending with MISSION COMPLETE (task-id).";
}

// --- Crash journal ---------------------------------------------------------------------
CrashJournal::CrashJournal(std::string dir) : dir_(std::move(dir)) {}
std::string CrashJournal::frozen_path() const { return dir_ + "/" + kFrozenStateFile; }
std::string CrashJournal::journal_path() const { return dir_ + "/" + kCrashJournalFile; }

Json FreezeSnapshot::to_json() const {
    Json::Object sub;
    sub.emplace("agent_name", Json(agents::agent_to_string(agent)));
    sub.emplace("prompt", Json(sub_req.prompt));
    Json::Array sn;
    for (auto& s : sub_req.snippets) sn.push_back(Json(s));
    sub.emplace("snippets", Json(std::move(sn)));
    if (sub_req.task_id) sub.emplace("task_id", Json(*sub_req.task_id));
    if (sub_req.image_urls) {
        Json::Array a;
        for (auto& s : *sub_req.image_urls) a.push_back(Json(s));
        sub.emplace("image_urls", Json(std::move(a)));
    }
    if (sub_req.audio_urls) {
        Json::Array a;
        for (auto& s : *sub_req.audio_urls) a.push_back(Json(s));
        sub.emplace("audio_urls", Json(std::move(a)));
    }
    sub.emplace("recursion_granted", Json(sub_req.recursion_granted));
    Json::Object o;
    o.emplace("worker_id", Json(worker_id));
    o.emplace("agent_name", Json(agents::agent_to_string(agent)));
    o.emplace("sub_req", Json(std::move(sub)));
    return Json(std::move(o));
}
FreezeSnapshot FreezeSnapshot::from_json(const Json& v) {
    FreezeSnapshot s;
    s.worker_id = v.str_or("worker_id", "");
    s.agent = agents::agent_from_str(v.str_or("agent_name", "")).value_or(agents::Agent::Generalist);
    if (v.contains("sub_req") && v.at("sub_req").is_object()) {
        const Json& q = v.at("sub_req");
        agents::DelegationRequest r;
        r.agent = agents::agent_from_str(q.str_or("agent_name", "")).value_or(s.agent);
        r.prompt = q.str_or("prompt", "");
        if (q.contains("snippets") && q.at("snippets").is_array())
            for (auto& x : q.at("snippets").as_array())
                if (x.is_string()) r.snippets.push_back(x.as_string());
        if (q.contains("task_id") && q.at("task_id").is_string()) r.task_id = q.at("task_id").as_string();
        r.recursion_granted = q.value("recursion_granted", Json(false)).as_bool(false);
        s.sub_req = std::move(r);
        s.agent = s.sub_req.agent;
    }
    return s;
}

std::string CrashJournal::snapshot(agents::Agent agent, const agents::DelegationRequest& req) {
    std::string worker_id = types::generate_uuid_v4();
    FreezeSnapshot snap{worker_id, agent, req};
    std::error_code ec;
    fs::create_directories(dir_, ec);
    {
        std::ofstream f(frozen_path(), std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write frozen state: " + frozen_path());
        f << snap.to_json().dump();
    }
    {
        std::ofstream j(journal_path(), std::ios::binary | std::ios::app);
        if (j) {
            Json::Object e;
            e.emplace("ts", Json(utc_now_rfc3339()));
            e.emplace("kind", Json("frozen"));
            e.emplace("worker_id", Json(worker_id));
            e.emplace("agent", Json(agents::agent_to_string(agent)));
            if (req.task_id) e.emplace("task_id", Json(*req.task_id));
            j << Json(std::move(e)).dump() << "\n";
        }
    }
    return worker_id;
}
std::optional<FreezeSnapshot> CrashJournal::frozen() const {
    std::ifstream f(frozen_path(), std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string t = trim_copy(ss.str());
    if (t.empty()) return std::nullopt;
    try {
        return FreezeSnapshot::from_json(Json::parse(t));
    } catch (...) {
        return std::nullopt;
    }
}
void CrashJournal::clear(const std::string& worker_id, bool resolved) const {
    // Only remove the frozen file when the ids match (never stomp a foreign freeze).
    bool match = false;
    if (auto snap = frozen()) match = (snap->worker_id == worker_id);
    if (match) {
        std::error_code ec;
        fs::remove(frozen_path(), ec);
    }
    std::ofstream j(journal_path(), std::ios::binary | std::ios::app);
    if (j) {
        Json::Object e;
        e.emplace("ts", Json(utc_now_rfc3339()));
        e.emplace("kind", Json(resolved ? "resolved" : "failed"));
        e.emplace("worker_id", Json(worker_id));
        e.emplace("agent", Json("coder")); // foreign-freeze case logs Coder/None (as in Rust)
        j << Json(std::move(e)).dump() << "\n";
    }
}
std::vector<JournalEvent> CrashJournal::journal() const {
    std::vector<JournalEvent> out;
    std::ifstream f(journal_path(), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (trim_copy(line).empty()) continue;
        try {
            Json v = Json::parse(line);
            JournalEvent e;
            e.ts = v.str_or("ts", "");
            e.kind = v.str_or("kind", "");
            e.worker_id = v.str_or("worker_id", "");
            e.agent =
                agents::agent_from_str(v.str_or("agent", "")).value_or(agents::Agent::Generalist);
            if (v.contains("task_id") && v.at("task_id").is_string())
                e.task_id = v.at("task_id").as_string();
            out.push_back(std::move(e));
        } catch (...) {
        }
    }
    return out;
}
bool CrashJournal::is_frozen() const { return frozen().has_value(); }

// --- Steer arbitrator -----------------------------------------------------------------------
std::string build_steer_prompt(const SteerContext& ctx) {
    std::string agents = ctx.available_agents.empty() ? "coder, debugger, researcher, generalist"
                                                      : ctx.available_agents;
    return "Main Goal:\n" + ctx.main_goal + "\n\nOrchestrator Status:\n" + ctx.orchestrator_status +
           "\n\nPending Approval:\n" + ctx.pending_approval + "\n\nPlan Progress:\n" +
           ctx.plan_progress + "\n\nFull Plan:\n" + ctx.plan_content +
           "\n\nAvailable Agents:\n" + agents + "\n\nSteering History:\n" + ctx.steering_history +
           "\n\nNew User Instruction:\n" + ctx.user_message + "\n\nActive Subtasks:\n" +
           ctx.active_subtasks;
}

std::optional<SteerDecision> parse_steer_json(const std::string& text) {
    std::string t = trim_copy(text);
    // Strip ```json fences.
    auto fence = t.find("```");
    if (fence != std::string::npos) {
        auto nl = t.find('\n', fence);
        std::string inner = nl == std::string::npos ? "" : t.substr(nl + 1);
        auto end = inner.rfind("```");
        if (end != std::string::npos) inner = inner.substr(0, end);
        t = trim_copy(inner);
    }
    if (t.empty() || t.front() != '{') {
        // Slice the first {...} block.
        auto b = t.find('{');
        auto e = t.rfind('}');
        if (b == std::string::npos || e == std::string::npos || e <= b) return std::nullopt;
        t = t.substr(b, e - b + 1);
    }
    Json v;
    try {
        v = Json::parse(t);
    } catch (...) {
        return std::nullopt;
    }
    if (!v.is_object()) return std::nullopt;
    return SteerDecision::from_json(v);
}

std::optional<SteerDecision> SteerDecision::from_json(const Json& v) {
    if (!v.is_object() || !v.contains("decision") || !v.at("decision").is_string())
        return std::nullopt;
    SteerDecision d;
    d.decision = v.at("decision").as_string();
    auto opt = [&](const char* k) -> std::optional<std::string> {
        if (v.contains(k) && v.at(k).is_string()) return v.at(k).as_string();
        return std::nullopt;
    };
    d.response = opt("response");
    d.tier = opt("tier");
    d.model = opt("model");
    if (v.contains("subtasks") && v.at("subtasks").is_array()) {
        for (auto& s : v.at("subtasks").as_array()) {
            if (!s.is_object()) continue;
            SteerSubtaskDecision sub;
            sub.tool_call_id = s.str_or("tool_call_id", "");
            sub.action = s.str_or("action", "");
            if (s.contains("message") && s.at("message").is_string())
                sub.message = s.at("message").as_string();
            if (s.contains("agent_name") && s.at("agent_name").is_string())
                sub.agent_name = s.at("agent_name").as_string();
            if (s.contains("prompt") && s.at("prompt").is_string())
                sub.prompt = s.at("prompt").as_string();
            d.subtasks.push_back(std::move(sub));
        }
    }
    return d;
}

std::pair<std::string, bool> StreamingResponseExtractor::push_chunk(const std::string& chunk) {
    std::string emitted;
    for (char c : chunk) {
        if (finished_) break;
        if (process_char(c, emitted)) break;
    }
    return {emitted, finished_};
}
bool StreamingResponseExtractor::process_char(char c, std::string& out) {
    if (finished_) return true;
    buffer_ += c;
    // Detect the "response" field opener: "response" followed by optional ws, ':', optional ws, '"'.
    if (!in_response_field_) {
        static const std::string kNeedle = "\"response\"";
        if (buffer_.size() >= kNeedle.size() &&
            buffer_.compare(buffer_.size() - kNeedle.size(), kNeedle.size(), kNeedle) == 0) {
            // Wait for the opening quote; scan handled below via state re-check.
        }
        auto pos = buffer_.find(kNeedle);
        if (pos != std::string::npos) {
            std::size_t i = pos + kNeedle.size();
            while (i < buffer_.size() && std::isspace(static_cast<unsigned char>(buffer_[i]))) i++;
            if (i < buffer_.size() && buffer_[i] == ':') {
                i++;
                while (i < buffer_.size() && std::isspace(static_cast<unsigned char>(buffer_[i])))
                    i++;
                if (i < buffer_.size() && buffer_[i] == '"') {
                    in_response_field_ = true;
                    escaping_ = false;
                    buffer_.clear();
                    return false;
                }
            }
        }
        // Bound the pre-field buffer.
        if (buffer_.size() > 4096) buffer_.erase(0, buffer_.size() - 4096);
        return false;
    }
    // Inside the response string: `buffer_` holds chars since field entry, but we
    // process incrementally — only the new char matters with escape tracking.
    if (escaping_) {
        escaping_ = false;
        switch (c) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': out += 'u'; break; // \uXXXX passed through simplified (documented)
            default: out += c; break;
        }
        return false;
    }
    if (c == '\\') {
        escaping_ = true;
        return false;
    }
    if (c == '"') {
        finished_ = true;
        in_response_field_ = false;
        return true;
    }
    out += c;
    return false;
}

SteerOutcome resolve_steer_outcome(std::optional<SteerDecision> decision, bool has_active) {
    if (decision) return SteerOutcome::decided(std::move(*decision));
    if (has_active) return SteerOutcome::queue();
    return SteerOutcome::immediate();
}

std::optional<SteerDecision> arbitrate_steer_context_stream(
    const llm::ChatClient& client, std::shared_ptr<harness::HarnessStats> stats,
    const SteerContext& ctx, const std::function<void(const std::string&)>& on_delta) {
    types::ChatRequest req;
    req.model = "";
    req.messages = {types::Message::system(std::string(prompts::STEER_ARBITRATOR_PROMPT)),
                    types::Message::user(build_steer_prompt(ctx))};
    req.temperature = 0.0f;
    req.top_p = 0.9f;
    req.stream = true;
    req.enable_thinking = false;
    StreamingResponseExtractor extractor;
    std::string full;
    try {
        llm::StreamedReply reply = client.chat_stream(req, [&](const std::string& delta) {
            auto [emitted, done] = extractor.push_chunk(delta);
            if (!emitted.empty()) {
                full += emitted;
                if (on_delta) on_delta(emitted);
            }
            return !done;
        });
        (void)reply;
    } catch (...) {
        if (stats) stats->record_steer_arbitration();
        return std::nullopt;
    }
    if (stats) stats->record_steer_arbitration();
    if (on_delta && !full.empty()) {
        // Non-streaming compat: surface the whole response when the backend
        // delivered it without incremental deltas (mirrors on_delta(response)).
    }
    return parse_steer_json(full);
}

std::optional<SteerDecision> arbitrate_steer(const llm::ChatClient& client,
                                            std::shared_ptr<harness::HarnessStats> stats,
                                            const std::string& main_goal,
                                            const std::string& plan_content,
                                            const std::string& active_subtask,
                                            const std::string& user_message) {
    SteerContext ctx;
    ctx.main_goal = main_goal;
    ctx.plan_content = plan_content;
    ctx.plan_progress = generate_plan_progress_summary(plan_content);
    ctx.user_message = user_message;
    ctx.active_subtasks = active_subtask;
    return arbitrate_steer_context_stream(client, std::move(stats), ctx, {});
}

SteerOutcome arbitrate_steer_stream_with_fallback(
    const llm::ChatClient& client, std::shared_ptr<harness::HarnessStats> stats,
    const SteerContext& ctx, bool has_active,
    const std::function<void(const std::string&)>& on_delta) {
    auto decision = arbitrate_steer_context_stream(client, std::move(stats), ctx, on_delta);
    return resolve_steer_outcome(std::move(decision), has_active);
}

SteerOutcome arbitrate_steer_with_fallback(const std::string& user_message, bool has_active) {
    (void)user_message;
    return resolve_steer_outcome(std::nullopt, has_active);
}

// --- Manager ---------------------------------------------------------------------------
OrchestratorManager::OrchestratorManager(std::shared_ptr<llm::ChatClient> client, agent::Plan plan,
                                         std::shared_ptr<harness::HarnessStats> stats)
    : client_(std::move(client)),
      plan_(std::move(plan)),
      registry_(agents::SpecialistRegistry::canonical()),
      stats_(std::move(stats)),
      journal_(plan_.dir()) {
    if (!stats_) stats_ = std::make_shared<harness::HarnessStats>();
    if (!client_) client_ = std::make_shared<llm::ChatClient>("", "");
}
OrchestratorManager OrchestratorManager::from_config(std::shared_ptr<llm::ChatClient> client,
                                                    agent::Plan plan,
                                                    std::shared_ptr<harness::HarnessStats> stats,
                                                    const config::Config& cfg) {
    OrchestratorManager m(std::move(client), std::move(plan), std::move(stats));
    m.cfg_ = cfg;
    m.orchestration_.max_recursion_depth = cfg.orchestration.max_recursion_depth;
    m.orchestration_.manager_module = cfg.orchestration.manager_module;
    return m;
}

void OrchestratorManager::guard_no_domain_work() const {
    if (orchestration_.manager_module.find("/agents/") != std::string::npos)
        throw std::runtime_error("manager_module must not perform domain work (agents/ detected)");
}
void OrchestratorManager::create_plan(const std::string& markdown) const { plan_.create(markdown); }

agents::Deliverable OrchestratorManager::delegate(const agents::DelegationRequest& req) {
    auto entry = registry_.resolve(req.agent);
    if (!entry) throw std::runtime_error("unknown specialist: " + agents::agent_to_string(req.agent));
    auto stepped = depth_.step(orchestration_.max_recursion_depth);
    if (!stepped)
        throw std::runtime_error("recursion depth " + std::to_string(depth_.value + 1) +
                                 " exceeds max " +
                                 std::to_string(orchestration_.max_recursion_depth));
    delegation_events_.push_back(
        {DelegationEvent::Kind::Started, req.agent, req.task_id});
    std::string worker_id;
    try {
        worker_id = journal_.snapshot(req.agent, req);
    } catch (const std::exception& e) {
        std::cerr << "[marmel] Deep-Freeze snapshot failed: " << e.what() << "\n";
    }
    agents::IsolatedContext ctx =
        agents::IsolatedContext::from_request(agents::role_prompt_for(req.agent), req);
    auto guard = register_active_worker(req.task_id, agents::agent_to_string(req.agent), req.prompt);
    agents::Deliverable d;
    bool live = false;
    if (client_ && !client_->backend_url().empty() && !cfg_.model.empty()) {
        if (auto maybe = agents::try_run_live(*client_, req.agent, ctx, cfg_, stats_)) {
            d = std::move(*maybe);
            live = true;
        }
    }
    if (!live) {
        auto worker = registry_.worker(req.agent);
        d = worker->run(ctx);
    }
    if (!worker_id.empty()) {
        try {
            journal_.clear(worker_id, true);
        } catch (...) {
        }
    }
    delegation_events_.push_back(
        {DelegationEvent::Kind::Completed, req.agent, req.task_id});
    // Bind task_id & auto check-off (double-gated on the marker).
    std::optional<std::string> tid = d.task_id ? d.task_id : req.task_id;
    if (tid && !tid->empty() && d.marker.is_complete()) {
        plan_.check_plan_on_marker(tid, d.content);
        d.task_id = tid;
    }
    return d;
}

std::optional<agents::Deliverable> OrchestratorManager::recover_frozen() {
    auto snap = journal_.frozen();
    if (!snap) return std::nullopt;
    auto entry = registry_.resolve(snap->agent);
    if (!entry) {
        try {
            journal_.clear(snap->worker_id, false);
        } catch (...) {
        }
        throw std::runtime_error("Deep-Freeze: frozen worker " + snap->worker_id + " (agent " +
                                 agents::agent_to_string(snap->agent) +
                                 ") cannot be rehydrated: role no longer registered");
    }
    agents::IsolatedContext ctx =
        agents::IsolatedContext::from_request(agents::role_prompt_for(entry->agent), snap->sub_req);
    auto worker = registry_.worker(entry->agent);
    agents::Deliverable d = worker->run(ctx);
    try {
        journal_.clear(snap->worker_id, true);
    } catch (...) {
    }
    std::optional<std::string> tid =
        d.task_id ? d.task_id : snap->sub_req.task_id;
    if (tid && !tid->empty() && d.marker.is_complete()) {
        plan_.check_plan_on_marker(tid, d.content);
        d.task_id = tid;
    }
    return d;
}

std::vector<agents::Deliverable> OrchestratorManager::run_executing(
    const std::function<agents::Agent(const std::string&)>& scheduler) {
    guard_no_domain_work();
    std::vector<agents::Deliverable> results;
    for (std::size_t attempt = 0; attempt < kMaxExecutingRounds; attempt++) {
        if (plan_.is_complete()) break;
        auto pending = plan_.pending_tasks();
        if (pending.empty()) break;
        bool progressed = false;
        for (auto& tid : pending) {
            agents::DelegationRequest req;
            req.agent = scheduler(tid);
            req.prompt = brief_for_task(plan_, tid);
            req.task_id = tid;
            results.push_back(delegate(req));
            progressed = true;
        }
        if (!progressed) break;
    }
    return results;
}

std::string OrchestratorManager::synthesize(const std::vector<agents::Deliverable>& results) {
    std::string out;
    for (std::size_t i = 0; i < results.size(); i++) {
        if (i) out += "\n";
        out += results[i].content;
    }
    return out;
}

std::pair<bool, std::string> handle_delegate_task(const Json& args) {
    agents::DelegationRequest req;
    try {
        req = agents::DelegationRequest::from_json(args);
    } catch (const std::exception& e) {
        return {false, "delegate_task: " + std::string(e.what())};
    }
    // Guard (always-on in C++; Rust gates it to non-test builds): reject
    // re-delegation of already checked-off tasks.
    if (req.task_id && !req.task_id->empty()) {
        agent::Plan plan = agent::Plan::default_plan();
        if (auto content = plan.read()) {
            std::string needle = to_lower(*req.task_id);
            std::istringstream in(*content);
            std::string line;
            while (std::getline(in, line)) {
                if (to_lower(line).find(needle) != std::string::npos &&
                    (line.find("[x]") != std::string::npos || line.find("[X]") != std::string::npos))
                    return {false, "Task '" + *req.task_id +
                                        "' is already completed and checked off in the execution "
                                        "plan. Do not re-delegate completed tasks. Proceed with "
                                        "your final report synthesis."};
            }
        }
    }
    auto stats = std::make_shared<harness::HarnessStats>();
    auto client = std::make_shared<llm::ChatClient>("http://127.0.0.1:11434/v1", "marmel-manager");
    OrchestratorManager manager(client, agent::Plan::default_plan(), stats);
    agents::Deliverable d;
    try {
        d = manager.delegate(req);
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
    std::string tid = d.task_id.value_or("unknown");
    switch (d.marker.kind) {
        case agent::MissionMarker::Kind::Complete:
            return {true, d.content + "\n\nMISSION COMPLETE (" + tid + ")"};
        case agent::MissionMarker::Kind::Failed:
            return {false, d.content + "\n\nFAILED: " + d.marker.reason};
        case agent::MissionMarker::Kind::Replan:
            return {false, d.content + "\n\nREPLAN REQUIRED: " + d.marker.reason};
    }
    return {false, d.content};
}

} // namespace marmel::orchestrator

// --- ManagerLoop (src/agent/loop.rs) ---------------------------------------------------------
namespace marmel::agent {

ManagerLoop::ManagerLoop(std::shared_ptr<orchestrator::OrchestratorManager> manager,
                         Scheduler scheduler)
    : manager_(std::move(manager)),
      scheduler_(std::move(scheduler)),
      abort_flag_(std::make_shared<std::atomic<bool>>(false)) {}

void ManagerLoop::signal(Signal s) {
    if (s.kind == Signal::Kind::Abort) abort_flag_->store(true);
    pending_signals_.push_back(std::move(s));
}
void ManagerLoop::track_pty_pid(std::int32_t pid) { active_pty_pids_.push_back(pid); }

bool ManagerLoop::drain_signals() {
    abort_flag_->store(false);
    bool aborted = false;
    for (auto& s : pending_signals_)
        if (s.kind == Signal::Kind::Abort) aborted = true;
    // Steer signals are deferred while Executing (silent dispatcher).
    pending_signals_.clear();
    if (aborted) abort_flag_->store(true);
    return aborted;
}

std::vector<DeliverableLite> ManagerLoop::run_executing() {
    std::vector<DeliverableLite> results;
    for (std::size_t attempt = 0; attempt < orchestrator::kMaxExecutingRounds; attempt++) {
        if (drain_signals()) {
            abort_flag_->store(true);
#ifdef __unix__
            for (auto pid : active_pty_pids_) harness::kill_process_group(pid);
#endif
            return results;
        }
        if (manager_->plan().is_complete()) break;
        auto pending = manager_->plan().pending_tasks();
        if (pending.empty()) break;
        using Fut = std::future<agents::Deliverable>;
        std::vector<Fut> handles;
        for (auto& tid : pending) {
            agents::DelegationRequest req;
            req.agent = scheduler_(tid);
            req.prompt = orchestrator::brief_for_task(manager_->plan(), tid);
            req.task_id = tid;
            handles.push_back(std::async(std::launch::async, [this, req] {
                if (abort_flag_->load())
                    throw std::runtime_error("aborted before delegation");
                return manager_->delegate(req);
            }));
        }
        // LIFO drain (as in Rust).
        std::vector<DeliverableLite> round;
        bool failed = false;
        while (!handles.empty()) {
            if (abort_flag_->load()) {
                for (auto& h : handles) {
                    try {
                        h.wait();
                    } catch (...) {
                    }
                }
#ifdef __unix__
                for (auto pid : active_pty_pids_) harness::kill_process_group(pid);
#endif
                return results;
            }
            Fut h = std::move(handles.back());
            handles.pop_back();
            try {
                agents::Deliverable d = h.get();
                round.push_back({d.content, d.task_id});
            } catch (const std::exception& e) {
                std::cerr << "[marmel] delegation failed: " << e.what() << "\n";
                failed = true;
                break;
            } catch (...) {
#ifdef __unix__
                for (auto pid : active_pty_pids_) harness::kill_process_group(pid);
#endif
                return results;
            }
        }
        if (failed) break;
        for (auto& d : round) results.push_back(std::move(d));
    }
    return results;
}

} // namespace marmel::agent
