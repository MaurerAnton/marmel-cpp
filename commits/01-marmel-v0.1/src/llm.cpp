// Rust origin: src/llm/{client,stream,thinking}.rs
#include "marmel/llm.hpp"

#include "marmel/config.hpp"
#include "marmel/harness.hpp"

#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>
#include <thread>

#ifdef MARMEL_HAVE_CURL
#include <curl/curl.h>
#endif

namespace marmel::llm {
namespace {

bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
bool at_boundary(const std::string& s, std::size_t i) {
    return i == 0 || i >= s.size() || !is_cont(static_cast<unsigned char>(s[i]));
}

} // namespace

// --- thinking -----------------------------------------------------------------------
ThinkingDemuxer::ThinkingDemuxer() = default;
ThinkingDemuxer ThinkingDemuxer::with_preserve(bool preserve) {
    ThinkingDemuxer d;
    d.preserve_thinking_ = preserve;
    return d;
}

void ThinkingDemuxer::append_segment(const std::string& s, bool thinking_channel) {
    if (s.empty()) return;
    if (thinking_channel) {
        thinking_ += s;
        if (preserve_thinking_) content_ += s;
    } else {
        content_ += s;
    }
}

DeltaKind ThinkingDemuxer::push(const std::string& delta) {
    if (delta.empty()) return is_in_thinking() ? DeltaKind::Thinking : DeltaKind::Content;
    DeltaKind last = is_in_thinking() ? DeltaKind::Thinking : DeltaKind::Content;
    push_delta(delta, [&](DeltaKind k, const std::string&) { last = k; });
    return last;
}

DeltaKind ThinkingDemuxer::push_delta(
    const std::string& delta, const std::function<void(DeltaKind, const std::string&)>& emit) {
    static const std::string kOpen = "[thinking]";
    static const std::string kClose = "[/thinking]";
    DeltaKind last = in_thinking_ ? DeltaKind::Thinking : DeltaKind::Content;
    pending_ += delta;
    while (true) {
        const std::string& tag = in_thinking_ ? kClose : kOpen;
        std::size_t pos = pending_.find(tag);
        if (pos != std::string::npos) {
            std::string pre = pending_.substr(0, pos);
            if (!pre.empty()) {
                append_segment(pre, in_thinking_);
                if (emit) emit(in_thinking_ ? DeltaKind::Thinking : DeltaKind::Content, pre);
                last = in_thinking_ ? DeltaKind::Thinking : DeltaKind::Content;
            }
            if (preserve_thinking_) {
                content_ += tag;
                if (emit) emit(DeltaKind::Content, tag);
            }
            in_thinking_ = !in_thinking_;
            pending_.erase(0, pos + tag.size());
            last = in_thinking_ ? DeltaKind::Thinking : DeltaKind::Content;
            continue;
        }
        // Hold back a tail that is a proper prefix of the tag (split-tag safe,
        // longest-first, UTF-8 boundary aware).
        std::size_t hold = 0;
        for (std::size_t len = std::min(tag.size() - 1, pending_.size()); len > 0; len--) {
            std::size_t start = pending_.size() - len;
            if (!at_boundary(pending_, start)) {
                if (len == 1) break;
                continue;
            }
            if (tag.compare(0, len, pending_, start, len) == 0) {
                hold = len;
                break;
            }
            if (len == 1) break;
        }
        std::string head = pending_.substr(0, pending_.size() - hold);
        if (!head.empty()) {
            append_segment(head, in_thinking_);
            if (emit) emit(in_thinking_ ? DeltaKind::Thinking : DeltaKind::Content, head);
            last = in_thinking_ ? DeltaKind::Thinking : DeltaKind::Content;
        }
        pending_ = pending_.substr(pending_.size() - hold);
        break;
    }
    return last;
}

void ThinkingDemuxer::finish_delta(
    const std::function<void(DeltaKind, const std::string&)>& emit) {
    if (!pending_.empty()) {
        append_segment(pending_, in_thinking_);
        if (emit) emit(in_thinking_ ? DeltaKind::Thinking : DeltaKind::Content, pending_);
        pending_.clear();
    }
}
void ThinkingDemuxer::finish() { finish_delta({}); }
types::Message ThinkingDemuxer::into_message() const {
    return types::Message::assistant(
        content_.empty() ? std::optional<std::string>{} : std::optional<std::string>{content_},
        thinking_.empty() ? std::optional<std::string>{} : std::optional<std::string>{thinking_},
        {});
}
void ThinkingDemuxer::demux_all(const std::string& raw) {
    push(raw);
    finish();
}
ThinkingDemuxer demux_stream(const std::string& raw) {
    ThinkingDemuxer d;
    d.demux_all(raw);
    return d;
}

types::ChatRequest apply_recovery(types::ChatRequest req, const RecoveryAdjustment& adj) {
    req.enable_thinking = false;
    float fp = req.frequency_penalty.value_or(0.0f);
    float t = req.temperature.value_or(0.7f);
    req.frequency_penalty = fp + adj.frequency_penalty_delta;
    req.temperature = t + adj.temperature_delta;
    return req;
}

std::vector<types::Message> NudgePolicy::nudge(std::vector<types::Message> transcript) const {
    transcript.push_back(types::Message::user(nudge_text));
    return transcript;
}

// --- client ----------------------------------------------------------------------------
ChatClient::ChatClient(std::string backend_url, std::string model, std::string auth_token)
    : backend_url_(std::move(backend_url)),
      model_(std::move(model)),
      auth_token_(std::move(auth_token)) {}
ChatClient ChatClient::from_config(const config::Config& cfg) {
    return ChatClient(cfg.backend_url, cfg.model, cfg.auth_token);
}

namespace {

struct SseAccum {
    std::string content;
    std::string reasoning;
    std::string raw;
    std::map<std::size_t, types::ChunkToolCall> tools;
    bool done = false;
    bool aborted = false;
    bool event_seen = false; // any SSE event yet (watchdog)
};

static std::string trim_trailing_ws(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                          s.back() == '\n'))
        s.pop_back();
    return s;
}

bool consume_sse_data(const std::string& data, SseAccum& acc,
                      const std::function<bool(const std::string&)>& on_delta) {
    if (trim_trailing_ws(data) == "[DONE]") {
        acc.done = true;
        return true;
    }
    Json v;
    try {
        v = Json::parse(data);
    } catch (...) {
        return false; // ignore non-JSON keep-alives/comments
    }
    types::ChatChunk chunk;
    try {
        chunk = types::ChatChunk::from_json(v);
    } catch (...) {
        return false; // structural error fails the whole chunk (serde)
    }
    for (auto& choice : chunk.choices) {
        if (choice.delta.content) {
            acc.content += *choice.delta.content;
            acc.raw += *choice.delta.content;
            if (on_delta && !on_delta(*choice.delta.content)) {
                acc.done = true;
                acc.aborted = true;
                return true;
            }
        }
        if (choice.delta.reasoning_content) {
            acc.reasoning += *choice.delta.reasoning_content;
            acc.raw += *choice.delta.reasoning_content;
            if (on_delta && !on_delta(*choice.delta.reasoning_content)) {
                acc.done = true;
                acc.aborted = true;
                return true;
            }
        }
        if (choice.delta.tool_calls) {
            for (auto& tc : *choice.delta.tool_calls) {
                auto& slot = acc.tools[tc.index];
                slot.index = tc.index;
                if (tc.id) slot.id = tc.id;
                if (tc.function) {
                    if (!slot.function) slot.function = types::ChunkToolFunction{};
                    if (tc.function->name) {
                        if (!slot.function->name)
                            slot.function->name = tc.function->name;
                        else
                            *slot.function->name += *tc.function->name;
                    }
                    if (tc.function->arguments) {
                        if (!slot.function->arguments)
                            slot.function->arguments = tc.function->arguments;
                        else
                            *slot.function->arguments += *tc.function->arguments;
                    }
                }
            }
        }
        // NOTE: finish_reason is recorded but never ends the stream (only
        // [DONE]/EOF/abort do, as in Rust).
    }
    return acc.done;
}

#ifdef MARMEL_HAVE_CURL
struct CurlWriteState {
    std::string buffer; // unprocessed bytes
    std::string line;   // current partial line
    SseAccum* acc = nullptr;
    std::function<bool(const std::string&)>* on_delta = nullptr;
    bool http_started = false;
};

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* st = static_cast<CurlWriteState*>(userdata);
    st->buffer.append(ptr, size * nmemb);
    // Extract complete lines.
    std::size_t pos = 0;
    while (true) {
        auto nl = st->buffer.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = st->buffer.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data:", 0) == 0) {
            st->acc->event_seen = true;
            std::string data = line.size() > 5 ? line.substr(5) : "";
            std::size_t a = data.find_first_not_of(" \t");
            if (a != std::string::npos)
                data = data.substr(a);
            else
                data.clear();
            consume_sse_data(data, *st->acc, st->on_delta ? *st->on_delta
                                                          : std::function<bool(const std::string&)>{});
        } else if (!line.empty() && line[0] == ':') {
            // SSE comment keep-alive: counts as an event for the watchdog.
            st->acc->event_seen = true;
        }
        // Other lines ignored.
    }
    st->buffer.erase(0, pos);
    if (st->acc->aborted) return 0; // abort transfer (CURLE_WRITE_ERROR, handled as abort)
    return size * nmemb;
}

struct HttpResult {
    bool transport_ok = false;
    long code = 0;
    std::string transport_error;
    bool aborted = false;
};

HttpResult post_once(const std::string& url, const std::string& body,
                     const std::string& auth_token, CurlWriteState& state) {
    HttpResult r;
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.transport_error = "curl_easy_init failed";
        return r;
    }
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth;
    if (!auth_token.empty()) {
        auth = "Authorization: Bearer " + auth_token;
        headers = curl_slist_append(headers, auth.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(kOverallReadTimeoutSecs));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // 50ms slices: heartbeat on_delta("") so the UI can abort mid-flight.
    auto start = std::chrono::steady_clock::now();
    auto last_beat = start;
    CURLM* multi = curl_multi_init();
    curl_multi_add_handle(multi, curl);
    int still_running = 1;
    CURLMcode mc = CURLM_OK;
    while (still_running) {
        mc = curl_multi_perform(multi, &still_running);
        if (mc != CURLM_OK) break;
        if (still_running) {
            int numfds = 0;
            mc = curl_multi_poll(multi, nullptr, 0, 50, &numfds);
            if (mc != CURLM_OK) break;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_beat >= std::chrono::milliseconds(50)) {
            last_beat = now;
            if (state.on_delta && *state.on_delta) {
                if (!(*state.on_delta)("")) {
                    state.acc->aborted = true;
                    break;
                }
            }
            // First-event watchdog: ANY SSE event (data or comment) counts,
            // matching Rust's first stream.next() item.
            if (!state.acc->event_seen &&
                now - start >= std::chrono::seconds(kInitialResponseWatchdogSecs)) {
                r.transport_error = "initial response timeout (60s watchdog)";
                curl_multi_remove_handle(multi, curl);
                curl_multi_cleanup(multi);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return r;
            }
            if (now - start >= std::chrono::seconds(kOverallReadTimeoutSecs)) {
                r.transport_error = "overall read timeout";
                curl_multi_remove_handle(multi, curl);
                curl_multi_cleanup(multi);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return r;
            }
        }
        if (state.acc->done || state.acc->aborted) break;
    }
    if (state.acc->aborted) {
        r.aborted = true;
        r.transport_ok = true;
        curl_multi_remove_handle(multi, curl);
        curl_multi_cleanup(multi);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return r;
    }
    int msgs = 0;
    CURLMsg* msg = nullptr;
    CURLcode res = CURLE_OK;
    while ((msg = curl_multi_info_read(multi, &msgs))) {
        if (msg->msg == CURLMSG_DONE) res = msg->data.result;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.code);
    if (res == CURLE_WRITE_ERROR && state.acc->aborted) {
        r.aborted = true;
        r.transport_ok = true;
    } else if (res != CURLE_OK) {
        r.transport_error = curl_easy_strerror(res);
    } else {
        r.transport_ok = true;
    }
    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}
#endif

} // namespace

StreamedReply ChatClient::chat(const types::ChatRequest& req) const {
    return chat_stream(req, {});
}

StreamedReply ChatClient::chat_stream(
    const types::ChatRequest& req,
    const std::function<bool(const std::string&)>& on_delta) const {
#ifndef MARMEL_HAVE_CURL
    (void)req;
    (void)on_delta;
    throw std::runtime_error("connectivity error: HTTP transport unavailable in this build "
                             "(libcurl not linked); configure a backend to enable live LLM calls");
#else
    std::string base = backend_url_;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string url = base + "/chat/completions";
    types::ChatRequest wire = req;
    if (wire.model.empty()) wire.model = model_;
    wire.stream = true;
    std::string body = wire.to_json().dump();

    for (unsigned attempt = 1; attempt <= kMaxAttempts; attempt++) {
        SseAccum acc;
        CurlWriteState state;
        state.acc = &acc;
        // on_delta may be empty (chat()): treat as always-true.
        std::function<bool(const std::string&)> delta =
            on_delta ? on_delta : std::function<bool(const std::string&)>([](const std::string&) {
                return true;
            });
        state.on_delta = &delta;
        HttpResult r = post_once(url, body, auth_token_, state);
        if (r.aborted) return StreamedReply{}; // early Ok(default) on abort
        if (!r.transport_ok) throw std::runtime_error("transport error: " + r.transport_error);
        if ((r.code == 503 || r.code == 429) && attempt < kMaxAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kBackoffBaseMs * attempt));
            continue;
        }
        if (r.code < 200 || r.code >= 300)
            throw std::runtime_error("http status " + std::to_string(r.code));
        StreamedReply reply;
        reply.content = acc.content;
        reply.reasoning = acc.reasoning;
        reply.raw = acc.raw;
        reply.tool_calls = types::map_to_tool_calls(acc.tools);
        return reply;
    }
    throw std::runtime_error("http status error: retries exhausted");
#endif
}

// --- stream ---------------------------------------------------------------------------------
StreamConfig StreamConfig::from_config(const config::Config& cfg) {
    StreamConfig s;
    s.model = cfg.model;
    s.temperature = cfg.temperature;
    s.top_p = cfg.top_p;
    s.frequency_penalty = cfg.frequency_penalty;
    s.presence_penalty = cfg.presence_penalty;
    s.preserve_thinking = cfg.preserve_thinking;
    return s;
}

types::ChatRequest build_request(const StreamConfig& cfg,
                                 const std::vector<types::Message>& transcript) {
    types::ChatRequest req;
    req.model = cfg.model;
    req.messages = transcript;
    req.temperature = cfg.temperature;
    req.top_p = cfg.top_p;
    req.frequency_penalty = cfg.frequency_penalty;
    req.presence_penalty = cfg.presence_penalty;
    req.stream = true;
    auto tools = types::ToolDef::default_tools();
    for (auto& md : harness::mcp_tool_defs()) tools.push_back(md);
    req.tools = std::move(tools);
    return req;
}

bool is_empty_production(const types::Message& msg) {
    if (msg.role != types::MsgRole::Assistant) return false;
    bool empty_content = !msg.has_content || msg.content.empty();
    return empty_content && msg.tool_calls.empty();
}

std::string VecSink::content() const {
    std::string out;
    for (auto& e : events)
        if (e.kind == StreamEvent::Kind::Content) out += e.text;
    return out;
}
std::string VecSink::thinking() const {
    std::string out;
    for (auto& e : events)
        if (e.kind == StreamEvent::Kind::Thinking) out += e.text;
    return out;
}

namespace {
/// Shared finish path for both turn drivers: into_message + extend with the
/// transport tool calls + unconditional XML rescue on empty tool calls with
/// Some content (mirrors stream.rs).
void finish_message_tools(types::Message& msg, const std::vector<types::ToolCall>& transported) {
    msg.tool_calls.insert(msg.tool_calls.end(), transported.begin(), transported.end());
    if (msg.tool_calls.empty() && msg.has_content && !msg.content.empty()) {
        harness::HarnessMonitor mon = harness::HarnessMonitor::with_new_stats();
        auto rescued = mon.rescue_xml(msg.content);
        if (!rescued.empty()) msg.tool_calls = std::move(rescued);
    }
}
void emit_nudge_status(StreamSink& sink, unsigned attempt, const NudgePolicy& nudges) {
    sink.emit(StreamEvent::status("empty production — nudge " + std::to_string(attempt) + "/" +
                                  std::to_string(nudges.max_attempts_count())));
}
} // namespace

types::Message drive_streamed_turn(
    const std::function<StreamedReply(const types::ChatRequest&)>& chat,
    std::vector<types::Message> messages, const StreamConfig& cfg, StreamSink& sink,
    std::shared_ptr<harness::HarnessStats> stats) {
    if (!stats) stats = std::make_shared<harness::HarnessStats>();
    std::vector<types::Message> transcript = messages;
    NudgePolicy nudges;
    unsigned empty_attempts = 0;
    StreamConfig cur = cfg;
    while (true) {
        types::ChatRequest req = build_request(cur, transcript);
        if (cur.recovery) {
            cur.recovery = false;
            req = apply_recovery(req, RecoveryAdjustment{});
        }
        StreamedReply reply = chat(req);
        ThinkingDemuxer demux = ThinkingDemuxer::with_preserve(cur.preserve_thinking);
        auto emit_seg = [&](DeltaKind k, const std::string& text) {
            sink.emit(k == DeltaKind::Thinking ? StreamEvent::thinking(text)
                                               : StreamEvent::content(text));
        };
        demux.push_delta(reply.raw, emit_seg);
        demux.finish_delta(emit_seg);
        types::Message assistant = demux.into_message();
        finish_message_tools(assistant, reply.tool_calls);
        if (is_empty_production(assistant) && nudges.should_nudge(empty_attempts)) {
            empty_attempts++;
            emit_nudge_status(sink, empty_attempts, nudges);
            stats->record_empty_prod();
            transcript.push_back(assistant);
            transcript = nudges.nudge(std::move(transcript));
            continue;
        }
        transcript.push_back(assistant);
        return transcript.back();
    }
}

types::Message chat_client_turn(const ChatClient& client, std::vector<types::Message> messages,
                                const StreamConfig& cfg, StreamSink& sink,
                                std::shared_ptr<harness::HarnessStats> stats) {
    if (!stats) stats = std::make_shared<harness::HarnessStats>();
    std::vector<types::Message> transcript = messages;
    NudgePolicy nudges;
    unsigned empty_attempts = 0;
    StreamConfig cur = cfg;
    while (true) {
        types::ChatRequest req = build_request(cur, transcript);
        if (cur.recovery) {
            cur.recovery = false;
            req = apply_recovery(req, RecoveryAdjustment{});
        }
        ThinkingDemuxer demux = ThinkingDemuxer::with_preserve(cur.preserve_thinking);
        StreamedReply reply = client.chat_stream(req, [&](const std::string& delta) {
            if (!delta.empty()) {
                demux.push_delta(delta, [&](DeltaKind k, const std::string& seg) {
                    sink.emit(k == DeltaKind::Thinking ? StreamEvent::thinking(seg)
                                                      : StreamEvent::content(seg));
                });
            }
            return !sink.is_aborted();
        });
        auto emit_seg = [&](DeltaKind k, const std::string& text) {
            sink.emit(k == DeltaKind::Thinking ? StreamEvent::thinking(text)
                                               : StreamEvent::content(text));
        };
        demux.finish_delta(emit_seg);
        types::Message assistant = demux.into_message();
        finish_message_tools(assistant, reply.tool_calls);
        if (is_empty_production(assistant) && nudges.should_nudge(empty_attempts)) {
            empty_attempts++;
            emit_nudge_status(sink, empty_attempts, nudges);
            stats->record_empty_prod();
            transcript.push_back(assistant);
            transcript = nudges.nudge(std::move(transcript));
            continue;
        }
        transcript.push_back(assistant);
        return transcript.back();
    }
}

} // namespace marmel::llm
