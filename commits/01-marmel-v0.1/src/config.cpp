// Rust origin: src/config.rs
#include "marmel/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

namespace marmel::config {
namespace {

namespace fs = std::filesystem;

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

std::string unquote(const std::string& s) {
    std::string t = trim(s);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
        std::string inner = t.substr(1, t.size() - 2);
        if (t.front() == '"') {
            std::string out;
            for (std::size_t i = 0; i < inner.size(); i++) {
                if (inner[i] == '\\' && i + 1 < inner.size()) {
                    char e = inner[++i];
                    if (e == 'n') out += '\n';
                    else if (e == 't') out += '\t';
                    else out += e;
                } else {
                    out += inner[i];
                }
            }
            return out;
        }
        return inner;
    }
    return t;
}

std::vector<std::string> parse_str_array(const std::string& s) {
    std::vector<std::string> out;
    std::string t = trim(s);
    if (t.empty() || t.front() != '[') return out;
    std::string cur;
    bool in_str = false;
    char q = 0;
    for (std::size_t i = 1; i < t.size(); i++) {
        char c = t[i];
        if (in_str) {
            if (c == '\\' && i + 1 < t.size()) {
                cur += c;
                cur += t[++i];
            } else if (c == q) {
                in_str = false;
                out.push_back(unquote(std::string(1, q) + cur + std::string(1, q)));
                cur.clear();
            } else {
                cur += c;
            }
        } else {
            if (c == '"' || c == '\'') {
                in_str = true;
                q = c;
            } else if (c == ']') {
                break;
            }
        }
    }
    return out;
}

bool parse_bool(const std::string& s, bool& out) {
    std::string t = trim(s);
    if (t == "true") { out = true; return true; }
    if (t == "false") { out = false; return true; }
    return false;
}

bool parse_double(const std::string& s, double& out) {
    try {
        std::size_t n = 0;
        out = std::stod(trim(s), &n);
        return n > 0;
    } catch (...) {
        return false;
    }
}

bool parse_ull(const std::string& s, unsigned long long& out) {
    try {
        std::string t = trim(s);
        if (t.empty() || t[0] == '-' || t[0] == '+') return false;
        std::size_t n = 0;
        out = std::stoull(t, &n);
        return n == t.size();
    } catch (...) {
        return false;
    }
}

std::optional<std::string> home_dir() {
    if (const char* h = std::getenv("HOME")) {
        if (*h) return std::string(h);
    }
#ifndef _WIN32
    if (const char* u = getpwuid(getuid())->pw_dir) {
        // NOTE: getpwuid result used immediately; matches Rust libc fallback.
        if (u && *u) return std::string(u);
    }
#else
    if (const char* h = std::getenv("USERPROFILE")) {
        if (*h) return std::string(h);
    }
#endif
    return std::nullopt;
}

std::string expand_path(const std::string& p) {
    if (p.rfind("~/", 0) == 0 || p == "~") {
        if (auto h = home_dir()) return *h + p.substr(1);
        return p;
    }
    fs::path path(p);
    if (path.is_absolute()) return p;
    return (fs::current_path() / path).string();
}

struct Partial {
    std::optional<std::string> backend_url, auth_token, model, system_prompt_path, ui_mode;
    std::optional<double> temperature, top_p, frequency_penalty, presence_penalty;
    std::optional<unsigned long long> max_context_tokens, command_timeout_secs, max_repetition_threshold;
    std::optional<bool> preserve_thinking, enable_xml_rescue;
    std::optional<MonitoringConfig> monitoring;
    bool has_monitoring = false;
    std::optional<unsigned long long> max_recursion_depth;
    std::optional<std::string> manager_module;
    std::map<std::string, SpecialistConfig> specialists;
    std::map<std::string, mcp::McpServerConfig> mcp_servers;
};

void apply_kv(Partial& p, const std::string& section, const std::string& key, const std::string& val,
              std::string& cur_specialist, std::string& cur_mcp) {
    if (section.empty()) {
        bool b = false;
        double d = 0;
        unsigned long long u = 0;
        if (key == "backend_url") p.backend_url = unquote(val);
        else if (key == "auth_token") p.auth_token = unquote(val);
        else if (key == "model") p.model = unquote(val);
        else if (key == "system_prompt_path") p.system_prompt_path = unquote(val);
        else if (key == "ui_mode") p.ui_mode = unquote(val);
        else if (key == "temperature" && parse_double(val, d)) p.temperature = d;
        else if (key == "top_p" && parse_double(val, d)) p.top_p = d;
        else if (key == "frequency_penalty" && parse_double(val, d)) p.frequency_penalty = d;
        else if (key == "presence_penalty" && parse_double(val, d)) p.presence_penalty = d;
        else if (key == "max_context_tokens" && parse_ull(val, u)) p.max_context_tokens = u;
        else if (key == "command_timeout_secs" && parse_ull(val, u)) p.command_timeout_secs = u;
        else if (key == "max_repetition_threshold" && parse_ull(val, u)) p.max_repetition_threshold = u;
        else if (key == "preserve_thinking" && parse_bool(val, b)) p.preserve_thinking = b;
        else if (key == "enable_xml_rescue" && parse_bool(val, b)) p.enable_xml_rescue = b;
        return;
    }
    if (section == "monitoring") {
        if (!p.has_monitoring) {
            p.monitoring = MonitoringConfig{};
            p.has_monitoring = true;
        }
        bool b = false;
        unsigned long long u = 0;
        if (key == "enabled" && parse_bool(val, b)) p.monitoring->enabled = b;
        else if (key == "repetition_threshold" && parse_ull(val, u))
            p.monitoring->repetition_threshold = static_cast<std::size_t>(u);
        else if (key == "min_pattern_len" && parse_ull(val, u))
            p.monitoring->min_pattern_len = static_cast<std::size_t>(u);
        return;
    }
    if (section == "orchestration") {
        unsigned long long u = 0;
        if (key == "max_recursion_depth" && parse_ull(val, u))
            p.max_recursion_depth = u;
        else if (key == "manager_module") p.manager_module = unquote(val);
        return;
    }
    if (section == "specialist") {
        SpecialistConfig& sc = p.specialists[cur_specialist];
        bool b = false;
        unsigned long long u = 0;
        if (key == "module") sc.module = unquote(val);
        else if (key == "tools") sc.tools = parse_str_array(val);
        else if (key == "model") sc.model = unquote(val);
        else if (key == "backend_url") sc.backend_url = unquote(val);
        else if (key == "auth_token") sc.auth_token = unquote(val);
        else if (key == "validator_model") sc.validator_model = unquote(val);
        else if (key == "validator_backend_url") sc.validator_backend_url = unquote(val);
        else if (key == "validator_auth_token") sc.validator_auth_token = unquote(val);
        else if (key == "max_validator_iterations" && parse_ull(val, u))
            sc.max_validator_iterations = static_cast<std::size_t>(u);
        else if ((key == "enable_validator" || key == "auto_validate" || key == "enable_validation") &&
                 parse_bool(val, b))
            sc.enable_validator = b;
        return;
    }
    if (section == "mcp") {
        mcp::McpServerConfig& mc = p.mcp_servers[cur_mcp];
        if (key == "command") mc.command = unquote(val);
        else if (key == "args") mc.args = parse_str_array(val);
        else if (key == "url") mc.url = unquote(val);
        else if (key.rfind("env.", 0) == 0) mc.env[key.substr(4)] = unquote(val);
        return;
    }
}

Partial parse_toml_text(const std::string& text, const std::string& path_for_errors) {
    Partial p;
    std::string section;
    std::string cur_specialist, cur_mcp;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    auto fail = [&](const std::string& msg) -> Partial {
        throw std::runtime_error(path_for_errors + ": line " + std::to_string(lineno) + ": " + msg);
    };
    while (std::getline(in, line)) {
        lineno++;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[') {
            if (t.back() != ']') fail("bad section header");
            std::string name = trim(t.substr(1, t.size() - 2));
            if (name == "monitoring") section = "monitoring";
            else if (name == "orchestration") section = "orchestration";
            else if (name.rfind("orchestration.specialists.", 0) == 0) {
                section = "specialist";
                cur_specialist = name.substr(std::string("orchestration.specialists.").size());
                if (cur_specialist.empty()) fail("empty specialist name");
            } else if (name.rfind("mcp_servers.", 0) == 0) {
                section = "mcp";
                cur_mcp = name.substr(12);
                if (cur_mcp.empty()) fail("empty mcp server name");
            } else {
                section = "unknown";
            }
            continue;
        }
        if (section == "unknown") continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) fail("expected key = value");
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        // Strip trailing comment outside quotes.
        {
            bool in_s = false;
            char q = 0;
            for (std::size_t i = 0; i < val.size(); i++) {
                char c = val[i];
                if (in_s) {
                    if (c == '\\') i++;
                    else if (c == q) in_s = false;
                } else if (c == '"' || c == '\'') {
                    in_s = true;
                    q = c;
                } else if (c == '#') {
                    val = trim(val.substr(0, i));
                    break;
                }
            }
        }
        // Multi-line arrays: keep consuming until brackets balance.
        {
            int depth = 0;
            bool in_s = false;
            char q = 0;
            for (std::size_t i = 0; i < val.size(); i++) {
                char c = val[i];
                if (in_s) {
                    if (c == '\\') i++;
                    else if (c == q) in_s = false;
                } else if (c == '"' || c == '\'') {
                    in_s = true;
                    q = c;
                } else if (c == '[') depth++;
                else if (c == ']') depth--;
            }
            std::string extra;
            while (depth > 0 && std::getline(in, extra)) {
                lineno++;
                val += "\n" + extra;
                for (char c : extra) {
                    if (c == '[') depth++;
                    else if (c == ']') depth--;
                }
            }
        }
        apply_kv(p, section, key, val, cur_specialist, cur_mcp);
    }
    return p;
}

Config merge(Config base, const Partial& q) {
    if (q.backend_url && !q.backend_url->empty()) base.backend_url = *q.backend_url;
    if (q.auth_token) base.auth_token = *q.auth_token;
    if (q.model && !q.model->empty()) base.model = *q.model;
    if (q.temperature) base.temperature = static_cast<float>(*q.temperature);
    if (q.top_p) base.top_p = static_cast<float>(*q.top_p);
    if (q.frequency_penalty) base.frequency_penalty = static_cast<float>(*q.frequency_penalty);
    if (q.presence_penalty) base.presence_penalty = static_cast<float>(*q.presence_penalty);
    if (q.max_context_tokens) base.max_context_tokens = static_cast<std::size_t>(*q.max_context_tokens);
    if (q.system_prompt_path && !q.system_prompt_path->empty()) base.system_prompt_path = *q.system_prompt_path;
    if (q.preserve_thinking) base.preserve_thinking = *q.preserve_thinking;
    if (q.command_timeout_secs) base.command_timeout_secs = *q.command_timeout_secs;
    if (q.max_repetition_threshold)
        base.max_repetition_threshold = static_cast<std::size_t>(*q.max_repetition_threshold);
    if (q.enable_xml_rescue) base.enable_xml_rescue = *q.enable_xml_rescue;
    if (q.ui_mode && !q.ui_mode->empty()) base.ui_mode = *q.ui_mode;
    if (q.has_monitoring && q.monitoring) {
        if (!base.monitoring) base.monitoring = MonitoringConfig{};
        // Field-wise merge: our Partial always carries full defaults for the
        // block, but TOML absence must not clobber file order — since Partial
        // only sets has_monitoring when the block exists, and the block parse
        // starts from defaults, field-wise copy is exact.
        *base.monitoring = *q.monitoring;
    }
    if (q.max_recursion_depth)
        base.orchestration.max_recursion_depth = static_cast<std::size_t>(*q.max_recursion_depth);
    if (q.manager_module) base.orchestration.manager_module = *q.manager_module;
    for (auto& [k, v] : q.specialists) base.orchestration.specialists[k] = v;
    for (auto& [k, v] : q.mcp_servers) base.mcp_servers[k] = v;
    return base;
}

} // namespace

OrchestrationConfig OrchestrationConfig::with_default_depth() {
    OrchestrationConfig o;
    o.max_recursion_depth = kDefaultMaxRecursionDepth;
    return o;
}

std::optional<std::string> resolve_config_path(const std::optional<std::string>& explicit_path) {
    if (explicit_path && !explicit_path->empty()) return explicit_path;
    std::vector<std::string> local = {"./marmel.toml", "./.marmel.toml", "./.marmel/marmel.toml",
                                      "./.marmel/config.toml"};
    for (auto& c : local) {
        std::error_code ec;
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) return c;
    }
    if (auto h = home_dir()) {
        std::vector<std::string> home = {*h + "/.marmel/marmel.toml", *h + "/.marmel/config.toml",
                                         *h + "/.config/marmel/config.toml",
                                         *h + "/.config/marmel/marmel.toml"};
        for (auto& c : home) {
            std::error_code ec;
            if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) return c;
        }
    }
    return std::nullopt;
}

Config load(const std::optional<std::string>& explicit_path) {
    Config cfg; // built-in defaults
    if (auto p = resolve_config_path(explicit_path)) {
        std::ifstream f(*p, std::ios::binary);
        if (!f) throw std::runtime_error("cannot read config file: " + *p);
        std::ostringstream ss;
        ss << f.rdbuf();
        cfg = merge(cfg, parse_toml_text(ss.str(), *p));
    }
    if (const char* t = std::getenv("MARMEL_AUTH_TOKEN")) {
        if (*t) cfg.auth_token = t;
    }
    if (const char* u = std::getenv("MARMEL_BACKEND_URL")) {
        if (*u) cfg.backend_url = u;
    }
    if (const char* m = std::getenv("MARMEL_MODEL")) {
        if (*m) cfg.model = m;
    }
    if (!cfg.system_prompt_path.empty()) cfg.system_prompt_path = expand_path(cfg.system_prompt_path);
    return cfg;
}

} // namespace marmel::config
