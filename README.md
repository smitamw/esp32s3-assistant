# esp32s3-assistant
An AI assistant for ESP32-S3. Designed to be used in conjunction with a momentary pushbutton switch, an INMP441 microphone and a MAX98357A amplifier.

## Project Purpose

This repository contains a single Arduino sketch, `sketch_may9a_speakertest.ino`, for an AI voice assistant running on an ESP32-S3. The device records microphone audio, sends it to Gemini, receives a concise text answer, converts that answer to speech, and plays it through an I2S amplifier.

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

Note: GPIO 48 is reserved for the onboard NeoPixel (`NEOPIXEL_PIN`). Keep speaker I2S signals off GPIO 48 unless the board wiring is intentionally changed and NeoPixel feedback is disabled or moved.

## Runtime Flow

- `setup()` initializes Serial, amp shutdown, NeoPixel, button pins, the recording buffer, an optional boot speaker test tone, then enters microphone mode.
- `loop()` debounces the button and dispatches work based on `audioMode`.
- In microphone mode, the sketch installs I2S RX at `AUDIO_SAMPLE_RATE` (`11025 Hz`), reads 32-bit stereo I2S frames from the INMP441, chooses the louder decoded channel, and stores mono 16-bit samples.
- Pressing the button while recording calls `processAssistantRequest()`: it stops I2S, connects Wi-Fi, uploads the recorded WAV to Gemini, asks Gemini for a concise response, then streams Gemini TTS directly to the speaker with `geminiTtsStreamToSpeaker()`.
- The older buffered speaker mode is still present: `enterSpeakerMode()` installs I2S TX at the returned TTS sample rate, enables the MAX98357A with `AMP_SD_PIN`, duplicates mono PCM to left/right output samples, applies `PLAYBACK_GAIN`, writes chunks through I2S, and returns to microphone mode when playback ends. `processAssistantRequest()` does not currently use this buffered path.

## Network and AI APIs

- The sketch uses `WiFi`, `WiFiClientSecure`, and `HTTPClient`.
- Gemini file upload uses the resumable upload endpoint.
- Gemini text generation uses `GEMINI_TEXT_MODEL`.
- Gemini TTS uses `GEMINI_TTS_MODEL` and `GEMINI_TTS_VOICE`; the current documented single-speaker REST model is `gemini-2.5-flash-preview-tts`.
- TLS is currently configured with `client.setInsecure()`.

The sketch currently contains Wi-Fi credentials and a Gemini API key inline. Future agents should avoid copying those values into logs, documentation, commits, or issue text. Prefer moving secrets to a local untracked header or build-time configuration if asked to harden the project.

## Audio Details

- Recording limit: `MAX_RECORDING_SECONDS` is 5 seconds.
- Recording buffer is allocated from PSRAM when possible, then regular heap as a fallback.
- Microphone samples are decoded by shifting raw 32-bit I2S samples right by `MIC_SAMPLE_SHIFT` (`16`).
- TTS output may arrive as raw PCM or WAV-like audio; helper functions parse headers, sample rates, and base64 payloads.
- The MAX98357A playback path expects 16-bit PCM samples and writes duplicated stereo frames.
- `PRINT_MIC_RAW_DEBUG` is currently `true`, so the sketch prints raw I2S microphone statistics about once per second.
- `PLAY_TEST_TONE_ON_BOOT` is currently `true`, so boot temporarily switches to speaker I2S and plays a short square-wave test tone before entering microphone mode.

## TTS Playback Notes

- The active TTS path is `geminiTtsStreamToSpeaker()`. It reads HTTP headers/body incrementally, parses the Gemini JSON response while bytes arrive, base64-decodes audio quartets, starts I2S TX once the raw PCM or WAV format is known, and writes samples directly to the MAX98357A path.
- The streaming TTS path supports raw 16-bit little-endian PCM-like audio and 16-bit PCM WAV. Stereo WAV is mixed down to mono before being duplicated to left/right I2S output.
- The streaming TTS path keeps only a small WAV header buffer (`WAV_HDR_MAX`, currently 128 bytes) plus sample frame buffers; large TTS audio is not stored in RAM during normal playback.
- Pressing the button during streaming TTS is checked after output chunks are written and interrupts playback.
- `geminiTextToSpeech()` and `readGeminiTtsAudioResponse()` are the older buffered TTS path. They stream JSON parsing into a linked list of `TtsPcmChunk` buffers of `TTS_PCM_CHUNK_BYTES` bytes each, allocated from PSRAM when possible, then normalize/play via the buffered speaker helpers.
- The TTS sample rate is inferred from the response MIME type using `rate=` or `sample_rate=`, with `TTS_SAMPLE_RATE` (`24000 Hz`) as a fallback.
- TTS requests retry up to `GEMINI_TTS_MAX_ATTEMPTS` only for HTTP 5xx responses or connection failures; 4xx errors are treated as request/configuration problems.

## API Flow Notes

- The active text path is `geminiGenerateText()`, which uploads a WAV through the Gemini resumable file API and then calls `geminiGenerateTextFromFile()`.
- `geminiGenerateTextInline()` is still present as an alternate direct inline-base64 request path, but `processAssistantRequest()` does not currently call it.
- `startGeminiFileUpload()` uses `HTTPClient` to get the resumable upload URL, while `uploadRecordedWavFile()` uses raw `WiFiClientSecure` writes so the WAV header and recorded PCM can be sent without building one large body in memory.
- TTS requests use raw `WiFiClientSecure` HTTP rather than `HTTPClient`, because the code needs incremental response parsing and playback while bytes arrive.
- JSON parsing is intentionally lightweight and string-based to fit the Arduino environment. Be cautious when changing Gemini response handling because the code depends on specific key order/anchors in a few places.

## Maintenance Guidance

- Keep the sketch Arduino-compatible C++ and preserve the existing style: `constexpr` constants, small helpers, and explicit Serial diagnostics.
- Be careful when changing I2S setup. The code intentionally uninstalls and reinstalls I2S0 when switching between microphone RX and speaker TX.
- Keep memory usage in mind. Audio buffers can be large for an ESP32-S3, and TTS audio is dynamically allocated.
- If changing button behavior, remember the input is active-low and debounced in `handleButton()`.
- Pressing the button during speaker playback cancels the response and returns to microphone mode.
- If changing NeoPixel feedback, check interactions between the thinking animation task and audio timing.
- Do not remove the `ESP_IDF_VERSION_MAJOR` conditional for I2S communication format or MCLK pin handling unless the supported Arduino/ESP-IDF version is known.
- This folder currently has only `AGENTS.md` and the `.ino` sketch; there is no committed Arduino CLI, PlatformIO, or test configuration in the project directory.
