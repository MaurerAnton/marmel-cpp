#pragma once
// Tool harness: dispatcher + policy + truncation (mod.rs), files (fs.rs),
// resilience monitors (monitor.rs), shell + PTYs (pty.rs), search (search.rs),
// workspace owner (workspace.rs).
// Rust origin: src/harness/{mod,fs,monitor,pty,search,workspace}.rs
//              (495+433+1346+765+190+157 lines).
//
// Fidelity notes (author's intent, preserved):
//  - NO path sandboxing in v0.1: only map_path() prefix-rewrite
//    (/home/coder/workspace/... -> $CWD/...). No `..` rejection.
//  - Generous tool-name aliasing for weak local models (normalize_*).
//  - Manager allowlist: delegate_task, create_plan, archive_current_plan,
//    rebirth(err without engine), read_file, grep_search, glob. Everything
//    else => Forbidden{tool, "Manager"}. create_plan is Manager-ONLY
//    (specialists => Forbidden).
//  - MCP overrides built-ins when a server registered the name.
//  - Output truncation at 10000 bytes with plan-exemption
//    (# EXECUTION PLAN / IMPLEMENTATION PLAN bypass) and char-boundary-safe
//    7000/2000 head/tail split. Literal template preserved.
//  - read_file is CHAR-paginated (Unicode scalars), limit=min(limit,8000),
//    with the "[Showing characters s-e of total...]" footer. (Doc header
//    claiming line numbers is stale upstream — behavior wins.)
//  - replace() requires EXACTLY ONE match (0 => not-found err, >1 =>
//    ambiguous err) + atomic tmp+rename write.
//  - Repetition detectors: tool buffer 50 / threshold default 5 (min 2),
//    text buffer 1000 chars / min-pattern 5 (min 1); pagination-only diffs
//    (offset|page, recursive strip) on read_file|grep_search are exempt;
//    consecutive => Block, alternating A-B => Cut. Literal SPEC strings kept.
//  - XML rescue: <tool_call>…</tool_call> (+ legacy <function=> form),
//    synthetic ids call_text_<uuid>, ONE stat per rescue() call.
//  - PTY: openpty + `sh -c "stty -echo; ulimit -f …; {cmd}"`, 300s one-shot
//    timeout (timeout arg IGNORED in v0.1), OSC strip + C0 filter keeping
//    \n\r\t\x1b, process-group SIGKILL; interactive manager with
//    unread-cursor + 30s reaper / 300s idle expiry.
//  - Workspace: .marmel/{execution_plan.md, marmel.log, forced_phase.txt,
//    archive/} + probe-write handshake in ensure_writable().

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "marmel/agent_loop.hpp"
#include "marmel/json.hpp"
#include "marmel/types.hpp"

namespace marmel::agent {
class ContextEngine;
}
namespace marmel::agents {
enum class Agent : int;
}
namespace marmel::config {
struct MonitoringConfig;
}

namespace marmel::harness {

// --- Shared result/error -------------------------------------------------------

struct ToolResult {
    std::string content;
    bool is_error = false;
    static ToolResult ok(std::string c) { return {std::move(c), false}; }
    static ToolResult err(std::string c) { return {std::move(c), true}; }
};

struct ToolError {
    enum class Kind { UnknownTool, BadArguments, Forbidden, Execution };
    Kind kind = Kind::Execution;
    std::string tool;
    std::string detail;
    std::string caller;
    std::string message() const;
    static ToolError unknown(std::string tool);
    static ToolError bad_arguments(std::string tool, std::string detail);
    static ToolError forbidden(std::string tool, std::string caller);
    static ToolError execution(std::string detail);
};

struct ToolInvocation {
    std::string name;
    Json arguments = Json::object();
};

struct HarnessStats {
    std::atomic<std::uint64_t> repetition_breaks{0};
    std::atomic<std::uint64_t> empty_prods{0};
    std::atomic<std::uint64_t> context_compactions{0};
    std::atomic<std::uint64_t> xml_tool_rescues{0};
    std::atomic<std::uint64_t> backend_retries{0};
    std::atomic<std::uint64_t> session_rebirths{0};
    std::atomic<std::uint64_t> steer_arbitrations{0};
    void record_compaction() { context_compactions++; }
    void record_rebirth() { session_rebirths++; }
    void record_repetition_break() { repetition_breaks++; }
    void record_empty_prod() { empty_prods++; }
    void record_xml_rescue() { xml_tool_rescues++; }
    void record_backend_retry() { backend_retries++; }
    void record_steer_arbitration() { steer_arbitrations++; }
};

// --- monitor.rs ------------------------------------------------------------------

inline constexpr std::size_t kToolBufferCapacity = 50;
inline constexpr std::size_t kDefaultRepetitionThreshold = 5;
inline constexpr std::size_t kTextBufferCapacity = 1000;
inline constexpr std::size_t kDefaultMinPatternLen = 5;

struct ToolCallRecord {
    std::string name;
    Json arguments = Json::object();
    static ToolCallRecord from_tool_call(const types::ToolCall& tc);
    bool semantically_eq(const ToolCallRecord& o) const;
    bool same_operation(const ToolCallRecord& o) const;
};

enum class Intervention { None, Block, Cut };
bool semantic_json_eq(const std::string& a, const std::string& b);

class XmlToolRescue {
public:
    XmlToolRescue();
    explicit XmlToolRescue(std::shared_ptr<HarnessStats> stats);
    void set_stats(std::shared_ptr<HarnessStats> stats);
    std::vector<types::ToolCall> rescue(const std::string& text) const;

private:
    std::shared_ptr<HarnessStats> stats_;
};

class ToolRepetitionDetector {
public:
    explicit ToolRepetitionDetector(std::size_t threshold = kDefaultRepetitionThreshold);
    std::size_t len() const { return buffer_.size(); }
    bool empty() const { return buffer_.empty(); }
    Intervention evaluate(const ToolCallRecord& rec);
    void record(ToolCallRecord rec);

private:
    bool detect_consecutive(const ToolCallRecord& rec) const;
    bool detect_cycle(const ToolCallRecord& rec) const;
    std::deque<ToolCallRecord> buffer_;
    std::size_t threshold_;
};

class RepetitionDetector {
public:
    RepetitionDetector(std::size_t threshold = kDefaultRepetitionThreshold,
                       std::size_t min_len = kDefaultMinPatternLen);
    void push(const std::string& text);
    std::size_t len() const { return buffer_.size(); }
    bool empty() const { return buffer_.empty(); }
    bool is_repeating() const;

private:
    std::deque<char> buffer_;
    std::size_t threshold_;
    std::size_t min_len_;
};

std::vector<types::Message> prune_orphan_tool_messages(std::vector<types::Message> messages);

class HarnessMonitor {
public:
    HarnessMonitor();
    explicit HarnessMonitor(std::shared_ptr<HarnessStats> stats);
    static HarnessMonitor new_with_config(std::shared_ptr<HarnessStats> stats,
                                          const config::MonitoringConfig& cfg);
    static HarnessMonitor with_new_stats();
    std::vector<types::ToolCall> rescue_xml(const std::string& text);
    Intervention observe_tool(const std::string& name, const Json& args);
    std::optional<std::string> intervention_error(Intervention iv) const;
    /// Returns true when the live stream must be terminated.
    bool feed_text(const std::string& chunk);
    void reset_text_break();
    std::size_t tool_buffer_len() const;
    std::shared_ptr<HarnessStats> stats() const { return stats_; }
    void attach_stats(std::shared_ptr<HarnessStats> stats);

private:
    std::size_t threshold_ = kDefaultRepetitionThreshold;
    std::size_t min_len_ = kDefaultMinPatternLen;
    XmlToolRescue xml_;
    ToolRepetitionDetector tool_rep_;
    RepetitionDetector text_rep_;
    std::shared_ptr<HarnessStats> stats_;
    bool repetition_fired_ = false;
};

// --- workspace.rs ------------------------------------------------------------------

class Workspace {
public:
    static Workspace at(std::string dir) { return Workspace(std::move(dir)); }
    /// Default ./.marmel + ensure_writable() handshake.
    static Workspace create_default();
    const std::string& root() const { return root_; }
    std::string plan_path() const;
    std::string log_path() const;
    std::string forced_phase_path() const;
    std::string archive_dir() const;
    void ensure_writable() const;

private:
    explicit Workspace(std::string dir) : root_(std::move(dir)) {}
    std::string root_;
};

// --- fs.rs --------------------------------------------------------------------------

std::string map_path(const std::string& path);
ToolResult fs_read_file(const Json& args);
ToolResult fs_replace(const Json& args);
ToolResult fs_write_file(const Json& args);

// --- search.rs -------------------------------------------------------------------------

ToolResult search_grep(const Json& args);
ToolResult search_glob(const Json& args);

// --- pty.rs -------------------------------------------------------------------------------

std::string sanitize_terminal_output(const std::string& raw);
ToolResult pty_run_command(const Json& args);
std::string run_command_pty(const std::string& command, std::chrono::seconds timeout);
ToolResult pty_spawn(const Json& args);
ToolResult pty_write(const Json& args);
ToolResult pty_read(const Json& args);
ToolResult pty_close(const Json& args);
ToolResult pty_list(const Json& args);
void kill_process_group(std::int32_t pid); // unix SIGKILL, ESRCH ignored; no-op elsewhere

// --- mod.rs dispatcher -----------------------------------------------------------------------

inline constexpr std::size_t kMaxToolOutputChars = 10000;
ToolResult apply_tool_output_length_limit(ToolResult res);

struct McpManagerSlot; // fwd; real type in mcp.hpp (kept decoupled here)
void set_mcp_tool_checker(std::function<bool(const std::string&)> has,
                          std::function<ToolResult(const std::string&, const Json&)> call);
void clear_mcp_tool_checker();
void set_mcp_tool_defs(std::vector<types::ToolDef> defs);
std::vector<types::ToolDef> mcp_tool_defs();

/// Legacy/shared table (MCP first, then built-ins), truncated.
ToolResult dispatch(const ToolInvocation& inv);
ToolResult dispatch_manager(const ToolInvocation& inv);
ToolResult dispatch_specialist(agents::Agent agent, const ToolInvocation& inv);
ToolResult dispatch_with_engine(const ToolInvocation& inv, agent::ContextEngine& engine);
ToolResult handle_rebirth(agent::ContextEngine& engine, const Json& args);
/// Policy entry used by agent loops (adds truncation).
ToolResult dispatch_for(const ToolInvocation& inv, const agent::ToolCaller& caller);
/// {name, arguments} convenience form for enqueue_tools() payloads.
ToolResult dispatch_for_json(const std::string& name, const Json& args,
                             const agent::ToolCaller& caller);

std::string normalize_tool_name(const std::string& name);

} // namespace marmel::harness
