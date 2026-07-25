#!/usr/bin/env python3
"""Compare candidate chat models on the latency that the voice loop actually feels.

A spoken turn that needs a tool costs two LLM round trips, and the user hears
nothing until the second one finishes, so the number that matters is not
tokens/sec on a marketing page but the wall time of those two calls carrying
this device's real system prompt and its real tool registry.

The probe replays exactly that: round 1 must choose a tool, round 2 must turn
the tool result into a spoken sentence. It also reports the reply length,
because every character is read aloud and the user has to sit through it.

  export OPENROUTER_API_KEY=...
  python3 scripts/llm_latency_probe.py google/gemini-2.5-flash-lite ...

With no model arguments it probes a default shortlist. Point it at another
gateway with --base-url (it speaks the OpenAI chat-completions dialect).
"""

import argparse
import json
import os
import statistics
import sys
import time
import urllib.error
import urllib.request

DEFAULT_MODELS = [
    "google/gemini-2.5-flash-lite",
    "google/gemini-2.5-flash",
    "openai/gpt-4o-mini",
]

# The turn being replayed: a weather question, which is the shortest possible
# tool-using turn and therefore the floor for this device's latency.
USER_TURN = "thời tiết hôm nay thế nào"
TOOL_RESULT = "Đà Nẵng: Nhiều mây · 28°C (27°–34°) · Ẩm 82%"

WEATHER_TOOL = {
    "type": "function",
    "function": {
        "name": "weather",
        "description": "Thời tiết hiện tại và dự báo cho vị trí của thiết bị.",
        "parameters": {"type": "object", "properties": {}},
    },
}

# Stand-ins for the rest of src/agent/tools.cc. The count and rough size matter
# for latency; the exact wording does not.
OTHER_TOOLS = [
    "device_status", "set_volume", "pc_power", "ringtone", "music_control",
    "music_play", "music_album", "alarm", "calendar_add", "calendar_list",
    "reminder_add", "reminder_list", "reminder_complete", "note_add",
    "note_list", "web_search", "web_open", "open_app", "wifi_control",
]


def filler_tool(name):
    return {
        "type": "function",
        "function": {
            "name": name,
            "description": "Điều khiển hoặc truy vấn %s trên thiết bị. Dùng khi "
                           "người dùng yêu cầu thao tác liên quan." % name,
            "parameters": {
                "type": "object",
                "properties": {"query": {"type": "string",
                                         "description": "Nội dung yêu cầu."}},
            },
        },
    }


def post(base_url, api_key, body, timeout):
    req = urllib.request.Request(
        base_url.rstrip("/") + "/chat/completions",
        data=json.dumps(body).encode("utf-8"),
        headers={"Authorization": "Bearer " + api_key,
                 "Content-Type": "application/json"},
    )
    started = time.time()
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        payload = json.load(resp)
    return payload["choices"][0]["message"], time.time() - started


def probe_once(base_url, api_key, model, prompt, tools, timeout):
    """Return (round1_s, round2_s, reply) or raise."""
    messages = [{"role": "system", "content": prompt},
                {"role": "user", "content": USER_TURN}]

    first, t1 = post(base_url, api_key, {
        "model": model, "messages": messages,
        "tools": tools, "temperature": 0.7}, timeout)

    calls = first.get("tool_calls") or []
    if not calls:
        raise RuntimeError("không gọi tool, trả thẳng: %r"
                           % (first.get("content") or "")[:60])

    messages.append({"role": "assistant", "content": first.get("content") or "",
                     "tool_calls": calls})
    for call in calls:
        messages.append({"role": "tool", "tool_call_id": call.get("id", "0"),
                         "content": TOOL_RESULT})

    second, t2 = post(base_url, api_key, {
        "model": model, "messages": messages,
        "tools": tools, "temperature": 0.7}, timeout)
    return t1, t2, (second.get("content") or "").strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("models", nargs="*", default=None)
    parser.add_argument("--base-url",
                        default=os.environ.get("OPENROUTER_BASE_URL",
                                               "https://openrouter.ai/api/v1"))
    parser.add_argument("--api-key-env", default="OPENROUTER_API_KEY")
    parser.add_argument("--prompt-file",
                        help="system prompt; defaults to a short stand-in")
    parser.add_argument("--rounds", type=int, default=3,
                        help="repeats per model; the median is reported")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    api_key = os.environ.get(args.api_key_env, "")
    if not api_key:
        sys.exit("%s chưa được đặt" % args.api_key_env)

    if args.prompt_file:
        prompt = open(args.prompt_file, encoding="utf-8").read()
    else:
        prompt = ("Bạn là Nova, trợ lý giọng nói trong thiết bị này. Tự gọi tool "
                  "khi cần. Trả lời đúng một câu ngắn để đọc qua loa.")

    tools = [WEATHER_TOOL] + [filler_tool(n) for n in OTHER_TOOLS]
    models = args.models or DEFAULT_MODELS

    print("prompt=%d B, %d tools, %d lần/model, tổng = 2 vòng LLM mỗi lần\n"
          % (len(prompt.encode("utf-8")), len(tools), args.rounds))
    print("  %-38s %8s %8s %8s   %s"
          % ("model", "vòng1", "vòng2", "TỔNG", "độ dài trả lời"))

    for model in models:
        totals, replies = [], []
        error = None
        for _ in range(args.rounds):
            try:
                t1, t2, reply = probe_once(base_url=args.base_url,
                                           api_key=api_key, model=model,
                                           prompt=prompt, tools=tools,
                                           timeout=args.timeout)
            except urllib.error.HTTPError as err:
                error = "HTTP %s %s" % (err.code, err.read()[:120].decode(
                    "utf-8", "replace"))
                break
            except Exception as err:  # noqa: BLE001 - report whatever broke
                error = str(err)
                break
            totals.append((t1, t2))
            replies.append(reply)
        if error:
            print("  %-38s   LỖI  %s" % (model, error))
            continue
        r1 = statistics.median(t for t, _ in totals)
        r2 = statistics.median(t for _, t in totals)
        chars = statistics.median(len(r) for r in replies)
        print("  %-38s %7.2fs %7.2fs %7.2fs   %d ký tự"
              % (model, r1, r2, r1 + r2, chars))
        print("      %s" % replies[-1][:100])


if __name__ == "__main__":
    main()
