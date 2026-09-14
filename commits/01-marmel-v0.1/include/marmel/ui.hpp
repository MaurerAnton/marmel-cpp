#pragma once
// Session loop (renderer-agnostic) + raw headless renderer + interactive TUI.
// Rust origin: src/ui/{mod (1336), raw (139), tui (~2831),
//              tui_click_impl (15)}.h
//
// Architecture (preserved):
//  - `Renderer` trait with init/on_event/flush/poll_input/read_input/
//    request_abort/aborted/clear_abort/shutdown/set_subagents.
//  - run_session(): init -> goal loop (recover_frozen -> outer turn loop).
//    Parallel fan-out ONLY when every pending tool is in
//    {delegate_task, read_file, grep_search, glob} and count>1; else
//    sequential. Both paths pump status/delegation/steer events while
//    waiting. Abort-with-steer redirects instead of quitting.
//  - RendererSink bridges llm StreamSink events to Renderer events and owns
//    steer-arbitration bookkeeping.
//  - reset slash aliases: /reset /reset_plan /reset-plan /clear_plan
//    /clear-plan /reset_execution_plan. Abort aliases: /abort /exit /quit
//    /q :q :q!. /thought toggles reasoning display, /help prints the legend.
//  - Raw labels: [assistant] [steer] [thinking] [tool] [tool-result]
//    [status] [delegation] STARTED/DONE [done]; 512-char UTF-8 chunks.
//  - TUI: Chat 60% + right 40% (Plan 50% / Subagents 50%); Chat/Plan/
//    Subagents focus cycle (Tab), Esc-confirm abort (double Esc / Ctrl+D),
//    Ctrl+P plan toggle, Ctrl+A subagents toggle, grapheme cursor, mouse
//    scroll/click, per-frame plan re-read with auto-open. The C++ port
//    renders via ANSI escapes on a plain terminal (no FTXUI/ncurses
//    dependency) but keeps the identical state machine, keybinding table
//    and slash commands so behavior matches v0.1.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "marmel/llm.hpp"
#include "marmel/orchestrator.hpp"

namespace marmel::config {
struct Config;
}

namespace marmel::ui {

struct SubagentDetail {
    std::string name;
    std::optional<std::string> task_id;
    std::string prompt;
    std::optional<std::chrono::steady_clock::time_point> started_at;
    std::vector<std::string> logs;
    std::string thinking;
    std::string content;
    bool is_active = false;
};

struct UiEvent {
    enum class Kind { Message, SteerResponse, Thinking, ToolCall, ToolResult, Status, Delegation, Done };
    Kind kind = Kind::Status;
    std::string text;
    orchestrator::DelegationEvent delegation = {orchestrator::DelegationEvent::Kind::Started, {},
                                               std::nullopt};
    static UiEvent message(std::string t) { return {Kind::Message, std::move(t), {}}; }
    static UiEvent steer(std::string t) { return {Kind::SteerResponse, std::move(t), {}}; }
    static UiEvent thinking(std::string t) { return {Kind::Thinking, std::move(t), {}}; }
    static UiEvent tool_call(std::string t) { return {Kind::ToolCall, std::move(t), {}}; }
    static UiEvent tool_result(std::string t) { return {Kind::ToolResult, std::move(t), {}}; }
    static UiEvent status(std::string t) { return {Kind::Status, std::move(t), {}}; }
    static UiEvent delegation_ev(orchestrator::DelegationEvent d) {
        return {Kind::Delegation, {}, std::move(d)};
    }
    static UiEvent done() { return {Kind::Done, {}, {}}; }
};

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void init() = 0;
    virtual void on_event(const UiEvent& ev) = 0;
    virtual void flush() = 0;
    /// Non-blocking input poll (nullopt => none).
    virtual std::optional<std::string> poll_input() = 0;
    /// Blocking next line (nullopt => EOF/abort).
    virtual std::optional<std::string> read_input() = 0;
    virtual void request_abort() = 0;
    virtual bool aborted() const = 0;
    virtual void clear_abort() {}
    virtual void shutdown() = 0;
    virtual void set_subagents(const std::vector<SubagentDetail>&) {}
};

std::string format_active_subtasks(const std::vector<SubagentDetail>& subs);
std::string format_plan_progress_summary(const std::string& plan_content);
std::vector<std::string> chunk_utf8(const std::string& s, std::size_t max_bytes);
void restore();

bool is_abort_command(const std::string& line);
bool is_reset_command(const std::string& line);
bool is_thought_command(const std::string& line);
bool is_help_command(const std::string& line);

int run_session(const config::Config& cfg, Renderer& renderer,
                std::optional<std::string> initial,
                std::shared_ptr<orchestrator::OrchestratorManager> manager = {});

/// Test seam: drive turns through an injected chat function instead of the
/// HTTP backend (mirrors wiremock-based tests in tests/test_ui_session.rs).
using ChatFn = std::function<types::Message(std::vector<types::Message>,
                                            const llm::StreamConfig&, llm::StreamSink&)>;
int run_session_with_chat(const config::Config& cfg, Renderer& renderer,
                          std::optional<std::string> initial,
                          std::shared_ptr<orchestrator::OrchestratorManager> manager,
                          ChatFn chat_fn);

// --- raw renderer ---------------------------------------------------------------

class RawRenderer final : public Renderer {
public:
    void init() override {}
    void on_event(const UiEvent& ev) override;
    void flush() override;
    std::optional<std::string> poll_input() override { return std::nullopt; }
    std::optional<std::string> read_input() override { return std::nullopt; }
    void request_abort() override { aborted_ = true; }
    bool aborted() const override { return aborted_; }
    void shutdown() override { flush(); }

private:
    void push_line(const std::string& label, const std::string& text);
    std::string buffer_;
    bool aborted_ = false;
};

int run_raw(const config::Config& cfg, std::optional<std::string> initial,
            std::shared_ptr<orchestrator::OrchestratorManager> manager = {});
void raw_restore();

// --- interactive TUI (ANSI, std-only) --------------------------------------------

class TuiRenderer final : public Renderer {
public:
    TuiRenderer();
    ~TuiRenderer() override;
    void init() override;
    void on_event(const UiEvent& ev) override;
    void flush() override;
    std::optional<std::string> poll_input() override;
    std::optional<std::string> read_input() override;
    void request_abort() override { aborted_ = true; }
    bool aborted() const override { return aborted_; }
    void clear_abort() override {
        aborted_ = false;
        confirm_abort_ = false;
    }
    void shutdown() override;
    void set_subagents(const std::vector<SubagentDetail>& subs) override { subagents_ = subs; }

    // Exposed for tests (mirrors tui.rs helpers).
    void submit_line(const std::string& line); // Enter handler incl. slash commands
    void handle_key(const std::string& key);   // unit-testable key driver
    void click_to_cursor(unsigned long x);
    const std::vector<std::string>& messages() const { return messages_; }
    bool show_thought() const { return show_thought_; }

private:
    enum class FocusedPanel { Chat, Plan, Subagents };
    void draw();
    void draw_noninteractive();
    void ensure_plan_loaded();
    static std::vector<std::string> wrap_lines(const std::string& text, std::size_t width);

    std::vector<std::string> messages_;
    std::string current_thought_;
    std::string current_content_;
    std::string plan_content_ = "No active execution plan.";
    std::string input_text_;
    std::size_t cursor_ = 0; // byte offset
    bool confirm_abort_ = false;
    std::string status_line_ = "Ready";
    FocusedPanel focused_ = FocusedPanel::Chat;
    long chat_scroll_ = 0;
    bool show_plan_ = false;
    bool show_thought_ = false;
    bool show_subagents_ = false;
    std::vector<SubagentDetail> subagents_;
    std::size_t selected_subagent_ = 0;
    std::vector<std::string> history_;
    std::optional<std::size_t> history_index_;
    std::string input_draft_;
    std::string active_agent_ = "Manager";
    bool aborted_ = false;
    bool alt_screen_ = false;
    std::vector<std::string> inbox_;
};

int run_tui(const config::Config& cfg, std::optional<std::string> initial,
            std::shared_ptr<orchestrator::OrchestratorManager> manager = {});
void tui_leave_alt_screen();

} // namespace marmel::ui
