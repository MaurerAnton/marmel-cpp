#pragma once
// Disk-backed execution plan + mission phase + terminal markers.
// Rust origin: src/agent/phase.rs (991 lines).
//
// Plan file: <dir>/execution_plan.md  (default dir ".marmel")
//   pending:  - [ ] [t-xxx] description   ("-"/"*" bullet, optional **)
//   done:     - [x] [t-xxx] description
// Forced phase override: <dir>/forced_phase.txt ("CONVERSATIONAL"|"EXECUTING").
// Marker precedence (author's bug-fix, preserved): REPLAN REQUIRED >
// MISSION COMPLETE > FAILED, all case-insensitive.
// check_off_on_success() gates on output_is_success() which rejects content
// containing ERROR|FAILED|REPLAN REQUIRED (case-insensitive).
// All filesystem ops serialize on a global mutex (parallel subagents).

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace marmel::agent {

inline constexpr char kMarmelDir[] = ".marmel";
inline constexpr char kPlanFile[] = "execution_plan.md";
inline constexpr char kForcedPhaseFile[] = "forced_phase.txt";

enum class MissionPhase { Conversational, Executing };

/// Parse "conversational"|"executing" (trim + uppercase). nullopt otherwise.
std::optional<MissionPhase> parse_phase(const std::string& text);

/// True unless upper-cased output contains ERROR, FAILED or REPLAN REQUIRED.
bool output_is_success(const std::string& output);

/// All unchecked [t-xxx] ids in markdown order.
std::vector<std::string> parse_unchecked_tasks(const std::string& markdown);
/// All [t-xxx] ids (checked or not) in markdown order.
std::vector<std::string> parse_all_tasks(const std::string& markdown);

struct MissionMarker {
    enum class Kind { Complete, Failed, Replan };
    Kind kind = Kind::Complete;
    std::optional<std::string> task_id; // Complete only
    std::string reason;                 // Failed/Replan (raw matched text)
    static std::optional<MissionMarker> parse(const std::string& text);
    bool is_complete() const { return kind == Kind::Complete; }
};

class Plan {
public:
    explicit Plan(std::string dir = kMarmelDir);
    static Plan at(std::string dir) { return Plan(std::move(dir)); }
    static Plan default_plan() { return Plan(kMarmelDir); }

    const std::string& dir() const { return dir_; }
    std::string plan_path() const;
    std::string forced_phase_path() const;

    void create(const std::string& plan_markdown) const;
    std::optional<std::string> read() const; // nullopt if no ACTIVE file (never archive fallback)
    bool exists() const;
    void clear() const; // deletes active + archive + forced-phase
    std::vector<std::string> pending_tasks() const;
    std::vector<std::string> all_tasks() const;
    bool is_complete() const;
    /// Archive only when complete; returns destination or nullopt.
    std::optional<std::string> archive() const;
    /// Flip the first unchecked line containing task_id (case-insensitive).
    /// Auto-snapshots to archive/ when the plan becomes complete.
    bool check_off(const std::string& task_id) const;
    bool check_off_on_success(const std::string& task_id, const std::string& output) const;
    bool check_plan_on_marker(const std::optional<std::string>& task_id,
                              const std::string& deliverable) const;
    std::optional<MissionPhase> forced_phase() const;
    MissionPhase determine_phase() const;
    static bool is_silent_dispatcher(MissionPhase phase) { return phase == MissionPhase::Executing; }

    static std::mutex& plan_mutex();

private:
    std::string dir_;
};

} // namespace marmel::agent
