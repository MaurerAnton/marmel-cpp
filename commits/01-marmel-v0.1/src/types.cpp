// Rust origin: src/types.rs
#include "marmel/types.hpp"

#include <random>
#include <sstream>

namespace marmel::types {
namespace {

Json schema(const std::initializer_list<std::pair<std::string, Json>>& props,
            const std::initializer_list<std::string>& required) {
    Json::Object p;
    for (auto& kv : props) p.emplace(kv.first, kv.second);
    Json::Array r;
    for (auto& s : required) r.emplace_back(s);
    Json::Object o;
    o.emplace("type", Json("object"));
    o.emplace("properties", Json(std::move(p)));
    o.emplace("required", Json(std::move(r)));
    return Json(std::move(o));
}

Json str_prop(const std::string& desc = "") {
    if (desc.empty()) return Json::object({{"type", Json("string")}});
    return Json::object({{"type", Json("string")}, {"description", Json(desc)}});
}

} // namespace

Message Message::system(std::string content) {
    Message m;
    m.role = MsgRole::System;
    m.content = std::move(content);
    m.has_content = true;
    return m;
}
Message Message::user(std::string content) {
    Message m;
    m.role = MsgRole::User;
    m.content = std::move(content);
    m.has_content = true;
    return m;
}
Message Message::assistant(std::optional<std::string> content, std::optional<std::string> reasoning,
                           std::vector<ToolCall> calls) {
    Message m;
    m.role = MsgRole::Assistant;
    m.has_content = content.has_value();
    m.content = content.value_or("");
    m.reasoning_content = std::move(reasoning);
    m.tool_calls = std::move(calls);
    return m;
}
Message Message::tool(std::string tool_call_id, std::string content) {
    Message m;
    m.role = MsgRole::Tool;
    m.content = std::move(content);
    m.has_content = true;
    m.tool_call_id = std::move(tool_call_id);
    return m;
}

const char* Message::role_str() const {
    switch (role) {
        case MsgRole::System: return "system";
        case MsgRole::User: return "user";
        case MsgRole::Assistant: return "assistant";
        case MsgRole::Tool: return "tool";
    }
    return "user";
}

std::optional<std::string> Message::content_opt() const {
    if (role == MsgRole::Assistant && !has_content) return std::nullopt;
    return content;
}

Json Message::to_json(bool preserve_thinking) const {
    Json::Object o;
    o.emplace("role", Json(role_str()));
    switch (role) {
        case MsgRole::System:
        case MsgRole::User:
            o.emplace("content", Json(content));
            break;
        case MsgRole::Assistant: {
            if (has_content)
                o.emplace("content", Json(content));
            else
                o.emplace("content", Json());
            if (preserve_thinking && reasoning_content.has_value())
                o.emplace("reasoning_content", Json(*reasoning_content));
            if (!tool_calls.empty()) {
                Json::Array calls;
                for (auto& tc : tool_calls) {
                    Json::Object fn;
                    fn.emplace("name", Json(tc.name));
                    fn.emplace("arguments", Json(tc.arguments));
                    Json::Object e;
                    e.emplace("id", Json(tc.id));
                    e.emplace("type", Json("function"));
                    e.emplace("function", Json(std::move(fn)));
                    calls.emplace_back(Json(std::move(e)));
                }
                o.emplace("tool_calls", Json(std::move(calls)));
            }
            break;
        }
        case MsgRole::Tool:
            o.emplace("tool_call_id", Json(tool_call_id));
            o.emplace("content", Json(content));
            break;
    }
    return Json(std::move(o));
}

Json ToolDef::to_json() const {
    Json::Object fn;
    fn.emplace("name", Json(name));
    fn.emplace("description", Json(description));
    fn.emplace("parameters", parameters);
    Json::Object o;
    o.emplace("type", Json("function"));
    o.emplace("function", Json(std::move(fn)));
    return Json(std::move(o));
}

ToolDef ToolDef::delegate_task(const std::vector<std::string>& roles) {
    Json::Array en;
    for (auto& r : roles) en.emplace_back(r);
    Json::Object props;
    props.emplace("agent_name", Json::object({{"type", Json("string")},
                                              {"description",
                                               Json("Specialist role to dispatch the work to.")},
                                              {"enum", Json(std::move(en))}}));
    props.emplace("prompt", Json::object({{"type", Json("string")},
                                          {"description",
                                           Json("Self-contained task brief in English, scoped to "
                                                "the selected specialist's domain.")}}));
    props.emplace("snippets",
                  Json::object({{"type", Json("array")},
                                {"items", Json::object({{"type", Json("string")}})},
                                {"description",
                                 Json("Bounded list of relevant excerpts or file paths passed to "
                                      "the subagent as its isolated context.")}}));
    props.emplace("task_id",
                  str_prop("Optional execution_plan.md task id (e.g. t-042) enabling automatic "
                           "check-off on success."));
    props.emplace("image_urls",
                  Json::object({{"type", Json("array")},
                                {"items", Json::object({{"type", Json("string")}})},
                                {"description",
                                 Json("Optional image references for multimodal specialists.")}}));
    props.emplace("audio_urls",
                  Json::object({{"type", Json("array")},
                                {"items", Json::object({{"type", Json("string")}})},
                                {"description",
                                 Json("Optional audio references for audio specialists.")}}));
    Json::Object o;
    o.emplace("type", Json("object"));
    o.emplace("properties", Json(std::move(props)));
    o.emplace("required", Json::array({Json("agent_name"), Json("prompt")}));
    ToolDef d;
    d.name = "delegate_task";
    d.description =
        "Dispatch a bounded unit of domain work to a specialist subagent. The subagent receives "
        "only the brief and the supplied snippets, never the full conversation history.";
    d.parameters = Json(std::move(o));
    return d;
}

ToolDef ToolDef::create_plan() {
    ToolDef d;
    d.name = "create_plan";
    d.description = "Write or overwrite the workspace execution plan in .marmel/execution_plan.md.";
    d.parameters = schema({{"plan_markdown",
                            str_prop("Markdown formatted plan with - [ ] [t-xxx] tasks.")}},
                          {"plan_markdown"});
    return d;
}
ToolDef ToolDef::read_file() {
    ToolDef d;
    d.name = "read_file";
    d.description = "Read paginated UTF-8 text from a file by character offset and limit.";
    d.parameters = schema({{"path", str_prop("Relative path to the file.")},
                           {"offset",
                            str_prop("Starting character index (0-based, default: 0).")},
                           {"limit",
                            str_prop("Maximum number of characters to read (default: 8000, max: "
                                     "8000).")}},
                          {"path"});
    // Upstream offset/limit have prose-declared integer semantics; give them
    // the explicit JSON type the backend expects (shape otherwise verbatim).
    Json::Object props;
    props.emplace("path",
                  Json::object({{"type", Json("string")},
                                {"description", Json("Relative path to the file.")}}));
    props.emplace("offset",
                  Json::object({{"type", Json("integer")},
                                {"description",
                                 Json("Starting character index (0-based, default: 0).")}}));
    props.emplace("limit",
                  Json::object({{"type", Json("integer")},
                                {"description",
                                 Json("Maximum number of characters to read (default: 8000, max: "
                                      "8000).")}}));
    Json::Object o;
    o.emplace("type", Json("object"));
    o.emplace("properties", Json(std::move(props)));
    o.emplace("required", Json::array({Json("path")}));
    d.parameters = Json(std::move(o));
    return d;
}
ToolDef ToolDef::write_file() {
    ToolDef d;
    d.name = "write_file";
    d.description = "Create a new file or completely overwrite an existing file.";
    d.parameters = schema({{"path", str_prop("Relative path to the file.")},
                           {"content", str_prop("Full file content to write.")}},
                          {"path", "content"});
    return d;
}
ToolDef ToolDef::replace() {
    ToolDef d;
    d.name = "replace";
    d.description = "Replace an exact, unique block of text within a file. Fails if old_str "
                    "matches 0 or >1 times.";
    d.parameters = schema({{"path", str_prop("Relative path to the file.")},
                           {"old_str", str_prop("Exact text block to replace.")},
                           {"new_str", str_prop("New text block to insert.")}},
                          {"path", "old_str", "new_str"});
    return d;
}
ToolDef ToolDef::run_command() {
    ToolDef d;
    d.name = "run_command";
    d.description =
        "Execute a command line inside a dedicated PTY with timeout and process-group isolation.";
    d.parameters = schema({{"command", str_prop("The shell command to execute.")}}, {"command"});
    return d;
}
ToolDef ToolDef::grep_search() {
    ToolDef d;
    d.name = "grep_search";
    d.description = "Search for a regex pattern across workspace files honoring .gitignore.";
    Json::Object props;
    props.emplace("pattern", str_prop("Regex pattern to search."));
    props.emplace("path", str_prop("Subdirectory to restrict search."));
    props.emplace("max_results",
                  Json::object({{"type", Json("integer")},
                                {"description", Json("Maximum match count (default: 100).")}}));
    Json::Object o;
    o.emplace("type", Json("object"));
    o.emplace("properties", Json(std::move(props)));
    o.emplace("required", Json::array({Json("pattern")}));
    d.parameters = Json(std::move(o));
    return d;
}
ToolDef ToolDef::glob() {
    ToolDef d;
    d.name = "glob";
    d.description = "Find files matching a glob pattern.";
    d.parameters = schema({{"pattern", str_prop("Glob pattern (e.g. '**/*.rs').")}}, {"pattern"});
    return d;
}
ToolDef ToolDef::rebirth() {
    ToolDef d;
    d.name = "rebirth";
    d.description =
        "Compact conversation history into a structured checkpoint summary to preserve context "
        "budget.";
    d.parameters = schema({{"summary",
                            str_prop("Structured summary of accomplishments and current state.")}},
                          {"summary"});
    return d;
}
ToolDef ToolDef::archive_current_plan() {
    ToolDef d;
    d.name = "archive_current_plan";
    d.description =
        "Archive the current execution plan to `.marmel/archive/` once all tasks have been "
        "completed.";
    d.parameters = schema({}, {});
    return d;
}
ToolDef ToolDef::pty_spawn() {
    ToolDef d;
    d.name = "pty_spawn";
    d.description = "Spawn an interactive persistent PTY terminal session (e.g. for gdb, "
                    "interactive shells, or long-running commands).";
    d.parameters = schema({{"id", str_prop("Unique session identifier (e.g. 'dbg-1').")},
                           {"command",
                            str_prop("Command to run interactively (e.g. 'gdb ./target/debug/app').")},
                           {"rows", str_prop("Terminal rows (default: 24).")},
                           {"cols", str_prop("Terminal columns (default: 80).")}},
                          {"id", "command"});
    Json::Object props;
    props.emplace("id", Json::object({{"type", Json("string")},
                                      {"description",
                                       Json("Unique session identifier (e.g. 'dbg-1').")}}));
    props.emplace("command",
                  Json::object({{"type", Json("string")},
                                {"description",
                                 Json("Command to run interactively (e.g. 'gdb "
                                      "./target/debug/app').")}}));
    props.emplace("rows", Json::object({{"type", Json("integer")},
                                        {"description",
                                         Json("Terminal rows (default: 24).")}}));
    props.emplace("cols", Json::object({{"type", Json("integer")},
                                        {"description",
                                         Json("Terminal columns (default: 80).")}}));
    Json::Object o;
    o.emplace("type", Json("object"));
    o.emplace("properties", Json(std::move(props)));
    o.emplace("required", Json::array({Json("id"), Json("command")}));
    d.parameters = Json(std::move(o));
    return d;
}
ToolDef ToolDef::pty_write() {
    ToolDef d;
    d.name = "pty_write";
    d.description = "Send input text or commands to an active interactive PTY session.";
    d.parameters = schema({{"id", str_prop("Session identifier.")},
                           {"input", str_prop("Text or command string to write.")},
                           {"wait_ms",
                            str_prop("Milliseconds to wait for output (default: 300).")}},
                          {"id", "input"});
    Json::Object props;
    props.emplace("id", Json::object({{"type", Json("string")},
                                      {"description", Json("Session identifier.")}}));
    props.emplace("input", Json::object({{"type", Json("string")},
                                         {"description",
                                          Json("Text or command string to write.")}}));
    props.emplace("wait_ms",
                  Json::object({{"type", Json("integer")},
                                {"description",
                                 Json("Milliseconds to wait for output (default: 300).")}}));
    Json::Object o;
    o.emplace("type", Json("object"));
    o.emplace("properties", Json(std::move(props)));
    o.emplace("required", Json::array({Json("id"), Json("input")}));
    d.parameters = Json(std::move(o));
    return d;
}
ToolDef ToolDef::pty_read() {
    ToolDef d;
    d.name = "pty_read";
    d.description = "Read unread buffer output from an active interactive PTY session.";
    d.parameters = schema({{"id", str_prop("Session identifier.")},
                           {"wait_ms",
                            str_prop("Milliseconds to wait before reading (default: 0).")}},
                          {"id"});
    Json::Object props;
    props.emplace("id", Json::object({{"type", Json("string")},
                                      {"description", Json("Session identifier.")}}));
    props.emplace("wait_ms",
                  Json::object({{"type", Json("integer")},
                                {"description",
                                 Json("Milliseconds to wait before reading (default: 0).")}}));
    Json::Object o;
    o.emplace("type", Json("object"));
    o.emplace("properties", Json(std::move(props)));
    o.emplace("required", Json::array({Json("id")}));
    d.parameters = Json(std::move(o));
    return d;
}
ToolDef ToolDef::pty_close() {
    ToolDef d;
    d.name = "pty_close";
    d.description = "Close an active interactive PTY session and kill its process group.";
    d.parameters = schema({{"id", str_prop("Session identifier.")}}, {"id"});
    return d;
}
ToolDef ToolDef::pty_list() {
    ToolDef d;
    d.name = "pty_list";
    d.description = "List all active interactive PTY sessions.";
    d.parameters = schema({}, {});
    return d;
}
ToolDef ToolDef::leave_verdict() {
    ToolDef d;
    d.name = "leave_verdict";
    d.description = "Record the final verification verdict for this task. You must call this tool "
                    "to finish validation.";
    Json::Array en;
    en.emplace_back("APPROVED");
    en.emplace_back("REJECTED");
    Json::Object props;
    props.emplace("verdict", Json::object({{"type", Json("string")},
                                           {"enum", Json(std::move(en))},
                                           {"description", Json("The validation verdict.")}}));
    props.emplace("comments", str_prop("Mandatory feedback/critique."));
    Json::Object o;
    o.emplace("type", Json("object"));
    o.emplace("properties", Json(std::move(props)));
    o.emplace("required", Json::array({Json("verdict"), Json("comments")}));
    d.parameters = Json(std::move(o));
    return d;
}
ToolDef ToolDef::from_mcp(const std::string& name, const std::string& description,
                          const Json& input_schema) {
    ToolDef d;
    d.name = name;
    d.description = description;
    d.parameters = input_schema.is_object() ? input_schema : schema({}, {});
    return d;
}

std::vector<ToolDef> ToolDef::default_tools(const std::vector<std::string>& roles) {
    std::vector<std::string> r = roles.empty()
                                     ? std::vector<std::string>{"coder", "researcher", "debugger",
                                                               "validator", "generalist"}
                                     : roles;
    return {delegate_task(r), create_plan(), archive_current_plan(), read_file(), write_file(),
            replace(), run_command(), grep_search(), glob(), pty_spawn(), pty_write(), pty_read(),
            pty_close(), pty_list(), rebirth(), leave_verdict()};
}

Json ChatRequest::to_json(bool preserve_thinking) const {
    Json::Object o;
    o.emplace("model", Json(model));
    Json::Array msgs;
    for (auto& m : messages) msgs.emplace_back(m.to_json(preserve_thinking));
    o.emplace("messages", Json(std::move(msgs)));
    if (temperature) o.emplace("temperature", Json(*temperature));
    if (top_p) o.emplace("top_p", Json(*top_p));
    if (frequency_penalty) o.emplace("frequency_penalty", Json(*frequency_penalty));
    if (presence_penalty) o.emplace("presence_penalty", Json(*presence_penalty));
    if (stream) o.emplace("stream", Json(*stream));
    if (enable_thinking) o.emplace("enable_thinking", Json(*enable_thinking));
    if (tools) {
        Json::Array t;
        for (auto& d : *tools) t.emplace_back(d.to_json());
        o.emplace("tools", Json(std::move(t)));
    }
    return Json(std::move(o));
}

ChatChunk ChatChunk::from_json(const Json& v) {
    ChatChunk c;
    if (v.contains("id") && v.at("id").is_string()) c.id = v.at("id").as_string();
    if (!v.contains("choices") || !v.at("choices").is_array()) return c;
    for (auto& ch : v.at("choices").as_array()) {
        if (!ch.is_object()) continue;
        ChunkChoice choice;
        if (ch.contains("finish_reason") && ch.at("finish_reason").is_string())
            choice.finish_reason = ch.at("finish_reason").as_string();
        if (ch.contains("delta") && ch.at("delta").is_object()) {
            const Json& d = ch.at("delta");
            if (d.contains("content") && d.at("content").is_string())
                choice.delta.content = d.at("content").as_string();
            auto rc = d.contains("reasoning_content") ? "reasoning_content"
                      : d.contains("reasoning")       ? "reasoning"
                                                      : nullptr;
            if (rc && d.at(rc).is_string()) choice.delta.reasoning_content = d.at(rc).as_string();
            if (d.contains("tool_calls") && d.at("tool_calls").is_array()) {
                std::vector<ChunkToolCall> tcs;
                for (auto& t : d.at("tool_calls").as_array()) {
                    if (!t.is_object()) continue;
                    ChunkToolCall tc;
                    if (t.contains("index") && t.at("index").is_number())
                        tc.index = static_cast<std::size_t>(t.at("index").as_int(0));
                    if (t.contains("id") && t.at("id").is_string()) tc.id = t.at("id").as_string();
                    if (t.contains("function") && t.at("function").is_object()) {
                        ChunkToolFunction fn;
                        const Json& f = t.at("function");
                        if (f.contains("name") && f.at("name").is_string())
                            fn.name = f.at("name").as_string();
                        if (f.contains("arguments") && f.at("arguments").is_string())
                            fn.arguments = f.at("arguments").as_string();
                        tc.function = std::move(fn);
                    }
                    tcs.push_back(std::move(tc));
                }
                choice.delta.tool_calls = std::move(tcs);
            }
        }
        c.choices.push_back(std::move(choice));
    }
    return c;
}

std::vector<ToolCall> map_to_tool_calls(const std::map<std::size_t, ChunkToolCall>& acc) {
    std::vector<ToolCall> out;
    for (auto& [idx, tc] : acc) {
        (void)idx;
        ToolCall call;
        call.id = tc.id.value_or("");
        if (call.id.empty()) call.id = "call_" + generate_uuid_v4();
        if (tc.function && tc.function->name) call.name = *tc.function->name;
        if (tc.function && tc.function->arguments) call.arguments = *tc.function->arguments;
        if (call.arguments.empty()) call.arguments = "{}";
        out.push_back(std::move(call));
    }
    return out;
}

std::string generate_uuid_v4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<unsigned long long> dist;
    unsigned long long hi = dist(rng), lo = dist(rng);
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL; // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL; // variant 10
    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08llx-%04llx-%04llx-%04llx-%012llx",
                  (hi >> 32) & 0xFFFFFFFFULL, (hi >> 16) & 0xFFFFULL, hi & 0xFFFFULL,
                  (lo >> 48) & 0xFFFFULL, lo & 0xFFFFFFFFFFFFULL);
    return std::string(buf);
}

} // namespace marmel::types
