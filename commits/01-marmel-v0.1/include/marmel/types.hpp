#pragma once
// OpenAI-compatible wire types + tool catalogue.
// Rust origin: src/types.rs (504 lines).
//
// Design (author's chain-of-thought, preserved):
//  - Single source of truth for the chat transcript JSON; Message is
//    externally tagged by "role" (system/user/assistant/tool).
//  - Assistant.content is nullable so pure tool-call turns are legal.
//  - reasoning_content is first-class (DeepSeek/Qwen style) with the
//    "reasoning" alias accepted on the streaming path.
//  - ChatRequest omits nullopt optionals so backend defaults apply.
//  - Each ToolDef embeds a static JSON-Schema; only delegate_task injects
//    a dynamic `enum` from the live role list.
//  - default_tools() hardcodes the canonical 16-tool set AND order —
//    tests and the harness depend on both.
//  - Streaming ChunkToolCall fragments accumulate by `index` until
//    finish_reason == "tool_calls".

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "marmel/json.hpp"

namespace marmel::types {

enum class MsgRole { System, User, Assistant, Tool };

struct ToolCall {
    std::string id;
    std::string name;       // function.name
    std::string arguments;  // JSON-encoded STRING (not object), as on the wire
};

struct Message {
    MsgRole role = MsgRole::User;
    std::string content;                          // system/user/tool content; assistant text ("" if none)
    bool has_content = true;                      // false => serialize assistant content as null
    std::optional<std::string> reasoning_content; // assistant only
    std::vector<ToolCall> tool_calls;             // assistant only
    std::string tool_call_id;                     // tool role only

    static Message system(std::string content);
    static Message user(std::string content);
    static Message assistant(std::optional<std::string> content,
                             std::optional<std::string> reasoning = std::nullopt,
                             std::vector<ToolCall> calls = {});
    static Message tool(std::string tool_call_id, std::string content);

    const char* role_str() const;
    std::optional<std::string> content_opt() const;
    Json to_json() const;
};

struct ToolDef {
    std::string name;
    std::string description;
    Json parameters; // JSON-Schema object

    Json to_json() const;

    // Factories (mirrors ToolDef::xxx() in types.rs).
    static ToolDef delegate_task(const std::vector<std::string>& roles);
    static ToolDef create_plan();
    static ToolDef read_file();
    static ToolDef write_file();
    static ToolDef replace();
    static ToolDef run_command();
    static ToolDef grep_search();
    static ToolDef glob();
    static ToolDef rebirth();
    static ToolDef archive_current_plan();
    static ToolDef pty_spawn();
    static ToolDef pty_write();
    static ToolDef pty_read();
    static ToolDef pty_close();
    static ToolDef pty_list();
    static ToolDef leave_verdict();
    static ToolDef from_mcp(const std::string& name, const std::string& description, const Json& input_schema);
    static std::vector<ToolDef> default_tools(const std::vector<std::string>& roles = {});
};

struct ChatRequest {
    std::string model;
    std::vector<Message> messages;
    std::optional<float> temperature;
    std::optional<float> top_p;
    std::optional<float> frequency_penalty;
    std::optional<float> presence_penalty;
    std::optional<bool> stream;
    std::optional<bool> enable_thinking;
    std::optional<std::vector<ToolDef>> tools;

    Json to_json() const;
};

// SSE streaming fragments (Deserialize in Rust).
struct ChunkToolFunction {
    std::optional<std::string> name;
    std::optional<std::string> arguments;
};
struct ChunkToolCall {
    std::size_t index = 0;
    std::optional<std::string> id;
    std::optional<ChunkToolFunction> function;
};
struct ChunkDelta {
    std::optional<std::string> content;
    std::optional<std::string> reasoning_content;
    std::optional<std::vector<ChunkToolCall>> tool_calls;
};
struct ChunkChoice {
    ChunkDelta delta;
    std::optional<std::string> finish_reason;
};
struct ChatChunk {
    std::optional<std::string> id;
    std::vector<ChunkChoice> choices;
    static ChatChunk from_json(const Json& v);
};

/// Reassemble accumulated (index -> (id, name, args)) fragments into ToolCalls.
/// Missing ids become "call_<uuid>" on the client path (see llm/client).
std::vector<ToolCall> map_to_tool_calls(const std::map<std::size_t, ChunkToolCall>& acc);

std::string generate_uuid_v4();

} // namespace marmel::types
