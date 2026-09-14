// Ported integration tests: tests/test_{agent,context,harness,llm,monitor,
// orchestrator,role_gating,ui_session}.rs + key inline unit tests.
// Runs with `marmel_tests` (or ctest). No external framework.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "marmel/agent_context.hpp"
#include "marmel/agent_loop.hpp"
#include "marmel/agent_phase.hpp"
#include "marmel/agents.hpp"
#include "marmel/config.hpp"
#include "marmel/harness.hpp"
#include "marmel/llm.hpp"
#include "marmel/orchestrator.hpp"
#include "marmel/tool_names.hpp"
#include "marmel/ui.hpp"
#include "marmel/widget.hpp"

namespace {

namespace fs = std::filesystem;
int g_pass = 0, g_fail = 0;
std::string g_current;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::cout << "FAIL [" << g_current << ":" << __LINE__ << "] " #cond "\n";         \
            g_fail++;                                                                        \
            return;                                                                          \
        }                                                                                    \
    } while (0)
#define CHECK_HAS(hay, needle)                                                               \
    do {                                                                                     \
        if ((hay).find(needle) == std::string::npos) {                                       \
            std::cout << "FAIL [" << g_current << ":" << __LINE__ << "] missing \"" needle    \
                      << "\" in: " << (hay).substr(0, 300) << "\n";                          \
            g_fail++;                                                                        \
            return;                                                                          \
        }                                                                                    \
    } while (0)

struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/marmel_test_XXXXXX";
        if (!::mkdtemp(tmpl)) throw std::runtime_error("mkdtemp failed");
        path = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
struct CwdGuard {
    std::string prev;
    CwdGuard() { prev = fs::current_path().string(); }
    explicit CwdGuard(const std::string& dir) : CwdGuard() { fs::current_path(dir); }
    ~CwdGuard() {
        std::error_code ec;
        fs::current_path(prev, ec);
    }
};

marmel::Json obj(std::initializer_list<std::pair<std::string, marmel::Json>> xs) {
    marmel::Json::Object o;
    for (auto& kv : xs) o.emplace(kv.first, kv.second);
    return marmel::Json(std::move(o));
}

std::shared_ptr<marmel::orchestrator::OrchestratorManager> test_manager(const std::string& dir) {
    auto client =
        std::make_shared<marmel::llm::ChatClient>("http://localhost:9999/v1", "test-model");
    return std::make_shared<marmel::orchestrator::OrchestratorManager>(
        client, marmel::agent::Plan::at(dir), std::make_shared<marmel::harness::HarnessStats>());
}

// --- test_agent.rs ------------------------------------------------------------------
void t_specialist_tool_filtering_and_allowlists() {
    using namespace marmel;
    auto reg = agents::SpecialistRegistry::canonical();
    auto tools = types::ToolDef::default_tools();
    auto names = [&](agents::Agent a) {
        std::vector<std::string> out;
        for (auto& t : tools)
            if (reg.resolve(a)->allows(t.name)) out.push_back(t.name);
        return out;
    };
    auto has = [](const std::vector<std::string>& v, const std::string& n) {
        return std::find(v.begin(), v.end(), n) != v.end();
    };
    auto researcher = names(agents::Agent::Researcher);
    for (auto t : {"write_file", "replace", "read_file", "run_command", "grep_search", "glob",
                   "delegate_task"})
        CHECK(has(researcher, t));
    CHECK(!has(researcher, "leave_verdict"));
    CHECK(!has(researcher, "create_plan"));
    CHECK(!has(researcher, "archive_current_plan"));
    auto coder = names(agents::Agent::Coder);
    for (auto t : {"write_file", "replace", "read_file", "run_command", "grep_search", "glob"})
        CHECK(has(coder, t));
    CHECK(!has(coder, "leave_verdict"));
    auto validator = names(agents::Agent::Validator);
    CHECK(has(validator, "leave_verdict"));
    CHECK(has(validator, "read_file"));
    CHECK(!has(validator, "create_plan"));
    // Registry entries mirror worker namespaces (canonical invariant).
    for (auto a : {agents::Agent::Coder, agents::Agent::Researcher, agents::Agent::Debugger,
                   agents::Agent::Validator, agents::Agent::Generalist}) {
        auto e = reg.resolve(a);
        auto w = reg.worker(a);
        CHECK(e && w);
        CHECK(e->tool_namespaces == w->tool_namespaces());
    }
}

void t_subagent_history_message_sequence() {
    using namespace marmel;
    agent::ContextEngineFactory f(128000);
    agent::ContextEngine eng = f.specialist_context("sys", "task");
    CHECK(eng.messages().size() == 2);
    types::ToolCall tc{"call_1", "read_file", "{}"};
    eng.append(types::Message::assistant(std::string("working"), std::nullopt, {tc}));
    eng.append(types::Message::tool("call_1", "content"));
    CHECK(eng.messages().size() == 4);
    CHECK(eng.messages()[0].role == types::MsgRole::System);
    CHECK(eng.messages()[1].role == types::MsgRole::User);
    CHECK(eng.messages()[2].role == types::MsgRole::Assistant);
    CHECK(eng.messages()[3].role == types::MsgRole::Tool);
    eng.append(types::Message::assistant(std::string("done MISSION COMPLETE"), std::nullopt, {}));
    CHECK(eng.messages().size() == 5);
}

// --- test_context.rs -----------------------------------------------------------------
void t_integration_context_engine_lifecycle() {
    using namespace marmel;
    agent::ContextEngineFactory f(1000);
    agent::ContextEngine eng = f.manager_context("sys", "goal");
    CHECK(eng.messages().size() == 2);
    eng.append(types::Message::user("u"));
    eng.append(types::Message::assistant(std::string("a"), std::nullopt, {}));
    CHECK(eng.messages().size() == 4);
    eng.perform_rebirth("summary");
    CHECK(eng.messages().size() == 4);
    CHECK(eng.messages()[0].content == "sys");
    CHECK(eng.messages()[1].content == "goal");
    CHECK(eng.messages()[2].content == "u");
    CHECK_HAS(eng.messages()[3].content, "summary");
    CHECK_HAS(eng.messages()[3].content, "REBIRTH CHECKPOINT");
}

void t_compaction_thresholds() {
    using namespace marmel;
    CHECK(agent::compaction_threshold(1000) == 900);
    CHECK(agent::compaction_target(1000) == 700);
    agent::ContextEngineFactory f(1000);
    agent::ContextEngine eng = f.manager_context("sys", "goal");
    for (int i = 0; i < 8; i++)
        eng.append(types::Message::user("payload-" + std::to_string(i) + "-" + std::string(1500, 'x')));
    CHECK(eng.should_compact());
    std::size_t before = eng.token_count();
    eng.compact();
    CHECK(eng.token_count() < before);
    CHECK(!eng.should_compact());
    // Prefix pinning survives compaction.
    CHECK(eng.messages()[0].content == "sys");
    CHECK(eng.messages()[1].content == "goal");
}

void t_rebirth_and_limit_messages() {
    using namespace marmel;
    CHECK_HAS(std::string(agent::kContextLimitExceededMessage), "rebirth");
    CHECK_HAS(std::string(agent::kRebirthCheckpointPrefix), "REBIRTH CHECKPOINT");
}

// --- test_harness.rs --------------------------------------------------------------------
void t_integration_harness_fs_and_dispatch() {
    using namespace marmel;
    TempDir tmp;
    std::string p = tmp.path + "/demo.txt";
    agent::ToolCaller caller = agent::ToolCaller::specialist("coder");
    harness::ToolResult r = harness::dispatch_for(
        harness::ToolInvocation{"write_file", obj({{"path", Json(p)}, {"content", Json("hello")}})},
        caller);
    CHECK(!r.is_error);
    r = harness::dispatch_for(
        harness::ToolInvocation{"read_file", obj({{"path", Json(p)}})}, caller);
    CHECK(!r.is_error);
    CHECK_HAS(r.content, "hello");
    r = harness::dispatch_for(
        harness::ToolInvocation{
            "replace", obj({{"path", Json(p)}, {"old_str", Json("hello")}, {"new_str", Json("bye")}})},
        caller);
    CHECK(!r.is_error);
    r = harness::dispatch_for(
        harness::ToolInvocation{"read_file",
                                obj({{"path", Json(p)}, {"offset", Json(0)}, {"limit", Json(8000)}})},
        caller);
    CHECK(!r.is_error);
    CHECK_HAS(r.content, "bye");
}

void t_harness_replace_uniqueness() {
    using namespace marmel;
    TempDir tmp;
    std::string p = tmp.path + "/u.txt";
    {
        std::ofstream f(p);
        f << "aaa bbb aaa";
    }
    agent::ToolCaller caller = agent::ToolCaller::specialist("coder");
    harness::ToolResult r = harness::dispatch_for(
        harness::ToolInvocation{
            "replace", obj({{"path", Json(p)}, {"old_str", Json("aaa")}, {"new_str", Json("zzz")}})},
        caller);
    CHECK(r.is_error);
    CHECK_HAS(r.content, "ambiguous");
    r = harness::dispatch_for(
        harness::ToolInvocation{
            "replace", obj({{"path", Json(p)}, {"old_str", Json("qqq")}, {"new_str", Json("zzz")}})},
        caller);
    CHECK(r.is_error);
    CHECK_HAS(r.content, "not found");
}

void t_read_pagination_footer() {
    using namespace marmel;
    TempDir tmp;
    std::string p = tmp.path + "/big.txt";
    std::string body(20000, 'y');
    {
        std::ofstream f(p);
        f << body;
    }
    agent::ToolCaller caller = agent::ToolCaller::specialist("coder");
    harness::ToolResult r = harness::dispatch_for(
        harness::ToolInvocation{"read_file", obj({{"path", Json(p)}})}, caller);
    CHECK(!r.is_error);
    CHECK_HAS(r.content, "[Showing characters 0-8000 of 20000. Use offset=8000 to read next chunk]");
}

void t_truncation() {
    using namespace marmel;
    std::string big(20000, 'z');
    harness::ToolResult r = harness::apply_tool_output_length_limit(harness::ToolResult::ok(big));
    CHECK_HAS(r.content, "TRUNCATED");
    CHECK_HAS(r.content, "total: 20000 chars");
    harness::ToolResult plan =
        harness::apply_tool_output_length_limit(harness::ToolResult::ok("# EXECUTION PLAN\n" + big));
    CHECK(plan.content.find("TRUNCATED") == std::string::npos);
}

// --- test_llm.rs --------------------------------------------------------------------------
void t_thinking_demuxer_streaming_chunks() {
    using namespace marmel;
    llm::ThinkingDemuxer d;
    llm::VecSink sink;
    d.push_delta("[thinking]bug ", [&](llm::DeltaKind k, const std::string& s) {
        sink.emit(k == llm::DeltaKind::Thinking ? llm::StreamEvent::thinking(s)
                                                : llm::StreamEvent::content(s));
    });
    d.push_delta("analysis[/thinking]the ", [&](llm::DeltaKind k, const std::string& s) {
        sink.emit(k == llm::DeltaKind::Thinking ? llm::StreamEvent::thinking(s)
                                                : llm::StreamEvent::content(s));
    });
    d.push_delta("fix", [&](llm::DeltaKind k, const std::string& s) {
        sink.emit(k == llm::DeltaKind::Thinking ? llm::StreamEvent::thinking(s)
                                                : llm::StreamEvent::content(s));
    });
    d.finish();
    CHECK_HAS(sink.thinking(), "bug analysis");
    CHECK_HAS(sink.content(), "fix");
}

void t_thinking_demux_split_tag() {
    using namespace marmel;
    llm::ThinkingDemuxer d;
    d.push("[thin");
    d.push("king]t[/thin");
    d.push("king]");
    d.finish();
    CHECK(d.thinking() == "t");
    CHECK(d.content().empty());
}

void t_thinking_recovery_and_nudge() {
    using namespace marmel;
    types::ChatRequest req;
    req.temperature = 0.7f;
    req.frequency_penalty = 0.0f;
    auto rec = llm::apply_recovery(req, llm::RecoveryAdjustment{});
    CHECK(rec.enable_thinking && !*rec.enable_thinking);
    CHECK(rec.frequency_penalty && *rec.frequency_penalty > 0.49f);
    llm::NudgePolicy n;
    CHECK(n.should_nudge(0) && n.should_nudge(2) && !n.should_nudge(3));
    auto t = n.nudge({types::Message::user("hi")});
    CHECK(t.size() == 2 && t.back().content == "?");
}

void t_chat_request_payload_construction() {
    using namespace marmel;
    types::ChatRequest req;
    req.model = "llama-3-70b";
    req.messages = {types::Message::system("s"), types::Message::user("u")};
    req.tools = std::vector<types::ToolDef>{types::ToolDef::read_file(), types::ToolDef::write_file()};
    req.stream = true;
    std::string dump = req.to_json().dump();
    CHECK_HAS(dump, "llama-3-70b");
    CHECK_HAS(dump, "read_file");
    CHECK_HAS(dump, "write_file");
}

// --- test_monitor.rs -------------------------------------------------------------------------
void t_monitor_xml_rescue_and_repetition() {
    using namespace marmel;
    harness::HarnessMonitor mon = harness::HarnessMonitor::with_new_stats();
    // Exact fixture from tests/test_monitor.rs (flat {"name", "arguments"} form).
    auto calls = mon.rescue_xml(
        "Let me search the files: <tool_call>{\"name\": \"grep_search\", \"arguments\": "
        "{\"pattern\": \"fn main\"}}</tool_call>");
    CHECK(calls.size() == 1);
    CHECK(calls[0].name == "grep_search");
    CHECK(calls[0].id.rfind("call_text_", 0) == 0);
    bool blocked = false;
    for (int i = 0; i < 10; i++) {
        auto iv = mon.observe_tool("read_file", obj({{"path", Json("src/main.rs")}}));
        if (auto err = mon.intervention_error(iv)) {
            CHECK(err->find("TOOL REPETITION DETECTED") != std::string::npos ||
                  err->find("TOOL CYCLE DETECTED") != std::string::npos);
            blocked = true;
            break;
        }
    }
    CHECK(blocked);
}

void t_monitor_cycle_cut() {
    using namespace marmel;
    harness::HarnessMonitor mon = harness::HarnessMonitor::with_new_stats();
    bool cut = false;
    for (int i = 0; i < 12; i++) {
        std::string name = (i % 2 == 0) ? "run_command" : "read_file";
        auto iv = mon.observe_tool(name, obj({{"command", Json("ls")}}));
        if (auto err = mon.intervention_error(iv)) {
            if (err->find("TOOL CYCLE DETECTED") != std::string::npos) {
                cut = true;
                break;
            }
        }
    }
    CHECK(cut);
}

void t_monitor_text_repetition() {
    using namespace marmel;
    harness::HarnessMonitor mon = harness::HarnessMonitor::with_new_stats();
    bool fired = false;
    for (int i = 0; i < 40 && !fired; i++) fired = mon.feed_text("ababababab");
    CHECK(fired);
    CHECK(mon.stats()->repetition_breaks.load() == 1);
}

void t_monitor_pagination_exempt() {
    using namespace marmel;
    harness::HarnessMonitor mon = harness::HarnessMonitor::with_new_stats();
    // Progressing offsets must NOT trip the consecutive blocker.
    for (int i = 0; i < 8; i++) {
        auto iv = mon.observe_tool(
            "read_file", obj({{"path", Json("f")}, {"offset", Json(i * 100)}}));
        CHECK(mon.intervention_error(iv) == std::nullopt);
    }
}

// --- test_orchestrator.rs -----------------------------------------------------------------------
void t_orchestr_depth_gate() {
    using namespace marmel;
    TempDir tmp;
    auto mgr = test_manager(tmp.path);
    mgr->set_max_recursion_depth_for_tests(3);
    mgr->set_recursion_depth_for_tests(3);
    agents::DelegationRequest req;
    req.agent = agents::Agent::Coder;
    req.prompt = "do it";
    req.task_id = "t-1";
    bool threw = false;
    try {
        mgr->delegate(req);
    } catch (const std::exception& e) {
        threw = true;
        std::string msg = e.what();
        CHECK(msg.find("exceeds max") != std::string::npos ||
              msg.find("recursion") != std::string::npos);
    }
    CHECK(threw);
    CHECK(mgr->delegation_events().empty());

    mgr->set_recursion_depth_for_tests(0);
    mgr->set_max_recursion_depth_for_tests(1);
    mgr->set_recursion_depth_for_tests(1);
    threw = false;
    try {
        mgr->delegate(req);
    } catch (const std::exception& e) {
        threw = true;
    }
    CHECK(threw);

    auto mgr2 = test_manager(tmp.path);
    agents::DelegationRequest ok = req;
    ok.task_id = "t-2";
    agents::Deliverable d = mgr2->delegate(ok);
    CHECK(d.marker.is_complete());
}

void t_orchestr_deep_freeze() {
    using namespace marmel;
    TempDir tmp;
    auto mgr = test_manager(tmp.path);
    agents::DelegationRequest req;
    req.agent = agents::Agent::Coder;
    req.prompt = "frozen work";
    req.task_id = "t-777";
    agents::Deliverable d = mgr->delegate(req);
    CHECK(d.marker.is_complete());
    orchestrator::CrashJournal journal(tmp.path);
    CHECK(!journal.is_frozen());
    CHECK(journal.journal().size() >= 2); // Frozen + Resolved

    std::string wid = journal.snapshot(agents::Agent::Generalist, req);
    CHECK(journal.is_frozen());
    auto mgr2 = test_manager(tmp.path);
    auto rec = mgr2->recover_frozen();
    CHECK(rec.has_value());
    CHECK_HAS(rec->content, "frozen work");
    CHECK(!journal.is_frozen());
    (void)wid;

    auto clean = mgr2->recover_frozen();
    CHECK(!clean.has_value());
}

void t_orchestr_events_and_checkoff() {
    using namespace marmel;
    TempDir tmp;
    auto mgr = test_manager(tmp.path);
    mgr->create_plan("- [ ] [t-77] do thing\n");
    agents::DelegationRequest req;
    req.agent = agents::Agent::Coder;
    req.prompt = "do thing";
    req.task_id = "t-77";
    agents::Deliverable d = mgr->delegate(req);
    CHECK(d.marker.is_complete());
    auto& evs = mgr->delegation_events();
    CHECK(evs.size() == 2);
    CHECK(evs[0].kind == orchestrator::DelegationEvent::Kind::Started);
    CHECK(evs[1].kind == orchestrator::DelegationEvent::Kind::Completed);

    auto mgr2 = test_manager(tmp.path);
    mgr2->create_plan("- [ ] [t-101] one\n- [ ] [t-102] two\n");
    agents::DelegationRequest r1;
    r1.agent = agents::Agent::Coder;
    r1.prompt = "one";
    r1.task_id = "t-101";
    mgr2->delegate(r1);
    auto pending = mgr2->plan().pending_tasks();
    CHECK(pending.size() == 1 && pending[0] == "t-102");

    // FAILED deliverables never flip the box.
    agent::Plan p = agent::Plan::at(tmp.path);
    p.create("- [ ] [t-200] fragile\n");
    CHECK(!p.check_plan_on_marker(std::optional<std::string>("t-200"), "FAILED: broke"));
    CHECK(p.pending_tasks().size() == 1);
}

void t_orchestr_handle_delegate_task() {
    using namespace marmel;
    TempDir tmp;
    CwdGuard guard(tmp.path);
    auto bad = orchestrator::handle_delegate_task(obj({{"agent_name", Json("planner")},
                                                       {"prompt", Json("x")}}));
    CHECK(!bad.ok && bad.hard);
    CHECK_HAS(bad.content, "invalid arguments for delegate_task");
    auto blank = orchestrator::handle_delegate_task(
        obj({{"agent_name", Json("coder")}, {"prompt", Json("   ")}}));
    CHECK(!blank.ok && blank.hard);
    marmel::Json::Array sn;
    auto ok = orchestrator::handle_delegate_task(obj({{"agent_name", Json("coder")},
                                                      {"prompt", Json("build it")},
                                                      {"snippets", marmel::Json(std::move(sn))},
                                                      {"task_id", Json("t-500")}}));
    CHECK(ok.ok && !ok.hard);
    CHECK_HAS(ok.content, "MISSION COMPLETE (t-500)");
    CHECK_HAS(ok.content, "build it");
}

// --- test_role_gating.rs --------------------------------------------------------------------------
void t_role_gating() {
    using namespace marmel;
    TempDir tmp;
    CwdGuard guard(tmp.path);
    agent::ToolCaller manager = agent::ToolCaller::manager();
    for (auto tool : {"write_file", "replace", "run_command"}) {
        harness::ToolResult r = harness::dispatch_for(
            harness::ToolInvocation{tool, obj({{"path", Json("x")}, {"command", Json("x")},
                                               {"content", Json("x")}, {"old_str", Json("a")},
                                               {"new_str", Json("b")}})},
            manager);
        CHECK(r.is_error);
        CHECK(r.content.find(tool) != std::string::npos);
        CHECK_HAS(r.content, "Manager");
    }
    {
        harness::ToolResult r = harness::dispatch_for(
            harness::ToolInvocation{"delegate_task",
                                    obj({{"agent_name", Json("coder")}, {"prompt", Json("hi")}})},
            manager);
        CHECK(!r.is_error);
    }
    agent::ToolCaller researcher = agent::ToolCaller::specialist("researcher");
    {
        harness::ToolResult r =
            harness::dispatch_for(harness::ToolInvocation{"gedcom__search", obj({})}, researcher);
        CHECK(r.is_error);
        CHECK_HAS(r.content, "researcher");
    }
    agent::ToolCaller coder = agent::ToolCaller::specialist("coder");
    {
        std::string p = tmp.path + "/w.txt";
        harness::ToolResult r = harness::dispatch_for(
            harness::ToolInvocation{"terminal__write_file",
                                    obj({{"path", Json(p)}, {"content", Json("ok")}})},
            coder);
        CHECK(!r.is_error);
    }
    agent::ToolCaller generalist = agent::ToolCaller::specialist("generalist");
    {
        harness::ToolResult r = harness::dispatch_for(
            harness::ToolInvocation{"create_plan", obj({{"plan_markdown", Json("x")}})}, generalist);
        CHECK(r.is_error);
    }
    {
        harness::ToolResult r = harness::dispatch_for(
            harness::ToolInvocation{"delegate_task",
                                    obj({{"agent_name", Json("validator")}, {"prompt", Json("q")}})},
            researcher);
        CHECK(!r.is_error);
    }
}

// --- test_ui_session.rs -------------------------------------------------------------------------------
struct ScriptedRenderer : public marmel::ui::Renderer {
    std::vector<std::string> script;
    std::size_t pos = 0;
    bool aborted_ = false;
    std::vector<marmel::ui::UiEvent> events;
    void init() override {}
    void on_event(const marmel::ui::UiEvent& ev) override { events.push_back(ev); }
    void flush() override {}
    std::optional<std::string> poll_input() override { return std::nullopt; }
    std::optional<std::string> read_input() override {
        if (aborted_ || pos >= script.size()) return std::nullopt;
        std::string line = script[pos++];
        if (line == "/abort") {
            aborted_ = true;
            return line;
        }
        return line;
    }
    void request_abort() override { aborted_ = true; }
    bool aborted() const override { return aborted_; }
    void shutdown() override {}
};

void t_ui_session_two_turn_abort() {
    using namespace marmel;
    config::Config cfg;
    cfg.backend_url = "";
    ScriptedRenderer renderer;
    renderer.script = {"goal", "steer2", "/abort"};
    int backend_calls = 0;
    ui::ChatFn chat = [&](std::vector<types::Message> msgs, const llm::StreamConfig& sc,
                          llm::StreamSink& sink) -> types::Message {
        (void)msgs;
        (void)sc;
        (void)sink;
        backend_calls++;
        return types::Message::assistant(std::string("ack"), std::nullopt, {});
    };
    int rc = ui::run_session_with_chat(cfg, renderer, std::nullopt, nullptr, chat);
    (void)rc;
    CHECK(backend_calls == 2);
    CHECK(renderer.aborted());
}

// --- inline unit tests (ports of #[cfg(test)] blocks) --------------------------------------------------
void t_plan_parse_and_markers() {
    using namespace marmel;
    std::string md = "- [ ] [t-001] first\n- [x] [t-002] second\n";
    auto pending = agent::parse_unchecked_tasks(md);
    CHECK(pending.size() == 1 && pending[0] == "t-001");
    auto all = agent::parse_all_tasks(md);
    CHECK(all.size() == 2);
    CHECK(agent::output_is_success("all good"));
    CHECK(!agent::output_is_success("ERROR boom"));
    CHECK(!agent::output_is_success("task FAILED"));
    CHECK(!agent::output_is_success("REPLAN REQUIRED now"));
    auto m1 = agent::MissionMarker::parse("MISSION COMPLETE (t-001)");
    CHECK(m1 && m1->is_complete() && m1->task_id && *m1->task_id == "t-001");
    auto m2 = agent::MissionMarker::parse("REPLAN REQUIRED: missing deps");
    CHECK(m2 && !m2->is_complete());
    auto m3 = agent::MissionMarker::parse("REPLAN REQUIRED but also MISSION COMPLETE");
    CHECK(m3 && !m3->is_complete()); // precedence: REPLAN wins
}

void t_steer_outcome_fallback() {
    using namespace marmel;
    orchestrator::SteerDecision d;
    d.decision = "RespondDirectly";
    d.response = "hi";
    auto o1 = orchestrator::resolve_steer_outcome(d, true);
    CHECK(o1.kind == orchestrator::SteerOutcome::Kind::Decided);
    auto o2 = orchestrator::resolve_steer_outcome(std::nullopt, true);
    CHECK(o2.kind == orchestrator::SteerOutcome::Kind::QueueInstruction);
    auto o3 = orchestrator::resolve_steer_outcome(std::nullopt, false);
    CHECK(o3.kind == orchestrator::SteerOutcome::Kind::SteerImmediately);
}

void t_widget_parse() {
    using namespace marmel;
    auto ws = widget::parse("widget chat paragraph\n"
                            "{\n"
                            "title = \"Chat\"\n"
                            "}\n");
    CHECK(ws.size() == 1 && ws[0].name == "chat");
    CHECK(ws[0].get("title") && *ws[0].get("title") == "Chat");
    bool threw = false;
    try {
        widget::parse("widget x frobnicate\n{\n}\n");
    } catch (const widget::ParseError& e) {
        threw = true;
        CHECK_HAS(e.to_string(), "line ");
    }
    CHECK(threw);
}

void t_agent_loop_gating() {
    using namespace marmel;
    TempDir tmp;
    agent::Plan plan = agent::Plan::at(tmp.path);
    plan.create("- [ ] [t-1] x\n");
    agent::AgentLoop loop(plan);
    loop.set_dispatcher([](const agent::PendingTool& t, const agent::ToolCaller& c) {
        harness::ToolResult r = harness::dispatch_for(harness::ToolInvocation{t.name, t.arguments}, c);
        return std::make_pair(!r.is_error, r.content);
    });
    // 5 identical failing-tool repeats trip the semantic gate.
    for (int i = 0; i < 5; i++) loop.enqueue_tools({obj({{"name", Json("run_command")},
                                                         {"arguments", obj({{"command", Json("ls")}}) }})});
    agent::TurnOutcome out = loop.run_turn();
    CHECK(out.kind == agent::TurnOutcome::Kind::ToolError);
    CHECK_HAS(out.message, "TOOL REPETITION DETECTED");
}

void t_config_defaults_and_example() {
    using namespace marmel;
    config::Config cfg;
    CHECK(cfg.backend_url == "http://localhost:8000/v1");
    CHECK(cfg.orchestration.max_recursion_depth == 3);
    CHECK(cfg.monitoring && cfg.monitoring->repetition_threshold == 5);
    // An explicit but missing file is an error (mirrors Rust load()).
    bool threw = false;
    try {
        config::load(std::optional<std::string>("nonexistent-marmel.toml"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    // The shipped example parses and carries the canonical specialist table.
    config::Config ex =
        config::load(std::optional<std::string>(MARMEL_EXAMPLE_PATH));
    CHECK(ex.orchestration.specialists.count("coder") == 1);
}

void t_tool_names_and_normalize() {
    using namespace marmel;
    CHECK(std::string(tool_names::kDelegateTask) == "delegate_task");
    CHECK(harness::normalize_tool_name("run") == "terminal__run_command");
    CHECK(harness::normalize_tool_name("read") == "terminal__read_file");
    CHECK(harness::normalize_tool_name("delegate_task") == "delegate_task");
}

void t_chunk_utf8_and_commands() {
    using namespace marmel;
    auto chunks = ui::chunk_utf8("héllo🌍world", 5);
    CHECK(chunks.size() >= 3);
    CHECK(ui::is_abort_command("/abort") && ui::is_abort_command(":q"));
    CHECK(ui::is_reset_command("/reset") && ui::is_reset_command("/clear_plan"));
    CHECK(!ui::is_abort_command("hello"));
}

void t_assemble_deliverable() {
    using namespace marmel;
    std::string ok = agents::assemble_final_deliverable(true, std::nullopt, "Code done.");
    CHECK_HAS(ok, "MISSION COMPLETE");
    std::string rej = agents::assemble_final_deliverable(false, std::string("bad"), "Code done.");
    CHECK(rej.find("MISSION COMPLETE") == std::string::npos);
    CHECK_HAS(rej, "FAILED");
    CHECK_HAS(rej, "VALIDATOR REJECTION");
}

void t_semantic_json_eq() {
    using namespace marmel;
    CHECK(harness::semantic_json_eq(R"({"a":1,"offset":5})", R"({"offset":9,"a":1})"));
    CHECK(!harness::semantic_json_eq(R"({"a":1})", R"({"a":2})"));
}

// --- workspace.rs unit tests (test_workspace_creates_and_validates) --------------------------
void t_tooldef_descriptions_exact() {
    // Tool schemas are LLM-visible prompt text: assert byte-exact parity with types.rs.
    using namespace marmel;
    CHECK(types::ToolDef::replace().description ==
          "Replace an exact, unique block of text within a file. Fails if old_str matches 0 or "
          ">1 times.");
    CHECK(types::ToolDef::run_command().description ==
          "Execute a command line inside a dedicated PTY with timeout and process-group "
          "isolation.");
    CHECK(types::ToolDef::leave_verdict().description ==
          "Record the final verification verdict for this task. You must call this tool to finish "
          "validation.");
    CHECK(types::ToolDef::pty_list().description == "List all active interactive PTY sessions.");
    auto tools = types::ToolDef::default_tools();
    CHECK(tools.size() == 16);
    CHECK(tools.front().name == "delegate_task" && tools.back().name == "leave_verdict");
}

void t_dispatch_outcome_hard_soft() {
    using namespace marmel;
    agent::ToolCaller manager = agent::ToolCaller::manager();
    // Hard: unknown tool for the Manager hits the allowlist first
    // (Rust dispatch_manager → Forbidden), exactly as upstream.
    {
        harness::HarnessOutcome o =
            harness::dispatch_for_outcome(harness::ToolInvocation{"frobnicate", obj({})}, manager);
        CHECK(o.hard_error && o.result.is_error);
        CHECK(o.result.content ==
              "tool `frobnicate` is forbidden for caller `Manager` by orchestration policy");
    }
    // Hard: the legacy dispatch() table reports UnknownTool verbatim.
    {
        harness::ToolResult r = harness::dispatch(harness::ToolInvocation{"frobnicate", obj({})});
        CHECK(r.is_error);
        CHECK_HAS(r.content, "unknown tool: frobnicate");
    }
    // Hard: Forbidden carries the exact thiserror text.
    {
        harness::HarnessOutcome o = harness::dispatch_for_outcome(
            harness::ToolInvocation{"write_file",
                                    obj({{"path", Json("x")}, {"content", Json("y")}})},
            manager);
        CHECK(o.hard_error);
        CHECK(o.result.content ==
              "tool `write_file` is forbidden for caller `Manager` by orchestration policy");
    }
    // Hard: BadArguments carries the exact thiserror text.
    {
        harness::HarnessOutcome o = harness::dispatch_for_outcome(
            harness::ToolInvocation{"read_file", obj({{"offset", Json("x")}})}, manager);
        CHECK(o.hard_error);
        CHECK_HAS(o.result.content, "invalid arguments for read_file");
        CHECK_HAS(o.result.content, "missing string field `path`");
    }
    // Soft: replace ambiguity is Ok(err) — no ERROR prefix upstream.
    {
        TempDir tmp;
        std::string p = tmp.path + "/u.txt";
        {
            std::ofstream f(p);
            f << "aaa bbb aaa";
        }
        agent::ToolCaller coder = agent::ToolCaller::specialist("coder");
        harness::HarnessOutcome o = harness::dispatch_for_outcome(
            harness::ToolInvocation{
                "replace",
                obj({{"path", Json(p)}, {"old_str", Json("aaa")}, {"new_str", Json("z")}})},
            coder);
        CHECK(!o.hard_error && o.result.is_error);
        CHECK_HAS(o.result.content, "ambiguous");
    }
    // Hard: missing file is an Execution error with the OS text.
    {
        agent::ToolCaller coder = agent::ToolCaller::specialist("coder");
        harness::HarnessOutcome o = harness::dispatch_for_outcome(
            harness::ToolInvocation{"read_file",
                                    obj({{"path", Json("/tmp/marmel-test-no-such-file-xyz")}})},
            coder);
        CHECK(o.hard_error && o.result.is_error);
        CHECK_HAS(o.result.content, "os error");
        (void)o;
    }
}

void t_update_revision_separator() {
    using namespace marmel;
    std::string acc;
    agents::update_revision(acc, "first");
    CHECK(acc == "first");
    agents::update_revision(acc, "second");
    CHECK(acc == "first\n\nsecond");
    agents::update_revision(acc, "first"); // substring: no-op
    CHECK(acc == "first\n\nsecond");
}

void t_workspace_creates_and_validates() {
    using namespace marmel;
    TempDir tmp;
    std::string dir = tmp.path + "/ws";
    CHECK(!fs::exists(dir));
    harness::Workspace ws = harness::Workspace::at(dir);
    ws.ensure_writable();
    CHECK(fs::is_directory(dir));
    CHECK(ws.plan_path() == dir + "/execution_plan.md");
    CHECK(ws.log_path() == dir + "/marmel.log");
    CHECK(ws.forced_phase_path() == dir + "/forced_phase.txt");
    CHECK(ws.archive_dir() == dir + "/archive");
    // No probe file left behind.
    bool empty = fs::is_empty(dir);
    CHECK(empty);
}

void t_workspace_default_root() {
    using namespace marmel;
    harness::Workspace ws = harness::Workspace::default_workspace();
    CHECK(ws.root() == ".marmel");
    CHECK(ws.plan_path() == std::string(".marmel/execution_plan.md"));
    CHECK(ws.log_path() == std::string(".marmel/marmel.log"));
}

} // namespace

#define RUN(fn)                                   \
    do {                                          \
        g_current = #fn;                          \
        int before = g_fail;                      \
        fn();                                     \
        if (g_fail == before) {                   \
            g_pass++;                             \
            std::cout << "ok - " << #fn << "\n";  \
        }                                         \
    } while (0)

int main() {
    CwdGuard root_guard; // tests chdir internally but always restore
    (void)root_guard;
    RUN(t_specialist_tool_filtering_and_allowlists);
    RUN(t_subagent_history_message_sequence);
    RUN(t_integration_context_engine_lifecycle);
    RUN(t_compaction_thresholds);
    RUN(t_rebirth_and_limit_messages);
    RUN(t_integration_harness_fs_and_dispatch);
    RUN(t_harness_replace_uniqueness);
    RUN(t_read_pagination_footer);
    RUN(t_truncation);
    RUN(t_thinking_demuxer_streaming_chunks);
    RUN(t_thinking_demux_split_tag);
    RUN(t_thinking_recovery_and_nudge);
    RUN(t_chat_request_payload_construction);
    RUN(t_monitor_xml_rescue_and_repetition);
    RUN(t_monitor_cycle_cut);
    RUN(t_monitor_text_repetition);
    RUN(t_monitor_pagination_exempt);
    RUN(t_orchestr_depth_gate);
    RUN(t_orchestr_deep_freeze);
    RUN(t_orchestr_events_and_checkoff);
    RUN(t_orchestr_handle_delegate_task);
    RUN(t_role_gating);
    RUN(t_ui_session_two_turn_abort);
    RUN(t_plan_parse_and_markers);
    RUN(t_steer_outcome_fallback);
    RUN(t_widget_parse);
    RUN(t_agent_loop_gating);
    RUN(t_config_defaults_and_example);
    RUN(t_tool_names_and_normalize);
    RUN(t_chunk_utf8_and_commands);
    RUN(t_assemble_deliverable);
    RUN(t_semantic_json_eq);
    RUN(t_dispatch_outcome_hard_soft);
    RUN(t_update_revision_separator);
    RUN(t_workspace_creates_and_validates);
    RUN(t_workspace_default_root);
    RUN(t_tooldef_descriptions_exact);
    std::cout << "\n" << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
