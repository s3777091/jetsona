#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "llm_client.h"  // ToolDef (used by ToolRegistry::Definitions)

namespace jetson {

/* One tool the agent can call. Execute() receives the raw JSON arguments
 * string from the model and returns a string result that is fed back to the
 * model as the "tool" role message content (plain text or JSON-as-text).
 *
 * All tools run on the worker thread (blocking) — they must not touch LVGL. */
class Tool {
public:
    Tool(const std::string &name, const std::string &description,
         const std::string &parameters_json)
        : name_(name), description_(description), parameters_json_(parameters_json) {}
    virtual ~Tool() = default;

    const std::string &name() const { return name_; }
    const std::string &description() const { return description_; }
    const std::string &parameters_json() const { return parameters_json_; }

    // Returns the result string. On failure, return a string starting with
    // "ERROR: ..." so the model can react to it.
    virtual std::string Execute(const std::string &arguments_json) = 0;

private:
    std::string name_;
    std::string description_;
    std::string parameters_json_;
};

/* Holds the tools the agent can use, and produces the ToolDef list that gets
 * sent to the LLM. Thread-safe enough for Register-at-startup + read-at-run. */
class ToolRegistry {
public:
    void Register(std::unique_ptr<Tool> tool);
    Tool *Find(const std::string &name) const;
    std::vector<ToolDef> Definitions() const; // for LlmClient::ChatWithTools
    size_t size() const { return tools_.size(); }

private:
    mutable std::mutex mtx_;
    std::vector<std::unique_ptr<Tool>> tools_;
};

/* ---- Concrete tools ---------------------------------------------------- */

/* Persistent task + note store backed by ~/.jetson-fw/tasks.json.
 * The "Agent sap xep cong viec" — create/list/complete/delete tasks and notes. */
class TaskStore {
public:
    static TaskStore &Instance();

    std::string CreateTask(const std::string &title, const std::string &due);
    std::string ListTasks() const;
    std::string CompleteTask(int id);
    std::string DeleteTask(int id);
    std::string AddNote(const std::string &text);
    std::string ListNotes() const;

private:
    TaskStore();
    void Load();
    void Save();

    mutable std::mutex mtx_;
    std::string path_;
    int next_id_ = 1;
    // Raw JSON kept in a string; we operate on it via nlohmann in the .cc.
    std::string data_json_;
};

/* Tool wrappers around TaskStore. */
class TaskTool : public Tool {
public:
    enum Op { Create, List, Complete, Delete };
    TaskTool(Op op);
    std::string Execute(const std::string &arguments_json) override;
private:
    Op op_;
};

class NoteTool : public Tool {
public:
    enum Op { Add, List };
    NoteTool(Op op);
    std::string Execute(const std::string &arguments_json) override;
private:
    Op op_;
};

/* Web search via an external HTTP gateway (e.g. a Lightpanda-backed gateway
 * on the VM). GET {LIGHTPANDA_SEARCH_URL}?q=<query> with an optional bearer
 * token (LIGHTPANDA_SEARCH_TOKEN). Returns a compact text digest of results.
 * If LIGHTPANDA_SEARCH_URL is empty, returns "web search chua cau hinh". */
class WebSearchTool : public Tool {
public:
    WebSearchTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Open a URL and read the page text through the gateway's /fetch endpoint
 * (LightPanda CDP render on the VM, plain-fetch fallback) so the agent can
 * read a search result instead of answering from snippets. The endpoint is
 * derived from LIGHTPANDA_SEARCH_URL (".../search" -> ".../fetch"); set
 * LIGHTPANDA_FETCH_URL to override. Same bearer token as web_search. */
class WebOpenTool : public Tool {
public:
    WebOpenTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Convenience: build the default registry used by the firmware. */
std::shared_ptr<ToolRegistry> BuildDefaultToolRegistry();

} // namespace jetson