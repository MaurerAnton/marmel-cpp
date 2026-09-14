#pragma once
// LLM backend: streaming HTTP client, shared stream channel, thinking demuxer.
// Rust origin: src/llm/{client,stream,thinking}.rs (399+341+505 lines).
//
// Transport (author's intent): OpenAI-compatible POST
// {backend}/chat/completions with stream=true; SSE `data:` lines parsed until
// `data: [DONE]`; delta.content/delta.reasoning_content accumulate to
// content/reasoning/raw; delta.tool_calls fragments accumulate BY INDEX
// (missing ids => call_<uuid>); retry ONLY on 503/429 (3 attempts,
// 1s*attempt backoff); 60s first-event watchdog with on_delta("") heartbeat
// (abort when it returns false); 1800s overall read timeout.
// The C++ port uses libcurl when available (MARMEL_HAVE_CURL) and otherwise a
// stub transport returning a connectivity error — same classify_llm_error()
// buckets (http/connectivity/timeout/stream/unknown) either way.
// ThinkingDemuxer: [thinking]…[/thinking] state machine with partial-tag
// holdback (UTF-8 boundary safe) and preserve_thinking mirroring.
// StreamConfig::from_config mirrors StreamConfig::from_config (preserve flag
// defaults FALSE on this path — intentional upstream quirk, kept).
// Empty-production nudge: up to 3 "?" follow-ups (NudgePolicy).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "marmel/json.hpp"
#include "marmel/types.hpp"

namespace marmel::config {
struct Config;
}
namespace marmel::harness {
struct HarnessStats;
}

namespace marmel::llm {

inline constexpr std::uint64_t kInitialResponseWatchdogSecs = 60;
inline constexpr std::uint64_t kOverallReadTimeoutSecs = 1800;
inline constexpr unsigned kMaxAttempts = 3;
inline constexpr std::uint64_t kBackoffBaseMs = 1000;

struct StreamedReply {
    std::string content;
    std::string reasoning;
    std::string raw;
    std::vector<types::ToolCall> tool_calls;
};

class ChatClient {
public:
    ChatClient(std::string backend_url, std::string model, std::string auth_token = {});
    static ChatClient from_config(const config::Config& cfg);

    const std::string& backend_url() const { return backend_url_; }
    const std::string& model() const { return model_; }

    StreamedReply chat(const types::ChatRequest& req) const;
    /// on_delta returning false aborts the stream (heartbeat "" included).
    StreamedReply chat_stream(const types::ChatRequest& req,
                              const std::function<bool(const std::string&)>& on_delta) const;

private:
    std::string backend_url_;
    std::string model_;
    std::string auth_token_;
};

// --- thinking.rs ------------------------------------------------------------------

enum class DeltaKind { Content, Thinking };

struct RecoveryAdjustment {
    float frequency_penalty_delta = 0.5f;
    float temperature_delta = 0.1f;
};

class ThinkingDemuxer {
public:
    ThinkingDemuxer();
    static ThinkingDemuxer with_preserve(bool preserve);
    DeltaKind push(const std::string& delta);
    DeltaKind push_delta(const std::string& delta,
                         const std::function<void(DeltaKind, const std::string&)>& emit);
    void finish_delta(const std::function<void(DeltaKind, const std::string&)>& emit);
    void finish();
    bool is_in_thinking() const { return in_thinking_; }
    const std::string& content() const { return content_; }
    const std::string& thinking() const { return thinking_; }
    types::Message into_message() const;
    void demux_all(const std::string& raw);

private:
    void append_segment(const std::string& s, bool thinking_channel);
    std::string content_;
    std::string thinking_;
    bool in_thinking_ = false;
    std::string pending_;
    bool preserve_thinking_ = false;
};

ThinkingDemuxer demux_stream(const std::string& raw);
types::ChatRequest apply_recovery(types::ChatRequest req, const RecoveryAdjustment& adj = {});

struct NudgePolicy {
    unsigned max_attempts = 3;
    std::string nudge_text = "?";
    bool should_nudge(unsigned used) const { return used < max_attempts; }
    std::vector<types::Message> nudge(std::vector<types::Message> transcript) const;
};

// --- stream.rs ---------------------------------------------------------------------

struct StreamEvent {
    enum class Kind { Content, Thinking, Status };
    Kind kind = Kind::Content;
    std::string text;
    static StreamEvent content(std::string t) { return {Kind::Content, std::move(t)}; }
    static StreamEvent thinking(std::string t) { return {Kind::Thinking, std::move(t)}; }
    static StreamEvent status(std::string t) { return {Kind::Status, std::move(t)}; }
};

class StreamSink {
public:
    virtual ~StreamSink() = default;
    virtual void emit(const StreamEvent& ev) = 0;
    virtual bool is_aborted() { return false; }
};

class NullSink final : public StreamSink {
public:
    void emit(const StreamEvent&) override {}
};

class VecSink final : public StreamSink {
public:
    void emit(const StreamEvent& ev) override { events.push_back(ev); }
    std::string content() const;
    std::string thinking() const;
    std::vector<StreamEvent> events;
};

struct StreamConfig {
    std::string model;
    float temperature = 0.7f;
    float top_p = 0.9f;
    float frequency_penalty = 0.0f;
    float presence_penalty = 0.0f;
    bool preserve_thinking = false;
    bool recovery = false;
    static StreamConfig from_config(const config::Config& cfg);
};

types::ChatRequest build_request(const StreamConfig& cfg,
                                 const std::vector<types::Message>& transcript);
bool is_empty_production(const types::Message& msg);

/// Testable driver: `chat` performs one backend round-trip for the request.
types::Message drive_streamed_turn(
    const std::function<StreamedReply(const types::ChatRequest&)>& chat,
    std::vector<types::Message> messages, const StreamConfig& cfg, StreamSink& sink,
    std::shared_ptr<harness::HarnessStats> stats = {});

/// Live driver through ChatClient with incremental demux + abort polling.
types::Message chat_client_turn(const ChatClient& client, std::vector<types::Message> messages,
                                const StreamConfig& cfg, StreamSink& sink,
                                std::shared_ptr<harness::HarnessStats> stats = {});

} // namespace marmel::llm
