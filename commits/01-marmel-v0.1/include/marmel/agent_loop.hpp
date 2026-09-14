#pragma once
// Agent turn state machine + tool dispatch gating.
// Rust origin: src/agent/loop.rs (1310 lines).
//
// Turn walk (REQ-LOOP-001):
//   PrepareTurn -> CallBackend -> StreamResponse -> ProcessResponse ->
//   ExecuteTools -> CheckFinish. run_turn() drives ONE turn; the caller loops
//   until TurnOutcome::Complete.
// v0.1 seam (preserved): CallBackend/StreamResponse/ProcessResponse are
// transport hooks owned by the UI/backend layer. The loop owns TOOL DISPATCH
// + GATING. The C++ port keeps the same seam: push_message(),
// enqueue_tools(), feed_stream_text(), rescue_xml_calls() are called by the
// session layer; run_turn() executes ExecuteTools -> CheckFinish.
// Tool classification (REQ-LOOP-003, correctness not perf):
//   read  = {read_file, grep_search, glob}          (parallel)
//   write = {write_file, replace, run_command,
//            delegate_task}                          (sequential, in order)
// Tools matching neither class are dropped silently (allowlist behavior).
// delegate_task is SEQUENTIAL because it mutates the shared disk plan.
// Non-delegation check-off uses the literal "ok" (not tool output) to avoid
// output_is_success() false-positives on words like "thiserror".
// Safety: MAX_TURNS=100, 600s watchdog, abort checks at top / between reads /
// before+after each write, SIGKILL of tracked PTY process groups (unix).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "marmel/agent_phase.hpp"
#include "marmel/json.hpp"
#include "marmel/types.hpp"

namespace marmel::harness {
struct HarnessStats;
class HarnessMonitor;
} // namespace marmel::harness

namespace marmel::agent {

inline constexpr std::size_t kMaxTurns = 100;
inline constexpr std::uint64_t kTurnWatchdogSecs = 600;

enum class TurnPhase { PrepareTurn, CallBackend, StreamResponse, ProcessResponse, ExecuteTools, CheckFinish };
TurnPhase next_phase(TurnPhase p);
const char* phase_name(TurnPhase p);

struct Signal {
    enum class Kind { Steer, Abort };
    Kind kind = Kind::Steer;
    std::string text; // Steer payload
    static Signal steer(std::string text) { return Signal{Kind::Steer, std::move(text)}; }
    static Signal abort() { return Signal{Kind::Abort, {}}; }
};

struct TurnOutcome {
    enum class Kind { Continue, ToolError, Error, Aborted, Complete };
    Kind kind = Kind::Continue;
    std::string message; // ToolError/Error detail
    static TurnOutcome cont() { return {Kind::Continue, {}}; }
    static TurnOutcome tool_error(std::string m) { return {Kind::ToolError, std::move(m)}; }
    static TurnOutcome error(std::string m) { return {Kind::Error, std::move(m)}; }
    static TurnOutcome aborted() { return {Kind::Aborted, {}}; }
    static TurnOutcome complete() { return {Kind::Complete, {}}; }
};

enum class ToolCallerKind { Manager, Specialist };
struct ToolCaller {
    ToolCallerKind kind = ToolCallerKind::Manager;
    std::string agent; // Specialist role id, e.g. "coder"
    static ToolCaller manager() { return {ToolCallerKind::Manager, {}}; }
    static ToolCaller specialist(std::string agent) { return {ToolCallerKind::Specialist, std::move(agent)}; }
    std::string display() const { return kind == ToolCallerKind::Manager ? "Manager" : agent; }
};

struct PendingTool {
    std::string name;
    Json arguments = Json::object();
    std::optional<std::string> task_id;
};

bool is_read_tool(const std::string& name);
bool is_write_tool(const std::string& name);
std::optional<std::string> extract_task_id(const std::string& name, const Json& args);

/// Queued-tool dispatcher injected by the harness (avoids a module cycle:
/// agent/loop -> harness/mod -> orchestrator -> agents -> agent/*).
using ToolDispatchFn = std::function<std::pair<bool, std::string>(const PendingTool&, const ToolCaller&)>;

class AgentLoop {
public:
    explicit AgentLoop(Plan plan);
    AgentLoop(Plan plan, std::shared_ptr<harness::HarnessStats> stats);
    ~AgentLoop();

    AgentLoop& with_caller(ToolCaller caller) {
        caller_ = std::move(caller);
        return *this;
    }
    void set_dispatcher(ToolDispatchFn fn) { dispatcher_ = std::move(fn); }
    harness::HarnessMonitor& monitor();

    /// Stream-text repetition gate. Returns true when the stream must be cut.
    bool feed_stream_text(const std::string& chunk);
    /// XML-rescue fallback for malformed tool calls (HARN-001).
    std::vector<types::ToolCall> rescue_xml_calls(const std::string& text);

    std::size_t turn() const { return turn_count_; }
    void signal(Signal s);
    TurnOutcome run_turn();
    void track_pty_pid(std::int32_t pid);
    std::shared_ptr<std::atomic<bool>> abort_flag_handle() const { return abort_flag_; }
    void push_message(types::Message msg) { transcript_.push_back(std::move(msg)); }
    const std::vector<types::Message>& transcript() const { return transcript_; }
    void enqueue_tools(const std::vector<Json>& tools); // [{name, arguments}]

private:
    bool drain_signals();
    void abort_pty_process_groups();
    bool check_off_tool(const PendingTool& tool, const std::string& output_content);

    Plan plan_;
    std::size_t turn_count_ = 0;
    std::vector<types::Message> transcript_;
    std::vector<Signal> pending_signals_;
    std::vector<PendingTool> pending_tools_;
    std::vector<std::int32_t> active_pty_pids_;
    std::shared_ptr<std::atomic<bool>> abort_flag_;
    std::unique_ptr<harness::HarnessMonitor> monitor_;
    std::shared_ptr<harness::HarnessStats> stats_;
    bool rescued_this_turn_ = false;
    ToolCaller caller_ = ToolCaller::manager();
    ToolDispatchFn dispatcher_;
};

struct DeliverableLite {
    std::string content;
    std::optional<std::string> task_id;
};

} // namespace marmel::agent

namespace marmel::orchestrator {
class OrchestratorManager;
}

#include "marmel/agents.hpp"

namespace marmel::agent {

/// Silent-Dispatcher parallel loop (src/agent/loop.rs ManagerLoop): one task
/// per DelegationRequest, independent tasks overlap via async workers, LIFO
/// drain, abort checks between joins. Steer signals are deferred; Abort stops.
class ManagerLoop {
public:
    using Scheduler = std::function<agents::Agent(const std::string&)>;
    ManagerLoop(std::shared_ptr<orchestrator::OrchestratorManager> manager, Scheduler scheduler);
    void signal(Signal s);
    void track_pty_pid(std::int32_t pid);
    std::shared_ptr<std::atomic<bool>> abort_flag_handle() const { return abort_flag_; }
    std::vector<DeliverableLite> run_executing();

private:
    bool drain_signals();
    std::shared_ptr<orchestrator::OrchestratorManager> manager_;
    Scheduler scheduler_;
    std::shared_ptr<std::atomic<bool>> abort_flag_;
    std::vector<Signal> pending_signals_;
    std::vector<std::int32_t> active_pty_pids_;
};

} // namespace marmel::agent
