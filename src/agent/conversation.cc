#include "conversation.h"
#include "esp_log.h"

#include <thread>
#include <utility>

#define TAG "Conversation"

namespace jetson {

namespace {
constexpr int kMaxToolRounds = 6;
}

Conversation::Conversation() = default;
Conversation::~Conversation() = default;

bool Conversation::Send(const std::string &user_text, ReplyCb cb) {
    if (busy_.exchange(true)) {
        ESP_LOGW(TAG, "Send rejected: request in flight");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        history_.push_back({"user", user_text, "", ""});
    }

    // Copy the registry pointer so the worker can read tool defs without
    // holding history_ mutex during the network call.
    auto tools = tools_;
    std::vector<ChatMessage> snapshot;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snapshot = history_;
    }

    std::thread([this, snapshot = std::move(snapshot), tools = std::move(tools),
                 cb = std::move(cb)]() mutable {
        std::string reply, err;
        if (tools && tools->size() > 0) {
            // Tool-calling loop works on a local copy and merges back at end.
            std::vector<ChatMessage> hist = snapshot;
            RunWithTools(hist, reply, err);
            if (err.empty()) {
                std::lock_guard<std::mutex> lk(mtx_);
                // Replace tail with the fully-evolved history.
                history_ = hist;
            }
        } else {
            bool ok = client_.Chat(snapshot, reply, err);
            if (ok) {
                std::lock_guard<std::mutex> lk(mtx_);
                history_.push_back({"assistant", reply, "", ""});
            }
        }
        busy_.store(false);
        if (cb) cb(err.empty() ? reply : std::string(), err);
    }).detach();

    return true;
}

void Conversation::RunWithTools(std::vector<ChatMessage> &history,
                                std::string &out_reply, std::string &out_err) {
    auto defs = tools_->Definitions();
    for (int round = 0; round < kMaxToolRounds; ++round) {
        ChatResult r = client_.ChatWithTools(history, defs);
        if (!r.ok) { out_err = r.error; return; }

        if (!r.HasToolCalls()) {
            // Final assistant text.
            history.push_back({"assistant", r.content, "", ""});
            out_reply = r.content;
            return;
        }

        // The assistant message that requested tools — replay verbatim.
        history.push_back({"assistant", r.content, "", r.tool_calls_json});

        // Execute every requested tool and feed each result back as a "tool"
        // message. OpenAI requires one tool message per tool_call, correlated
        // by tool_call_id.
        for (const auto &tc : r.tool_calls) {
            if (on_tool_event_) on_tool_event_(tc.name, "start");
            std::string result;
            Tool *tool = tools_->Find(tc.name);
            if (!tool) {
                result = "ERROR: tool khong ton tai: " + tc.name;
            } else {
                result = tool->Execute(tc.arguments_json);
            }
            if (on_tool_event_) {
                on_tool_event_(tc.name, (result.rfind("ERROR", 0) == 0) ? "err" : "ok");
            }
            ESP_LOGI(TAG, "tool %s -> %s", tc.name.c_str(),
                     result.substr(0, 120).c_str());
            history.push_back({"tool", result, tc.id, ""});
        }
        // Loop again: the model sees the tool results and either calls more
        // tools or produces a final answer.
    }
    out_err = "Agent vuot qua so luong tool-call cho phep";
}

void Conversation::Clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    history_.clear();
}

size_t Conversation::TurnCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return history_.size();
}

std::vector<ChatMessage> Conversation::History() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return history_;
}

void Conversation::ReloadConfig() {
    client_.ConfigureFromSettings();
}

} // namespace jetson