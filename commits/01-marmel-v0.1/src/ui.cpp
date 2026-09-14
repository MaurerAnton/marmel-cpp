// Rust origin: src/ui/{mod,raw,tui,tui_click_impl}.rs
#include "marmel/ui.hpp"

#include "marmel/agent_context.hpp"
#include "marmel/agent_loop.hpp"
#include "marmel/agent_phase.hpp"
#include "marmel/config.hpp"
#include "marmel/harness.hpp"
#include "marmel/llm.hpp"
#include "marmel/prompts.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef __unix__
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace marmel::ui {
namespace {

std::string trim_copy(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}
std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
std::size_t cp_len(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s)
        if (!is_cont(c)) n++;
    return n;
}
std::size_t prev_char(const std::string& s, std::size_t byte) {
    if (byte == 0) return 0;
    std::size_t i = byte - 1;
    while (i > 0 && is_cont(static_cast<unsigned char>(s[i]))) i--;
    return i;
}
std::size_t next_char(const std::string& s, std::size_t byte) {
    if (byte >= s.size()) return s.size();
    std::size_t i = byte + 1;
    while (i < s.size() && is_cont(static_cast<unsigned char>(s[i]))) i++;
    return i;
}

std::string load_system_prompt(const config::Config& cfg) {
    std::string base;
    {
        std::ifstream f(cfg.system_prompt_path, std::ios::binary);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            base = ss.str();
        } else {
            base = std::string(prompts::SYSTEM_PROMPT);
        }
    }
    std::error_code ec;
    std::string cwd = std::filesystem::current_path(ec).string();
    if (ec) cwd = ".";
    return base + "\n\n## Environment & Workspace\nCWD: " + cwd;
}

std::string classify_llm_error(const std::string& e) {
    std::string l = to_lower(e);
    if (l.find("http status") != std::string::npos || l.find("http/") != std::string::npos ||
        l.find("status ") != std::string::npos)
        return "http";
    if (l.find("timeout") != std::string::npos || l.find("timed out") != std::string::npos ||
        l.find("deadline") != std::string::npos)
        return "timeout";
    if (l.find("connect") != std::string::npos || l.find("transport") != std::string::npos ||
        l.find("curl") != std::string::npos || l.find("resolve") != std::string::npos ||
        l.find("network") != std::string::npos)
        return "connectivity";
    if (l.find("stream") != std::string::npos || l.find("sse") != std::string::npos ||
        l.find("event") != std::string::npos)
        return "stream";
    return "unknown";
}

void handle_reset_command(const agent::Plan& plan, Renderer& renderer, agent::ContextEngine& ctx) {
    try {
        plan.clear();
    } catch (...) {
    }
    renderer.on_event(UiEvent::status("execution plan cleared; phase reset to Conversational"));
    (void)ctx;
}

std::string subagent_key(const std::string& agent, const std::optional<std::string>& task) {
    return task && !task->empty() ? agent + "-" + *task : agent;
}

void update_subagent_lifecycle(std::vector<SubagentDetail>& subs, agents::Agent agent,
                               std::optional<std::string> task, std::optional<std::string> prompt,
                               bool started) {
    std::string name = agents::agent_to_string(agent);
    std::string key = subagent_key(name, task);
    for (auto& s : subs) {
        if (subagent_key(s.name, s.task_id) == key) {
            s.is_active = started;
            if (prompt) s.prompt = *prompt;
            if (started) {
                s.started_at = std::chrono::steady_clock::now();
                s.logs.push_back("started task " + task.value_or("(no task id)"));
            } else {
                s.logs.push_back("completed task " + task.value_or("(no task id)"));
            }
            return;
        }
    }
    SubagentDetail d;
    d.name = name;
    d.task_id = task;
    d.prompt = prompt.value_or("");
    d.is_active = started;
    if (started) d.started_at = std::chrono::steady_clock::now();
    d.logs.push_back((started ? "started task " : "completed task ") + task.value_or("(no task id)"));
    subs.push_back(std::move(d));
}

void drain_delegation_events(orchestrator::OrchestratorManager* manager, Renderer& renderer,
                             std::vector<SubagentDetail>& subs) {
    if (!manager) return;
    for (auto& ev : manager->delegation_events()) {
        renderer.on_event(UiEvent::delegation_ev(ev));
        if (ev.kind == orchestrator::DelegationEvent::Kind::Started)
            update_subagent_lifecycle(subs, ev.agent, ev.task, std::nullopt, true);
        else
            update_subagent_lifecycle(subs, ev.agent, ev.task, std::nullopt, false);
    }
    manager->clear_delegation_events();
    renderer.set_subagents(subs);
}

void drain_status_queue(Renderer& renderer) {
    for (auto& m : orchestrator::drain_status_queue()) renderer.on_event(UiEvent::status(m));
}

// Apply a steer-arbitration outcome (mirrors drain_steer_arbitration in Rust).
void apply_steer_outcome(const orchestrator::SteerOutcome& outcome, const std::string& user_msg,
                         Renderer& renderer, agent::ContextEngine& ctx,
                         std::vector<std::string>& steer_queue) {
    using Kind = orchestrator::SteerOutcome::Kind;
    if (outcome.kind == Kind::Decided && outcome.decision) {
        const auto& d = *outcome.decision;
        if (d.decision == "RespondDirectly") {
            renderer.on_event(UiEvent::status(d.response.value_or("(no response)")));
        } else if (d.decision == "AbortImmediately") {
            renderer.request_abort();
            ctx.append(types::Message::user(user_msg));
        } else if (d.decision == "QueueAndContinue" || d.decision == "ForwardToWorker") {
            steer_queue.push_back(user_msg);
            renderer.on_event(UiEvent::status(d.response.value_or("steering queued")));
        } else if (d.decision == "ApprovePlan") {
            ctx.append(types::Message::user("User approved plan."));
        } else if (d.decision == "RejectPlan") {
            renderer.request_abort();
            ctx.append(types::Message::user("User rejected plan: " + d.response.value_or("")));
        } else {
            if (d.response && !d.response->empty())
                ctx.append(types::Message::user(user_msg + "\n" + *d.response));
            else
                ctx.append(types::Message::user(user_msg));
        }
    } else if (outcome.kind == Kind::QueueInstruction) {
        steer_queue.push_back(user_msg);
        renderer.on_event(UiEvent::status("steering queued behind active work"));
    } else {
        ctx.append(types::Message::user(user_msg));
    }
}

orchestrator::SteerOutcome arbitrate_line(const llm::ChatClient& client,
                                          std::shared_ptr<harness::HarnessStats> stats,
                                          const std::string& goal,
                                          const std::string& user_msg) {
    std::string plan_content;
    if (auto c = agent::Plan::default_plan().read()) plan_content = *c;
    orchestrator::SteerContext ctx;
    ctx.main_goal = goal;
    ctx.orchestrator_status = "executing";
    ctx.plan_content = plan_content;
    ctx.plan_progress = orchestrator::generate_plan_progress_summary(plan_content);
    ctx.user_message = user_msg;
    ctx.active_subtasks = orchestrator::get_active_subtasks_str();
    bool has_active = orchestrator::has_active_workers();
    if (client.backend_url().empty())
        return orchestrator::resolve_steer_outcome(std::nullopt, has_active);
    auto decision =
        orchestrator::arbitrate_steer_context_stream(client, stats, ctx, {});
    return orchestrator::resolve_steer_outcome(std::move(decision), has_active);
}

// RendererSink: bridges llm StreamSink events to Renderer events.
struct RendererSink final : public llm::StreamSink {
    Renderer& renderer;
    std::vector<std::string>& steer_queue;
    const llm::ChatClient& client;
    std::shared_ptr<harness::HarnessStats> stats;
    const std::string& goal;
    RendererSink(Renderer& r, std::vector<std::string>& q, const llm::ChatClient& c,
                 std::shared_ptr<harness::HarnessStats> s, const std::string& g)
        : renderer(r), steer_queue(q), client(c), stats(std::move(s)), goal(g) {}

    void emit(const llm::StreamEvent& ev) override {
        using K = llm::StreamEvent::Kind;
        if (ev.kind == K::Content) renderer.on_event(UiEvent::message(ev.text));
        else if (ev.kind == K::Thinking) renderer.on_event(UiEvent::thinking(ev.text));
        else renderer.on_event(UiEvent::status(ev.text));
        renderer.flush();
    }
    bool is_aborted() override {
        renderer.flush();
        drain_status_queue(renderer);
        while (auto input = renderer.poll_input()) {
            if (is_abort_command(*input)) {
                renderer.request_abort();
                return true;
            }
            if (is_reset_command(*input)) continue; // handled at turn boundary
            if (!trim_copy(*input).empty()) steer_queue.push_back(*input);
        }
        return renderer.aborted();
    }
};

} // namespace

std::string format_active_subtasks(const std::vector<SubagentDetail>& subs) {
    std::string global = orchestrator::get_active_subtasks_str();
    if (global != "No active subtasks." && !trim_copy(global).empty()) return global;
    std::string out;
    for (auto& s : subs) {
        if (!s.is_active) continue;
        if (!out.empty()) out += "\n";
        out += "- " + s.name;
        if (s.task_id) out += " on " + *s.task_id;
    }
    return out.empty() ? "No active subtasks." : out;
}
std::string format_plan_progress_summary(const std::string& plan_content) {
    return orchestrator::generate_plan_progress_summary(plan_content);
}

std::vector<std::string> chunk_utf8(const std::string& s, std::size_t max_bytes) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = std::min(i + max_bytes, s.size());
        while (j > i && is_cont(static_cast<unsigned char>(s[j]))) j--;
        if (j == i) j = std::min(i + max_bytes, s.size());
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    if (out.empty()) out.emplace_back("");
    return out;
}

void restore() { tui_leave_alt_screen(); }

bool is_abort_command(const std::string& line) {
    std::string t = to_lower(trim_copy(line));
    return t == "/abort" || t == "/exit" || t == "/quit" || t == "/q" || t == ":q" || t == ":q!";
}
bool is_reset_command(const std::string& line) {
    std::string t = to_lower(trim_copy(line));
    return t == "/reset" || t == "/reset_plan" || t == "/reset-plan" || t == "/clear_plan" ||
           t == "/clear-plan" || t == "/reset_execution_plan";
}
bool is_thought_command(const std::string& line) {
    return to_lower(trim_copy(line)) == "/thought";
}
bool is_help_command(const std::string& line) {
    return to_lower(trim_copy(line)) == "/help";
}

int run_session_with_chat(const config::Config& cfg, Renderer& renderer,
                          std::optional<std::string> initial,
                          std::shared_ptr<orchestrator::OrchestratorManager> manager,
                          ChatFn chat_fn) {
    auto stats = std::make_shared<harness::HarnessStats>();
    llm::ChatClient client = llm::ChatClient::from_config(cfg);
    if (!manager)
        manager = std::make_shared<orchestrator::OrchestratorManager>(
            orchestrator::OrchestratorManager::from_config(
                std::make_shared<llm::ChatClient>(client), agent::Plan::default_plan(), stats, cfg));
    renderer.init();

    agent::ContextEngineFactory factory(cfg.max_context_tokens);
    agent::ContextEngine ctx = factory.manager_context("", "");

    // Goal acquisition.
    std::string goal;
    if (initial && !trim_copy(*initial).empty()) {
        goal = *initial;
    } else {
        while (true) {
            auto line = renderer.read_input();
            if (!line || is_abort_command(*line)) {
                renderer.on_event(UiEvent::done());
                renderer.shutdown();
                return 0;
            }
            if (is_reset_command(*line)) {
                try {
                    agent::Plan::default_plan().clear();
                } catch (...) {
                }
                continue;
            }
            if (trim_copy(*line).empty()) continue;
            goal = *line;
            break;
        }
    }
    ctx.set_system_prompt(load_system_prompt(cfg));
    ctx.set_goal(goal);

    // Deep-Freeze recovery.
    try {
        if (auto rec = manager->recover_frozen()) {
            renderer.on_event(UiEvent::status("[Recovered task " +
                                              rec->task_id.value_or("(no task)") + "]"));
            ctx.append(types::Message::assistant(rec->content, std::nullopt, {}));
        }
    } catch (const std::exception& e) {
        renderer.on_event(UiEvent::status(std::string("recovery failed: ") + e.what()));
    }

    llm::StreamConfig stream_cfg = llm::StreamConfig::from_config(cfg);
    std::vector<std::string> steer_queue;
    std::vector<SubagentDetail> subagents;
    bool keep_going = true;

    while (keep_going && !renderer.aborted()) {
        drain_status_queue(renderer);
        drain_delegation_events(manager.get(), renderer, subagents);

        if (auto steer = renderer.poll_input()) {
            if (is_abort_command(*steer)) {
                renderer.request_abort();
                break;
            }
            if (is_reset_command(*steer)) {
                handle_reset_command(agent::Plan::default_plan(), renderer, ctx);
                continue;
            }
            if (!trim_copy(*steer).empty()) steer_queue.push_back(*steer);
        }
        for (auto& s : steer_queue) ctx.append(types::Message::user(s));
        steer_queue.clear();

        for (unsigned turn = 0; turn < agent::kMaxTurns; turn++) {
            if (renderer.aborted()) break;
            drain_status_queue(renderer);
            drain_delegation_events(manager.get(), renderer, subagents);
            renderer.flush();
            for (auto& s : steer_queue) ctx.append(types::Message::user(s));
            steer_queue.clear();

            renderer.on_event(UiEvent::status("Running (" + stream_cfg.model + ")"));
            renderer.flush();

            RendererSink bridge{renderer, steer_queue, client, stats, goal};
            types::Message assistant = types::Message::assistant(std::nullopt);
            try {
                assistant = chat_fn
                                ? chat_fn(ctx.messages(), stream_cfg, bridge)
                                : llm::chat_client_turn(client, ctx.messages(), stream_cfg, bridge,
                                                        stats);
            } catch (const std::exception& e) {
                std::string cat = classify_llm_error(e.what());
                renderer.on_event(
                    UiEvent::status("LLM error (" + cat + "): " + std::string(e.what())));
                keep_going = false;
                break;
            }
            if (renderer.aborted()) break;
            std::vector<types::ToolCall> tool_calls = assistant.tool_calls;
            ctx.append(assistant);
            renderer.flush();

            if (ctx.should_compact()) {
                ctx.compact();
                renderer.on_event(UiEvent::status("context compacted"));
            }
            for (auto& s : steer_queue) ctx.append(types::Message::user(s));
            steer_queue.clear();

            if (tool_calls.empty()) break;

            bool all_parallel = tool_calls.size() > 1;
            for (auto& c : tool_calls) {
                if (c.name != "delegate_task" && c.name != "read_file" && c.name != "grep_search" &&
                    c.name != "glob") {
                    all_parallel = false;
                    break;
                }
            }
            agent::ToolCaller caller = agent::ToolCaller::manager();
            auto run_one = [&](const types::ToolCall& c) {
                Json args = Json::parse_or_string(c.arguments);
                if (!args.is_object()) args = Json::object();
                // Pre-announce delegations for subagent-pane visibility.
                if (c.name == "delegate_task") {
                    auto ag = agents::agent_from_str(args.str_or("agent_name", ""));
                    std::optional<std::string> tid;
                    if (args.contains("task_id") && args.at("task_id").is_string())
                        tid = args.at("task_id").as_string();
                    std::string pr = args.str_or("prompt", "");
                    if (ag)
                        update_subagent_lifecycle(subagents, *ag, tid, pr, true);
                    renderer.on_event(UiEvent::tool_call(c.name + "(" + c.arguments + ")"));
                } else {
                    renderer.on_event(UiEvent::tool_call(c.name + "(" + c.arguments + ")"));
                }
                harness::ToolResult r =
                    harness::dispatch_for(harness::ToolInvocation{c.name, args}, caller);
                renderer.on_event(UiEvent::tool_result(r.content));
                if (c.name == "delegate_task") {
                    auto ag = agents::agent_from_str(args.str_or("agent_name", ""));
                    std::optional<std::string> tid;
                    if (args.contains("task_id") && args.at("task_id").is_string())
                        tid = args.at("task_id").as_string();
                    if (ag) update_subagent_lifecycle(subagents, *ag, tid, std::nullopt, false);
                }
                renderer.set_subagents(subagents);
                return std::make_pair(c.id, r);
            };
            if (all_parallel) {
                std::vector<std::future<std::pair<std::string, harness::ToolResult>>> handles;
                for (auto& c : tool_calls)
                    handles.push_back(std::async(std::launch::async, run_one, c));
                for (auto& h : handles) {
                    auto [call_id, r] = h.get();
                    ctx.append(types::Message::tool(call_id, r.content));
                }
            } else {
                for (auto& c : tool_calls) {
                    if (renderer.aborted()) break;
                    auto [call_id, r] = run_one(c);
                    ctx.append(types::Message::tool(call_id, r.content));
                    drain_status_queue(renderer);
                    drain_delegation_events(manager.get(), renderer, subagents);
                }
            }
            renderer.flush();
            if (renderer.aborted()) break;
        }
        if (!keep_going) break;

        if (!steer_queue.empty()) {
            // Arbitrate queued steering before blocking for the next goal.
            std::vector<std::string> pending;
            pending.swap(steer_queue);
            for (auto& s : pending) {
                auto outcome = arbitrate_line(client, stats, goal, s);
                apply_steer_outcome(outcome, s, renderer, ctx, steer_queue);
                if (renderer.aborted()) break;
            }
            for (auto& s : steer_queue) ctx.append(types::Message::user(s));
            steer_queue.clear();
            if (!renderer.aborted()) continue;
            break;
        }

        auto line = renderer.read_input();
        if (!line) break;
        if (is_abort_command(*line)) {
            renderer.request_abort();
            break;
        }
        if (is_reset_command(*line)) {
            handle_reset_command(agent::Plan::default_plan(), renderer, ctx);
            continue;
        }
        if (is_thought_command(*line) || is_help_command(*line)) {
            renderer.on_event(UiEvent::status(*line));
            continue;
        }
        if (!trim_copy(*line).empty()) ctx.append(types::Message::user(*line));
    }

    renderer.on_event(UiEvent::done());
    renderer.flush();
    renderer.shutdown();
    return 0;
}

int run_session(const config::Config& cfg, Renderer& renderer, std::optional<std::string> initial,
                std::shared_ptr<orchestrator::OrchestratorManager> manager) {
    return run_session_with_chat(cfg, renderer, std::move(initial), std::move(manager), {});
}

// --- raw ----------------------------------------------------------------------------------
void RawRenderer::push_line(const std::string& label, const std::string& text) {
    for (auto& chunk : chunk_utf8(text, 512)) buffer_ += "[" + label + "] " + chunk + "\n";
}
void RawRenderer::on_event(const UiEvent& ev) {
    using K = UiEvent::Kind;
    switch (ev.kind) {
        case K::Message: push_line("assistant", ev.text); break;
        case K::SteerResponse: push_line("steer", ev.text); break;
        case K::Thinking: push_line("thinking", ev.text); break;
        case K::ToolCall: push_line("tool", ev.text); break;
        case K::ToolResult: push_line("tool-result", ev.text); break;
        case K::Status: push_line("status", ev.text); break;
        case K::Delegation: {
            std::string task = ev.delegation.task.value_or("(no task id)");
            std::string agent = agents::agent_to_string(ev.delegation.agent);
            if (ev.delegation.kind == orchestrator::DelegationEvent::Kind::Started)
                push_line("delegation", "STARTED → " + agent + " on " + task);
            else
                push_line("delegation", "DONE    " + agent + " on " + task);
            break;
        }
        case K::Done: push_line("done", ""); break;
    }
    flush();
}
void RawRenderer::flush() {
    if (buffer_.empty()) return;
    std::cout << buffer_ << std::flush;
    buffer_.clear();
}
int run_raw(const config::Config& cfg, std::optional<std::string> initial,
            std::shared_ptr<orchestrator::OrchestratorManager> manager) {
    RawRenderer r;
    return run_session(cfg, r, std::move(initial), std::move(manager));
}
void raw_restore() {
#ifdef __unix__
    // No-op when not a TTY (mirrors Rust: disable_raw_mode only if tty).
    if (::isatty(STDOUT_FILENO)) {
    }
#endif
}

// --- TUI (ANSI, std-only) ----------------------------------------------------------------------
namespace {
bool stdout_is_tty() {
#ifdef __unix__
    return ::isatty(STDOUT_FILENO) != 0;
#else
    return false;
#endif
}
#ifdef __unix__
struct TermGuard {
    bool active = false;
    struct termios saved {};
    void enable_raw() {
        if (!::isatty(STDIN_FILENO)) return;
        if (::tcgetattr(STDIN_FILENO, &saved) != 0) return;
        struct termios raw = saved;
        raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) active = true;
    }
    void restore() {
        if (active) {
            ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
            active = false;
        }
    }
};
TermGuard& term_guard() {
    static TermGuard g;
    return g;
}
#endif
} // namespace

TuiRenderer::TuiRenderer() = default;
TuiRenderer::~TuiRenderer() { shutdown(); }

void TuiRenderer::init() {
    if (stdout_is_tty() && !alt_screen_) {
        std::cout << "\x1b[?1049h\x1b[H" << std::flush; // EnterAlternateScreen
        alt_screen_ = true;
    }
#ifdef __unix__
    term_guard().enable_raw();
#endif
    ensure_plan_loaded();
    draw();
}
void TuiRenderer::shutdown() {
    tui_leave_alt_screen();
#ifdef __unix__
    term_guard().restore();
#endif
}
void tui_leave_alt_screen() {
    // Idempotent: safe to call from panic/exit hooks (mirrors Rust restore()).
    static bool left = false;
    if (!left && stdout_is_tty()) {
        std::cout << "\x1b[?1049l" << std::flush; // LeaveAlternateScreen
        left = true;
    }
}

void TuiRenderer::ensure_plan_loaded() {
    if (auto c = agent::Plan::default_plan().read()) {
        plan_content_ = *c;
        if (!show_plan_) {
            show_plan_ = true; // auto-open on live check-off (as in Rust draw())
        }
    }
}

std::vector<std::string> TuiRenderer::wrap_lines(const std::string& text, std::size_t width) {
    if (width == 0) return {text};
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string para;
    while (std::getline(in, para)) {
        std::size_t i = 0;
        std::string cur;
        std::size_t cur_w = 0;
        auto flush_cur = [&] {
            // Trim trailing whitespace.
            while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) cur.pop_back();
            out.push_back(cur);
            cur.clear();
            cur_w = 0;
        };
        while (i < para.size()) {
            // Next word (zero-width spaces are separators; NBSP is not).
            std::size_t j = i;
            while (j < para.size() && para[j] != ' ' && para[j] != '\t' &&
                   !(para[j] == '\xe2' && j + 2 < para.size() && para[j + 1] == '\x80' &&
                     para[j + 2] == '\x8b'))
                j = next_char(para, j);
            std::string word = para.substr(i, j - i);
            if (word == "\xe2\x80\x8b") word.clear(); // zero-width space
            for (char& c : word)
                if (c == '\t') c = ' ';
            std::size_t ww = cp_len(word);
            if (cur_w > 0 && cur_w + 1 + ww > width) flush_cur();
            if (!cur.empty()) {
                cur += ' ';
                cur_w++;
            }
            cur += word;
            cur_w += ww;
            i = j;
            while (i < para.size() && (para[i] == ' ' || para[i] == '\t')) i++;
            // Skip zero-width separators.
            if (i + 2 < para.size() && para[i] == '\xe2' && para[i + 1] == '\x80' &&
                para[i + 2] == '\x8b')
                i += 3;
        }
        flush_cur();
    }
    return out;
}

void TuiRenderer::on_event(const UiEvent& ev) {
    using K = UiEvent::Kind;
    switch (ev.kind) {
        case K::Message:
            if (!current_content_.empty()) current_content_ += "\n";
            current_content_ += ev.text;
            messages_.push_back(ev.text);
            break;
        case K::SteerResponse:
            messages_.push_back("Marmennill: " + ev.text);
            break;
        case K::Thinking:
            current_thought_ += ev.text;
            if (show_thought_) messages_.push_back("[thinking] " + ev.text);
            break;
        case K::ToolCall:
            if (!current_content_.empty()) {
                messages_.push_back(current_content_);
                current_content_.clear();
            }
            messages_.push_back("[Tool Call] " + ev.text);
            break;
        case K::ToolResult: messages_.push_back("[Tool Result] " + ev.text); break;
        case K::Status: {
            auto nl = ev.text.find('\n');
            status_line_ = nl == std::string::npos ? ev.text : ev.text.substr(0, nl);
            if (ev.text.find("System Error") != std::string::npos ||
                ev.text.find("LLM error") != std::string::npos ||
                ev.text.find("[Validator]") != std::string::npos)
                messages_.push_back(ev.text);
            break;
        }
        case K::Delegation: {
            std::string agent = agents::agent_to_string(ev.delegation.agent);
            std::string task = ev.delegation.task.value_or("(no task id)");
            if (ev.delegation.kind == orchestrator::DelegationEvent::Kind::Started) {
                active_agent_ = agent;
                status_line_ = "Delegating to " + agent + "…";
                bool found = false;
                for (auto& s : subagents_) {
                    if (s.name == agent && s.task_id == ev.delegation.task) {
                        s.is_active = true;
                        found = true;
                    }
                }
                if (!found) {
                    SubagentDetail d;
                    d.name = agent;
                    d.task_id = ev.delegation.task;
                    d.is_active = true;
                    d.started_at = std::chrono::steady_clock::now();
                    subagents_.push_back(std::move(d));
                }
                if (!subagents_.empty()) show_subagents_ = true;
            } else {
                for (auto& s : subagents_) {
                    if (s.name == agent && s.task_id == ev.delegation.task) s.is_active = false;
                }
                status_line_ = agent + " finished " + task;
                if (active_agent_ == agent) active_agent_ = "Manager";
            }
            break;
        }
        case K::Done: messages_.push_back("[done]"); break;
    }
}

void TuiRenderer::draw() {
    ensure_plan_loaded();
    if (!stdout_is_tty()) {
        draw_noninteractive();
        return;
    }
    std::cout << "\x1b[H\x1b[2J";
    std::cout << "=== Chat & Logs (Tab: focus) ===\n";
    std::size_t from = 0;
    if (messages_.size() > 30) from = messages_.size() - 30;
    if (chat_scroll_ < 0) chat_scroll_ = 0;
    for (std::size_t i = from; i < messages_.size(); i++) std::cout << messages_[i] << "\n";
    if (show_plan_) {
        std::cout << "--- Execution Plan ---\n" << plan_content_ << "\n";
    }
    if (show_subagents_ && !subagents_.empty()) {
        std::cout << "--- Specialist Subagents [" << (selected_subagent_ + 1) << "/"
                  << subagents_.size() << "] ---\n";
        for (std::size_t i = 0; i < subagents_.size(); i++) {
            auto& s = subagents_[i];
            std::cout << (i == selected_subagent_ ? "> " : "  ") << s.name
                      << (s.is_active ? " (Active)" : " (Idle)") << "\n";
        }
        auto& sel = subagents_[std::min(selected_subagent_, subagents_.size() - 1)];
        if (!sel.thinking.empty()) std::cout << "[Thinking] " << sel.thinking << "\n";
        if (!sel.content.empty()) std::cout << "[Output] " << sel.content << "\n";
        for (auto& l : sel.logs) std::cout << "[Logs] " << l << "\n";
    }
    std::string active;
    {
        std::string a = format_active_subtasks(subagents_);
        if (a != "No active subtasks.") active = " [" + std::to_string(subagents_.size()) + " agents]";
    }
    std::cout << "Session: local | Status: " + status_line_ + active << "\n";
    std::cout << "> " << input_text_ << "\n" << std::flush;
}

void TuiRenderer::draw_noninteractive() {
    // Non-TTY (tests/pipes): no escape codes, keep the message log only.
}

void TuiRenderer::flush() { draw(); }

std::optional<std::string> TuiRenderer::poll_input() {
    if (inbox_.empty()) return std::nullopt;
    std::string line = inbox_.front();
    inbox_.erase(inbox_.begin());
    return line;
}
std::optional<std::string> TuiRenderer::read_input() {
    status_line_ = "Ready";
    while (!aborted_) {
        if (!inbox_.empty()) {
            std::string line = inbox_.front();
            inbox_.erase(inbox_.begin());
            return line;
        }
#ifdef __unix__
        if (!::isatty(STDIN_FILENO)) return std::nullopt;
        char c = 0;
        ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        if (c == '\n' || c == '\r') {
            std::string line = input_text_;
            input_text_.clear();
            cursor_ = 0;
            submit_line(line);
            continue;
        }
        if (c == '\x1b') {
            // Minimal escape-sequence pump (arrows); full keymap via handle_key().
            continue;
        }
        if (c == '\x7f' || c == '\x08') {
            handle_key("backspace");
        } else if (c >= 32) {
            input_text_.insert(cursor_, 1, c);
            cursor_++;
        }
        draw();
#else
        std::string line;
        if (!std::getline(std::cin, line)) return std::nullopt;
        return line;
#endif
    }
    return std::nullopt;
}

void TuiRenderer::submit_line(const std::string& line) {
    if (is_thought_command(line)) {
        show_thought_ = !show_thought_;
        status_line_ = show_thought_ ? "thinking shown" : "thinking hidden";
        return;
    }
    if (is_help_command(line)) {
        messages_.push_back("/help /thought /reset /abort | Tab focus | Ctrl+P plan | Ctrl+A "
                            "agents | Esc-Esc abort");
        return;
    }
    if (is_reset_command(line)) {
        try {
            agent::Plan::default_plan().clear();
        } catch (...) {
        }
        plan_content_ = "No active execution plan.";
        show_plan_ = false;
        messages_.push_back("[Status] execution plan cleared");
        return;
    }
    if (is_abort_command(line)) {
        request_abort();
        return;
    }
    history_.push_back(line);
    history_index_.reset();
    inbox_.push_back(line);
}

void TuiRenderer::handle_key(const std::string& key) {
    if (key == "enter") {
        std::string line = input_text_;
        input_text_.clear();
        cursor_ = 0;
        submit_line(line);
    } else if (key == "esc") {
        if (confirm_abort_) {
            request_abort();
        } else {
            confirm_abort_ = true;
            status_line_ = "press Esc again to abort";
        }
    } else if (key == "ctrl-d") {
        request_abort();
    } else if (key == "ctrl-c") {
        if (confirm_abort_) request_abort();
        else {
            confirm_abort_ = true;
            status_line_ = "press Ctrl+C again to abort";
        }
    } else if (key == "tab") {
        if (focused_ == FocusedPanel::Chat)
            focused_ = show_plan_ ? FocusedPanel::Plan
                      : (!subagents_.empty() ? FocusedPanel::Subagents : FocusedPanel::Chat);
        else if (focused_ == FocusedPanel::Plan)
            focused_ = !subagents_.empty() ? FocusedPanel::Subagents : FocusedPanel::Chat;
        else
            focused_ = FocusedPanel::Chat;
        confirm_abort_ = false;
    } else if (key == "ctrl-p") {
        show_plan_ = !show_plan_;
    } else if (key == "ctrl-a") {
        show_subagents_ = !show_subagents_;
        if (show_subagents_ && !subagents_.empty()) focused_ = FocusedPanel::Subagents;
    } else if (key == "left") {
        if (focused_ == FocusedPanel::Subagents && !subagents_.empty()) {
            if (selected_subagent_ > 0) selected_subagent_--;
        } else {
            cursor_ = prev_char(input_text_, cursor_);
        }
    } else if (key == "right") {
        if (focused_ == FocusedPanel::Subagents && !subagents_.empty()) {
            if (selected_subagent_ + 1 < subagents_.size()) selected_subagent_++;
        } else {
            cursor_ = next_char(input_text_, cursor_);
        }
    } else if (key == "home") {
        cursor_ = 0;
    } else if (key == "end") {
        cursor_ = input_text_.size();
    } else if (key == "up") {
        if (focused_ == FocusedPanel::Chat) {
            if (chat_scroll_ > 0) chat_scroll_--;
        }
    } else if (key == "down") {
        chat_scroll_++;
    } else if (key == "pageup") {
        chat_scroll_ = std::max<long>(0, chat_scroll_ - 10);
    } else if (key == "pagedown") {
        chat_scroll_ += 10;
    } else if (key == "ctrl-up") {
        if (!history_.empty()) {
            if (!history_index_) {
                input_draft_ = input_text_;
                history_index_ = history_.size() - 1;
            } else if (*history_index_ > 0) {
                (*history_index_)--;
            }
            input_text_ = history_[*history_index_];
            cursor_ = input_text_.size();
        }
    } else if (key == "ctrl-down") {
        if (history_index_) {
            if (*history_index_ + 1 < history_.size()) {
                (*history_index_)++;
                input_text_ = history_[*history_index_];
            } else {
                history_index_.reset();
                input_text_ = input_draft_;
            }
            cursor_ = input_text_.size();
        }
    } else if (key == "backspace") {
        if (cursor_ > 0) {
            std::size_t p = prev_char(input_text_, cursor_);
            input_text_.erase(p, cursor_ - p);
            cursor_ = p;
        }
        confirm_abort_ = false;
    } else if (key == "delete") {
        if (cursor_ < input_text_.size()) {
            std::size_t n = next_char(input_text_, cursor_);
            input_text_.erase(cursor_, n - cursor_);
        }
        confirm_abort_ = false;
    } else if (key.size() == 1 || (key.size() > 1 && (unsigned char)key[0] >= 32)) {
        input_text_.insert(cursor_, key);
        cursor_ += key.size();
        confirm_abort_ = false;
    }
}

void TuiRenderer::click_to_cursor(unsigned long x) {
    // F7 mouse-click → grapheme cursor (mirrors tui_click_impl.rs).
    std::size_t byte = 0, vis = 0;
    std::size_t i = 0;
    while (i < input_text_.size()) {
        std::size_t n = next_char(input_text_, i);
        if (vis >= x) break;
        vis += 1; // width(grapheme) ≈ 1 in the click stub (canonical impl clamps)
        byte = n;
        i = n;
        if (vis > x) break;
    }
    cursor_ = std::min(byte, input_text_.size());
    while (cursor_ > 0 && is_cont(static_cast<unsigned char>(input_text_[cursor_]))) cursor_--;
}

int run_tui(const config::Config& cfg, std::optional<std::string> initial,
            std::shared_ptr<orchestrator::OrchestratorManager> manager) {
    TuiRenderer r;
    return run_session(cfg, r, std::move(initial), std::move(manager));
}

} // namespace marmel::ui
