#!/usr/bin/env python3
"""Gemini Live sidecar: the conversation half of the voice loop.

jetson_fw owns the microphone and the speaker and nothing else changes about
that. What changes is everything between them. The old path ran four network
stages back to back -- transcribe, think, think again, synthesise -- and the
user heard nothing until the last one finished, could not interrupt, and lost
whatever they said while it worked. This replaces all four with one streaming
session: audio goes up as it is captured, audio comes back as it is generated,
and the server's own voice-activity detection decides when a turn ended.

Tools stay where they are. The firmware sends its registry on connect and
answers tool calls itself, so weather, music and alarms keep running through
the same C++ code they always did.

The session is deliberately tied to the socket connection: jetson_fw connects
when the wake word fires and disconnects when the conversation goes idle, so
billing follows conversation time instead of uptime.

Protocol, both directions: 4-byte little-endian payload length, one type byte,
then the payload.

  firmware -> here      here -> firmware
  0x01 audio (16 kHz)   0x81 audio (24 kHz), play immediately
  0x02 tool result      0x82 interrupted, stop playback now
  0x03 config           0x83 tool call
                        0x84 turn complete
                        0x85 transcript (logging only)
"""

import argparse
import asyncio
import json
import os
import struct
import sys
import traceback

from google import genai
from google.genai import types

MSG_AUDIO_IN = 0x01
MSG_TOOL_RESULT = 0x02
MSG_CONFIG = 0x03

MSG_AUDIO_OUT = 0x81
MSG_INTERRUPTED = 0x82
MSG_TOOL_CALL = 0x83
MSG_TURN_COMPLETE = 0x84
MSG_TRANSCRIPT = 0x85

INPUT_MIME = "audio/pcm;rate=16000"


def log(message):
    print(message, flush=True)


async def read_frame(reader):
    """Return (type, payload) or (None, None) when the peer goes away."""
    header = await reader.readexactly(5)
    length, kind = struct.unpack("<IB", header)
    payload = await reader.readexactly(length) if length else b""
    return kind, payload


def frame(kind, payload=b""):
    return struct.pack("<IB", len(payload), kind) + payload


def build_tools(declarations):
    """Turn the firmware's tool registry into Live API declarations.

    The firmware sends the same JSON Schema it already sends to the chat
    endpoint, so the registry stays the single source of truth and cannot drift
    from what the model is told exists.
    """
    functions = []
    for decl in declarations:
        params = decl.get("parameters") or {}
        try:
            schema = types.Schema.model_validate(params) if params else None
        except Exception:  # noqa: BLE001 - a bad schema must not kill the session
            log("bỏ qua schema hỏng của tool %r" % decl.get("name"))
            schema = None
        functions.append(types.FunctionDeclaration(
            name=decl["name"],
            description=decl.get("description", ""),
            parameters=schema,
        ))
    return [types.Tool(function_declarations=functions)] if functions else None


class Session:
    """One conversation, lasting exactly as long as one firmware connection."""

    def __init__(self, reader, writer, client, model, default_voice):
        self.reader = reader
        self.writer = writer
        self.client = client
        self.model = model
        self.default_voice = default_voice
        self.live = None
        self.pending = {}   # tool call id -> name, for correlating results

    async def send(self, kind, payload=b""):
        self.writer.write(frame(kind, payload))
        await self.writer.drain()

    async def run(self):
        kind, payload = await read_frame(self.reader)
        if kind != MSG_CONFIG:
            log("kết nối không mở bằng CONFIG (nhận %#x); đóng" % kind)
            return
        config_json = json.loads(payload.decode("utf-8"))

        voice = config_json.get("voice") or self.default_voice
        live_config = types.LiveConnectConfig(
            response_modalities=["AUDIO"],
            system_instruction=config_json.get("system") or None,
            tools=build_tools(config_json.get("tools") or []),
            input_audio_transcription=types.AudioTranscriptionConfig(),
            output_audio_transcription=types.AudioTranscriptionConfig(),
            speech_config=types.SpeechConfig(
                voice_config=types.VoiceConfig(
                    prebuilt_voice_config=types.PrebuiltVoiceConfig(
                        voice_name=voice))),
        )
        tool_count = len(config_json.get("tools") or [])
        log("mở session: model=%s voice=%s tools=%d" %
            (self.model, voice, tool_count))

        async with self.client.aio.live.connect(
                model=self.model, config=live_config) as live:
            self.live = live
            uplink = asyncio.create_task(self.pump_firmware())
            downlink = asyncio.create_task(self.pump_gemini())
            done, pending = await asyncio.wait(
                {uplink, downlink}, return_when=asyncio.FIRST_COMPLETED)
            for task in pending:
                task.cancel()
            for task in done:
                if task.exception():
                    raise task.exception()

    async def pump_firmware(self):
        """Microphone audio and tool results, firmware -> Gemini."""
        while True:
            try:
                kind, payload = await read_frame(self.reader)
            except (asyncio.IncompleteReadError, ConnectionResetError):
                log("firmware ngắt kết nối; đóng session")
                return
            if kind == MSG_AUDIO_IN:
                await self.live.send_realtime_input(
                    audio=types.Blob(data=payload, mime_type=INPUT_MIME))
            elif kind == MSG_TOOL_RESULT:
                result = json.loads(payload.decode("utf-8"))
                call_id = result.get("id")
                await self.live.send_tool_response(
                    function_responses=[types.FunctionResponse(
                        id=call_id,
                        name=result.get("name") or self.pending.get(call_id, ""),
                        response={"result": result.get("result", "")})])
                self.pending.pop(call_id, None)
            else:
                log("bỏ qua khung lạ từ firmware: %#x" % kind)

    async def pump_gemini(self):
        """Generated audio, interruptions and tool calls, Gemini -> firmware."""
        while True:
            async for message in self.live.receive():
                if message.tool_call:
                    for call in message.tool_call.function_calls:
                        self.pending[call.id] = call.name
                        await self.send(MSG_TOOL_CALL, json.dumps({
                            "id": call.id,
                            "name": call.name,
                            "args": dict(call.args or {}),
                        }).encode("utf-8"))

                content = message.server_content
                if content:
                    # Barge-in: the user started talking over the reply, so the
                    # audio already queued downstream is stale and must be
                    # dropped rather than played after they stopped.
                    if content.interrupted:
                        await self.send(MSG_INTERRUPTED)
                    for who, part in (
                            ("user", content.input_transcription),
                            ("assistant", content.output_transcription)):
                        if part and part.text:
                            await self.send(MSG_TRANSCRIPT, json.dumps(
                                {"role": who, "text": part.text}
                            ).encode("utf-8"))
                    if content.turn_complete:
                        await self.send(MSG_TURN_COMPLETE)

                if message.data:
                    await self.send(MSG_AUDIO_OUT, message.data)


async def serve(args):
    api_key = os.environ.get("GEMINI_API_KEY", "")
    if not api_key:
        sys.exit("GEMINI_API_KEY chưa được đặt")
    client = genai.Client(api_key=api_key,
                          http_options={"api_version": "v1beta"})

    socket_path = args.socket
    os.makedirs(os.path.dirname(socket_path), exist_ok=True)
    try:
        os.unlink(socket_path)
    except FileNotFoundError:
        pass

    async def on_client(reader, writer):
        session = Session(reader, writer, client, args.model, args.voice)
        try:
            await session.run()
        except Exception:  # noqa: BLE001
            # One failed conversation must never take the sidecar down with it;
            # the next wake word has to find a working socket.
            log("session lỗi:\n%s" % traceback.format_exc())
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:  # noqa: BLE001
                pass
            log("session đóng")

    server = await asyncio.start_unix_server(on_client, path=socket_path)
    os.chmod(socket_path, 0o666)
    log("Gemini Live sẵn sàng: model=%s socket=%s" % (args.model, socket_path))
    async with server:
        await server.serve_forever()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument(
        "--model",
        default="gemini-2.5-flash-native-audio-preview-12-2025")
    parser.add_argument("--voice", default="Aoede")
    asyncio.run(serve(parser.parse_args()))


if __name__ == "__main__":
    main()
