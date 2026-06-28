#include <Arduino.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_intr_alloc.h>
#include <mbedtls/base64.h>
// Raise the WebSockets frame cap before the (header-only) library is included so
// large Gemini Live audio frames aren't rejected with close code 1009. Requires
// the matching #ifndef guard in WebSockets_Generic.h. PSRAM must be enabled so
// the library's per-frame malloc lands in PSRAM, not internal RAM. Keep this well
// below the playback FIFO size so a single frame can't overflow the FIFO.
#define WEBSOCKETS_MAX_DATA_SIZE (256 * 1024)
#include <WebSocketsClient_Generic.h>

constexpr char WIFI_SSID[] = "YOUR_WIFI_SSID_HERE";
constexpr char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD_HERE";
constexpr char GEMINI_API_KEY[] = "YOUR_GEMINI_API_KEY_HERE";
// "Gemini 3 Flash Live" audio-to-audio model. Change here if the ID moves.
constexpr char GEMINI_LIVE_MODEL[] = "gemini-3.1-flash-live-preview";
constexpr char GEMINI_TTS_VOICE[] = "Kore";

constexpr char GEMINI_LIVE_HOST[] = "generativelanguage.googleapis.com";
constexpr uint16_t GEMINI_LIVE_PORT = 443;

constexpr int SPEAKER_I2S_LRCLK_PIN = 45;  // MAX98357A LRC.
constexpr int SPEAKER_I2S_BCLK_PIN = 16;   // MAX98357A BCLK.
constexpr int SPEAKER_I2S_DOUT_PIN = 47;   // MAX98357A DIN.
constexpr int MIC_I2S_WS_PIN = 4;          // Mic WS.
constexpr int MIC_I2S_SCK_PIN = 5;         // Mic SCK.
constexpr int MIC_I2S_DIN_PIN = 6;         // Mic SD.
constexpr int AMP_SD_PIN = 9;

constexpr int BUTTON_GROUND_PIN = 7;
constexpr int BUTTON_INPUT_PIN = 15;

constexpr int NEOPIXEL_PIN = 48;
constexpr uint8_t NEOPIXEL_BRIGHTNESS = 32;
static_assert(NEOPIXEL_PIN != SPEAKER_I2S_BCLK_PIN,
              "NeoPixel pin must not share speaker I2S BCLK");
static_assert(NEOPIXEL_PIN != SPEAKER_I2S_LRCLK_PIN,
              "NeoPixel pin must not share speaker I2S LRCLK");
static_assert(NEOPIXEL_PIN != SPEAKER_I2S_DOUT_PIN,
              "NeoPixel pin must not share speaker I2S DOUT");

// Gemini Live: input audio is raw 16-bit LE mono PCM @ 16 kHz; output @ 24 kHz.
constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr uint32_t TTS_SAMPLE_RATE = 24000;
constexpr size_t MONO_SAMPLES_PER_CHUNK = 512;
// Mic samples buffered before each realtimeInput audio message (~64 ms @ 16 kHz).
constexpr size_t MIC_SEND_SAMPLES = 1024;
constexpr size_t MIC_B64_BUFFER =
    ((MIC_SEND_SAMPLES * sizeof(int16_t) + 2) / 3) * 4 + 4;

constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
constexpr uint8_t PLAYBACK_GAIN = 1;
constexpr bool PLAY_TEST_TONE_ON_BOOT = true;
constexpr uint16_t TEST_TONE_HZ = 660;
constexpr uint16_t TEST_TONE_MS = 250;
constexpr int16_t TEST_TONE_AMPLITUDE = 12000;
constexpr uint8_t MIC_SAMPLE_SHIFT = 16;
constexpr bool PRINT_MIC_RAW_DEBUG = true;
constexpr uint32_t MIC_DEBUG_INTERVAL_MS = 1000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t LIVE_CONNECT_TIMEOUT_MS = 10000;

int16_t i2sOutputBuffer[MONO_SAMPLES_PER_CHUNK * 2];
int32_t i2sInputBuffer[MONO_SAMPLES_PER_CHUNK * 2];

int16_t micSendBuffer[MIC_SEND_SAMPLES];
size_t micSendCount = 0;

uint32_t lastMicDebugMs = 0;
bool i2sReady = false;
volatile bool thinkingAnimationActive = false;

bool lastButtonReading = HIGH;
bool stableButtonReading = HIGH;
uint32_t lastButtonChangeMs = 0;

WebSocketsClient webSocket;
bool wsConnected = false;
bool liveSetupComplete = false;

// Decoded TTS PCM is buffered in this PSRAM single-producer/single-consumer ring.
// The WebSocket callback (producer, loop()/core 1) appends bytes; a dedicated
// playback task (consumer, core 0) drains them to I2S at real time, paced by the
// blocking i2s_write. Because Gemini delivers audio faster than real time, the
// ring must hold the whole reply lead, so it is large. head==tail means empty;
// one slot is kept unused to distinguish empty from full. Indices are volatile
// and updated after the data with a full memory barrier (safe across cores).
constexpr size_t AUDIO_FIFO_CAP = 2 * 1024 * 1024;
constexpr size_t PLAYBACK_PREBUFFER_BYTES = TTS_SAMPLE_RATE * sizeof(int16_t) / 7;  // ~150 ms
uint8_t *audioFifo = nullptr;
volatile size_t audioFifoHead = 0;  // write index (producer)
volatile size_t audioFifoTail = 0;  // read index (consumer)

TaskHandle_t playbackTaskHandle = nullptr;
volatile bool playbackRunning = false;  // true while the speaker bus is live
volatile bool playbackTaskIdle = true;  // task is not touching I2S right now

// Per-turn playback state.
volatile bool turnCompleteSeen = false;
volatile bool audioStarted = false;
volatile bool resetPending = false;

enum class AppState {
  Idle,        // mic installed, not streaming; waiting for a press.
  Capturing,   // streaming mic PCM to the Live session.
  Responding,  // turn ended; awaiting/playing Gemini audio.
};

AppState appState = AppState::Idle;

enum class I2SBusMode {
  None,
  Microphone,
  Speaker,
};

I2SBusMode i2sBusMode = I2SBusMode::None;

void playTestTone(uint32_t sampleRate);
void onWsEvent(WStype_t type, uint8_t *payload, size_t length);
void liveSendSetup();
void finishTurn();
void enterIdle();

#if ESP_IDF_VERSION_MAJOR >= 5
constexpr i2s_comm_format_t I2S_COMM_FORMAT_USED = I2S_COMM_FORMAT_STAND_I2S;
#else
constexpr i2s_comm_format_t I2S_COMM_FORMAT_USED = I2S_COMM_FORMAT_I2S;
#endif

void setNeoPixel(uint8_t red, uint8_t green, uint8_t blue) {
  red = (static_cast<uint16_t>(red) * NEOPIXEL_BRIGHTNESS) / 255;
  green = (static_cast<uint16_t>(green) * NEOPIXEL_BRIGHTNESS) / 255;
  blue = (static_cast<uint16_t>(blue) * NEOPIXEL_BRIGHTNESS) / 255;
  neopixelWrite(NEOPIXEL_PIN, red, green, blue);
}

void setNeoPixelWheel(uint8_t position) {
  if (position < 85) {
    setNeoPixel(255 - position * 3, position * 3, 0);
  } else if (position < 170) {
    position -= 85;
    setNeoPixel(0, 255 - position * 3, position * 3);
  } else {
    position -= 170;
    setNeoPixel(position * 3, 0, 255 - position * 3);
  }
}

void thinkingAnimationTask(void *parameter) {
  uint8_t hue = 0;
  while (thinkingAnimationActive) {
    setNeoPixelWheel(hue++);
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  vTaskDelete(nullptr);
}

void startThinkingAnimation() {
  if (thinkingAnimationActive) {
    return;
  }
  thinkingAnimationActive = true;
  const BaseType_t created = xTaskCreatePinnedToCore(thinkingAnimationTask,
                                                     "thinking_led",
                                                     2048,
                                                     nullptr,
                                                     1,
                                                     nullptr,
                                                     0);
  if (created != pdPASS) {
    Serial.println("Could not start thinking LED task");
  }
}

void stopThinkingAnimation() {
  if (!thinkingAnimationActive) {
    return;
  }
  thinkingAnimationActive = false;
  delay(40);
}

void showStatus(const __FlashStringHelper *line1,
                const String &line2 = String()) {
  Serial.println(line1);
  if (!line2.isEmpty()) {
    Serial.println(line2);
  }
}

void uninstallI2S() {
  if (i2sReady) {
    i2s_driver_uninstall(I2S_NUM_0);
    i2sReady = false;
  }
  i2sBusMode = I2SBusMode::None;
}

bool installI2S(i2s_mode_t direction,
                i2s_bits_per_sample_t bitsPerSample,
                uint32_t sampleRate,
                int bitClockPin,
                int wordSelectPin,
                int dataOutPin,
                int dataInPin,
                I2SBusMode busMode) {
  uninstallI2S();

  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | direction);
  i2sConfig.sample_rate = sampleRate;
  i2sConfig.bits_per_sample = bitsPerSample;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_USED;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 8;
  i2sConfig.dma_buf_len = 256;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;

  i2s_pin_config_t pinConfig = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
  pinConfig.bck_io_num = bitClockPin;
  pinConfig.ws_io_num = wordSelectPin;
  pinConfig.data_out_num = dataOutPin;
  pinConfig.data_in_num = dataInPin;

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("i2s_driver_install failed: %d\n", err);
    return false;
  }

  err = i2s_set_pin(I2S_NUM_0, &pinConfig);
  if (err != ESP_OK) {
    Serial.printf("i2s_set_pin failed: %d\n", err);
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  i2sReady = true;
  i2sBusMode = busMode;
  return true;
}

bool setupMicrophoneI2S() {
  if (i2sBusMode == I2SBusMode::Microphone) {
    return true;
  }

  return installI2S(I2S_MODE_RX,
                    I2S_BITS_PER_SAMPLE_32BIT,
                    AUDIO_SAMPLE_RATE,
                    MIC_I2S_SCK_PIN,
                    MIC_I2S_WS_PIN,
                    I2S_PIN_NO_CHANGE,
                    MIC_I2S_DIN_PIN,
                    I2SBusMode::Microphone);
}

bool setupSpeakerI2S(uint32_t sampleRate) {
  return installI2S(I2S_MODE_TX,
                    I2S_BITS_PER_SAMPLE_16BIT,
                    sampleRate,
                    SPEAKER_I2S_BCLK_PIN,
                    SPEAKER_I2S_LRCLK_PIN,
                    SPEAKER_I2S_DOUT_PIN,
                    I2S_PIN_NO_CHANGE,
                    I2SBusMode::Speaker);
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  showStatus(F("Connecting WiFi"), WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    showStatus(F("WiFi failed"));
    return false;
  }

  showStatus(F("WiFi connected"), WiFi.localIP().toString());
  return true;
}

int16_t applyPlaybackGain(int16_t sample) {
  const int32_t amplifiedSample = static_cast<int32_t>(sample) * PLAYBACK_GAIN;
  return constrain(amplifiedSample, -32768, 32767);
}

void writeSpeakerSamples(const int16_t *samples, size_t samplesThisChunk) {
  size_t bytesWritten = 0;
  const size_t bytesToWrite = samplesThisChunk * 2 * sizeof(int16_t);
  const esp_err_t err = i2s_write(I2S_NUM_0,
                                  samples,
                                  bytesToWrite,
                                  &bytesWritten,
                                  portMAX_DELAY);

  if (err != ESP_OK || bytesWritten != bytesToWrite) {
    Serial.printf("I2S write failed/short: err=%d %u of %u bytes\n",
                  err,
                  static_cast<unsigned>(bytesWritten),
                  static_cast<unsigned>(bytesToWrite));
  }
}

void playTestTone(uint32_t sampleRate) {
  const uint32_t totalSamples =
      (sampleRate * static_cast<uint32_t>(TEST_TONE_MS)) / 1000;
  const uint32_t halfPeriodSamples =
      max(1UL, sampleRate / (TEST_TONE_HZ * 2UL));
  uint32_t samplesWritten = 0;

  while (samplesWritten < totalSamples) {
    const size_t samplesThisChunk =
        min(static_cast<size_t>(MONO_SAMPLES_PER_CHUNK),
            static_cast<size_t>(totalSamples - samplesWritten));

    for (size_t i = 0; i < samplesThisChunk; ++i) {
      const bool highHalf =
          ((samplesWritten + i) / halfPeriodSamples) % 2 == 0;
      const int16_t sample =
          highHalf ? TEST_TONE_AMPLITUDE : -TEST_TONE_AMPLITUDE;
      i2sOutputBuffer[i * 2] = sample;
      i2sOutputBuffer[i * 2 + 1] = sample;
    }

    writeSpeakerSamples(i2sOutputBuffer, samplesThisChunk);
    samplesWritten += samplesThisChunk;
  }
}

int16_t decodeMicSample(int32_t rawSample) {
  return static_cast<int16_t>(rawSample >> MIC_SAMPLE_SHIFT);
}

int16_t chooseMicSample(int32_t leftSample, int32_t rightSample) {
  const int16_t left = decodeMicSample(leftSample);
  const int16_t right = decodeMicSample(rightSample);

  if (abs(static_cast<int32_t>(right)) > abs(static_cast<int32_t>(left))) {
    return right;
  }

  return left;
}

void printMicRawDebug(size_t stereoSamplesRead) {
  if (!PRINT_MIC_RAW_DEBUG || stereoSamplesRead < 2 ||
      millis() - lastMicDebugMs < MIC_DEBUG_INTERVAL_MS) {
    return;
  }

  lastMicDebugMs = millis();

  int32_t minLeft = INT32_MAX;
  int32_t maxLeft = INT32_MIN;
  int32_t minRight = INT32_MAX;
  int32_t maxRight = INT32_MIN;
  size_t nonZeroLeft = 0;
  size_t nonZeroRight = 0;
  const size_t frames = stereoSamplesRead / 2;

  for (size_t i = 0; i < frames; ++i) {
    const int32_t left = i2sInputBuffer[i * 2];
    const int32_t right = i2sInputBuffer[i * 2 + 1];
    minLeft = min(minLeft, left);
    maxLeft = max(maxLeft, left);
    minRight = min(minRight, right);
    maxRight = max(maxRight, right);

    if (left != 0) {
      ++nonZeroLeft;
    }
    if (right != 0) {
      ++nonZeroRight;
    }
  }

  Serial.printf(
      "Mic raw: frames=%u Lnz=%u Rnz=%u Lmin=%ld Lmax=%ld Rmin=%ld Rmax=%ld\n",
      static_cast<unsigned>(frames),
      static_cast<unsigned>(nonZeroLeft),
      static_cast<unsigned>(nonZeroRight),
      static_cast<long>(minLeft),
      static_cast<long>(maxLeft),
      static_cast<long>(minRight),
      static_cast<long>(maxRight));
}

// Returns the first index at/after `from` where `token` occurs, else -1.
int bufFind(const uint8_t *buf, size_t len, const char *token, size_t from) {
  const size_t tlen = strlen(token);
  if (tlen == 0 || tlen > len) {
    return -1;
  }
  for (size_t i = from; i + tlen <= len; ++i) {
    if (memcmp(buf + i, token, tlen) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool bufContains(const uint8_t *buf, size_t len, const char *token) {
  return bufFind(buf, len, token, 0) >= 0;
}

// Locates the base64 value of the inline audio "data" field, if present.
bool findInlineAudioData(const uint8_t *buf, size_t len, size_t &valueStart) {
  int marker = bufFind(buf, len, "inlineData", 0);
  if (marker < 0) {
    marker = bufFind(buf, len, "inline_data", 0);
  }
  if (marker < 0) {
    return false;
  }

  const int dataKey = bufFind(buf, len, "\"data\"", marker);
  if (dataKey < 0) {
    return false;
  }

  size_t i = static_cast<size_t>(dataKey) + 6;
  while (i < len && buf[i] != ':') {
    ++i;
  }
  while (i < len && buf[i] != '"') {
    ++i;
  }
  if (i >= len) {
    return false;
  }

  valueStart = i + 1;
  return true;
}

size_t audioFifoFill() {
  return (audioFifoHead + AUDIO_FIFO_CAP - audioFifoTail) % AUDIO_FIFO_CAP;
}

// Producer side: append decoded PCM bytes to the ring. Drops on overflow (the
// 2 MB ring should never fill for a normal reply). Only the producer advances
// audioFifoHead; the barrier publishes the bytes before the new head is seen.
void audioFifoPush(const uint8_t *data, size_t length) {
  if (audioFifo == nullptr) {
    return;
  }

  const size_t freeBytes =
      (audioFifoTail + AUDIO_FIFO_CAP - audioFifoHead - 1) % AUDIO_FIFO_CAP;
  if (length > freeBytes) {
    length = freeBytes;
    if (length == 0) {
      Serial.println("Audio FIFO full; dropping TTS audio");
      return;
    }
  }

  size_t head = audioFifoHead;
  for (size_t i = 0; i < length; ++i) {
    audioFifo[head] = data[i];
    head = (head + 1) % AUDIO_FIFO_CAP;
  }
  __sync_synchronize();
  audioFifoHead = head;
}

// Producer: runs inside the WS callback. Decodes the message's base64 audio into
// the ring and returns immediately. No I2S, no blocking, no LED work.
void enqueueLiveAudio(const uint8_t *payload, size_t length) {
  if (appState != AppState::Responding) {
    return;
  }

  size_t valueStart = 0;
  if (!findInlineAudioData(payload, length, valueStart)) {
    return;
  }

  char quartet[4];
  size_t quartetLength = 0;

  for (size_t i = valueStart; i < length; ++i) {
    const char c = static_cast<char>(payload[i]);
    if (c == '"') {
      break;
    }
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      continue;
    }

    quartet[quartetLength++] = c;
    if (quartetLength < 4) {
      continue;
    }
    quartetLength = 0;

    uint8_t decoded[3];
    size_t decodedLength = 0;
    if (mbedtls_base64_decode(decoded,
                              sizeof(decoded),
                              &decodedLength,
                              reinterpret_cast<const unsigned char *>(quartet),
                              4) != 0) {
      Serial.println("Live audio base64 decode error");
      break;
    }

    audioFifoPush(decoded, decodedLength);
    audioStarted = true;
  }
}

// Consumer side: pop up to one chunk of samples from the ring and write it to the
// speaker (blocking ~21 ms at real time). Returns the number of samples written.
// Only the consumer advances audioFifoTail.
size_t drainPlaybackChunk() {
  __sync_synchronize();
  const size_t availableSamples = audioFifoFill() / sizeof(int16_t);
  if (availableSamples == 0) {
    return 0;
  }

  const size_t samplesThisChunk =
      min(static_cast<size_t>(MONO_SAMPLES_PER_CHUNK), availableSamples);
  size_t tail = audioFifoTail;
  for (size_t i = 0; i < samplesThisChunk; ++i) {
    const uint8_t low = audioFifo[tail];
    tail = (tail + 1) % AUDIO_FIFO_CAP;
    const uint8_t high = audioFifo[tail];
    tail = (tail + 1) % AUDIO_FIFO_CAP;

    const int16_t sample = static_cast<int16_t>(
        static_cast<uint16_t>(low) | (static_cast<uint16_t>(high) << 8));
    const int16_t gained = applyPlaybackGain(sample);
    i2sOutputBuffer[i * 2] = gained;
    i2sOutputBuffer[i * 2 + 1] = gained;
  }
  __sync_synchronize();
  audioFifoTail = tail;

  writeSpeakerSamples(i2sOutputBuffer, samplesThisChunk);
  return samplesThisChunk;
}

// Dedicated consumer task (core 0). Drains the ring to I2S whenever a turn is in
// its speaker phase, independent of how fast frames arrive on the WebSocket.
void playbackTask(void *parameter) {
  bool prebuffered = false;
  for (;;) {
    if (!playbackRunning) {
      prebuffered = false;
      playbackTaskIdle = true;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    playbackTaskIdle = false;

    // Pre-buffer a little before the first sample so brief jitter doesn't
    // underrun, but start immediately once the whole turn has been received.
    if (!prebuffered) {
      if (audioFifoFill() < PLAYBACK_PREBUFFER_BYTES && !turnCompleteSeen) {
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      prebuffered = true;
    }

    if (drainPlaybackChunk() == 0) {
      // Nothing ready yet; yield briefly without hogging the core.
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

// Stop the playback task touching I2S and wait until it confirms it is idle, so
// the caller can safely uninstall/reinstall the I2S bus.
void stopPlaybackAndWait() {
  playbackRunning = false;
  const uint32_t deadline = millis() + 500;
  while (!playbackTaskIdle && millis() < deadline) {
    delay(2);
  }
}

void resetPlaybackState() {
  audioFifoHead = 0;
  audioFifoTail = 0;
  turnCompleteSeen = false;
  audioStarted = false;
}

void handleLiveMessage(const uint8_t *payload, size_t length) {
  if (!liveSetupComplete && bufContains(payload, length, "setupComplete")) {
    liveSetupComplete = true;
    Serial.println("Live setupComplete");
    return;
  }

  enqueueLiveAudio(payload, length);

  // Don't finish here: the FIFO may still hold audio. loop() finishes the turn
  // once turnCompleteSeen and the FIFO has drained.
  if (bufContains(payload, length, "turnComplete")) {
    turnCompleteSeen = true;
  }
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsConnected = true;
      liveSetupComplete = false;
      Serial.println("Live WS connected");
      liveSendSetup();
      break;

    case WStype_DISCONNECTED:
      wsConnected = false;
      liveSetupComplete = false;
      Serial.println("Live WS disconnected");
      if (appState != AppState::Idle) {
        resetPending = true;
      }
      break;

    case WStype_TEXT:
    case WStype_BIN:
      handleLiveMessage(payload, length);
      break;

    case WStype_ERROR:
      Serial.println("Live WS error");
      break;

    default:
      break;
  }
}

void liveSendSetup() {
  String msg;
  msg.reserve(512);
  msg += F("{\"setup\":{\"model\":\"models/");
  msg += GEMINI_LIVE_MODEL;
  msg += F("\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],");
  msg += F("\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"");
  msg += GEMINI_TTS_VOICE;
  msg += F("\"}}}},\"systemInstruction\":{\"parts\":[{\"text\":\"");
  msg += F("You are a concise voice assistant. Answer naturally in one or two short sentences.");
  msg += F("\"}]},\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":true}}}}");
  webSocket.sendTXT(msg);
  Serial.println("Live setup sent");
}

void startLiveSocket() {
  String path =
      String(F("/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=")) +
      GEMINI_API_KEY;
  // ESP32 SSL path; no CA pinned (insecure), matching the prior setInsecure()
  // behaviour. Swap to beginSslWithCA() if certificate validation is wanted.
  webSocket.beginSSL(GEMINI_LIVE_HOST, GEMINI_LIVE_PORT, path.c_str());
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(5000);
}

bool ensureLiveSession() {
  if (!connectWifi()) {
    return false;
  }

  if (wsConnected && liveSetupComplete) {
    return true;
  }

  if (!wsConnected) {
    startLiveSocket();
    const uint32_t startedAt = millis();
    while (!wsConnected && millis() - startedAt < LIVE_CONNECT_TIMEOUT_MS) {
      webSocket.loop();
      delay(5);
    }
    if (!wsConnected) {
      Serial.println("Live WS connect timed out");
      return false;
    }
  }

  const uint32_t setupStartedAt = millis();
  while (!liveSetupComplete &&
         millis() - setupStartedAt < LIVE_CONNECT_TIMEOUT_MS) {
    webSocket.loop();
    delay(5);
  }

  if (!liveSetupComplete) {
    Serial.println("Live setup did not complete");
  }
  return liveSetupComplete;
}

void liveSendText(const char *json) {
  if (wsConnected) {
    webSocket.sendTXT(const_cast<char *>(json));
  }
}

void sendMicChunk() {
  if (micSendCount == 0) {
    return;
  }

  const size_t pcmBytes = micSendCount * sizeof(int16_t);
  static uint8_t encoded[MIC_B64_BUFFER];
  size_t encodedLength = 0;
  if (mbedtls_base64_encode(encoded,
                            sizeof(encoded),
                            &encodedLength,
                            reinterpret_cast<const uint8_t *>(micSendBuffer),
                            pcmBytes) != 0) {
    Serial.println("Mic base64 encode failed");
    micSendCount = 0;
    return;
  }
  encoded[encodedLength] = 0;

  String msg;
  msg.reserve(encodedLength + 96);
  msg += F("{\"realtimeInput\":{\"audio\":{\"data\":\"");
  msg += reinterpret_cast<const char *>(encoded);
  msg += F("\",\"mimeType\":\"audio/pcm;rate=");
  msg += String(AUDIO_SAMPLE_RATE);
  msg += F("\"}}}");
  webSocket.sendTXT(msg);
  micSendCount = 0;
}

void captureAndSendMic() {
  size_t bytesRead = 0;
  const esp_err_t err = i2s_read(I2S_NUM_0,
                                 i2sInputBuffer,
                                 sizeof(i2sInputBuffer),
                                 &bytesRead,
                                 pdMS_TO_TICKS(100));
  if (err != ESP_OK || bytesRead == 0) {
    return;
  }

  const size_t stereoSamplesRead = bytesRead / sizeof(int32_t);
  const size_t monoSamplesRead = stereoSamplesRead / 2;
  printMicRawDebug(stereoSamplesRead);

  for (size_t i = 0; i < monoSamplesRead; ++i) {
    micSendBuffer[micSendCount++] =
        chooseMicSample(i2sInputBuffer[i * 2], i2sInputBuffer[i * 2 + 1]);
    if (micSendCount >= MIC_SEND_SAMPLES) {
      sendMicChunk();
    }
  }
}

void enterIdle() {
  appState = AppState::Idle;
  stopPlaybackAndWait();  // ensure the task isn't touching I2S before we switch buses
  stopThinkingAnimation();
  digitalWrite(AMP_SD_PIN, LOW);
  setNeoPixel(255, 0, 0);
  micSendCount = 0;
  resetPlaybackState();
  setupMicrophoneI2S();
  showStatus(F("Ready"), F("Press to talk"));
}

void startCapture() {
  if (!ensureLiveSession()) {
    setNeoPixel(255, 0, 0);
    showStatus(F("Live connect failed"), F("See Serial log"));
    delay(1500);
    enterIdle();
    return;
  }

  if (i2sBusMode != I2SBusMode::Microphone) {
    setupMicrophoneI2S();
  }
  micSendCount = 0;
  liveSendText("{\"realtimeInput\":{\"activityStart\":{}}}");
  appState = AppState::Capturing;
  setNeoPixel(255, 90, 0);
  showStatus(F("Listening"), F("Press again to send"));
}

void endCapture() {
  if (micSendCount > 0) {
    sendMicChunk();
  }
  liveSendText("{\"realtimeInput\":{\"activityEnd\":{}}}");

  uninstallI2S();
  if (!setupSpeakerI2S(TTS_SAMPLE_RATE)) {
    Serial.println("Could not switch I2S to speaker mode");
    enterIdle();
    return;
  }
  digitalWrite(AMP_SD_PIN, HIGH);
  delay(5);
  i2s_zero_dma_buffer(I2S_NUM_0);

  resetPlaybackState();
  appState = AppState::Responding;
  playbackRunning = true;  // let the playback task drain to the speaker
  startThinkingAnimation();
  showStatus(F("Thinking"));
}

void finishTurn() {
  Serial.println("Live turnComplete");
  enterIdle();  // stops the playback task, amp off, reinstalls the mic
}

void cancelTurn() {
  Serial.println("Turn cancelled by button");
  // Any further audio for this turn is ignored once state is Idle.
  enterIdle();
}

void handleButton() {
  const bool reading = digitalRead(BUTTON_INPUT_PIN);
  const uint32_t now = millis();

  if (reading != lastButtonReading) {
    lastButtonChangeMs = now;
    lastButtonReading = reading;
  }

  if ((now - lastButtonChangeMs) < BUTTON_DEBOUNCE_MS ||
      reading == stableButtonReading) {
    return;
  }

  stableButtonReading = reading;
  if (stableButtonReading != LOW) {
    return;
  }

  switch (appState) {
    case AppState::Idle:
      startCapture();
      break;
    case AppState::Capturing:
      endCapture();
      break;
    case AppState::Responding:
      cancelTurn();
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(AMP_SD_PIN, OUTPUT);
  digitalWrite(AMP_SD_PIN, LOW);

  pinMode(NEOPIXEL_PIN, OUTPUT);
  setNeoPixel(0, 0, 0);

  pinMode(BUTTON_GROUND_PIN, OUTPUT);
  digitalWrite(BUTTON_GROUND_PIN, LOW);
  pinMode(BUTTON_INPUT_PIN, INPUT_PULLUP);

  audioFifo = static_cast<uint8_t *>(
      heap_caps_malloc(AUDIO_FIFO_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (audioFifo == nullptr) {
    audioFifo =
        static_cast<uint8_t *>(heap_caps_malloc(AUDIO_FIFO_CAP, MALLOC_CAP_8BIT));
  }
  if (audioFifo == nullptr) {
    Serial.printf("Could not allocate %u-byte audio FIFO\n",
                  static_cast<unsigned>(AUDIO_FIFO_CAP));
  }

  // Playback runs on core 0 so it keeps feeding I2S at real time even while
  // loop()/WebSocket work (on core 1) blocks ingesting a large audio frame.
  xTaskCreatePinnedToCore(playbackTask, "playback", 4096, nullptr, 2,
                          &playbackTaskHandle, 0);

  if (PLAY_TEST_TONE_ON_BOOT && setupSpeakerI2S(AUDIO_SAMPLE_RATE)) {
    Serial.println("Playing boot speaker test tone");
    digitalWrite(AMP_SD_PIN, HIGH);
    delay(5);
    playTestTone(AUDIO_SAMPLE_RATE);
    digitalWrite(AMP_SD_PIN, LOW);
    delay(50);
  }

  connectWifi();
  enterIdle();

  Serial.printf("Gemini Live ready (%s), mic %lu Hz / speaker %lu Hz\n",
                GEMINI_LIVE_MODEL,
                static_cast<unsigned long>(AUDIO_SAMPLE_RATE),
                static_cast<unsigned long>(TTS_SAMPLE_RATE));
}

void loop() {
  webSocket.loop();
  handleButton();

  if (resetPending) {
    resetPending = false;
    if (appState != AppState::Idle) {
      stopThinkingAnimation();
      Serial.println("Resetting to Idle after disconnect");
      enterIdle();
    }
  }

  if (appState == AppState::Capturing) {
    captureAndSendMic();
  } else if (appState == AppState::Responding) {
    // Playback runs in playbackTask(); loop() only ingests frames (via
    // webSocket.loop() above), updates the LED, and ends the turn.
    if (audioStarted && thinkingAnimationActive) {
      stopThinkingAnimation();
      setNeoPixel(0, 255, 0);
      showStatus(F("Speaking"));
    }
    if (turnCompleteSeen && audioFifoFill() < sizeof(int16_t)) {
      finishTurn();
    } else {
      delay(1);
    }
  } else {
    delay(2);
  }
}
