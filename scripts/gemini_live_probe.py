#!/usr/bin/env python3
"""Exercise the Gemini Live sidecar the way jetson_fw will.

Speaks the sidecar's framed protocol end to end: opens with the system prompt
and tool registry, streams a WAV in at real-time pace as the capture thread
would, answers whatever tool the model reaches for, and writes the reply audio
out. Running this against a change is how the socket protocol and the session
lifecycle get checked without rebuilding the firmware.

  python3 scripts/gemini_live_probe.py /run/jetsona-live/realtime.sock \\
      question.wav reply.pcm

The WAV must be 16 kHz mono S16LE -- the same format the ReSpeaker delivers.
The reply is 24 kHz mono S16LE; play it with
  ffplay -f s16le -ar 24000 -ac 1 reply.pcm
"""
import argparse
import json
import socket
import struct
import time
import wave

MSG_AUDIO_IN, MSG_TOOL_RESULT, MSG_CONFIG = 0x01, 0x02, 0x03
MSG_AUDIO_OUT, MSG_INTERRUPTED, MSG_TOOL_CALL = 0x81, 0x82, 0x83
MSG_TURN_COMPLETE, MSG_TRANSCRIPT = 0x84, 0x85

# A small stand-in for src/agent/tools.cc, enough to prove tools survive the
# round trip. The firmware sends its real registry.
TOOLS = [{
    "name": "weather",
    "description": "Thời tiết hiện tại và dự báo cho vị trí của thiết bị.",
    "parameters": {"type": "OBJECT", "properties": {}},
}, {
    "name": "music_play",
    "description": "Phát một bài hát hoặc nghệ sĩ.",
    "parameters": {"type": "OBJECT",
                   "properties": {"query": {"type": "STRING"}},
                   "required": ["query"]},
}]

FAKE_RESULTS = {
    "weather": "Đà Nẵng: Nhiều mây, 28 độ C, độ ẩm 82%",
    "music_play": "Đang phát.",
}


def send(sock, kind, payload=b""):
    sock.sendall(struct.pack("<IB", len(payload), kind) + payload)


def recv_exact(sock, count):
    buf = bytearray()
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("socket_path")
    parser.add_argument("wav_in")
    parser.add_argument("pcm_out")
    parser.add_argument("--system-file", default="",
                        help="system prompt; a short stand-in is used without it")
    parser.add_argument("--voice", default="Aoede")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    with wave.open(args.wav_in, "rb") as wav:
        if wav.getframerate() != 16000 or wav.getsampwidth() != 2 \
                or wav.getnchannels() != 1:
            raise SystemExit("cần WAV 16 kHz mono S16LE")
        pcm = wav.readframes(wav.getnframes())

    system = (open(args.system_file, encoding="utf-8").read()
              if args.system_file else
              "Bạn là Nova, trợ lý giọng nói trong thiết bị này. Tự gọi tool "
              "khi cần. Trả lời ngắn gọn để đọc qua loa.")

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(args.socket_path)
    send(sock, MSG_CONFIG, json.dumps(
        {"system": system, "tools": TOOLS, "voice": args.voice}
    ).encode("utf-8"))

    step = 16000 * 2 * 20 // 1000          # 20 ms per frame, as the mic delivers
    for offset in range(0, len(pcm), step):
        send(sock, MSG_AUDIO_IN, pcm[offset:offset + step])
        time.sleep(0.02)
    spoke_at = time.time()
    print("gửi xong %.2f s audio" % (len(pcm) / 2 / 16000))

    sock.settimeout(args.timeout)
    out = bytearray()
    heard, said = [], []
    first_audio = None
    while True:
        header = recv_exact(sock, 5)
        if not header:
            print("sidecar đóng kết nối")
            break
        length, kind = struct.unpack("<IB", header)
        payload = recv_exact(sock, length) if length else b""

        if kind == MSG_AUDIO_OUT:
            if first_audio is None:
                first_audio = time.time() - spoke_at
                print("  tiếng đầu tiên sau %.2f s" % first_audio)
            out.extend(payload)
        elif kind == MSG_TOOL_CALL:
            call = json.loads(payload.decode("utf-8"))
            print("  tool: %s(%s)  (+%.2fs)"
                  % (call["name"], call["args"], time.time() - spoke_at))
            send(sock, MSG_TOOL_RESULT, json.dumps({
                "id": call["id"], "name": call["name"],
                "result": FAKE_RESULTS.get(call["name"], "ok"),
            }).encode("utf-8"))
        elif kind == MSG_INTERRUPTED:
            # The firmware must drop queued audio here, not play it late.
            print("  BỊ NGẮT LỜI -> bỏ audio đang chờ")
            out.clear()
        elif kind == MSG_TRANSCRIPT:
            item = json.loads(payload.decode("utf-8"))
            (heard if item["role"] == "user" else said).append(item["text"])
        elif kind == MSG_TURN_COMPLETE:
            print("  lượt kết thúc")
            break

    print("nghe được: %r" % "".join(heard))
    print("nói      : %r" % "".join(said))
    with open(args.pcm_out, "wb") as handle:
        handle.write(bytes(out))
    print("audio    : %.2f s @24kHz -> %s" % (len(out) / 2 / 24000, args.pcm_out))
    sock.close()


if __name__ == "__main__":
    main()
