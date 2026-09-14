#pragma once
// MCP (Model Context Protocol) client: JSON-RPC 2.0 over stdio.
// Rust origin: src/mcp/{mod,client}.rs (8+383 lines).
//
// Minimal synchronous stdio MCP: spawn `command args` with piped stdin/stdout
// (stderr inherited), handshake initialize{protocolVersion 2024-11-05,
// capabilities.tools, clientInfo marmel/<version>} + notifications/initialized,
// tools/list (inputSchema default {"type":"object"}), tools/call with text
// concatenation (isError=true => error). 30s per-response timeout, mismatched
// ids ignored, EOF => closed error. `url` servers are registered but unused in
// v0.1 (stdio only). boot() is tolerant: spawn/list failures warn + continue.
// shutdown() SIGKILLs the child.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "marmel/config.hpp"
#include "marmel/json.hpp"

namespace marmel::mcp {

struct McpTool {
    std::string name;
    std::optional<std::string> description;
    Json input_schema = Json::object();
    std::string server_name;
};

class McpClient {
public:
    virtual ~McpClient() = default;
    virtual std::vector<McpTool> list_tools() = 0;
    virtual std::string call_tool(const std::string& name, const Json& args) = 0;
    virtual void shutdown() = 0;
    static std::shared_ptr<McpClient> spawn_stdio(const std::string& server_name,
                                                 const std::string& command,
                                                 const std::vector<std::string>& args,
                                                 const std::map<std::string, std::string>& env);
};

class McpManager {
public:
    McpManager();
    static McpManager boot(const std::map<std::string, McpServerConfig>& servers);
    std::vector<McpTool> tools() const;
    bool has_tool(const std::string& name) const;
    std::string call_tool(const std::string& name, const Json& args) const;
    void shutdown() const;

private:
    struct State {
        mutable std::mutex mutex;
        std::map<std::string, std::shared_ptr<McpClient>> clients;
        std::map<std::string, McpTool> tools;
    };
    std::shared_ptr<State> st_;
};

} // namespace marmel::mcp
