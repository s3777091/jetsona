#pragma once

#include "llm_client.h"
#include "tools.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jetson {

/* Owns the chat history + the LlmClient and runs LLM calls off the UI thread.
 *
 * If a ToolRegistry is attached (SetTools), Send() runs a tool-calling loop:
 *   1. append the user turn
 *   2. ChatWithTools(history, tool defs) -> {content, tool_calls}
 *   3. if tool_calls: execute each tool locally, append the assistant
 *      tool_call message + a "tool" role result per call, loop (max 6 rounds)
 *   4. else: append the assistant text, call cb(reply, "")
 *
 * If no registry is attached, Send() does a single plain Chat() (legacy).
 *
 * Optional OnToolEvent(name, status) fires on the WORKER thread when a tool is
 * about to run ("start") and when it finishes ("ok"/"err"); the caller can
 * marshal it to the UI to show "dang go web_search...". May be null.
 *
 * cb is invoked once, on the worker thread. The caller must marshal to the UI
 * (Application::Schedule + lv_lock) before touching LVGL. busy() is set while a
 * request is in flight; Send() rejects concurrent calls. */
class Conversation {
public:
    using ReplyCb = std::function<void(std::string reply, std::string err)>;
    using ToolEventCb = std::function<void(std::string tool_name, std::string status)>;

    Conversation();
    ~Conversation();

    bool Send(const std::string &user_text, ReplyCb cb);

    void SetTools(std::shared_ptr<ToolRegistry> tools) { tools_ = std::move(tools); }
    void SetOnToolEvent(ToolEventCb cb) { on_tool_event_ = std::move(cb); }

    bool busy() const { return busy_.load(); }
    void Clear();
    size_t TurnCount() const;
    std::vector<ChatMessage> History() const;
    void ReloadConfig();

private:
    LlmClient client_;
    mutable std::mutex mtx_;
    std::vector<ChatMessage> history_;
    std::atomic<bool> busy_{false};
    std::shared_ptr<ToolRegistry> tools_;
    ToolEventCb on_tool_event_;

    // Runs the tool loop on the worker thread. Returns final reply + err.
    void RunWithTools(std::vector<ChatMessage> &history,
                     std::string &out_reply, std::string &out_err);
};

} // namespace jetson