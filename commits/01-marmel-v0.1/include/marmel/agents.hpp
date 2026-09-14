#pragma once
// Specialists: roles, isolated contexts, live loop, validation loop.
// Rust origin: src/agents/mod.rs (1112) + coder/debugger/researcher/
//              validator/generalist.rs (~50 each) + src/orchestrator/
//              registry.rs (250).
//
// Fractal rules (REQ-ORCH-002, preserved):
//  - Manager never does domain work (guarded by manager_module not
//    containing "/agents/").
//  - Every unit of work runs in an IsolatedContext: EXACTLY 2 messages
//    (role system prompt + brief). No manager transcript leaks.
//  - Only Coder + Generalist may recurse (may_recurse).
//  - Only Debugger gets pty_*; only Validator gets leave_verdict;
//    Generalist allowlist is ["*"].
//  - Terminal protocol: MISSION COMPLETE (t-xxx) / FAILED: r /
//    REPLAN REQUIRED: r, precedence REPLAN > COMPLETE > FAILED.
// Validation loop: skipped when disabled, max_iter==0, or agent==Validator
// (defaults: enabled, 2 iterations). assemble_final_deliverable() appends
// MISSION COMPLETE on pass, or VALIDATOR REJECTION + REVOKED + FAILED on
// rejection — the REVOKED rewrite is why orchestrator check-off double-gates
// on the marker still being present.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "marmel/agent_context.hpp"
#include "marmel/agent_phase.hpp"
#include "marmel/config.hpp"
#include "marmel/json.hpp"
#include "marmel/llm.hpp"
#include "marmel/types.hpp"

namespace marmel::harness {
struct HarnessStats;
}

namespace marmel::agents {

enum class Agent { Coder, Researcher, Debugger, Validator, Generalist };

const char* agent_as_str(Agent a);
std::optional<Agent> agent_from_str(const std::string& s);
std::string agent_to_string(Agent a);
std::vector<std::string> all_agent_ids(); // canonical order

struct DelegationRequest {
    Agent agent = Agent::Generalist;
    std::string prompt;
    std::vector<std::string> snippets;
    std::optional<std::string> task_id;
    std::optional<std::vector<std::string>> image_urls;
    std::optional<std::vector<std::string>> audio_urls;
    bool recursion_granted = false;
    static DelegationRequest from_json(const Json& v); // throws on unknown agent/empty prompt
};

struct IsolatedContext {
    std::string role_system_prompt;
    std::string brief;
    std::optional<std::string> task_id;
    std::vector<std::string> snippets;
    std::vector<std::string> image_urls;
    std::vector<std::string> audio_urls;
    static IsolatedContext from_request(std::string role_prompt, const DelegationRequest& req);
    agent::ContextEngine into_engine(std::size_t max_context_tokens) const;
};

struct Deliverable {
    agent::MissionMarker marker = {agent::MissionMarker::Kind::Failed, std::nullopt, {}};
    std::string content;
    std::optional<std::string> task_id;
    static Deliverable complete(std::string content, std::optional<std::string> task_id = std::nullopt);
    static Deliverable failed(std::string content, std::string reason);
    static Deliverable replan(std::string content, std::string reason);
};

/// Append-or-keep revision text (no duplicates grow the deliverable).
void update_revision(std::string& final_content, const std::string& revised);
std::string assemble_final_deliverable(bool validation_passed,
                                       const std::optional<std::string>& validator_critique,
                                       const std::string& final_content);

std::string role_prompt_for(Agent agent);

/// Live specialist loop (run_specialist_live). Returns nullopt when no live
/// backend is configured, letting the caller fall back to the deterministic
/// canned Specialist::run() (mirrors try_run_specialist_live).
std::optional<Deliverable> try_run_live(const llm::ChatClient& client, Agent agent,
                                        const IsolatedContext& ctx, const config::Config& cfg,
                                        std::shared_ptr<harness::HarnessStats> stats);
Deliverable run_live(const llm::ChatClient& client, Agent agent, const IsolatedContext& ctx,
                     const config::Config& cfg, std::shared_ptr<harness::HarnessStats> stats);

class Specialist {
public:
    virtual ~Specialist() = default;
    virtual Agent name() const = 0;
    virtual std::vector<std::string> tool_namespaces() const = 0;
    virtual bool may_recurse() const { return false; }
    virtual Deliverable run(const IsolatedContext& ctx) const;
};

class Coder final : public Specialist {
public:
    Agent name() const override { return Agent::Coder; }
    std::vector<std::string> tool_namespaces() const override;
    bool may_recurse() const override { return true; }
};
class Researcher final : public Specialist {
public:
    Agent name() const override { return Agent::Researcher; }
    std::vector<std::string> tool_namespaces() const override;
};
class Debugger final : public Specialist {
public:
    Agent name() const override { return Agent::Debugger; }
    std::vector<std::string> tool_namespaces() const override;
};
class Validator final : public Specialist {
public:
    Agent name() const override { return Agent::Validator; }
    std::vector<std::string> tool_namespaces() const override;
};
class Generalist final : public Specialist {
public:
    Agent name() const override { return Agent::Generalist; }
    std::vector<std::string> tool_namespaces() const override { return {"*"}; }
    bool may_recurse() const override { return true; }
};

// --- Registry (src/orchestrator/registry.rs) -------------------------------

struct SpecialistEntry {
    Agent agent = Agent::Generalist;
    std::string module;
    std::vector<std::string> tool_namespaces;
    std::optional<std::string> model;
    bool allows(const std::string& tool) const;
};

class SpecialistRegistry {
public:
    static SpecialistRegistry canonical();
    void reg(Agent agent, std::string module, std::vector<std::string> tools,
             std::optional<std::string> model = std::nullopt);
    std::shared_ptr<const SpecialistEntry> resolve(Agent agent) const;
    std::shared_ptr<const Specialist> worker(Agent agent) const;
    std::vector<std::string> agent_ids() const;

private:
    std::map<Agent, std::shared_ptr<SpecialistEntry>> entries_;
    std::map<Agent, std::shared_ptr<Specialist>> workers_;
};

/// Routing-boundary gate used by the harness (strips terminal__ prefixes,
///
/// supports "*" and "prefix*" globs on both sides).
bool caller_allows_tool(Agent agent, const std::string& tool, const SpecialistRegistry& registry);

} // namespace marmel::agents
