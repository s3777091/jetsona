#pragma once

#include "tools.h"

#include <string>

namespace jetson {

/* Countdown timer. The existing alarm only fires at a wall-clock time, so
 * "hẹn giờ mười phút nữa" -- one of the things people most often ask a speaker
 * for -- had nowhere to go and the model would either refuse or misuse the
 * alarm. Speaks when it expires, so the user does not have to watch anything. */
class TimerTool : public Tool {
public:
    TimerTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Things the user has asked Nova to remember about them.
 *
 * Notes are a list the user reads back; this is different. It holds facts that
 * should shape later answers -- where they work, who their sister is, that
 * they take coffee without sugar -- and the model is told to consult it rather
 * than asking again. Without it every session starts as a stranger, which is
 * the opposite of an assistant that lives in the room. */
class MemoryTool : public Tool {
public:
    enum Op { Remember, Recall, Forget };
    explicit MemoryTool(Op op);
    std::string Execute(const std::string &arguments_json) override;

    // Everything remembered, as a short block for the system prompt. Empty when
    // nothing has been stored yet.
    static std::string Summary();

private:
    Op op_;
};

} // namespace jetson
