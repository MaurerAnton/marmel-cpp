// Binary entry: marmel.
// Rust origin: src/main.rs (248 lines).
//
// Thin composition root: parse args -> load config -> decide raw/TUI ->
// logging + panic/terminal-restore hook -> boot MCP (failure swallowed) ->
// boot manager -> run. TUI logs to .marmel/marmel.log with rotation
// (5MB x3); raw logs to stderr.

#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "marmel/agent_phase.hpp"
#include "marmel/config.hpp"
#include "marmel/harness.hpp"
#include "marmel/llm.hpp"
#include "marmel/mcp.hpp"
#include "marmel/orchestrator.hpp"
#include "marmel/ui.hpp"

#ifdef __unix__
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

struct CliArgs {
    std::optional<std::string> config;
    bool raw = false;
    bool debug = false;
    std::optional<std::string> prompt;
};

void print_help() {
    std::cout << "marmel — autonomous agentic coding assistant (C++ port of Marmel v0.1)\n"
                 "\n"
                 "Usage: marmel [--config <path>] [--raw] [--debug] [PROMPT]\n"
                 "\n"
                 "  --config <path>  Override config file path\n"
                 "  --raw            Force headless stdout mode\n"
                 "  --debug          Detailed debug logging to debug.log\n"
                 "  -h, --help       Print usage\n";
}

CliArgs parse_args(int argc, char** argv) {
    CliArgs args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "marmel: --config requires a path argument\n";
                std::exit(2);
            }
            args.config = argv[++i];
        } else if (a == "--raw") {
            args.raw = true;
        } else if (a == "--debug") {
            args.debug = true;
        } else if (a == "-h" || a == "--help") {
            print_help();
            std::exit(0);
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "marmel: unknown flag " << a << "\n";
            print_help();
            std::exit(2);
        } else if (!args.prompt) {
            args.prompt = a;
        } else {
            std::cerr << "marmel: warning: ignoring extra positional argument\n";
        }
    }
    return args;
}

std::string backup_path(const std::string& path, unsigned n) {
    return path + "." + std::to_string(n);
}
void rotate_log(const std::string& path, unsigned long long max_bytes, unsigned backups) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec || size < max_bytes) return;
    // log -> log.1 -> log.2 -> log.3 (oldest dropped).
    std::error_code ec2;
    fs::remove(backup_path(path, backups), ec2);
    for (unsigned n = backups; n > 1; n--) {
        fs::rename(backup_path(path, n - 1), backup_path(path, n), ec2);
    }
    fs::rename(path, backup_path(path, 1), ec2);
}

bool stdout_is_terminal() {
#ifdef __unix__
    return ::isatty(STDOUT_FILENO) != 0 && ::isatty(STDIN_FILENO) != 0;
#else
    return false;
#endif
}

std::shared_ptr<marmel::orchestrator::OrchestratorManager> boot_manager(
    const marmel::config::Config& cfg) {
    auto client = std::make_shared<marmel::llm::ChatClient>(marmel::llm::ChatClient::from_config(cfg));
    auto stats = std::make_shared<marmel::harness::HarnessStats>();
    return std::make_shared<marmel::orchestrator::OrchestratorManager>(
        marmel::orchestrator::OrchestratorManager::from_config(
            client, marmel::agent::Plan::default_plan(), stats, cfg));
}

} // namespace

int main(int argc, char** argv) {
    CliArgs args = parse_args(argc, argv);

    marmel::config::Config cfg;
    try {
        cfg = marmel::config::load(args.config);
    } catch (const std::exception& e) {
        std::cerr << "marmel: " << e.what() << "\n";
        return 1;
    }

    bool use_raw = args.raw || cfg.ui_mode == "raw" || !stdout_is_terminal();

    // Panic/terminal-restore hook: always restore the terminal first.
    std::signal(SIGINT, SIG_DFL);
    std::atexit([] { marmel::ui::restore(); });

    if (!use_raw) {
        // Mirrors Rust setup_panic_hook: resolve the log path through the
        // workspace owner (probe-validated), falling back to the literal.
        std::string log_path = ".marmel/marmel.log";
        try {
            auto workspace = marmel::harness::Workspace::create_default();
            log_path = workspace.log_path();
        } catch (const std::exception& e) {
            std::cerr << "[marmel] workspace unavailable (" << e.what() << "); using " << log_path
                      << "\n";
        }
        try {
            rotate_log(log_path, 5ULL * 1024 * 1024, 3);
        } catch (...) {
        }
    }
    if (args.debug) {
        std::cerr << "[marmel] debug logging enabled (backend=" << cfg.backend_url
                  << " model=" << cfg.model << ")\n";
    }

    // Boot MCP servers (ephemeral failure must not kill the session).
    try {
        if (!cfg.mcp_servers.empty()) {
            marmel::mcp::McpManager::boot(cfg.mcp_servers);
        }
    } catch (const std::exception& e) {
        std::cerr << "[marmel] mcp boot failed (continuing without MCP tools): " << e.what()
                  << "\n";
    }

    auto manager = boot_manager(cfg);
    if (use_raw) return marmel::ui::run_raw(cfg, args.prompt, manager);
    return marmel::ui::run_tui(cfg, args.prompt, manager);
}
