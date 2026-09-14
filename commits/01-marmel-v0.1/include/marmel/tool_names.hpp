#pragma once
// Tool-name constants. Rust origin: src/tool_names.rs (57 lines).
// Single audit point: dispatchers, allowlists and the registry must compare
// through these constants. TERMINAL__* are the namespaced aliases accepted
// by the harness normalizer (incl. list_directory, which has no TOOL_*
// counterpart in v0.1).

#include <string_view>

namespace marmel::tool_names {

inline constexpr std::string_view kDelegateTask = "delegate_task";
inline constexpr std::string_view kReadFile = "read_file";
inline constexpr std::string_view kWriteFile = "write_file";
inline constexpr std::string_view kReplace = "replace";
inline constexpr std::string_view kRunCommand = "run_command";
inline constexpr std::string_view kGrepSearch = "grep_search";
inline constexpr std::string_view kGlob = "glob";
inline constexpr std::string_view kCreatePlan = "create_plan";
inline constexpr std::string_view kArchivePlan = "archive_current_plan";
inline constexpr std::string_view kRebirth = "rebirth";
inline constexpr std::string_view kPtySpawn = "pty_spawn";
inline constexpr std::string_view kPtyWrite = "pty_write";
inline constexpr std::string_view kPtyRead = "pty_read";
inline constexpr std::string_view kPtyClose = "pty_close";
inline constexpr std::string_view kPtyList = "pty_list";
inline constexpr std::string_view kLeaveVerdict = "leave_verdict";

inline constexpr std::string_view kTerminalReadFile = "terminal__read_file";
inline constexpr std::string_view kTerminalWriteFile = "terminal__write_file";
inline constexpr std::string_view kTerminalReplace = "terminal__replace";
inline constexpr std::string_view kTerminalRunCommand = "terminal__run_command";
inline constexpr std::string_view kTerminalGrepSearch = "terminal__grep_search";
inline constexpr std::string_view kTerminalGlob = "terminal__glob";
inline constexpr std::string_view kTerminalListDirectory = "terminal__list_directory";

} // namespace marmel::tool_names
