// Rust origin: src/agents/{mod,coder,debugger,researcher,validator,
//              generalist}.rs + src/orchestrator/registry.rs
#include "marmel/agents.hpp"

#include "marmel/harness.hpp"
#include "marmel/orchestrator.hpp"
#include "marmel/prompts.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace marmel::agents {
namespace {

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
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

agent::ToolCaller caller_for(Agent agent) {
    return agent::ToolCaller::specialist(agent_to_string(agent));
}

std::string format_tool_preview(const std::string& name, const Json& args) {
    auto str_field = [&](const char* key) {
        return args.is_object() ? args.str_or(key, "") : std::string();
    };
    auto utf8_head = [](const std::string& s, std::size_t max_bytes) {
        if (s.size() <= max_bytes) return s;
        std::size_t n = max_bytes;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
        return s.substr(0, n);
    };
    if (name == "read_file" || name == "write_file") return str_field("path");
    if (name == "replace") return str_field("path");
    if (name == "run_command") {
        std::string cmd = str_field("command");
        // NOTE: Rust slices cmd[..32] in bytes (panics on non-boundary);
        // the port backs off to the boundary instead.
        if (cmd.size() > 35) return utf8_head(cmd, 32) + "...";
        return cmd;
    }
    if (name == "grep_search") {
        // NOTE: upstream reads the "query" alias key here, not "pattern".
        if (args.is_object()) {
            auto it = args.as_object().find("query");
            if (it != args.as_object().end() && it->second.is_string())
                return it->second.as_string();
        }
        return "";
    }
    if (name == "glob") return str_field("pattern");
    if (name == "delegate_task") {
        return "agent=" + str_field("agent_name") + ", task_id=" + str_field("task_id") +
               ", prompt=\"" + str_field("prompt") + "\"";
    }
    if (name == "leave_verdict") {
        return "verdict=" + str_field("verdict") + ", comments=\"" + str_field("comments") + "\"";
    }
    std::string s = args.dump();
    if (s.size() > 30) return utf8_head(s, 27) + "...";
    return s;
}

} // namespace

const char* agent_as_str(Agent a) {
    switch (a) {
        case Agent::Coder: return "coder";
        case Agent::Researcher: return "researcher";
        case Agent::Debugger: return "debugger";
        case Agent::Validator: return "validator";
        case Agent::Generalist: return "generalist";
    }
    return "generalist";
}
std::string agent_to_string(Agent a) { return agent_as_str(a); }

std::optional<Agent> agent_from_str(const std::string& s) {
    std::string t = to_lower(trim_copy(s));
    if (t == "coder") return Agent::Coder;
    if (t == "researcher") return Agent::Researcher;
    if (t == "debugger") return Agent::Debugger;
    if (t == "validator") return Agent::Validator;
    if (t == "generalist" || t == "deepbrain") return Agent::Generalist;
    return std::nullopt;
}
std::vector<std::string> all_agent_ids() {
    return {"coder", "researcher", "debugger", "validator", "generalist"};
}

DelegationRequest DelegationRequest::from_json(const Json& v) {
    DelegationRequest r;
    std::string agent_name = v.is_object() ? v.str_or("agent_name", "") : "";
    auto agent = agent_from_str(agent_name);
    if (!agent) throw std::runtime_error("unknown specialist: " + agent_name);
    r.agent = *agent;
    r.prompt = v.is_object() ? v.str_or("prompt", "") : "";
    if (trim_copy(r.prompt).empty()) throw std::runtime_error("delegate_task requires a prompt");
    if (v.is_object() && v.contains("snippets") && v.at("snippets").is_array())
        for (auto& s : v.at("snippets").as_array())
            if (s.is_string()) r.snippets.push_back(s.as_string());
    if (v.is_object() && v.contains("task_id") && v.at("task_id").is_string())
        r.task_id = v.at("task_id").as_string();
    auto str_vec = [&](const char* key) -> std::optional<std::vector<std::string>> {
        if (!v.is_object() || !v.contains(key)) return std::nullopt;
        if (!v.at(key).is_array()) return std::nullopt;
        std::vector<std::string> out;
        for (auto& s : v.at(key).as_array())
            if (s.is_string()) out.push_back(s.as_string());
        return out;
    };
    r.image_urls = str_vec("image_urls");
    r.audio_urls = str_vec("audio_urls");
    return r;
}

IsolatedContext IsolatedContext::from_request(std::string role_prompt, const DelegationRequest& req) {
    IsolatedContext c;
    c.role_system_prompt = std::move(role_prompt);
    c.brief = req.prompt;
    c.task_id = req.task_id;
    c.snippets = req.snippets;
    if (req.image_urls) c.image_urls = *req.image_urls;
    if (req.audio_urls) c.audio_urls = *req.audio_urls;
    return c;
}
agent::ContextEngine IsolatedContext::into_engine(std::size_t max_context_tokens) const {
    agent::ContextEngineFactory f(max_context_tokens);
    return f.specialist_context(role_system_prompt, brief);
}

Deliverable Deliverable::complete(std::string content, std::optional<std::string> task_id) {
    agent::MissionMarker m{agent::MissionMarker::Kind::Complete, task_id, {}};
    return {m, std::move(content), std::move(task_id)};
}
Deliverable Deliverable::failed(std::string content, std::string reason) {
    agent::MissionMarker m{agent::MissionMarker::Kind::Failed, std::nullopt, std::move(reason)};
    return {m, std::move(content), std::nullopt};
}
Deliverable Deliverable::replan(std::string content, std::string reason) {
    agent::MissionMarker m{agent::MissionMarker::Kind::Replan, std::nullopt, std::move(reason)};
    return {m, std::move(content), std::nullopt};
}

void update_revision(std::string& final_content, const std::string& revised) {
    if (revised.empty()) return;
    if (final_content.empty()) {
        final_content = revised;
    } else if (final_content.find(revised) == std::string::npos) {
        final_content += "\n\n";
        final_content += revised;
    }
}

std::string assemble_final_deliverable(bool validation_passed,
                                       const std::optional<std::string>& validator_critique,
                                       const std::string& final_content) {
    auto has_marker = [](const std::string& s) {
        return s.find("MISSION COMPLETE") != std::string::npos ||
               s.find("FAILED") != std::string::npos ||
               s.find("REPLAN REQUIRED") != std::string::npos;
    };
    if (validation_passed) {
        std::string out = final_content;
        if (!has_marker(out)) out += "\n\nMISSION COMPLETE";
        return out;
    }
    std::string rejected = final_content;
    {
        std::string::size_type pos = 0;
        while ((pos = rejected.find("MISSION COMPLETE", pos)) != std::string::npos) {
            rejected.replace(pos, 16, "REVOKED");
            pos += 7;
        }
    }
    std::string out = "VALIDATOR REJECTION: " + validator_critique.value_or("(no critique)") +
                      "\n---------------\n" + rejected;
    if (out.find("FAILED") == std::string::npos && out.find("REPLAN REQUIRED") == std::string::npos)
        out += "\n\nFAILED (Validator rejected the deliverable)";
    return out;
}

namespace {
const std::string_view& role_prompt_view(Agent agent) {
    static const std::string_view kCoder = prompts::CODER_PROMPT;
    static const std::string_view kResearcher = prompts::RESEARCHER_PROMPT;
    static const std::string_view kDebugger = prompts::DEBUGGER_PROMPT;
    static const std::string_view kValidator = prompts::VALIDATOR_PROMPT;
    static const std::string_view kGeneralist = prompts::GENERALIST_PROMPT;
    switch (agent) {
        case Agent::Coder: return kCoder;
        case Agent::Researcher: return kResearcher;
        case Agent::Debugger: return kDebugger;
        case Agent::Validator: return kValidator;
        case Agent::Generalist: return kGeneralist;
    }
    return kGeneralist;
}
} // namespace

std::string role_prompt_for(Agent agent) { return std::string(role_prompt_view(agent)); }

Deliverable Specialist::run(const IsolatedContext& ctx) const {
    // Deterministic canned double (mirrors #[cfg(test)] behavior): isolated
    // role + brief + bounded snippets + terminal marker, no backend needed.
    std::string task = ctx.task_id.value_or("t-000");
    std::string out = "Specialist role ";
    out += agent_as_str(name());
    out += " working on " + task + "\nTASK BRIEF\n" + ctx.brief + "\nBOUNDED SNIPPETS\n";
    for (auto& s : ctx.snippets) {
        out += s;
        out += "\n";
    }
    out += "MISSION COMPLETE (" + task + ")";
    return Deliverable::complete(out, ctx.task_id);
}

std::vector<std::string> Coder::tool_namespaces() const {
    return {"delegate_task", "write_file", "replace", "read_file", "run_command", "grep_search",
            "glob"};
}
std::vector<std::string> Researcher::tool_namespaces() const {
    return {"delegate_task", "write_file", "replace", "read_file", "run_command", "grep_search",
            "glob"};
}
std::vector<std::string> Debugger::tool_namespaces() const {
    return {"delegate_task", "write_file", "replace",         "read_file", "run_command",
            "grep_search",   "glob",        "pty_spawn",       "pty_write", "pty_read",
            "pty_close",     "pty_list",    "pty__spawn",      "pty__write", "pty__read",
            "pty__close",    "pty__list",   "pty_*"};
}
std::vector<std::string> Validator::tool_namespaces() const {
    return {"delegate_task", "write_file", "replace", "read_file",         "run_command",
            "grep_search",   "glob",       "leave_verdict"};
}

// --- Registry ----------------------------------------------------------------------
bool SpecialistEntry::allows(const std::string& tool) const {
    auto strip = [](const std::string& s) {
        constexpr char kP[] = "terminal__";
        return s.rfind(kP, 0) == 0 ? s.substr(sizeof(kP) - 1) : s;
    };
    std::string bare = strip(tool);
    for (auto& ns : tool_namespaces) {
        if (ns == "*") return true;
        std::string nb = strip(ns);
        if (!ns.empty() && ns.back() == '*') {
            std::string prefix = ns.substr(0, ns.size() - 1);
            std::string bprefix = nb.substr(0, nb.size() > 0 && nb.back() == '*' ? nb.size() - 1 : nb.size());
            if (tool.rfind(prefix, 0) == 0 || bare.rfind(bprefix, 0) == 0) return true;
            continue;
        }
        if (ns == tool || nb == bare || ns == bare || nb == tool) return true;
    }
    return false;
}

SpecialistRegistry SpecialistRegistry::canonical() {
    SpecialistRegistry r;
    r.reg(Agent::Coder, "src/agents/coder.rs",
          {"delegate_task", "write_file", "replace", "read_file", "run_command", "grep_search",
           "glob"});
    r.reg(Agent::Researcher, "src/agents/researcher.rs",
          {"delegate_task", "write_file", "replace", "read_file", "run_command", "grep_search",
           "glob"});
    r.reg(Agent::Debugger, "src/agents/debugger.rs",
          {"delegate_task", "write_file", "replace", "read_file", "run_command", "grep_search",
           "glob", "pty_spawn", "pty_write", "pty_read", "pty_close", "pty_list", "pty__spawn",
           "pty__write", "pty__read", "pty__close", "pty__list", "pty_*"});
    r.reg(Agent::Validator, "src/agents/validator.rs",
          {"delegate_task", "write_file", "replace", "read_file", "run_command", "grep_search",
           "glob", "leave_verdict"});
    r.reg(Agent::Generalist, "src/agents/generalist.rs", {"*"});
    return r;
}
void SpecialistRegistry::reg(Agent agent, std::string module, std::vector<std::string> tools,
                             std::optional<std::string> model) {
    auto e = std::make_shared<SpecialistEntry>();
    e->agent = agent;
    e->module = std::move(module);
    e->tool_namespaces = std::move(tools);
    e->model = std::move(model);
    entries_[agent] = e;
    std::shared_ptr<Specialist> w;
    switch (agent) {
        case Agent::Coder: w = std::make_shared<Coder>(); break;
        case Agent::Researcher: w = std::make_shared<Researcher>(); break;
        case Agent::Debugger: w = std::make_shared<Debugger>(); break;
        case Agent::Validator: w = std::make_shared<Validator>(); break;
        case Agent::Generalist: w = std::make_shared<Generalist>(); break;
    }
    workers_[agent] = std::move(w);
}
std::shared_ptr<const SpecialistEntry> SpecialistRegistry::resolve(Agent agent) const {
    auto it = entries_.find(agent);
    return it == entries_.end() ? nullptr : it->second;
}
std::shared_ptr<const Specialist> SpecialistRegistry::worker(Agent agent) const {
    auto it = workers_.find(agent);
    if (it != workers_.end()) return it->second;
    static const auto kGeneralist = std::make_shared<Generalist>();
    return kGeneralist;
}
std::vector<std::string> SpecialistRegistry::agent_ids() const { return all_agent_ids(); }

bool caller_allows_tool(Agent agent, const std::string& tool, const SpecialistRegistry& registry) {
    auto e = registry.resolve(agent);
    if (!e) return false;
    return e->allows(tool);
}

// --- Live specialist loop ---------------------------------------------------------------
namespace {

std::string_view validator_prompt_for(Agent agent) {
    switch (agent) {
        case Agent::Coder: return prompts::VALIDATOR_CODER_PROMPT;
        case Agent::Debugger: return prompts::VALIDATOR_DEBUGGER_PROMPT;
        case Agent::Researcher: return prompts::VALIDATOR_RESEARCHER_PROMPT;
        case Agent::Generalist: return prompts::VALIDATOR_GENERALIST_PROMPT;
        case Agent::Validator: return prompts::VALIDATOR_PROMPT;
    }
    return prompts::VALIDATOR_PROMPT;
}

struct LiveCfg {
    std::string model;
    bool enable_validator = true;
    std::size_t max_validator_iterations = 2;
    std::string validator_model;
    std::string validator_backend;
    std::string validator_token;
    bool xml_rescue = true;
};

LiveCfg live_cfg_for(Agent agent, const config::Config& cfg) {
    LiveCfg lc;
    lc.model = cfg.model;
    lc.xml_rescue = cfg.enable_xml_rescue;
    auto it = cfg.orchestration.specialists.find(agent_to_string(agent));
    if (it != cfg.orchestration.specialists.end()) {
        const auto& sc = it->second;
        if (sc.model) lc.model = *sc.model;
        if (sc.enable_validator) lc.enable_validator = *sc.enable_validator;
        if (sc.max_validator_iterations) lc.max_validator_iterations = *sc.max_validator_iterations;
        if (sc.validator_model) lc.validator_model = *sc.validator_model;
        if (sc.validator_backend_url) lc.validator_backend = *sc.validator_backend_url;
        if (sc.validator_auth_token) lc.validator_token = *sc.validator_auth_token;
    }
    if (lc.validator_model.empty()) {
        auto vit = cfg.orchestration.specialists.find("validator");
        if (vit != cfg.orchestration.specialists.end() && vit->second.model)
            lc.validator_model = *vit->second.model;
        else
            lc.validator_model = cfg.model;
    }
    // Backend/token chain: specialist.validator_* → validator-role backend/auth
    // → global (mirrors run_automated_validation).
    auto vit = cfg.orchestration.specialists.find("validator");
    if (lc.validator_backend.empty()) {
        if (vit != cfg.orchestration.specialists.end() && vit->second.backend_url)
            lc.validator_backend = *vit->second.backend_url;
        else
            lc.validator_backend = cfg.backend_url;
    }
    if (lc.validator_token.empty()) {
        if (vit != cfg.orchestration.specialists.end() && vit->second.auth_token)
            lc.validator_token = *vit->second.auth_token;
        else
            lc.validator_token = cfg.auth_token;
    }
    return lc;
}

// Execute one assistant turn's tool calls; returns false when the loop must stop.
bool execute_live_tools(const llm::ChatClient& client, Agent agent, agent::ContextEngine& engine,
                        const std::vector<types::ToolCall>& calls, harness::HarnessMonitor& monitor,
                        const std::string& status_tag,
                        std::shared_ptr<harness::HarnessStats> stats) {
    (void)client;
    (void)stats;
    auto caller = caller_for(agent);
    for (auto& tc : calls) {
        Json args = Json::parse_or_string(tc.arguments);
        if (!args.is_object()) args = Json::object();
        std::string preview = format_tool_preview(tc.name, args);
        if (preview.empty())
            orchestrator::emit_status(status_tag + ": " + tc.name);
        else
            orchestrator::emit_status(status_tag + ": " + tc.name + "(" + preview + ")");
        auto iv = monitor.observe_tool(tc.name, args);
        std::string result_text;
        if (auto err = monitor.intervention_error(iv)) {
            result_text = *err;
        } else {
            harness::HarnessOutcome outcome = harness::dispatch_for_outcome(
                harness::ToolInvocation{tc.name, args}, caller);
            result_text = outcome.hard_error ? "ERROR: " + outcome.result.content
                                             : outcome.result.content;
        }
        engine.append(types::Message::tool(tc.id, result_text));
    }
    return true;
}

bool has_terminal_marker(const std::string& text) {
    std::string u = to_upper(text);
    return u.find("MISSION COMPLETE") != std::string::npos ||
           u.find("FAILED") != std::string::npos || u.find("REPLAN REQUIRED") != std::string::npos;
}

// Automated validator: returns {approved, critique}.
std::pair<bool, std::string> run_automated_validation(const llm::ChatClient& client, Agent agent,
                                                      const std::string& brief,
                                                      const std::string& deliverable,
                                                      const LiveCfg& lc,
                                                      const config::Config& cfg) {
    llm::ChatClient vclient(lc.validator_backend.empty() ? cfg.backend_url : lc.validator_backend,
                            lc.validator_model.empty() ? cfg.model : lc.validator_model,
                            lc.validator_token);
    std::string sys(validator_prompt_for(agent));
    std::string user = "Task Brief:\n" + brief + "\n\nSpecialist Deliverable:\n" + deliverable +
                       "\n\nInstructions:\n1. Inspect the workspace, verify files, compile, and run "
                       "tests as needed using available tools.\n2. When your verification is "
                       "complete, you MUST call the `leave_verdict` tool with `verdict` "
                       "('APPROVED' or 'REJECTED') and detailed `comments`.";
    agent::ContextEngineFactory f(cfg.max_context_tokens);
    agent::ContextEngine engine = f.specialist_context(sys, user);
    harness::HarnessMonitor monitor;
    if (cfg.monitoring) monitor = harness::HarnessMonitor::new_with_config(
        std::make_shared<harness::HarnessStats>(), *cfg.monitoring);
    SpecialistRegistry reg = SpecialistRegistry::canonical();
    auto entry = reg.resolve(Agent::Validator);
    std::vector<std::string> allow =
        entry ? entry->tool_namespaces : std::vector<std::string>{"*"};
    auto all = types::ToolDef::default_tools(all_agent_ids());
    for (auto loop = 0; loop < 50; loop++) {
        types::ChatRequest req;
        req.model = lc.validator_model.empty() ? cfg.model : lc.validator_model;
        req.messages = engine.messages();
        req.temperature = 0.0f;
        req.top_p = cfg.top_p;
        req.presence_penalty = cfg.presence_penalty;
        req.frequency_penalty = cfg.frequency_penalty;
        req.stream = false;
        std::vector<types::ToolDef> tools;
        for (auto& t : all) {
            bool ok = false;
            for (auto& ns : allow) {
                SpecialistEntry tmp;
                tmp.tool_namespaces = {ns};
                if (tmp.allows(t.name)) {
                    ok = true;
                    break;
                }
            }
            if (ok) tools.push_back(t);
        }
        // MCP-bridged tools are visible unfiltered (as in Rust).
        for (auto& md : harness::mcp_tool_defs()) tools.push_back(md);
        req.tools = tools;
        llm::StreamedReply reply;
        try {
            reply = vclient.chat(req);
        } catch (const std::exception&) {
            break; // fall through to the no-verdict message (as in Rust)
        }
        std::vector<types::ToolCall> calls = reply.tool_calls;
        if (calls.empty() && cfg.enable_xml_rescue) {
            auto rescued = monitor.rescue_xml(reply.content);
            if (!rescued.empty()) calls = rescued;
        }
        engine.append(types::Message::assistant(std::optional<std::string>{reply.content},
                                               reply.reasoning.empty()
                                                   ? std::optional<std::string>{}
                                                   : std::optional<std::string>{reply.reasoning},
                                               calls));
        // leave_verdict wins immediately.
        for (auto& tc : calls) {
            if (tc.name != "leave_verdict") continue;
            Json args = Json::parse_or_string(tc.arguments);
            if (!args.is_object()) args = Json::object();
            std::string verdict = args.str_or("verdict", "APPROVED");
            std::string comments;
            for (auto k : {"comments", "comment", "feedback", "reason", "critique", "details",
                           "explanation"}) {
                comments = args.str_or(k, "");
                if (!comments.empty()) break;
            }
            bool approved = to_upper(trim_copy(verdict)) == "APPROVED";
            std::string critique;
            if (!comments.empty()) {
                critique = trim_copy(comments);
            } else if (approved) {
                critique = "Deliverable verified and approved.";
            } else {
                critique = "Deliverable rejected by validator without detailed comments.";
            }
            return {approved, critique};
        }
        if (calls.empty()) {
            engine.append(types::Message::user(
                "System: You have not submitted a verdict. If you need to perform further "
                "verification, please invoke the appropriate tools (e.g., running commands or "
                "reading files). If your analysis is complete, you must call the 'leave_verdict' "
                "tool to submit your final verdict (APPROVED or REJECTED)."));
            continue;
        }
        std::string vtag = std::string("validator-") + agent_to_string(agent);
        execute_live_tools(vclient, Agent::Validator, engine, calls, monitor, vtag, nullptr);
    }
    return {false, "The validator failed to submit a verdict using the 'leave_verdict' tool."};
}

} // namespace

std::optional<Deliverable> try_run_live(const llm::ChatClient& client, Agent agent,
                                        const IsolatedContext& ctx, const config::Config& cfg,
                                        std::shared_ptr<harness::HarnessStats> stats) {
    if (client.backend_url().empty()) return std::nullopt;
    try {
        return run_live(client, agent, ctx, cfg, std::move(stats));
    } catch (...) {
        return std::nullopt;
    }
}

Deliverable run_live(const llm::ChatClient& client, Agent agent, const IsolatedContext& ctx,
                     const config::Config& cfg, std::shared_ptr<harness::HarnessStats> stats) {
    if (!stats) stats = std::make_shared<harness::HarnessStats>();
    LiveCfg lc = live_cfg_for(agent, cfg);
    std::string tag = (ctx.task_id && !trim_copy(*ctx.task_id).empty())
                          ? agent_to_string(agent) + "-" + *ctx.task_id
                          : agent_to_string(agent);
    std::error_code ec;
    std::string cwd = std::filesystem::current_path(ec).string();
    if (ec) cwd = ".";
    std::string enhanced =
        std::string(role_prompt_view(agent)) +
        "\n\n## Environment & Workspace\n- Current Working Directory (CWD): `" + cwd +
        "`\n- All relative paths and file operations resolve against this workspace directory.\n- "
        "Tools available: `write_file`, `replace`, `read_file`, `run_command`, `grep_search`, "
        "`glob`.\n- You MUST save files and execute real work to complete the task.";
    agent::ContextEngineFactory factory(cfg.max_context_tokens);
    agent::ContextEngine engine = factory.specialist_context(enhanced, ctx.brief);
    if (!ctx.snippets.empty()) {
        std::string sn = "Snippets:\n";
        for (std::size_t i = 0; i < ctx.snippets.size(); i++) {
            if (i) sn += "\n---\n";
            sn += ctx.snippets[i];
        }
        engine.append(types::Message::user(sn));
    }
    // NOTE: fresh (non-shared) stats for the live loop, as in Rust.
    config::MonitoringConfig default_mon;
    const config::MonitoringConfig& mon_cfg = cfg.monitoring ? *cfg.monitoring : default_mon;
    harness::HarnessMonitor monitor =
        harness::HarnessMonitor::new_with_config(std::make_shared<harness::HarnessStats>(), mon_cfg);
    auto guard = orchestrator::register_active_worker(ctx.task_id, agent_to_string(agent), ctx.brief);

    SpecialistRegistry reg = SpecialistRegistry::canonical();
    auto entry = reg.resolve(agent);
    std::vector<std::string> allow =
        entry ? entry->tool_namespaces : std::vector<std::string>{"*"};

    auto build_tools = [&] {
        auto defs = types::ToolDef::default_tools(all_agent_ids());
        std::vector<types::ToolDef> tools;
        for (auto& t : defs) {
            bool ok = false;
            for (auto& ns : allow) {
                SpecialistEntry tmp;
                tmp.tool_namespaces = {ns};
                if (tmp.allows(t.name)) {
                    ok = true;
                    break;
                }
            }
            if (ok) tools.push_back(t);
        }
        // MCP-bridged tools are visible unfiltered (as in Rust).
        for (auto& md : harness::mcp_tool_defs()) tools.push_back(md);
        return tools;
    };

    std::string final_content;
    unsigned nudge_count = 0;
    for (int turn = 0; turn < 100; turn++) {
        orchestrator::emit_status(tag + ": thinking / calling model (" + lc.model + ")...");
        types::ChatRequest req;
        req.model = lc.model;
        req.messages = engine.messages();
        req.temperature = cfg.temperature;
        req.top_p = cfg.top_p;
        req.frequency_penalty = cfg.frequency_penalty;
        req.presence_penalty = cfg.presence_penalty;
        req.stream = false;
        req.tools = build_tools();
        llm::StreamedReply reply = client.chat(req);
        update_revision(final_content, reply.content);
        std::vector<types::ToolCall> calls = reply.tool_calls;
        if (calls.empty() && lc.xml_rescue) {
            auto rescued = monitor.rescue_xml(reply.content);
            if (!rescued.empty()) calls = rescued;
        }
        engine.append(types::Message::assistant(
            std::optional<std::string>{reply.content},
            reply.reasoning.empty() ? std::optional<std::string>{}
                                    : std::optional<std::string>{reply.reasoning},
            calls));
        if (calls.empty()) {
            if (has_terminal_marker(reply.content)) break;
            if (nudge_count < 3) {
                nudge_count++;
                engine.append(types::Message::user(
                    "SYSTEM NOTICE: You did not call any tools or output MISSION COMPLETE. Please "
                    "use your tools (such as `read_file`, `write_file`, `run_command`, etc.) to "
                    "perform the required work, create/update any requested files in the "
                    "workspace, and conclude with 'MISSION COMPLETE'."));
                continue;
            }
            break;
        }
        execute_live_tools(client, agent, engine, calls, monitor, tag, stats);
        if (engine.should_compact()) engine.compact();
    }

    bool validation_passed = !lc.enable_validator || lc.max_validator_iterations == 0 ||
                             agent == Agent::Validator;
    std::optional<std::string> critique;
    bool do_validate = lc.enable_validator && lc.max_validator_iterations > 0 &&
                       agent != Agent::Validator && !final_content.empty();
    if (do_validate) {
        validation_passed = false;
        for (std::size_t vi = 0; vi < lc.max_validator_iterations; vi++) {
            orchestrator::emit_status("validator-" + tag + ": testing deliverable (pass " +
                                      std::to_string(vi + 1) + "/" +
                                      std::to_string(lc.max_validator_iterations) + ")...");
            auto [approved, fb] = run_automated_validation(client, agent, ctx.brief, final_content,
                                                           lc, cfg);
            if (approved) {
                std::string feedback =
                    trim_copy(fb).empty() ? "All verification checks passed." : fb;
                orchestrator::emit_status("[Validator] APPROVED deliverable for " + tag + ":\n" +
                                          feedback);
                validation_passed = true;
                critique.reset();
                break;
            }
            std::string feedback =
                trim_copy(fb).empty() ? "Deliverable failed verification checks." : fb;
            critique = feedback;
            orchestrator::emit_status("[Validator] REJECTED deliverable for " + tag + " (pass " +
                                      std::to_string(vi + 1) + "/" +
                                      std::to_string(lc.max_validator_iterations) +
                                      "):\n" + feedback);
            engine.append(types::Message::user(
                "Validation feedback: The validator tested your changes and found issues:\n" +
                feedback +
                "\n\nPlease address all validator critique points, verify your work with available "
                "tools, and conclude with 'MISSION COMPLETE'."));
            std::string latest;
            for (int rt = 0; rt < 25; rt++) {
                orchestrator::emit_status(tag + ": revising code per validator critique (step " +
                                          std::to_string(rt + 1) + "/25)...");
                types::ChatRequest req;
                req.model = lc.model;
                req.messages = engine.messages();
                req.temperature = cfg.temperature;
                req.top_p = cfg.top_p;
                req.frequency_penalty = cfg.frequency_penalty;
                req.presence_penalty = cfg.presence_penalty;
                req.stream = false;
                req.tools = build_tools();
                llm::StreamedReply reply = client.chat(req);
                latest = reply.content;
                std::vector<types::ToolCall> calls = reply.tool_calls;
                if (calls.empty() && lc.xml_rescue) {
                    auto rescued = monitor.rescue_xml(reply.content);
                    if (!rescued.empty()) calls = rescued;
                }
                engine.append(types::Message::assistant(
                    std::optional<std::string>{reply.content},
                    reply.reasoning.empty() ? std::optional<std::string>{}
                                            : std::optional<std::string>{reply.reasoning},
                    calls));
                if (calls.empty()) break;
                execute_live_tools(client, agent, engine, calls, monitor, tag, stats);
            }
            if (!latest.empty()) final_content = latest;
        }
    }
    std::string assembled;
    if (!final_content.empty())
        assembled = assemble_final_deliverable(validation_passed, critique, final_content);
    auto marker = agent::MissionMarker::parse(assembled);
    Deliverable d;
    d.content = assembled;
    d.task_id = ctx.task_id;
    if (marker) {
        d.marker = *marker;
        if (marker->is_complete() && marker->task_id) d.task_id = marker->task_id;
    } else {
        d.marker = agent::MissionMarker{agent::MissionMarker::Kind::Failed, std::nullopt,
                                       "no terminal marker"};
    }
    return d;
}

} // namespace marmel::agents
