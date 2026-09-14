#pragma once
// Context engine: token counting, KV-cache prefix pinning, compaction,
// rebirth, slow-prefill tracking.
// Rust origin: src/agent/context.rs (1187 lines).
//
// Hard invariants (author's intent — do not "improve"):
//  - messages[0] is ALWAYS the system prompt, messages[1] the goal.
//    set_goal() inserts an "" system placeholder when empty to keep indices.
//  - Counting: 3 framing tokens per message + ~1 token per 4 chars of
//    content/reasoning/tool name+args (cl100k_base approximation; the Rust
//    original uses tiktoken-rs. Threshold behavior is identical: compact at
//    90%, target 70%, retry 70%->50%, hard-limit path 80%. Tests assert the
//    ratios and the literal checkpoint/limit strings, not exact BPE counts.)
//  - perform_rebirth() collapses to EXACTLY 4 messages:
//      [sys, goal, last-user-or-goal, REBIRTH_CHECKPOINT_PREFIX+summary+")"]
//  - SlowPrefillTracker: 300s threshold inclusive, needs 2 consecutive slow
//    prefills, suppressed during the first 4 turns after a rebirth.

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "marmel/types.hpp"

namespace marmel::harness {
struct HarnessStats;
}

namespace marmel::agent {

inline constexpr char kRebirthCheckpointPrefix[] =
    "(SYSTEM: REBIRTH CHECKPOINT. The previous turn-by-turn history has been compacted. Summarized progress: ";
inline constexpr unsigned long long kSlowPrefillThresholdSecs = 300;
inline constexpr char kContextLimitExceededMessage[] =
    "(SYSTEM: CONTEXT LIMIT EXCEEDED. Your context window overflowed. The proxy had to FORCIBLY compact "
    "your history by pruning older messages and tool results. You MUST immediately summarize your progress "
    "and invoke the 'rebirth' tool with your summary to reset your memory properly.)";

// Named budget ratios (Rust COMPACTION_* consts in context.rs).
inline constexpr double kCompactTriggerRatio = 0.90;
inline constexpr double kCompactTargetRatio = 0.70;
inline constexpr double kCompactRetry1Ratio = 0.70;
inline constexpr double kCompactRetry2Ratio = 0.50;
inline constexpr double kCompactOverLimitRatio = 0.80;
inline constexpr unsigned kCompactionRetryCap = 2;
inline constexpr unsigned long long kMinTurnsAfterRebirth = 5;

std::size_t count_tokens(const std::vector<types::Message>& messages);
std::size_t compaction_threshold(std::size_t max_tokens); // round(max*0.90)
std::size_t compaction_target(std::size_t max_tokens);    // round(max*0.70)

/// UTF-8-safe substring by BYTE offsets: clamps start up to the next char
/// boundary and end down to the previous one; "" when begin>=stop or OOB.
std::string utf8_safe_slice(const std::string& s, std::size_t start, std::size_t end);

/// Drop Tool messages whose tool_call_id has no surviving assistant owner.
std::vector<types::Message> prune_orphan_tool_messages(std::vector<types::Message> messages);

class SlowPrefillTracker {
public:
    SlowPrefillTracker() = default;
    /// Returns true when cooling action is due (2nd consecutive slow prefill
    /// at least 5 turns after the last rebirth).
    bool record_prefill(std::chrono::milliseconds duration);
    void note_rebirth();
    std::uint64_t turns_since_rebirth() const { return turns_since_rebirth_; }

private:
    std::uint64_t turns_since_rebirth_ = 0;
    unsigned consecutive_slow_ = 0;
};

class ContextEngine {
public:
    explicit ContextEngine(std::size_t max_context_tokens);
    void set_stats(std::shared_ptr<harness::HarnessStats> stats);
    void set_system_prompt(std::string prompt); // messages[0]
    void set_goal(std::string goal);            // messages[1]
    void append(types::Message msg);
    const std::vector<types::Message>& messages() const { return messages_; }
    std::size_t max_context_tokens() const { return max_context_tokens_; }
    std::size_t token_count() const;
    bool should_compact() const;
    void compact(); // to 70% + prune + stat
    unsigned compaction_retry_count() const { return compaction_retry_count_; }
    void reset_compaction_retry_count();
    /// Escalating retry used by the orchestrator on context overflow.
    bool compact_with_retry(std::size_t limit);
    bool compact_context(std::size_t max_tokens);       // to 80% if over
    bool force_compact_context(std::size_t target_tokens);
    void inject_context_limit_exceeded();
    void perform_rebirth(const std::string& summary); // collapse to 4 msgs
    const SlowPrefillTracker& prefill_tracker() const { return prefill_; }
    SlowPrefillTracker& prefill_tracker_mut() { return prefill_; }

private:
    bool compact_to_target(std::size_t target, bool force);
    std::vector<types::Message> messages_;
    std::size_t max_context_tokens_;
    std::shared_ptr<harness::HarnessStats> stats_;
    SlowPrefillTracker prefill_;
    unsigned compaction_retry_count_ = 0;
};

class ContextEngineFactory {
public:
    explicit ContextEngineFactory(std::size_t max_context_tokens);
    std::size_t max_context_tokens() const { return max_context_tokens_; }
    /// Full transcript seed for the manager.
    ContextEngine manager_context(std::string system_prompt, std::string goal) const;
    /// Isolated 2-message context for specialists: NO manager history leaks.
    ContextEngine specialist_context(std::string role_prompt, std::string brief) const;

private:
    std::size_t max_context_tokens_;
};

} // namespace marmel::agent
