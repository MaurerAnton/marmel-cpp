#pragma once
// Layered TOML configuration + merge + path expansion.
// Rust origin: src/config.rs (462 lines).
//
// Lookup order (first match wins):
//   --config > ./marmel.toml, ./.marmel.toml, ./.marmel/marmel.toml,
//   ./.marmel/config.toml > $HOME/{.marmel/marmel.toml, .marmel/config.toml,
//   .config/marmel/config.toml, .config/marmel/marmel.toml}
//   > env (MARMEL_AUTH_TOKEN, MARMEL_BACKEND_URL, MARMEL_MODEL) > defaults.
// Missing file => defaults (no error); unreadable/invalid TOML => error.
// Merge rule: strings overwrite only when non-empty; optionals overwrite when
// engaged; specialist + mcp tables upsert; monitoring lazily created.
// system_prompt_path is absolutized at load (~ expansion + cwd join).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace marmel::mcp {
struct McpServerConfig {
    std::optional<std::string> command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::optional<std::string> url; // registered but unused in v0.1 (stdio only)
};
} // namespace marmel::mcp

namespace marmel::config {

inline constexpr std::size_t kDefaultMaxRecursionDepth = 3;

struct MonitoringConfig {
    bool enabled = true;
    std::size_t repetition_threshold = 5;
    std::size_t min_pattern_len = 5;
};

struct SpecialistConfig {
    std::string module;
    std::vector<std::string> tools;
    std::optional<std::string> model;
    std::optional<std::string> backend_url;
    std::optional<std::string> auth_token;
    std::optional<std::string> validator_model;
    std::optional<std::string> validator_backend_url;
    std::optional<std::string> validator_auth_token;
    std::optional<std::size_t> max_validator_iterations;
    std::optional<bool> enable_validator; // nullopt => treated as true downstream
};

struct OrchestrationConfig {
    std::size_t max_recursion_depth = kDefaultMaxRecursionDepth;
    std::string manager_module;
    std::map<std::string, SpecialistConfig> specialists; // sorted => deterministic order
    static OrchestrationConfig with_default_depth();
};

struct Config {
    std::string backend_url = "http://localhost:8000/v1";
    std::string auth_token;
    std::string model = "llama3.1-8b-instruct";
    float temperature = 0.7f;
    float top_p = 0.9f;
    float frequency_penalty = 0.0f;
    float presence_penalty = 0.0f;
    std::size_t max_context_tokens = 8192;
    std::string system_prompt_path = "prompts/system.md";
    bool preserve_thinking = true;
    std::uint64_t command_timeout_secs = 60;
    std::size_t max_repetition_threshold = 3;
    bool enable_xml_rescue = true;
    std::string ui_mode = "tui";
    std::optional<MonitoringConfig> monitoring = MonitoringConfig{};
    OrchestrationConfig orchestration = OrchestrationConfig::with_default_depth();
    std::map<std::string, mcp::McpServerConfig> mcp_servers;
};

/// Load config following the lookup order above. Never throws: returns the
/// default config when no file is found; throws std::runtime_error on
/// unreadable/invalid TOML.
Config load(const std::optional<std::string>& explicit_path = std::nullopt);

/// Resolve which file would be used (nullopt => defaults/env only).
std::optional<std::string> resolve_config_path(const std::optional<std::string>& explicit_path);

} // namespace marmel::config
