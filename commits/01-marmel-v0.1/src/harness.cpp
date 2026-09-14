// Rust origin: src/harness/{mod,fs,pty,search,workspace}.rs
// (monitor.rs lives in harness_monitor.cpp)
#include "marmel/harness.hpp"

#include "marmel/agent_context.hpp"
#include "marmel/agents.hpp"
#include "marmel/orchestrator.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#ifdef __unix__
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace marmel::harness {
namespace {

namespace fs = std::filesystem;

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// --- UTF-8 helpers ---------------------------------------------------------------
bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
std::string utf8_lossy(const std::string& bytes) {
    std::string out;
    for (std::size_t i = 0; i < bytes.size();) {
        unsigned char c = bytes[i];
        std::size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else {
            out += "\xEF\xBF\xBD";
            i++;
            continue;
        }
        if (i + len > bytes.size()) {
            out += "\xEF\xBF\xBD";
            break;
        }
        bool ok = true;
        for (std::size_t k = 1; k < len; k++)
            if (!is_cont(static_cast<unsigned char>(bytes[i + k]))) ok = false;
        if (!ok) {
            out += "\xEF\xBF\xBD";
            i++;
            continue;
        }
        out.append(bytes, i, len);
        i += len;
    }
    return out;
}
std::size_t char_count(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s)
        if (!is_cont(c)) n++;
    return n;
}
std::string chars_slice(const std::string& s, std::size_t start, std::size_t len) {
    std::string out;
    std::size_t idx = 0, i = 0;
    while (i < s.size() && idx < start) {
        if (!is_cont(static_cast<unsigned char>(s[i]))) idx++;
        i++;
    }
    std::size_t taken = 0;
    while (i < s.size() && taken < len) {
        if (!is_cont(static_cast<unsigned char>(s[i]))) taken++;
        out += s[i++];
    }
    return out;
}
// Back `pos` down to a char boundary (for head truncation).
std::size_t back_to_boundary(const std::string& s, std::size_t pos) {
    if (pos > s.size()) pos = s.size();
    while (pos > 0 && is_cont(static_cast<unsigned char>(s[pos]))) pos--;
    return pos;
}
// Forward `pos` up to a char boundary (for tail truncation).
std::size_t fwd_to_boundary(const std::string& s, std::size_t pos) {
    if (pos > s.size()) pos = s.size();
    while (pos < s.size() && is_cont(static_cast<unsigned char>(s[pos]))) pos++;
    return pos;
}

// --- ToolError ----------------------------------------------------------------------

// --- arg helpers (fs.rs str_arg/usize_arg, shared by pty/search) -----------------------
const char* str_field(const Json& args, std::initializer_list<const char*> keys) {
    if (!args.is_object()) return nullptr;
    for (auto k : keys) {
        auto it = args.as_object().find(k);
        if (it != args.as_object().end() && it->second.is_string()) return it->second.as_string().c_str();
    }
    return nullptr;
}

} // namespace

std::string ToolError::message() const {
    switch (kind) {
        case Kind::UnknownTool: return "unknown tool: " + tool;
        case Kind::BadArguments: return "invalid arguments for " + tool + ": " + detail;
        case Kind::Forbidden:
            return "tool `" + tool + "` is forbidden for caller `" + caller +
                   "` by orchestration policy";
        case Kind::Execution: return detail;
    }
    return detail;
}
ToolError ToolError::unknown(std::string tool) { return {Kind::UnknownTool, std::move(tool), {}, {}}; }
ToolError ToolError::bad_arguments(std::string tool, std::string detail) {
    return {Kind::BadArguments, std::move(tool), std::move(detail), {}};
}
ToolError ToolError::forbidden(std::string tool, std::string caller) {
    return {Kind::Forbidden, std::move(tool), {}, std::move(caller)};
}
ToolError ToolError::execution(std::string detail) { return {Kind::Execution, {}, std::move(detail), {}}; }

// --- Workspace --------------------------------------------------------------------------
Workspace Workspace::default_workspace() { return Workspace(agent::kMarmelDir); }
Workspace Workspace::create_default() {
    Workspace w(agent::kMarmelDir);
    w.ensure_writable();
    return w;
}
std::string Workspace::plan_path() const { return root_ + "/" + agent::kPlanFile; }
std::string Workspace::log_path() const { return root_ + "/marmel.log"; }
std::string Workspace::forced_phase_path() const { return root_ + "/" + agent::kForcedPhaseFile; }
std::string Workspace::archive_dir() const { return root_ + "/archive"; }
void Workspace::ensure_writable() const {
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (ec) throw std::runtime_error("creating workspace dir " + root_ + ": " + ec.message());
#ifdef __unix__
    long long pid = static_cast<long long>(::getpid());
#else
    // No getpid off-unix; probe uniqueness falls back to a timestamp.
    long long pid = static_cast<long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    std::string probe = root_ + "/.marmel_probe_" + std::to_string(pid);
    {
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("workspace " + root_ + " is not writable");
        f << "marmel-write-probe";
    }
    fs::remove(probe, ec);
    if (ec) throw std::runtime_error("cleaning up probe " + probe + ": " + ec.message());
}

// --- fs.rs ----------------------------------------------------------------------------------
std::string map_path(const std::string& path) {
    constexpr char kPrefix[] = "/home/coder/workspace";
    std::error_code ec;
    std::string cwd = fs::current_path(ec).string();
    if (ec) cwd = ".";
    if (path.rfind(kPrefix, 0) == 0) {
        std::string rel = path.substr(sizeof(kPrefix) - 1);
        while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
        if (rel.empty()) return cwd;
        return (fs::path(cwd) / rel).string();
    }
    return path;
}

namespace {

std::string os_error_text() {
    int e = errno;
    if (e == 0) return "I/O error";
    return std::string(std::strerror(e)) + " (os error " + std::to_string(e) + ")";
}

/// Required string arg with model-paraphrase aliases. Throws HardToolError
/// (Rust `?` on BadArguments) when absent — mirrors str_arg call sites.
std::string req_str(const Json& args, const char* key, const char* tool,
                    std::initializer_list<const char*> aliases = {}) {
    if (args.is_object()) {
        auto it = args.as_object().find(key);
        if (it != args.as_object().end() && it->second.is_string()) return it->second.as_string();
        for (auto a : aliases) {
            auto jt = args.as_object().find(a);
            if (jt != args.as_object().end() && jt->second.is_string())
                return jt->second.as_string();
        }
    }
    throw_hard(ToolError::bad_arguments(tool, std::string("missing string field `") + key + "`"));
}

/// Strict integer arg (read_file/grep): present-but-not-u64 throws.
std::size_t req_usize(const Json& args, const char* key, std::size_t def, const char* tool) {
    if (!args.is_object() || !args.contains(key)) return def;
    const Json& v = args.at(key);
    double d = v.as_double(-1.0);
    if (!v.is_number() || d < 0 || d != static_cast<long long>(d))
        throw_hard(ToolError::bad_arguments(tool, std::string("field `") + key + "` must be an integer"));
    return static_cast<std::size_t>(d);
}

/// Lenient integer arg (pty rows/cols/wait_ms): invalid values fall back to
/// the default, mirroring `and_then(as_u64).unwrap_or(def)`.
std::size_t opt_u64(const Json& args, const char* key, std::size_t def) {
    if (!args.is_object() || !args.contains(key)) return def;
    const Json& v = args.at(key);
    double d = v.as_double(-1.0);
    if (!v.is_number() || d < 0 || d != static_cast<long long>(d)) return def;
    return static_cast<std::size_t>(d);
}

std::string trim_sv(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

} // namespace

ToolResult fs_read_file(const Json& args) {
    std::string path =
        req_str(args, "path", "read_file",
                {"file_path", "filepath", "file", "filename", "target", "target_file"});
    std::size_t offset = req_usize(args, "offset", 0, "read_file");
    std::size_t limit = std::min(req_usize(args, "limit", 8000, "read_file"),
                                 static_cast<std::size_t>(8000));

    errno = 0;
    std::ifstream f(map_path(path), std::ios::binary);
    if (!f) throw_hard(ToolError::execution(os_error_text()));
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = utf8_lossy(ss.str());
    std::size_t total = char_count(content);
    std::size_t start = std::min(offset, total);
    std::size_t end = std::min(start + limit, total);
    std::string out = chars_slice(content, start, end - start);
    if (end < total)
        out += "\n\n[Showing characters " + std::to_string(start) + "-" + std::to_string(end) +
               " of " + std::to_string(total) + ". Use offset=" + std::to_string(end) +
               " to read next chunk]";
    return ToolResult::ok(out);
}

ToolResult fs_replace(const Json& args) {
    std::string path_s =
        req_str(args, "path", "replace",
                {"file_path", "filepath", "file", "filename", "target", "target_file"});
    std::string old_str =
        req_str(args, "old_str", "replace", {"target", "search", "find", "old", "target_content"});
    std::string new_str = req_str(args, "new_str", "replace",
                                  {"replacement", "replace", "new", "replacement_content"});

    errno = 0;
    std::ifstream f(map_path(path_s), std::ios::binary);
    if (!f) throw_hard(ToolError::execution(os_error_text()));
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    std::size_t count = 0, pos = 0;
    if (!old_str.empty()) {
        while ((pos = content.find(old_str, pos)) != std::string::npos) {
            count++;
            pos += old_str.size();
        }
    } else {
        // Rust "".matches("") semantics: empty needle matches at every char
        // boundary (chars+1 matches); empty file matches exactly once.
        count = char_count(content) + 1;
    }
    if (count == 0) return ToolResult::err("old_str not found in file: " + path_s);
    if (count > 1)
        return ToolResult::err("old_str is ambiguous (matches " + std::to_string(count) +
                               " times in " + path_s + "). Provide more unique surrounding context.");
    std::string updated = content;
    updated.replace(updated.find(old_str), old_str.size(), new_str);
    fs::path p(map_path(path_s));
    fs::path parent = p.parent_path();
    std::string parent_s = parent.empty() ? "." : parent.string();
#ifdef __unix__
    long long pid = static_cast<long long>(::getpid());
#else
    long long pid = static_cast<long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    std::string tmp = parent_s + "/." + p.filename().string() + ".tmp." + std::to_string(pid);
    errno = 0;
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) throw_hard(ToolError::execution(os_error_text()));
        o << updated;
        if (!o) throw_hard(ToolError::execution(os_error_text()));
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        throw_hard(ToolError::execution(ec.message() + " (os error " + std::to_string(ec.value()) + ")"));
    }
    return ToolResult::ok("replace applied");
}

ToolResult fs_write_file(const Json& args) {
    std::string path_s =
        req_str(args, "path", "write_file",
                {"file_path", "filepath", "file", "filename", "target", "target_file"});
    std::string body = req_str(args, "content", "write_file",
                               {"contents", "text", "code", "body", "file_content"});
    fs::path p(map_path(path_s));
    if (!p.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (ec)
            throw_hard(ToolError::execution(ec.message() + " (os error " +
                                            std::to_string(ec.value()) + ")"));
#ifdef __unix__
        ::chmod(p.parent_path().c_str(), 0755); // best-effort, errors ignored (as in Rust)
#endif
    }
    errno = 0;
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (!o) throw_hard(ToolError::execution(os_error_text()));
    o << body;
    if (!o) throw_hard(ToolError::execution(os_error_text()));
    return ToolResult::ok("wrote " + std::to_string(body.size()) + " bytes to " + path_s);
}

// --- search.rs -----------------------------------------------------------------------------------
namespace {
std::vector<std::string> load_gitignore() {
    std::vector<std::string> pats;
    std::ifstream f(".gitignore");
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = line;
        std::size_t a = t.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        t = t.substr(a);
        if (t.empty() || t[0] == '#') continue;
        pats.push_back(t);
    }
    return pats;
}

// Minimal gitignore matcher: *, ?, **, trailing / (dir), leading / (anchored),
// `!` negation (last match wins). Good enough for the v0.1 test-suite.
bool fnmatch_pat(const std::string& pat, const std::string& s) {
    std::string re = "^";
    for (std::size_t i = 0; i < pat.size();) {
        char c = pat[i];
        if (c == '*') {
            if (i + 1 < pat.size() && pat[i + 1] == '*') {
                re += ".*";
                i += 2;
                if (i < pat.size() && pat[i] == '/') i++;
            } else {
                re += "[^/]*";
                i++;
            }
        } else if (c == '?') {
            re += "[^/]";
            i++;
        } else if (c == '.') {
            re += "\\.";
            i++;
        } else if (c == '/') {
            re += "/";
            i++;
        } else if (std::string("+()^$.{}|[]\\").find(c) != std::string::npos) {
            re += '\\';
            re += c;
            i++;
        } else {
            re += c;
            i++;
        }
    }
    re += "$";
    try {
        return std::regex_match(s, std::regex(re));
    } catch (...) {
        return false;
    }
}
bool ignored_rel(const std::string& rel, bool is_dir, const std::vector<std::string>& pats) {
    bool ignored = false;
    for (auto& raw : pats) {
        bool neg = !raw.empty() && raw[0] == '!';
        std::string p = neg ? raw.substr(1) : raw;
        bool dir_only = !p.empty() && p.back() == '/';
        if (dir_only) p.pop_back();
        bool anchored = p.find('/') != std::string::npos;
        std::string target = anchored ? rel : (rel.find('/') == std::string::npos
                                                   ? rel
                                                   : rel.substr(rel.find_last_of('/') + 1));
        if (dir_only && !is_dir) {
            // `dir/` also ignores everything beneath it.
            if (rel != p && rel.rfind(p + "/", 0) != 0) continue;
            ignored = !neg;
            continue;
        }
        if (fnmatch_pat(p, target) || (!anchored && fnmatch_pat("**/" + p, rel))) ignored = !neg;
    }
    return ignored;
}

std::string glob_to_regex_str(const std::string& pattern) {
    std::string re = "^";
    for (std::size_t i = 0; i < pattern.size();) {
        char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                i += 2;
                if (i < pattern.size() && pattern[i] == '/') {
                    i++;
                    re += "(?:.*/)?";
                } else {
                    re += ".*";
                }
            } else {
                re += "[^/]*";
                i++;
            }
        } else if (c == '?') {
            re += "[^/]";
            i++;
        } else if (c == '.') {
            re += "\\.";
            i++;
        } else if (c == '/') {
            re += "/";
            i++;
        } else {
            static const std::string kMeta = "+()^$.{}|[]\\";
            if (kMeta.find(c) != std::string::npos) re += '\\';
            re += c;
            i++;
        }
    }
    return re + "$";
}
} // namespace

ToolResult search_grep(const Json& args) {
    std::string pat =
        req_str(args, "pattern", "grep_search", {"query", "search", "glob", "regex"});
    std::string root = ".";
    if (args.is_object()) {
        auto it = args.as_object().find("path");
        if (it != args.as_object().end() && it->second.is_string()) root = it->second.as_string();
    }
    std::size_t max_results = std::min(req_usize(args, "max_results", 100, "grep_search"),
                                       static_cast<std::size_t>(500));

    std::regex re;
    try {
        re = std::regex(pat);
    } catch (const std::regex_error& e) {
        throw_hard(ToolError::bad_arguments("grep_search",
                                            std::string("invalid regex: ") + e.what()));
    }
    auto pats = load_gitignore();
    std::vector<std::string> results;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return ToolResult::ok("no matches");
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::string p = it->path().string();
        bool is_dir = it->is_directory(ec);
        std::string rel = fs::relative(it->path(), root, ec).string();
        if (ec) rel = p;
        if (rel == ".") continue;
        if (rel.rfind(".git/", 0) == 0 || rel == ".git") {
            if (is_dir) it.disable_recursion_pending();
            continue;
        }
        if (ignored_rel(rel, is_dir, pats)) {
            if (is_dir) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        std::ifstream f(it->path(), std::ios::binary);
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string content = utf8_lossy(ss.str());
        if (content.find('\0') != std::string::npos) continue;
        std::istringstream lines(content);
        std::string line;
        std::size_t idx = 0;
        while (std::getline(lines, line)) {
            idx++;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            bool hit = false;
            try {
                hit = std::regex_search(line, re);
            } catch (...) {
                hit = false;
            }
            if (hit) {
                results.push_back(p + ":" + std::to_string(idx) + ": " + line);
                if (results.size() >= max_results) {
                    std::string out;
                    for (std::size_t i = 0; i < results.size(); i++) {
                        if (i) out += '\n';
                        out += results[i];
                    }
                    return ToolResult::ok(out);
                }
            }
        }
    }
    if (results.empty()) return ToolResult::ok("no matches");
    std::string out;
    for (std::size_t i = 0; i < results.size(); i++) {
        if (i) out += '\n';
        out += results[i];
    }
    return ToolResult::ok(out);
}

ToolResult search_glob(const Json& args) {
    std::string pattern =
        req_str(args, "pattern", "glob", {"query", "search", "glob", "regex"});
    std::regex re;
    try {
        re = std::regex(glob_to_regex_str(pattern));
    } catch (...) {
        return ToolResult::ok("no matches");
    }
    auto pats = load_gitignore();
    std::vector<std::string> matches;
    std::error_code ec;
    fs::recursive_directory_iterator it(".", fs::directory_options::skip_permission_denied, ec), end;
    if (!ec) {
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            bool is_dir = it->is_directory(ec);
            std::string rel = fs::relative(it->path(), ".", ec).string();
            if (ec) continue;
            if (rel == ".") continue;
            while (rel.rfind("./", 0) == 0) rel.erase(0, 2);
            if (rel.rfind(".git/", 0) == 0 || rel == ".git") {
                if (is_dir) it.disable_recursion_pending();
                continue;
            }
            if (ignored_rel(rel, is_dir, pats)) {
                if (is_dir) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            try {
                if (std::regex_match(rel, re)) matches.push_back(rel);
            } catch (...) {
            }
        }
    }
    std::sort(matches.begin(), matches.end());
    if (matches.size() > 500) matches.resize(500);
    if (matches.empty()) return ToolResult::ok("no matches");
    std::string out;
    for (std::size_t i = 0; i < matches.size(); i++) {
        if (i) out += '\n';
        out += matches[i];
    }
    return ToolResult::ok(out);
}

// --- pty.rs --------------------------------------------------------------------------------------
std::string sanitize_terminal_output(const std::string& raw) {
    // Strip OSC sequences: ESC ] <digits> ; ... (BEL | ESC \).
    std::string no_osc;
    for (std::size_t i = 0; i < raw.size();) {
        if (raw[i] == '\x1b' && i + 1 < raw.size() && raw[i + 1] == ']') {
            std::size_t j = i + 2;
            while (j < raw.size() && std::isdigit(static_cast<unsigned char>(raw[j]))) j++;
            if (j < raw.size() && raw[j] == ';') {
                j++;
                bool closed = false;
                while (j < raw.size()) {
                    if (raw[j] == '\x07') {
                        j++;
                        closed = true;
                        break;
                    }
                    if (raw[j] == '\x1b' && j + 1 < raw.size() && raw[j + 1] == '\\') {
                        j += 2;
                        closed = true;
                        break;
                    }
                    j++;
                }
                if (closed) {
                    i = j;
                    continue;
                }
            }
        }
        no_osc += raw[i++];
    }
    std::string out;
    for (char c : no_osc) {
        unsigned char u = c;
        if (c == '\x07' || c == '\x08') continue;
        if (u < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\x1b') continue;
        out += c;
    }
    return out;
}

void kill_process_group(std::int32_t pid) {
#ifdef __unix__
    if (pid <= 0) return;
    ::kill(-pid, SIGKILL);
    if (::kill(pid, 0) == 0) ::kill(pid, SIGKILL);
#else
    (void)pid;
#endif
}

namespace {
#ifdef __unix__
std::string wrapped_one_shot(const std::string& cmd) {
    return "stty -echo; ulimit -f 4194304 2>/dev/null || ulimit -f 2097152 2>/dev/null; " + cmd;
}
std::string wrapped_interactive(const std::string& cmd) {
    return "stty -echo 2>/dev/null || true; ulimit -f 4194304 2>/dev/null || "
           "ulimit -f 2097152 2>/dev/null; " +
           cmd;
}
#endif
} // namespace

std::string run_command_pty(const std::string& command, std::chrono::seconds timeout) {
#ifdef __unix__
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, nullptr);
    if (pid < 0) return "failed to spawn pty";
    if (pid == 0) {
        std::string wrapped = wrapped_one_shot(command);
        ::execl("/bin/sh", "sh", "-c", wrapped.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    std::string output;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timed_out = false;
    int status = 0;
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(master, &rfds);
        struct timeval tv { 0, 100000 };
        int r = ::select(master + 1, &rfds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(master, &rfds)) {
            char buf[4096];
            ssize_t n = ::read(master, buf, sizeof(buf));
            if (n <= 0) break;
            output.append(buf, static_cast<std::size_t>(n));
        }
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            // Drain remaining output.
            int flags = ::fcntl(master, F_GETFL, 0);
            ::fcntl(master, F_SETFL, flags | O_NONBLOCK);
            while (true) {
                char buf[4096];
                ssize_t n = ::read(master, buf, sizeof(buf));
                if (n <= 0) break;
                output.append(buf, static_cast<std::size_t>(n));
            }
            break;
        }
    }
    if (timed_out) {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        ::close(master);
        return "[command timed out after " + std::to_string(timeout.count()) + "s and was killed]";
    }
    ::close(master);
    std::string clean = sanitize_terminal_output(output);
    while (!clean.empty() && clean.back() == '\n') clean.pop_back();
    return clean;
#else
    (void)timeout;
    std::string out;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) return "failed to spawn command";
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    ::pclose(pipe);
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
#endif
}

ToolResult pty_run_command(const Json& args) {
    std::string cmd =
        req_str(args, "command", "run_command", {"cmd", "script", "exec", "command_line"});
    // NOTE: a `timeout` argument is accepted for forward-compatibility but the
    // timeout is strict — always DEFAULT_TIMEOUT_SECS (300 s).
    return ToolResult::ok(run_command_pty(cmd, std::chrono::seconds(300)));
}

// --- interactive PTY manager -----------------------------------------------------------------------
namespace {
struct InteractiveSession {
    std::string id;
    int master = -1;
    pid_t pid = -1;
    std::string output;
    std::size_t cursor = 0;
    bool alive = true;
    std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
    std::mutex mutex;
};
struct PtyManagerState {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<InteractiveSession>> sessions;
};
PtyManagerState& pty_state() {
    static PtyManagerState s;
    return s;
}
void reap_idle(PtyManagerState& st) {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> dead;
    for (auto& [id, sess] : st.sessions) {
        std::lock_guard<std::mutex> l(sess->mutex);
        if (now - sess->last_activity > std::chrono::seconds(300)) dead.push_back(id);
    }
    for (auto& id : dead) {
        auto it = st.sessions.find(id);
        if (it == st.sessions.end()) continue;
        auto sess = it->second;
        st.sessions.erase(it);
        std::cerr << "[marmel] PTY session '" << id << "' timed out after 300s of inactivity. Reaping.\n";
#ifdef __unix__
        if (sess->pid > 0) {
            ::kill(-sess->pid, SIGKILL);
            ::kill(sess->pid, SIGKILL);
        }
        if (sess->master >= 0) ::close(sess->master);
#endif
    }
}
std::string take_delta(const std::shared_ptr<InteractiveSession>& sess) {
    std::lock_guard<std::mutex> l(sess->mutex);
    std::string delta = sess->output.substr(sess->cursor);
    sess->cursor = sess->output.size();
    sess->last_activity = std::chrono::steady_clock::now();
    return sanitize_terminal_output(delta);
}
/// Session id with the exact upstream lookup + BadArguments detail.
std::string session_id_of(const Json& args, const char* tool) {
    if (args.is_object()) {
        for (auto k : {"id", "session_id"}) {
            auto it = args.as_object().find(k);
            if (it != args.as_object().end() && it->second.is_string())
                return it->second.as_string();
        }
    }
    throw_hard(ToolError::bad_arguments(tool, "missing string field `id` or `session_id`"));
}
} // namespace

ToolResult pty_spawn(const Json& args) {
#ifdef __unix__
    std::string sid = session_id_of(args, "pty_spawn");
    if (!(args.is_object() && args.contains("command") && args.at("command").is_string()))
        throw_hard(ToolError::bad_arguments("pty_spawn", "missing string field `command`"));
    std::string command = args.at("command").as_string();
    std::size_t rows = opt_u64(args, "rows", 24);
    std::size_t cols = opt_u64(args, "cols", 80);

    auto& st = pty_state();
    {
        std::lock_guard<std::mutex> l(st.mutex);
        reap_idle(st);
        st.sessions.erase(sid); // close existing same id (Drop => SIGKILL)
    }
    struct winsize ws {};
    ws.ws_row = static_cast<unsigned short>(rows ? rows : 24);
    ws.ws_col = static_cast<unsigned short>(cols ? cols : 80);
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        int e = errno;
        throw_hard(ToolError::execution(
            std::string("Failed to create PTY: ") +
            (e ? std::string(std::strerror(e)) + " (os error " + std::to_string(e) + ")"
               : "unknown error")));
    }
    if (pid == 0) {
        std::string wrapped = wrapped_interactive(command);
        ::execl("/bin/sh", "sh", "-c", wrapped.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    auto sess = std::make_shared<InteractiveSession>();
    sess->id = sid;
    sess->master = master;
    sess->pid = pid;
    {
        std::lock_guard<std::mutex> l(st.mutex);
        st.sessions[sid] = sess;
    }
    std::thread([sess] {
        char buf[4096];
        while (true) {
            ssize_t n = ::read(sess->master, buf, sizeof(buf));
            if (n <= 0) break;
            std::lock_guard<std::mutex> l(sess->mutex);
            sess->output.append(buf, static_cast<std::size_t>(n));
        }
        std::lock_guard<std::mutex> l(sess->mutex);
        sess->alive = false;
    }).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::string out = take_delta(sess);
    bool alive;
    {
        std::lock_guard<std::mutex> l(sess->mutex);
        alive = sess->alive;
    }
    (void)alive;
    return ToolResult::ok("PTY session '" + sid + "' started.\nOutput:\n" + out);
#else
    (void)args;
    return ToolResult::err("pty_spawn: interactive PTY is not supported on this platform");
#endif
}

ToolResult pty_write(const Json& args) {
#ifdef __unix__
    std::string sid = session_id_of(args, "pty_write");
    std::string text;
    if (args.is_object()) {
        auto it = args.as_object().find("input");
        if (it != args.as_object().end() && it->second.is_string()) text = it->second.as_string();
    }
    if (!(args.is_object() && args.contains("input") && args.at("input").is_string()))
        throw_hard(ToolError::bad_arguments("pty_write", "missing string field `input`"));
    std::size_t wait_ms = opt_u64(args, "wait_ms", 300);
    auto& st = pty_state();
    std::shared_ptr<InteractiveSession> sess;
    {
        std::lock_guard<std::mutex> l(st.mutex);
        reap_idle(st);
        auto it = st.sessions.find(trim_sv(sid));
        if (it == st.sessions.end())
            throw_hard(ToolError::execution(
                "PTY session '" + sid +
                "' not found or was terminated. Please call pty_spawn to start a new terminal "
                "session."));
        sess = it->second;
    }
    {
        std::lock_guard<std::mutex> l(sess->mutex);
        sess->last_activity = std::chrono::steady_clock::now();
    }
    const char* p = text.c_str();
    std::size_t left = text.size();
    while (left > 0) {
        ssize_t n = ::write(sess->master, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            throw_hard(ToolError::execution(
                std::string("Failed to write to PTY: ") +
                (e ? std::string(std::strerror(e)) + " (os error " + std::to_string(e) + ")"
                   : "unknown error")));
        }
        if (n == 0) break;
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    // NOTE: upstream flushes a BufWriter here; the raw fd needs no flush.
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms ? wait_ms : 300));
    std::string out = take_delta(sess);
    bool alive;
    {
        std::lock_guard<std::mutex> l(sess->mutex);
        alive = sess->alive;
    }
    return ToolResult::ok("Status: alive=" + std::string(alive ? "true" : "false") +
                          "\nOutput:\n" + out);
#else
    (void)args;
    return ToolResult::err("pty_write: interactive PTY is not supported on this platform");
#endif
}

ToolResult pty_read(const Json& args) {
#ifdef __unix__
    std::string sid = session_id_of(args, "pty_read");
    std::size_t wait_ms = opt_u64(args, "wait_ms", 0);
    auto& st = pty_state();
    std::shared_ptr<InteractiveSession> sess;
    {
        std::lock_guard<std::mutex> l(st.mutex);
        reap_idle(st);
        auto it = st.sessions.find(trim_sv(sid));
        if (it == st.sessions.end())
            throw_hard(ToolError::execution(
                "PTY session '" + sid +
                "' not found or was terminated. Please call pty_spawn to start a new terminal "
                "session."));
        sess = it->second;
    }
    if (wait_ms) std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    std::string out = take_delta(sess);
    bool alive;
    {
        std::lock_guard<std::mutex> l(sess->mutex);
        alive = sess->alive;
    }
    return ToolResult::ok("Status: alive=" + std::string(alive ? "true" : "false") +
                          "\nOutput:\n" + out);
#else
    (void)args;
    return ToolResult::err("pty_read: interactive PTY is not supported on this platform");
#endif
}

ToolResult pty_close(const Json& args) {
    std::string sid = session_id_of(args, "pty_close");
    std::string key = trim_sv(sid);
    auto& st = pty_state();
    std::shared_ptr<InteractiveSession> sess;
    {
        std::lock_guard<std::mutex> l(st.mutex);
        auto it = st.sessions.find(key);
        if (it == st.sessions.end()) return ToolResult::ok("PTY session '" + sid + "' was not running.");
        sess = it->second;
        st.sessions.erase(it);
    }
#ifdef __unix__
    if (sess->pid > 0) {
        ::kill(-sess->pid, SIGKILL);
        ::kill(sess->pid, SIGKILL);
    }
    if (sess->master >= 0) ::close(sess->master);
#endif
    return ToolResult::ok("PTY session '" + sid + "' closed.");
}

ToolResult pty_list(const Json& args) {
    (void)args;
    auto& st = pty_state();
    std::lock_guard<std::mutex> l(st.mutex);
    reap_idle(st);
    Json::Array arr;
    auto now = std::chrono::steady_clock::now();
    for (auto& [id, sess] : st.sessions) {
        std::lock_guard<std::mutex> sl(sess->mutex);
        Json::Object o;
        o.emplace("session_id", Json(id));
        o.emplace("pid", Json(static_cast<long long>(sess->pid)));
        o.emplace("is_alive", Json(sess->alive));
        o.emplace("idle_seconds",
                  Json(static_cast<long long>(
                      std::chrono::duration_cast<std::chrono::seconds>(now - sess->last_activity)
                          .count())));
        o.emplace("total_bytes_read", Json(static_cast<long long>(sess->output.size())));
        arr.push_back(Json(std::move(o)));
    }
    return ToolResult::ok(Json(std::move(arr)).dump_pretty());
}

// --- mod.rs dispatcher ------------------------------------------------------------------
namespace {
std::function<bool(const std::string&)> g_mcp_has;
std::function<ToolResult(const std::string&, const Json&)> g_mcp_call;

bool mcp_has_tool(const std::string& name) { return g_mcp_has ? g_mcp_has(name) : false; }
ToolResult mcp_call_tool(const std::string& name, const Json& args) {
    try {
        return g_mcp_call(name, args);
    } catch (const std::exception& e) {
        return ToolResult::err("MCP tool error: " + std::string(e.what()));
    }
}

std::string opt_str(const Json& args, std::initializer_list<const char*> keys) {
    const char* s = str_field(args, keys);
    return s ? std::string(s) : std::string();
}

ToolResult write_plan_impl(const std::string& md) {
    try {
        agent::Plan::default_plan().create(md);
    } catch (const std::exception& e) {
        return ToolResult::err(e.what());
    }
    return ToolResult::ok("plan written to .marmel/execution_plan.md");
}
ToolResult archive_plan_impl() {
    if (orchestrator::has_active_workers())
        return ToolResult::err(
            "Cannot archive plan while background specialist workers are still actively running.");
    auto dest = agent::Plan::default_plan().archive();
    if (dest) return ToolResult::ok("plan archived to " + *dest);
    if (agent::Plan::default_plan().exists())
        return ToolResult::err("plan is not complete and cannot be archived yet");
    return ToolResult::ok("no plan file to archive");
}

bool name_in(const std::string& name, std::initializer_list<const char*> xs) {
    for (auto x : xs)
        if (name == x) return true;
    return false;
}
} // namespace

void set_mcp_tool_checker(std::function<bool(const std::string&)> has,
                          std::function<ToolResult(const std::string&, const Json&)> call) {
    g_mcp_has = std::move(has);
    g_mcp_call = std::move(call);
}
void clear_mcp_tool_checker() {
    g_mcp_has = nullptr;
    g_mcp_call = nullptr;
}
namespace {
std::vector<types::ToolDef>& mcp_defs_store() {
    static std::vector<types::ToolDef> v;
    return v;
}
} // namespace
void set_mcp_tool_defs(std::vector<types::ToolDef> defs) { mcp_defs_store() = std::move(defs); }
std::vector<types::ToolDef> mcp_tool_defs() { return mcp_defs_store(); }

std::string normalize_tool_name(const std::string& name) {
    if (name_in(name, {"read_file", "view_file", "get_file", "read"})) return "terminal__read_file";
    if (name_in(name, {"write_file", "create_file", "write_to_file", "save_file", "write"}))
        return "terminal__write_file";
    if (name_in(name, {"replace", "replace_file_content", "edit_file"})) return "terminal__replace";
    if (name_in(name, {"run_command", "execute_command", "run", "exec", "bash", "sh", "cmd"}))
        return "terminal__run_command";
    if (name_in(name, {"grep_search", "grep", "search"})) return "terminal__grep_search";
    if (name_in(name, {"glob", "find_files", "glob_search"})) return "terminal__glob";
    if (name_in(name, {"list_directory", "ls", "list_files"})) return "terminal__list_directory";
    return name;
}

ToolResult apply_tool_output_length_limit(ToolResult res) {
    std::string upper = to_upper(res.content);
    if (upper.find("# EXECUTION PLAN") != std::string::npos ||
        upper.find("IMPLEMENTATION PLAN") != std::string::npos)
        return res;
    std::size_t full_len = res.content.size();
    if (full_len <= kMaxToolOutputChars) return res;
    std::size_t head_len = back_to_boundary(res.content, 7000);
    std::size_t tail_start = fwd_to_boundary(res.content, full_len - 2000);
    if (tail_start < head_len) tail_start = head_len; // overlap => empty tail
    std::string head = res.content.substr(0, head_len);
    std::string tail = tail_start < full_len ? res.content.substr(tail_start) : "";
    std::size_t omitted = full_len - head.size() - tail.size();
    res.content = head + "\n\n[... TRUNCATED " + std::to_string(omitted) + " CHARACTERS (total: " +
                  std::to_string(full_len) +
                  " chars). Use specific commands, filters, or paginated tools to inspect "
                  "specific sections ...]\n\n" +
                  tail;
    return res;
}

/// Outcome-threaded dispatcher core: soft branches return normally,
/// Rust `Err(ToolError)` branches throw HardToolError (or std::exception for
/// Execution IO failures). Mirrors `?` propagation; truncation applies to
/// soft results only (`Result::map` semantics).
static HarnessOutcome guard_outcome(std::function<ToolResult()> fn) {
    try {
        return {fn(), false};
    } catch (const HardToolError& e) {
        return {ToolResult::err(e.what()), true};
    } catch (const std::exception& e) {
        return {ToolResult::err(e.what()), true};
    }
}

static ToolResult map_delegate(const orchestrator::DelegateOutcome& d) {
    if (!d.ok && d.hard) throw_hard(ToolError::execution(d.content));
    return d.ok ? ToolResult::ok(d.content) : ToolResult::err(d.content);
}

static ToolResult dispatch_soft_body(const ToolInvocation& inv) {
    const std::string& n = inv.name;
    if (n == "delegate_task") {
        return map_delegate(orchestrator::handle_delegate_task(inv.arguments));
    } else if (name_in(n, {"read_file", "terminal__read_file", "view_file", "get_file", "read"})) {
        return fs_read_file(inv.arguments);
    } else if (name_in(n, {"replace", "terminal__replace", "replace_file_content", "edit_file"})) {
        return fs_replace(inv.arguments);
    } else if (name_in(n, {"write_file", "terminal__write_file", "create_file", "write_to_file",
                           "save_file", "write"})) {
        return fs_write_file(inv.arguments);
    } else if (name_in(n, {"run_command", "terminal__run_command", "execute_command", "run", "exec",
                           "bash", "sh", "cmd"})) {
        return pty_run_command(inv.arguments);
    } else if (name_in(n, {"grep_search", "terminal__grep_search", "grep", "search"})) {
        return search_grep(inv.arguments);
    } else if (name_in(n, {"glob", "terminal__glob", "find_files", "glob_search"})) {
        return search_glob(inv.arguments);
    } else if (n == "pty_spawn" || n == "pty__spawn") {
        return pty_spawn(inv.arguments);
    } else if (n == "pty_write" || n == "pty__write") {
        return pty_write(inv.arguments);
    } else if (n == "pty_read" || n == "pty__read") {
        return pty_read(inv.arguments);
    } else if (n == "pty_close" || n == "pty__close") {
        return pty_close(inv.arguments);
    } else if (n == "pty_list" || n == "pty__list") {
        return pty_list(inv.arguments);
    } else if (n == "create_plan") {
        bool has = inv.arguments.is_object() &&
                   ((inv.arguments.contains("plan_markdown") &&
                     inv.arguments.at("plan_markdown").is_string()) ||
                    (inv.arguments.contains("plan") && inv.arguments.at("plan").is_string()));
        if (!has)
            return ToolResult::err(
                "create_plan requires a `plan_markdown` or `plan` string argument");
        return write_plan_impl(opt_str(inv.arguments, {"plan_markdown", "plan"}));
    } else if (n == "archive_current_plan") {
        return archive_plan_impl();
    } else if (n == "rebirth") {
        throw_hard(ToolError::bad_arguments(
            "rebirth", "rebirth requires a live ContextEngine; use dispatch_with_engine"));
    }
    throw_hard(ToolError::unknown(n));
}

static ToolResult dispatch_manager_soft_body(const ToolInvocation& inv) {
    const std::string& n = inv.name;
    if (n == "delegate_task") {
        return map_delegate(orchestrator::handle_delegate_task(inv.arguments));
    }
    if (n == "create_plan") {
        bool has = inv.arguments.is_object() &&
                   ((inv.arguments.contains("plan") && inv.arguments.at("plan").is_string()) ||
                    (inv.arguments.contains("plan_markdown") &&
                     inv.arguments.at("plan_markdown").is_string()));
        if (!has)
            return ToolResult::err("create_plan requires a `plan` or `plan_markdown` string argument");
        return write_plan_impl(opt_str(inv.arguments, {"plan", "plan_markdown"}));
    }
    if (n == "archive_current_plan") return archive_plan_impl();
    if (n == "rebirth")
        throw_hard(ToolError::bad_arguments(
            "rebirth", "rebirth requires a live ContextEngine; use dispatch_with_engine"));
    // NOTE: base names only — terminal__* aliases are Forbidden for the
    // Manager, exactly as in Rust dispatch_manager.
    if (n == "read_file") return fs_read_file(inv.arguments);
    if (n == "grep_search") return search_grep(inv.arguments);
    if (n == "glob") return search_glob(inv.arguments);
    throw_hard(ToolError::forbidden(n, "Manager"));
}

static ToolResult dispatch_specialist_soft_body(agents::Agent agent, const ToolInvocation& inv) {
    const std::string& n = inv.name;
    if (n == "create_plan")
        throw_hard(ToolError::forbidden("create_plan", agents::agent_to_string(agent)));
    if (mcp_has_tool(n)) {
        return mcp_call_tool(n, inv.arguments);
    }
    std::string gate = normalize_tool_name(n);
    if (!agents::caller_allows_tool(agent, gate, agents::SpecialistRegistry::canonical()))
        throw_hard(ToolError::forbidden(n, agents::agent_to_string(agent)));
    if (n == "leave_verdict") {
        std::string verdict = "APPROVED";
        if (inv.arguments.is_object() && inv.arguments.contains("verdict") &&
            inv.arguments.at("verdict").is_string())
            verdict = inv.arguments.at("verdict").as_string();
        std::string comments = opt_str(inv.arguments, {"comments", "comment", "feedback", "reason",
                                                       "critique", "details", "explanation"});
        return ToolResult::ok("Verdict recorded via leave_verdict: " + verdict +
                              " with comments: " + comments);
    }
    if (n == "delegate_task") {
        return map_delegate(orchestrator::handle_delegate_task(inv.arguments));
    }
    if (n == "archive_current_plan") return archive_plan_impl();
    if (n == "rebirth")
        throw_hard(ToolError::bad_arguments(
            "rebirth", "rebirth requires a live ContextEngine; use dispatch_with_engine"));
    if (name_in(n, {"read_file", "terminal__read_file", "view_file", "get_file", "read"}))
        return fs_read_file(inv.arguments);
    if (name_in(n, {"replace", "terminal__replace", "replace_file_content", "edit_file"}))
        return fs_replace(inv.arguments);
    if (name_in(n, {"write_file", "terminal__write_file", "create_file", "write_to_file",
                    "save_file", "write"}))
        return fs_write_file(inv.arguments);
    if (name_in(n, {"run_command", "terminal__run_command", "execute_command", "run", "exec", "bash",
                    "sh", "cmd"}))
        return pty_run_command(inv.arguments);
    if (name_in(n, {"grep_search", "terminal__grep_search", "grep", "search"}))
        return search_grep(inv.arguments);
    if (name_in(n, {"glob", "terminal__glob", "find_files", "glob_search"}))
        return search_glob(inv.arguments);
    if (n == "pty_spawn" || n == "pty__spawn") return pty_spawn(inv.arguments);
    if (n == "pty_write" || n == "pty__write") return pty_write(inv.arguments);
    if (n == "pty_read" || n == "pty__read") return pty_read(inv.arguments);
    if (n == "pty_close" || n == "pty__close") return pty_close(inv.arguments);
    if (n == "pty_list" || n == "pty__list") return pty_list(inv.arguments);
    throw_hard(ToolError::unknown(n));
}

static HarnessOutcome truncate_outcome(HarnessOutcome o) {
    if (!o.hard_error) o.result = apply_tool_output_length_limit(o.result);
    return o;
}

static HarnessOutcome dispatch_outcome_inner(const ToolInvocation& inv,
                                             const agent::ToolCaller& caller) {
    if (caller.kind == agent::ToolCallerKind::Manager)
        return guard_outcome([&] { return dispatch_manager_soft_body(inv); });
    agents::Agent agent =
        agents::agent_from_str(caller.agent).value_or(agents::Agent::Generalist);
    return guard_outcome([&] { return dispatch_specialist_soft_body(agent, inv); });
}

ToolResult dispatch(const ToolInvocation& inv) {
    if (mcp_has_tool(inv.name)) {
        ToolResult r = mcp_call_tool(inv.name, inv.arguments);
        return apply_tool_output_length_limit(r);
    }
    return truncate_outcome(guard_outcome([&] { return dispatch_soft_body(inv); })).result;
}

ToolResult dispatch_manager(const ToolInvocation& inv) {
    return guard_outcome([&] { return dispatch_manager_soft_body(inv); }).result;
}

ToolResult dispatch_specialist(agents::Agent agent, const ToolInvocation& inv) {
    return guard_outcome([&] { return dispatch_specialist_soft_body(agent, inv); }).result;
}


ToolResult handle_rebirth(agent::ContextEngine& engine, const Json& args) {
    if (!(args.is_object() && args.contains("summary") && args.at("summary").is_string()))
        throw_hard(ToolError::bad_arguments("rebirth", "rebirth requires a `summary` string argument"));
    engine.perform_rebirth(args.at("summary").as_string());
    return ToolResult::ok("Rebirth: compacted history and summarized progress.");
}

HarnessOutcome dispatch_with_engine_outcome(const ToolInvocation& inv,
                                           agent::ContextEngine& engine) {
    if (inv.name == "rebirth") return guard_outcome([&] { return handle_rebirth(engine, inv.arguments); });
    if (mcp_has_tool(inv.name)) {
        ToolResult r = mcp_call_tool(inv.name, inv.arguments);
        return {apply_tool_output_length_limit(r), false};
    }
    return truncate_outcome(guard_outcome([&] { return dispatch_soft_body(inv); }));
}

ToolResult dispatch_with_engine(const ToolInvocation& inv, agent::ContextEngine& engine) {
    return dispatch_with_engine_outcome(inv, engine).result;
}

HarnessOutcome dispatch_for_outcome(const ToolInvocation& inv, const agent::ToolCaller& caller) {
    return truncate_outcome(dispatch_outcome_inner(inv, caller));
}

ToolResult dispatch_for(const ToolInvocation& inv, const agent::ToolCaller& caller) {
    return dispatch_for_outcome(inv, caller).result;
}

ToolResult dispatch_for_json(const std::string& name, const Json& args,
                             const agent::ToolCaller& caller) {
    return dispatch_for(ToolInvocation{name, args}, caller);
}

} // namespace marmel::harness
