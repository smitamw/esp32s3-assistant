# Agent Notes

## Project Purpose

This repository contains a single Arduino sketch, `ai_assistant_esp32s3.ino`, for an AI voice assistant running on an ESP32-S3. The device records microphone audio, streams it to the Gemini **Live API** (audio-to-audio), and plays the model's spoken reply through an I2S amplifier. There is no separate speech-to-text, text, or text-to-speech step: a single bidirectional WebSocket session handles the whole turn.

## Hardware Context

- Board target: ESP32-S3 using the Arduino framework and ESP-IDF I2S driver APIs.
- Speaker amplifier: MAX98357A I2S amplifier.
- Microphone: INMP441 I2S microphone.
- Input: active-low pushbutton using `INPUT_PULLUP`.
- Status LED: onboard/addressable NeoPixel driven with `neopixelWrite`.

## Pin Map

- MAX98357A LRC/LRCLK: GPIO 45 (`SPEAKER_I2S_LRCLK_PIN`)
- MAX98357A BCLK: GPIO 16 (`SPEAKER_I2S_BCLK_PIN`)
- MAX98357A DIN: GPIO 47 (`SPEAKER_I2S_DOUT_PIN`)
- MAX98357A SD/shutdown: GPIO 9 (`AMP_SD_PIN`)
- INMP441 WS: GPIO 4 (`MIC_I2S_WS_PIN`)
- INMP441 SCK/BCLK: GPIO 5 (`MIC_I2S_SCK_PIN`)
- INMP441 SD/DOUT: GPIO 6 (`MIC_I2S_DIN_PIN`)
- Button ground: GPIO 7 (`BUTTON_GROUND_PIN`)
- Button input: GPIO 15 (`BUTTON_INPUT_PIN`)
- NeoPixel: GPIO 48 (`NEOPIXEL_PIN`)

*Note: GPIO 48 is reserved for the onboard NeoPixel (`NEOPIXEL_PIN`). Keep speaker I2S signals off GPIO 48 unless the board wiring is intentionally changed and NeoPixel feedback is disabled or moved.*

## Dependencies

- **`WebSockets_Generic`** (Khoi Hoang's fork of `links2004/arduinoWebSockets`) provides the `wss://` client via the `WebSocketsClient` class: HTTP upgrade handshake, TLS, frame masking, and ping/pong. Install it via the Arduino Library Manager before compiling. Its header is `WebSocketsClient_Generic.h` (note the `_Generic` suffix); the class/API are identical to the upstream library, so the upstream `links2004/arduinoWebSockets` works too if you switch the include back to `WebSocketsClient.h`.
- The repo still declares no Arduino CLI / PlatformIO / test configuration; the library must be installed in the build environment manually.
- `WebSocketsClient::beginSSL()` is used without a pinned CA (insecure TLS), matching the project's prior `setInsecure()` behaviour. If certificate validation is wanted, switch to `beginSslWithCA()`.
- **Frame-size patch (required).** Gemini Live audio frames exceed the library's default 15 KB cap, which otherwise force-closes the socket with close code 1009 mid-reply. The library header `WebSockets_Generic.h` (around line 146) is patched to wrap its `#define WEBSOCKETS_MAX_DATA_SIZE (15 * 1024)` in an `#ifndef … #endif` guard, and the sketch raises it to `256 * 1024` with a `#define` placed **before** `#include <WebSocketsClient_Generic.h>` (works because the library is header-only). Keep this cap well below `AUDIO_FIFO_CAP` so a single frame can't overflow the playback ring. **This library edit must be re-applied if `WebSockets_Generic` is updated.**
- **PSRAM must be enabled** in the board menu (Tools → PSRAM). The library `malloc`s each frame; with PSRAM enabled the Arduino-ESP32 allocator routes large allocations to PSRAM instead of internal RAM.

## Runtime Flow

- `setup()` initializes Serial, amp shutdown, NeoPixel, button pins, plays an optional boot speaker test tone, connects Wi-Fi, then enters Idle.
- `loop()` pumps `webSocket.loop()`, debounces the button, and (only while capturing) reads the mic and streams it.
- A turn is a **press-to-start / press-to-stop**, half-duplex interaction driven by `AppState`:
  - **Idle** (red LED): mic I2S installed at `AUDIO_SAMPLE_RATE` (`16000 Hz`), not streaming. Waiting for a press.
  - **Capturing** (amber LED): on the first press, `ensureLiveSession()` connects the WebSocket if needed, `activityStart` is sent, and mic frames stream up.
  - **Responding** (thinking animation, then green): the second press sends `activityEnd`, switches I2S to speaker TX at `TTS_SAMPLE_RATE` (`24000 Hz`), enables the MAX98357A, and plays Gemini audio from the playback FIFO (see Audio Details).
- The device returns to Idle once `serverContent.turnComplete` has been seen **and** the playback FIFO has drained. A press during Responding cancels playback and returns to Idle (further audio for that turn is ignored). A mid-turn WebSocket disconnect sets `resetPending`, which `loop()` honors by aborting to Idle.
- The single I2S peripheral (`I2S_NUM_0`) is uninstalled and reinstalled when switching between microphone RX and speaker TX; the half-duplex turn structure maps onto this naturally.

## Gemini Live API

- Transport is a single bidirectional WebSocket:
  `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=API_KEY`
- Model: `GEMINI_LIVE_MODEL` = `gemini-3.1-flash-live-preview` ("Gemini 3 Flash Live"), an audio-to-audio model. `GEMINI_TTS_VOICE` selects the prebuilt voice via the setup `speechConfig`.
- First message is `BidiGenerateContentSetup` (`liveSendSetup()`): model, `responseModalities:["AUDIO"]`, `speechConfig` voice, a system instruction, and `realtimeInputConfig.automaticActivityDetection.disabled=true` for manual turn control.
- Input audio: raw little-endian 16-bit mono PCM @ 16 kHz, base64-encoded and sent as `realtimeInput.audio` JSON messages with `mimeType` `audio/pcm;rate=16000` (`sendMicChunk()`).
- Output audio: raw little-endian 16-bit mono PCM @ 24 kHz, arriving in `serverContent.modelTurn.parts[].inlineData.data` (base64). `enqueueLiveAudio()` decodes it into the playback FIFO; `drainPlaybackChunk()` plays it (see Audio Details).
- Turn signals handled: `setupComplete`, `serverContent.turnComplete` (sets `turnCompleteSeen`). Activity is bracketed by `activityStart` / `activityEnd`.
- TLS uses insecure mode (no CA pinned) — see Dependencies.

## Audio Details

- Microphone samples are decoded by shifting raw 32-bit I2S samples right by `MIC_SAMPLE_SHIFT` (`16`); the louder of the two decoded channels is kept (`chooseMicSample`).
- Mic audio is streamed continuously while capturing — there is no fixed recording-length cap and no large PSRAM clip buffer. Samples are batched in `micSendBuffer` (`MIC_SEND_SAMPLES`, ~64 ms) and sent per chunk.
- The MAX98357A playback path expects 16-bit PCM and writes duplicated stereo frames; `PLAYBACK_GAIN` is applied per sample.
- **Playback FIFO + dedicated task (decoupled from the WebSocket).** Audio is *not* written to I2S from inside the WS callback — doing so blocks `webSocket.loop()` and starves server PING/PONG, dropping the session. It's a single-producer/single-consumer PSRAM ring (`audioFifo`, `AUDIO_FIFO_CAP` = 2 MB, allocated once in `setup()`):
  - **Producer** — `enqueueLiveAudio()` runs in the WS callback (loop()/core 1): base64-decodes audio and appends raw PCM via `audioFifoPush()`, advancing `audioFifoHead`. Returns immediately.
  - **Consumer** — `playbackTask()` runs pinned to **core 0**, draining one `MONO_SAMPLES_PER_CHUNK` block at a time via `drainPlaybackChunk()` → `writeSpeakerSamples()`, advancing `audioFifoTail`. Because it runs on a different core, it keeps feeding I2S at real time even while core 1 blocks ingesting a large frame. Real-time pacing comes from the blocking `i2s_write`.
  - The ring is large because Gemini delivers a reply faster than real time, so the buffer must hold the whole lead. `head == tail` is empty (one slot kept unused); indices are `volatile` and published after the data with `__sync_synchronize()`, so the SPSC ring is safe across cores without a lock. Decoded bytes are appended contiguously, so 16-bit alignment is preserved across frames. A ~150 ms pre-buffer (`PLAYBACK_PREBUFFER_BYTES`) avoids initial underrun.
  - **I2S teardown safety:** `playbackRunning` gates the task; before switching the I2S bus (`enterIdle`), `stopPlaybackAndWait()` clears it and waits for `playbackTaskIdle`, so the task is never mid-`i2s_write` during uninstall. `endCapture` sets `playbackRunning = true` only after the speaker bus is installed. `loop()` ends the turn when `turnCompleteSeen` and the ring has drained (`audioFifoFill() < 2`).
- `PRINT_MIC_RAW_DEBUG` is `true`, so the sketch prints raw I2S microphone statistics about once per second while capturing.
- `PLAY_TEST_TONE_ON_BOOT` is `true`, so boot temporarily switches to speaker I2S and plays a short square-wave test tone before entering Idle.

**The sketch currently contains placeholder Wi-Fi credentials and a Gemini API key inline.**

## Maintenance Guidance

- Keep the sketch Arduino-compatible C++ and preserve the existing style: `constexpr` constants, small helpers, and explicit Serial diagnostics.
- Be careful when changing I2S setup. The code intentionally uninstalls and reinstalls I2S0 when switching between microphone RX (16 kHz, 32-bit) and speaker TX (24 kHz, 16-bit).
- `webSocket.loop()` must be called frequently; server messages are handled in the `onWsEvent` callback, which runs synchronously inside `webSocket.loop()`. **The callback must never block** (e.g. on `i2s_write`) — it only decodes audio into the FIFO; playback happens in `loop()`. Blocking here drops the session.
- JSON parsing of server messages is intentionally lightweight: `bufFind`/`bufContains` scan the raw frame for tokens like `inlineData`, `data`, `setupComplete`, and `turnComplete`. **Be cautious changing this — it depends on those literal field names.** Both camelCase and snake_case variants (`inlineData`/`inline_data`) are checked.
- Server frames may arrive as text or binary; `onWsEvent` handles `WStype_TEXT` and `WStype_BIN` identically.
- If changing button behavior, remember the input is active-low and debounced in `handleButton()`, and the turn flow is press-to-start / press-to-stop, with a third press cancelling playback.
- If changing NeoPixel feedback, check interactions between the thinking-animation task and audio timing.
- Do not remove the `ESP_IDF_VERSION_MAJOR` conditional for I2S communication format or MCLK pin handling unless the supported Arduino/ESP-IDF version is known.
- This folder currently has only `AGENTS.md` and the `.ino` sketch; there is no committed Arduino CLI, PlatformIO, or test configuration in the project directory.
