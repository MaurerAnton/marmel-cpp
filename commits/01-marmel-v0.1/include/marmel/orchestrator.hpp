#pragma once
// Orchestrator: Manager, delegation, plan lifecycle driver, steer
// arbitrator, crash journal (Deep-Freeze).
// Rust origin: src/orchestrator/mod.rs (1393) + steer.rs (588) +
//              freeze.rs (286).
//
// delegate(req) order (preserved exactly):
//   1. resolve agent (unknown => error)
//   2. depth gate UNCONDITIONAL (ignores recursion_granted; precedes Started)
//   3. push Started + journal.snapshot (IO failure => warn + "")
//   4. isolated context + active-worker guard + worker.run(ctx)
//   5. journal.clear + push Completed + apply_check_off (double-gated:
//      explicit-or-marker task id AND marker Complete AND body still holds
//      MISSION COMPLETE — defeats validator REVOKED replays).
// recover_frozen() re-runs the preserved sub-request on a new manager.
// steer fallback (§5.3): Some(decision) => Decided; None+active => Queue;
// None+idle => SteerImmediately. Streaming extractor unescapes the `response`
// JSON string field live (fences + {…} slicing at the end).
// Journal layout: .marmel/.session_frozen.json (single snapshot, pretty) +
// .marmel/.session_journal.json (JSONL append of Frozen/Resolved/Failed).

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "marmel/agent_phase.hpp"
#include "marmel/agents.hpp"
#include "marmel/config.hpp"
#include "marmel/json.hpp"

namespace marmel::harness {
struct HarnessStats;
}
namespace marmel::llm {
class ChatClient;
}

namespace marmel::orchestrator {

inline constexpr std::size_t kDefaultMaxRecursionDepth = 3;
inline constexpr std::size_t kMaxExecutingRounds = 100;
inline constexpr char kFrozenStateFile[] = ".session_frozen.json";
inline constexpr char kCrashJournalFile[] = ".session_journal.json";

struct RecursionDepth {
    std::size_t value = 0;
    static RecursionDepth root() { return {}; }
    std::optional<RecursionDepth> step(std::size_t max) const {
        if (value >= max) return std::nullopt;
        return RecursionDepth{value + 1};
    }
};

struct DelegationEvent {
    enum class Kind { Started, Completed };
    Kind kind = Kind::Started;
    agents::Agent agent = agents::Agent::Generalist;
    std::optional<std::string> task;
};

// --- Active workers + status channel (UI visibility) ------------------------

struct ActiveWorkerInfo {
    std::optional<std::string> task_id;
    std::string agent_name;
    std::string prompt;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
};

/// RAII guard: unregisters the worker on destruction.
class ActiveWorkerGuard {
public:
    ActiveWorkerGuard() = default;
    explicit ActiveWorkerGuard(std::string key) : key_(std::move(key)), armed_(!key_.empty()) {}
    ~ActiveWorkerGuard();
    ActiveWorkerGuard(const ActiveWorkerGuard&) = delete;
    ActiveWorkerGuard& operator=(const ActiveWorkerGuard&) = delete;
    ActiveWorkerGuard(ActiveWorkerGuard&& o) noexcept;
    ActiveWorkerGuard& operator=(ActiveWorkerGuard&& o) noexcept;

private:
    std::string key_;
    bool armed_ = false;
};

using StatusCallback = std::function<void(const std::string&)>;
void set_status_callback(StatusCallback cb);
void emit_status(const std::string& msg);
std::vector<std::string> drain_status_queue();
ActiveWorkerGuard register_active_worker(std::optional<std::string> task_id, std::string agent_name,
                                         std::string prompt);
bool has_active_workers();
std::string get_active_subtasks_str();
std::map<std::string, ActiveWorkerInfo> snapshot_active_workers();

std::string generate_plan_progress_summary(const std::string& plan_content);
std::string brief_for_task(const agent::Plan& plan, const std::string& task_id);

// --- Crash journal -----------------------------------------------------------

struct FreezeSnapshot {
    std::string worker_id;
    agents::Agent agent = agents::Agent::Generalist;
    agents::DelegationRequest sub_req;
    Json to_json() const;
    static FreezeSnapshot from_json(const Json& v);
};

struct JournalEvent {
    std::string ts;
    std::string kind; // "frozen"|"resolved"|"failed"
    std::string worker_id;
    agents::Agent agent = agents::Agent::Generalist;
    std::optional<std::string> task_id;
};

class CrashJournal {
public:
    explicit CrashJournal(std::string dir = agent::kMarmelDir);
    const std::string& dir() const { return dir_; }
    std::string frozen_path() const;
    std::string journal_path() const;
    std::string snapshot(agents::Agent agent, const agents::DelegationRequest& req);
    std::optional<FreezeSnapshot> frozen() const;
    void clear(const std::string& worker_id, bool resolved) const;
    std::vector<JournalEvent> journal() const;
    bool is_frozen() const;

private:
    std::string dir_;
};

// --- Steer arbitrator ---------------------------------------------------------

struct SteerSubtaskDecision {
    std::string tool_call_id;
    std::string action; // ForwardNotice|Cancel|DelegateTask
    std::optional<std::string> message;
    std::optional<std::string> agent_name;
    std::optional<std::string> prompt;
};

struct SteerDecision {
    std::string decision; // RespondDirectly|AbortImmediately|QueueAndContinue|
                          // ForwardToWorker|ApprovePlan|RejectPlan|DelegateTask|
                          // SwitchTier|SwitchModel
    std::optional<std::string> response;
    std::optional<std::string> tier;
    std::optional<std::string> model;
    std::vector<SteerSubtaskDecision> subtasks;
    static std::optional<SteerDecision> from_json(const Json& v);
};

struct SteerOutcome {
    enum class Kind { Decided, QueueInstruction, SteerImmediately };
    Kind kind = Kind::SteerImmediately;
    std::optional<SteerDecision> decision;
    static SteerOutcome decided(SteerDecision d) { return {Kind::Decided, std::move(d)}; }
    static SteerOutcome queue() { return {Kind::QueueInstruction, std::nullopt}; }
    static SteerOutcome immediate() { return {Kind::SteerImmediately, std::nullopt}; }
};

struct SteerContext {
    std::string main_goal;
    std::string orchestrator_status;
    std::string pending_approval = "None";
    std::string plan_progress;
    std::string plan_content;
    std::string available_agents;
    std::string steering_history = "None";
    std::string user_message;
    std::string active_subtasks;
};

/// Live-streaming `response`-field extractor (unescapes JSON string escapes,
///
/// closes on the first unescaped quote; strips ```json fences / {…} slice).
class StreamingResponseExtractor {
public:
    StreamingResponseExtractor() = default;
    /// Returns {emitted_text, finished}.
    std::pair<std::string, bool> push_chunk(const std::string& chunk);
    bool finished() const { return finished_; }

private:
    bool process_char(char c, std::string& out);
    std::string buffer_;
    bool in_response_field_ = false;
    bool finished_ = false;
    bool escaping_ = false;
};

SteerOutcome resolve_steer_outcome(std::optional<SteerDecision> decision, bool has_active);

std::string build_steer_prompt(const SteerContext& ctx);
std::optional<SteerDecision> parse_steer_json(const std::string& text);
std::optional<SteerDecision> arbitrate_steer_context_stream(
    const llm::ChatClient& client, std::shared_ptr<harness::HarnessStats> stats,
    const SteerContext& ctx, const std::function<void(const std::string&)>& on_delta);
std::optional<SteerDecision> arbitrate_steer(const llm::ChatClient& client,
                                            std::shared_ptr<harness::HarnessStats> stats,
                                            const std::string& main_goal,
                                            const std::string& plan_content,
                                            const std::string& active_subtask,
                                            const std::string& user_message);
SteerOutcome arbitrate_steer_stream_with_fallback(
    const llm::ChatClient& client, std::shared_ptr<harness::HarnessStats> stats,
    const SteerContext& ctx, bool has_active,
    const std::function<void(const std::string&)>& on_delta = {});

/// Null-client variant used when no backend is configured: falls back per §5.3.
SteerOutcome arbitrate_steer_with_fallback(const std::string& user_message, bool has_active);

// --- Manager ------------------------------------------------------------------

struct OrchestrationConfigView {
    std::size_t max_recursion_depth = kDefaultMaxRecursionDepth;
    std::string manager_module;
};

class OrchestratorManager {
public:
    OrchestratorManager(std::shared_ptr<llm::ChatClient> client, agent::Plan plan,
                        std::shared_ptr<harness::HarnessStats> stats);
    static OrchestratorManager from_config(std::shared_ptr<llm::ChatClient> client, agent::Plan plan,
                                           std::shared_ptr<harness::HarnessStats> stats,
                                           const config::Config& cfg);

    void guard_no_domain_work() const;
    void create_plan(const std::string& markdown) const;
    agents::Deliverable delegate(const agents::DelegationRequest& req);
    std::optional<agents::Deliverable> recover_frozen();
    std::vector<agents::Deliverable> run_executing(
        const std::function<agents::Agent(const std::string&)>& scheduler);
    static std::string synthesize(const std::vector<agents::Deliverable>& results);

    const std::vector<DelegationEvent>& delegation_events() const { return delegation_events_; }
    void clear_delegation_events() { delegation_events_.clear(); }
    const config::Config& config() const { return cfg_; }
    const agent::Plan& plan() const { return plan_; }
    // Test hooks (mirror Rust pub(crate) field access in tests).
    void set_recursion_depth_for_tests(std::size_t v) { depth_.value = v; }
    void set_max_recursion_depth_for_tests(std::size_t v) {
        orchestration_.max_recursion_depth = v;
    }

private:
    std::shared_ptr<llm::ChatClient> client_;
    agent::Plan plan_;
    agents::SpecialistRegistry registry_;
    OrchestrationConfigView orchestration_;
    std::shared_ptr<harness::HarnessStats> stats_;
    RecursionDepth depth_;
    CrashJournal journal_;
    std::vector<DelegationEvent> delegation_events_;
    config::Config cfg_;
};

/// Tool-entrypoint behind `delegate_task` (used by the harness dispatcher).
/// Mirrors Rust `Result<ToolResult, ToolError>`: hard=true is the Err path
/// (unknown agent/blank prompt/execution failure → transcript "ERROR: "),
/// hard=false with ok=false is Ok(err) (FAILED/REPLAN markers, re-delegation
/// guard).
struct DelegateOutcome {
    bool ok = false;
    bool hard = false;
    std::string content;
};
DelegateOutcome handle_delegate_task(const Json& args);

} // namespace marmel::orchestrator
