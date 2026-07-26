FROM python:3.11-slim

RUN python -m pip install --no-cache-dir \
    "private-assistant-openwakeword[onnx]==0.6.0" \
    "edge-tts==7.2.8" \
    "google-genai==2.14.0"

# The wheel intentionally omits the shared mel/embedding ONNX files. Fetch
# them into the immutable image at build time; production KWS then runs with
# --network none and never downloads anything while microphone audio exists.
RUN python -c "import openwakeword.utils; openwakeword.utils.download_models(model_names=['none'])"

ENTRYPOINT ["python3"]
