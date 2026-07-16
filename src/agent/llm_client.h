#pragma once

#include <string>
#include <vector>

namespace jetson {

/* One chat turn. role is "system" | "user" | "assistant" | "tool".
 * For role=="tool", tool_call_id names which tool call this result answers. */
struct ChatMessage {
    std::string role;
    std::string content;
    std::string tool_call_id;   // only for role=="tool"
    // For role=="assistant" with tool calls: serialized list of tool calls
    // (the raw JSON the server returned in message.tool_calls), replayed
    // verbatim in the next request so the server can correlate them.
    std::string tool_calls_json;
};

/* A function the model can call. parameters_json is a JSON Schema string
 * describing the arguments (e.g. {"type":"object","properties":{...}}). */
struct ToolDef {
    std::string name;
    std::string description;
    std::string parameters_json;
};

/* One tool invocation the model wants us to run. arguments is the raw JSON
 * string the model produced (parse it as you see fit). */
struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
};

/* Result of a chat turn that may contain tool calls. */
struct ChatResult {
    bool ok = false;
    std::string error;
    std::string content;              // assistant text (empty if tool_calls)
    std::vector<ToolCall> tool_calls; // non-empty when the model wants tools
    // Raw assistant-message JSON (role+content+tool_calls) the server returned,
    // replayed verbatim in the next request so the server can correlate them.
    std::string tool_calls_json;
    bool HasToolCalls() const { return !tool_calls.empty(); }
};

/* Minimal OpenAI-compatible chat-completions client. Targets Ollama Cloud
 * (https://ollama.com/v1) but works with any endpoint that accepts
 * POST {base_url}/chat/completions with a Bearer token. Supports tool calling
 * (function calling) for models that expose it (qwen2.5, gpt-oss, llama3.1+).
 *
 * Blocking API: must be called on a worker thread, never the LVGL thread.
 * Non-streaming (stream=false) — we wait for the full reply / tool_calls.
 *
 * Config (highest priority first):
 *   env OLLAMA_API_KEY / OLLAMA_BASE_URL / OLLAMA_MODEL
 *   Settings namespace "llm": api_key, base_url, model, system_prompt, temperature_x100
 * Defaults: base_url=https://ollama.com/v1, model=qwen2.5:7b */
class LlmClient {
public:
    LlmClient();
    ~LlmClient();

    void ConfigureFromSettings();

    bool Configured() const { return !base_url_.empty() && !model_.empty(); }
    std::string Model() const { return model_; }
    std::string SystemPrompt() const { return system_prompt_; }

    // Plain chat (no tools). Returns true on success with reply in out_reply.
    bool Chat(const std::vector<ChatMessage> &messages,
              std::string &out_reply, std::string &out_err);

    // Chat with tools. If the model returns tool_calls, result.tool_calls is
    // filled and result.content is empty; the caller should execute the tools,
    // append "tool" role messages, and call again. Returns result.ok=false on
    // network/HTTP/parse errors (result.error set).
    ChatResult ChatWithTools(const std::vector<ChatMessage> &messages,
                             const std::vector<ToolDef> &tools);

private:
    std::string base_url_;
    std::string api_key_;
    std::string model_;
    std::string system_prompt_;
    double temperature_ = 0.7;
};

} // namespace jetson