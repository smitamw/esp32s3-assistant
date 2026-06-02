#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_intr_alloc.h>
#include <mbedtls/base64.h>

constexpr char WIFI_SSID[] = "Zapisco";
constexpr char WIFI_PASSWORD[] = "Zapisco2020@757!";
constexpr char GEMINI_API_KEY[] = "AIzaSyDwf5F52Zz_CLpWTrTQ_U9CJvhRyOjkGjs";
constexpr char GEMINI_TEXT_MODEL[] = "gemini-3-flash-preview";
constexpr char GEMINI_TTS_MODEL[] = "gemini-2.5-flash-preview-tts";
constexpr char GEMINI_TTS_VOICE[] = "Kore";

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

constexpr uint32_t AUDIO_SAMPLE_RATE = 11025;
constexpr uint32_t TTS_SAMPLE_RATE = 24000;
constexpr uint8_t MAX_RECORDING_SECONDS = 5;
constexpr size_t MAX_RECORDING_SAMPLES =
    AUDIO_SAMPLE_RATE * MAX_RECORDING_SECONDS;
constexpr size_t MONO_SAMPLES_PER_CHUNK = 512;
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
constexpr uint8_t GEMINI_TTS_MAX_ATTEMPTS = 3;
constexpr size_t TLS_WRITE_CHUNK_BYTES = 64;
constexpr size_t TTS_PCM_CHUNK_BYTES = 256;

int16_t *recordingBuffer = nullptr;
int16_t i2sOutputBuffer[MONO_SAMPLES_PER_CHUNK * 2];
int32_t i2sInputBuffer[MONO_SAMPLES_PER_CHUNK * 2];

struct TtsPcmChunk {
  uint8_t *data;
  size_t used;
  TtsPcmChunk *next;
};

TtsPcmChunk *ttsPcmHead = nullptr;
TtsPcmChunk *ttsPcmTail = nullptr;
TtsPcmChunk *playbackChunk = nullptr;
size_t ttsPcmBytes = 0;
uint32_t ttsPlaybackSampleRate = TTS_SAMPLE_RATE;
size_t recordedSampleCount = 0;
size_t playbackCursor = 0;
size_t playbackChunkOffset = 0;
uint32_t lastMicDebugMs = 0;
bool i2sReady = false;
volatile bool thinkingAnimationActive = false;

bool lastButtonReading = HIGH;
bool stableButtonReading = HIGH;
uint32_t lastButtonChangeMs = 0;

enum class AudioMode {
  Microphone,
  Thinking,
  Speaker,
};

AudioMode audioMode = AudioMode::Microphone;

enum class I2SBusMode {
  None,
  Microphone,
  Speaker,
};

struct HttpBodyReader {
  WiFiClientSecure *client;
  bool chunked;
  int contentLength;
  int bytesRead;
  int chunkRemaining;
  bool finished;
};

I2SBusMode i2sBusMode = I2SBusMode::None;

void playTestTone(uint32_t sampleRate);
void printClipStats();

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

bool allocateRecordingBuffer() {
  if (recordingBuffer != nullptr) {
    return true;
  }

  const size_t bufferBytes = MAX_RECORDING_SAMPLES * sizeof(int16_t);
  recordingBuffer = static_cast<int16_t *>(
      heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (recordingBuffer == nullptr) {
    recordingBuffer =
        static_cast<int16_t *>(heap_caps_malloc(bufferBytes, MALLOC_CAP_8BIT));
  }

  if (recordingBuffer == nullptr) {
    Serial.printf("Could not allocate %u bytes for recording\n",
                  static_cast<unsigned>(bufferBytes));
    return false;
  }

  memset(recordingBuffer, 0, bufferBytes);
  return true;
}

void freeRecordingBuffer() {
  if (recordingBuffer != nullptr) {
    heap_caps_free(recordingBuffer);
    recordingBuffer = nullptr;
  }
  recordedSampleCount = 0;
}

void freeTtsAudio() {
  TtsPcmChunk *chunk = ttsPcmHead;
  while (chunk != nullptr) {
    TtsPcmChunk *next = chunk->next;
    if (chunk->data != nullptr) {
      heap_caps_free(chunk->data);
    }
    heap_caps_free(chunk);
    chunk = next;
  }
  ttsPcmHead = nullptr;
  ttsPcmTail = nullptr;
  playbackChunk = nullptr;
  ttsPcmBytes = 0;
  ttsPlaybackSampleRate = TTS_SAMPLE_RATE;
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

void enterMicrophoneMode(bool clearClip) {
  audioMode = AudioMode::Microphone;
  digitalWrite(AMP_SD_PIN, LOW);
  setNeoPixel(255, 0, 0);
  playbackCursor = 0;
  playbackChunkOffset = 0;
  freeTtsAudio();

  if (!allocateRecordingBuffer()) {
    showStatus(F("Memory failed"), F("No recording buffer"));
    return;
  }

  if (clearClip) {
    recordedSampleCount = 0;
  }

  setupMicrophoneI2S();
  showStatus(F("Recording"), F("Press button to ask"));
}

void enterSpeakerMode() {
  audioMode = AudioMode::Speaker;
  playbackCursor = 0;
  playbackChunk = ttsPcmHead;
  playbackChunkOffset = 0;
  setNeoPixel(0, 255, 0);

  if (ttsPcmHead == nullptr || ttsPcmBytes == 0) {
    Serial.println("Speaker mode requested, but no TTS audio is available");
    enterMicrophoneMode(true);
    return;
  }

  if (!setupSpeakerI2S(ttsPlaybackSampleRate)) {
    Serial.println("Could not switch I2S to speaker mode");
    enterMicrophoneMode(false);
    return;
  }

  digitalWrite(AMP_SD_PIN, HIGH);
  delay(5);
  i2s_zero_dma_buffer(I2S_NUM_0);
  String sampleCountText = String(ttsPcmBytes / sizeof(int16_t));
  sampleCountText += F(" @ ");
  sampleCountText += String(ttsPlaybackSampleRate);
  sampleCountText += F(" Hz");
  showStatus(F("Speaking"), sampleCountText);
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

void appendLittleEndian16(uint8_t *buffer, size_t offset, uint16_t value) {
  buffer[offset] = value & 0xFF;
  buffer[offset + 1] = (value >> 8) & 0xFF;
}

void appendLittleEndian32(uint8_t *buffer, size_t offset, uint32_t value) {
  buffer[offset] = value & 0xFF;
  buffer[offset + 1] = (value >> 8) & 0xFF;
  buffer[offset + 2] = (value >> 16) & 0xFF;
  buffer[offset + 3] = (value >> 24) & 0xFF;
}

void buildWavHeader(uint8_t *wavHeader) {
  const size_t pcmBytes = recordedSampleCount * sizeof(int16_t);
  const size_t wavBytes = 44 + pcmBytes;

  memcpy(wavHeader, "RIFF", 4);
  appendLittleEndian32(wavHeader, 4, wavBytes - 8);
  memcpy(wavHeader + 8, "WAVEfmt ", 8);
  appendLittleEndian32(wavHeader, 16, 16);
  appendLittleEndian16(wavHeader, 20, 1);
  appendLittleEndian16(wavHeader, 22, 1);
  appendLittleEndian32(wavHeader, 24, AUDIO_SAMPLE_RATE);
  appendLittleEndian32(wavHeader, 28, AUDIO_SAMPLE_RATE * 2);
  appendLittleEndian16(wavHeader, 32, 2);
  appendLittleEndian16(wavHeader, 34, 16);
  memcpy(wavHeader + 36, "data", 4);
  appendLittleEndian32(wavHeader, 40, pcmBytes);
}

bool writeAll(WiFiClientSecure &client, const uint8_t *data, size_t length) {
  size_t written = 0;
  while (written < length) {
    const size_t bytesThisWrite =
        min(TLS_WRITE_CHUNK_BYTES, length - written);
    size_t chunkWritten = 0;

    for (uint8_t attempt = 0; attempt < 50 && chunkWritten == 0; ++attempt) {
      chunkWritten = client.write(data + written, bytesThisWrite);
      if (chunkWritten == 0) {
        while (client.available()) {
          Serial.write(client.read());
        }
        delay(10);
      }
    }

    if (chunkWritten == 0) {
      return false;
    }
    written += chunkWritten;
    delay(0);
  }
  return true;
}

bool writeAll(WiFiClientSecure &client, const char *text) {
  return writeAll(client,
                  reinterpret_cast<const uint8_t *>(text),
                  strlen(text));
}

bool writeBase64Triplet(WiFiClientSecure &client,
                        const uint8_t *data,
                        size_t length) {
  char encoded[8];
  size_t encodedLength = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(encoded),
                            sizeof(encoded),
                            &encodedLength,
                            data,
                            length) != 0) {
    return false;
  }

  return writeAll(client,
                  reinterpret_cast<const uint8_t *>(encoded),
                  encodedLength);
}

bool writeBase64StreamChunk(WiFiClientSecure &client,
                            const uint8_t *data,
                            size_t length,
                            uint8_t *carry,
                            size_t &carryLength) {
  size_t offset = 0;

  if (carryLength > 0) {
    while (carryLength < 3 && offset < length) {
      carry[carryLength++] = data[offset++];
    }
    if (carryLength == 3) {
      if (!writeBase64Triplet(client, carry, 3)) {
        return false;
      }
      carryLength = 0;
    }
  }

  uint8_t encodeBuffer[384];
  char encodedBuffer[512];
  while (length - offset >= 3) {
    const size_t rawLength =
        min(sizeof(encodeBuffer), ((length - offset) / 3) * 3);
    memcpy(encodeBuffer, data + offset, rawLength);

    size_t encodedLength = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char *>(encodedBuffer),
            sizeof(encodedBuffer),
            &encodedLength,
            encodeBuffer,
            rawLength) != 0) {
      return false;
    }

    if (!writeAll(client,
                  reinterpret_cast<const uint8_t *>(encodedBuffer),
                  encodedLength)) {
      return false;
    }

    offset += rawLength;
  }

  while (offset < length) {
    carry[carryLength++] = data[offset++];
  }

  return true;
}

bool finishBase64Stream(WiFiClientSecure &client,
                        uint8_t *carry,
                        size_t &carryLength) {
  if (carryLength == 0) {
    return true;
  }

  const bool ok = writeBase64Triplet(client, carry, carryLength);
  carryLength = 0;
  return ok;
}

uint16_t readLittleEndian16(const uint8_t *buffer) {
  return static_cast<uint16_t>(buffer[0]) |
         (static_cast<uint16_t>(buffer[1]) << 8);
}

uint32_t readLittleEndian32(const uint8_t *buffer) {
  return static_cast<uint32_t>(buffer[0]) |
         (static_cast<uint32_t>(buffer[1]) << 8) |
         (static_cast<uint32_t>(buffer[2]) << 16) |
         (static_cast<uint32_t>(buffer[3]) << 24);
}

bool decodeWavPcmInPlace(uint8_t *buffer,
                         size_t decodedBytes,
                         size_t &pcmOffset,
                         size_t &pcmBytes,
                         uint32_t &sampleRate) {
  uint16_t audioFormat = 0;
  uint16_t channelCount = 0;
  uint16_t bitsPerSample = 0;
  bool foundFmt = false;
  bool foundData = false;
  size_t offset = 12;

  while (offset + 8 <= decodedBytes) {
    const uint8_t *chunk = buffer + offset;
    const uint32_t chunkSize = readLittleEndian32(chunk + 4);
    const size_t chunkDataOffset = offset + 8;
    if (chunkDataOffset + chunkSize > decodedBytes) {
      Serial.println("WAV chunk extends past decoded TTS audio");
      return false;
    }

    if (memcmp(chunk, "fmt ", 4) == 0) {
      if (chunkSize < 16) {
        Serial.println("WAV fmt chunk is too short");
        return false;
      }
      audioFormat = readLittleEndian16(buffer + chunkDataOffset);
      channelCount = readLittleEndian16(buffer + chunkDataOffset + 2);
      sampleRate = readLittleEndian32(buffer + chunkDataOffset + 4);
      bitsPerSample = readLittleEndian16(buffer + chunkDataOffset + 14);
      foundFmt = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      pcmOffset = chunkDataOffset;
      pcmBytes = chunkSize;
      foundData = true;
    }

    offset = chunkDataOffset + chunkSize + (chunkSize % 2);
  }

  if (!foundFmt || !foundData) {
    Serial.println("WAV TTS audio is missing fmt or data chunk");
    return false;
  }

  if (audioFormat != 1 || bitsPerSample != 16) {
    Serial.printf("Unsupported TTS WAV format: format=%u bits=%u\n",
                  audioFormat,
                  bitsPerSample);
    return false;
  }

  if (channelCount != 1 && channelCount != 2) {
    Serial.printf("Unsupported TTS WAV channel count: %u\n", channelCount);
    return false;
  }

  if (channelCount == 2) {
    const size_t stereoFrames = pcmBytes / (2 * sizeof(int16_t));
    int16_t *pcm = reinterpret_cast<int16_t *>(buffer + pcmOffset);
    for (size_t i = 0; i < stereoFrames; ++i) {
      const int32_t mixed =
          (static_cast<int32_t>(pcm[i * 2]) + pcm[i * 2 + 1]) / 2;
      pcm[i] = static_cast<int16_t>(mixed);
    }
    pcmBytes = stereoFrames * sizeof(int16_t);
  }

  if (pcmOffset > 0 && pcmBytes > 0) {
    memmove(buffer, buffer + pcmOffset, pcmBytes);
    pcmOffset = 0;
  }

  return pcmBytes > 0;
}

void printPcmStats(const char *label, const uint8_t *buffer, size_t pcmBytes) {
  if (pcmBytes < sizeof(int16_t)) {
    Serial.printf("%s PCM stats: no samples\n", label);
    return;
  }

  const int16_t *samples = reinterpret_cast<const int16_t *>(buffer);
  const size_t sampleCount = pcmBytes / sizeof(int16_t);
  int16_t minSample = 32767;
  int16_t maxSample = -32768;
  uint64_t levelSum = 0;

  for (size_t i = 0; i < sampleCount; ++i) {
    const int16_t sample = samples[i];
    minSample = min(minSample, sample);
    maxSample = max(maxSample, sample);
    levelSum += abs(static_cast<int32_t>(sample));
  }

  Serial.printf("%s PCM stats: min=%d max=%d avg_abs=%lu\n",
                label,
                minSample,
                maxSample,
                static_cast<unsigned long>(levelSum / sampleCount));
}

uint32_t sampleRateFromMimeType(const String &mimeType, uint32_t fallback) {
  int rateIndex = mimeType.indexOf("rate=");
  if (rateIndex < 0) {
    rateIndex = mimeType.indexOf("sample_rate=");
  }
  if (rateIndex < 0) {
    return fallback;
  }

  const int valueStart = mimeType.indexOf('=', rateIndex) + 1;
  if (valueStart <= 0) {
    return fallback;
  }

  uint32_t rate = 0;
  for (int i = valueStart; i < static_cast<int>(mimeType.length()); ++i) {
    const char c = mimeType[i];
    if (c < '0' || c > '9') {
      break;
    }
    rate = rate * 10 + static_cast<uint32_t>(c - '0');
  }

  return rate > 0 ? rate : fallback;
}

bool isRawPcmMimeType(const String &mimeType) {
  if (mimeType.isEmpty()) {
    return true;
  }

  String normalized = mimeType;
  normalized.toLowerCase();
  return normalized.indexOf("audio/l16") >= 0 ||
         normalized.indexOf("audio/pcm") >= 0 ||
         normalized.indexOf("audio/raw") >= 0 ||
         normalized.indexOf("s16le") >= 0;
}

void printAudioHeaderBytes(const uint8_t *buffer, size_t length) {
  Serial.print("TTS first bytes:");
  const size_t bytesToPrint = min(static_cast<size_t>(16), length);
  for (size_t i = 0; i < bytesToPrint; ++i) {
    Serial.printf(" %02X", buffer[i]);
  }
  Serial.println();
}

bool readHttpBody(WiFiClientSecure &client, int &statusCode, String &body) {
  const uint32_t deadline = millis() + 60000;
  while (!client.available() && millis() < deadline) {
    delay(10);
  }

  const String statusLine = client.readStringUntil('\n');
  statusCode = 0;
  const int firstSpace = statusLine.indexOf(' ');
  if (firstSpace >= 0) {
    statusCode = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
  }

  bool chunked = false;
  int contentLength = -1;
  while (client.connected() || client.available()) {
    String header = client.readStringUntil('\n');
    header.trim();
    if (header.length() == 0) {
      break;
    }
    header.toLowerCase();
    if (header.indexOf(F("transfer-encoding: chunked")) >= 0) {
      chunked = true;
    } else if (header.startsWith(F("content-length:"))) {
      contentLength = header.substring(strlen("content-length:")).toInt();
    }
  }

  body = "";
  if (contentLength > 0) {
    body.reserve(contentLength + 1);
  }

  if (chunked) {
    while (client.connected() || client.available()) {
      String sizeLine = client.readStringUntil('\n');
      sizeLine.trim();
      const int chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize <= 0) {
        break;
      }
      for (int i = 0; i < chunkSize; ++i) {
        const uint32_t byteDeadline = millis() + 60000;
        while (!client.available() && millis() < byteDeadline) {
          delay(5);
        }
        if (!client.available()) {
          return false;
        }
        body += static_cast<char>(client.read());
      }
      client.readStringUntil('\n');
    }
  } else if (contentLength >= 0) {
    while (static_cast<int>(body.length()) < contentLength) {
      const uint32_t byteDeadline = millis() + 60000;
      while (!client.available() && client.connected() && millis() < byteDeadline) {
        delay(5);
      }
      if (!client.available()) {
        break;
      }
      while (client.available() &&
             static_cast<int>(body.length()) < contentLength) {
        body += static_cast<char>(client.read());
      }
    }

    if (static_cast<int>(body.length()) != contentLength) {
      Serial.printf("HTTP body was short: got %u of %d bytes\n",
                    static_cast<unsigned>(body.length()),
                    contentLength);
      return false;
    }
  } else {
    while (client.connected() || client.available()) {
      while (client.available()) {
        body += static_cast<char>(client.read());
      }
      delay(5);
    }
  }

  return statusCode > 0;
}

bool waitForClientData(WiFiClientSecure &client, uint32_t timeoutMs = 60000) {
  const uint32_t deadline = millis() + timeoutMs;
  while (!client.available() && client.connected() && millis() < deadline) {
    delay(5);
  }
  return client.available();
}

bool readHttpHeaders(WiFiClientSecure &client,
                     int &statusCode,
                     bool &chunked,
                     int &contentLength) {
  const uint32_t deadline = millis() + 60000;
  while (!client.available() && millis() < deadline) {
    delay(10);
  }

  const String statusLine = client.readStringUntil('\n');
  statusCode = 0;
  const int firstSpace = statusLine.indexOf(' ');
  if (firstSpace >= 0) {
    statusCode = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
  }

  chunked = false;
  contentLength = -1;
  while (client.connected() || client.available()) {
    String header = client.readStringUntil('\n');
    header.trim();
    if (header.length() == 0) {
      break;
    }
    header.toLowerCase();
    if (header.indexOf(F("transfer-encoding: chunked")) >= 0) {
      chunked = true;
    } else if (header.startsWith(F("content-length:"))) {
      contentLength = header.substring(strlen("content-length:")).toInt();
    }
  }

  return statusCode > 0;
}

bool readHttpBodyByte(HttpBodyReader &reader, char &out) {
  if (reader.finished) {
    return false;
  }

  if (reader.chunked) {
    while (reader.chunkRemaining <= 0) {
      if (!waitForClientData(*reader.client)) {
        reader.finished = true;
        return false;
      }

      String sizeLine = reader.client->readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) {
        continue;
      }

      reader.chunkRemaining = strtol(sizeLine.c_str(), nullptr, 16);
      if (reader.chunkRemaining <= 0) {
        reader.finished = true;
        return false;
      }
    }

    if (!waitForClientData(*reader.client)) {
      reader.finished = true;
      return false;
    }

    out = static_cast<char>(reader.client->read());
    --reader.chunkRemaining;
    ++reader.bytesRead;
    if (reader.chunkRemaining == 0) {
      reader.client->readStringUntil('\n');
    }
    return true;
  }

  if (reader.contentLength >= 0 && reader.bytesRead >= reader.contentLength) {
    reader.finished = true;
    return false;
  }

  if (!waitForClientData(*reader.client)) {
    reader.finished = true;
    return false;
  }

  out = static_cast<char>(reader.client->read());
  ++reader.bytesRead;
  return true;
}

bool parseHttpsUrl(const String &url, String &host, String &path) {
  constexpr char prefix[] = "https://";
  if (!url.startsWith(prefix)) {
    return false;
  }

  const int hostStart = strlen(prefix);
  const int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) {
    return false;
  }

  host = url.substring(hostStart, pathStart);
  path = url.substring(pathStart);
  return host.length() > 0 && path.length() > 0;
}

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 16);

  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    switch (c) {
      case '"':
        escaped += F("\\\"");
        break;
      case '\\':
        escaped += F("\\\\");
        break;
      case '\n':
        escaped += F("\\n");
        break;
      case '\r':
        escaped += F("\\r");
        break;
      case '\t':
        escaped += F("\\t");
        break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          escaped += ' ';
        } else {
          escaped += c;
        }
        break;
    }
  }

  return escaped;
}

bool appendJsonStringValue(const String &json, int keyIndex, String &value) {
  int colon = json.indexOf(':', keyIndex);
  if (colon < 0) {
    return false;
  }

  int start = json.indexOf('"', colon + 1);
  if (start < 0) {
    return false;
  }

  value = "";
  bool escaping = false;
  for (int i = start + 1; i < static_cast<int>(json.length()); ++i) {
    const char c = json[i];
    if (escaping) {
      switch (c) {
        case 'n':
          value += '\n';
          break;
        case 'r':
          value += '\r';
          break;
        case 't':
          value += '\t';
          break;
        default:
          value += c;
          break;
      }
      escaping = false;
    } else if (c == '\\') {
      escaping = true;
    } else if (c == '"') {
      return true;
    } else {
      value += c;
    }
  }

  return false;
}

bool extractJsonStringAfter(const String &json,
                            const char *anchor,
                            const char *key,
                            String &value) {
  int start = anchor == nullptr ? 0 : json.indexOf(anchor);
  if (start < 0) {
    return false;
  }

  const int keyIndex = json.indexOf(key, start);
  if (keyIndex < 0) {
    return false;
  }

  return appendJsonStringValue(json, keyIndex, value);
}

bool findJsonStringBoundsAfter(const String &json,
                               const char *anchor,
                               const char *key,
                               int &valueStart,
                               int &valueLength) {
  int searchStart = anchor == nullptr ? 0 : json.indexOf(anchor);
  if (searchStart < 0) {
    return false;
  }

  const int keyIndex = json.indexOf(key, searchStart);
  if (keyIndex < 0) {
    return false;
  }

  const int colon = json.indexOf(':', keyIndex);
  if (colon < 0) {
    return false;
  }

  valueStart = json.indexOf('"', colon + 1);
  if (valueStart < 0) {
    return false;
  }
  ++valueStart;

  bool escaping = false;
  for (int i = valueStart; i < static_cast<int>(json.length()); ++i) {
    const char c = json[i];
    if (escaping) {
      escaping = false;
    } else if (c == '\\') {
      escaping = true;
    } else if (c == '"') {
      valueLength = i - valueStart;
      return true;
    }
  }

  Serial.println("JSON string was not terminated; refusing partial audio");
  return false;
}

bool addTtsPcmChunk() {
  TtsPcmChunk *chunk =
      static_cast<TtsPcmChunk *>(heap_caps_malloc(sizeof(TtsPcmChunk),
                                                  MALLOC_CAP_8BIT));
  if (chunk == nullptr) {
    Serial.println("Could not allocate TTS PCM chunk metadata");
    return false;
  }

  chunk->data = static_cast<uint8_t *>(heap_caps_malloc(
      TTS_PCM_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (chunk->data == nullptr) {
    chunk->data = static_cast<uint8_t *>(heap_caps_malloc(
        TTS_PCM_CHUNK_BYTES, MALLOC_CAP_8BIT));
  }
  if (chunk->data == nullptr) {
    heap_caps_free(chunk);
    Serial.printf("Could not allocate %u-byte TTS PCM chunk\n",
                  static_cast<unsigned>(TTS_PCM_CHUNK_BYTES));
    return false;
  }

  chunk->used = 0;
  chunk->next = nullptr;
  if (ttsPcmTail == nullptr) {
    ttsPcmHead = chunk;
  } else {
    ttsPcmTail->next = chunk;
  }
  ttsPcmTail = chunk;
  return true;
}

bool appendTtsPcmBytes(const uint8_t *data, size_t length) {
  size_t copied = 0;
  while (copied < length) {
    if (ttsPcmTail == nullptr || ttsPcmTail->used >= TTS_PCM_CHUNK_BYTES) {
      if (!addTtsPcmChunk()) {
        return false;
      }
    }

    const size_t available = TTS_PCM_CHUNK_BYTES - ttsPcmTail->used;
    const size_t bytesThisCopy = min(available, length - copied);
    memcpy(ttsPcmTail->data + ttsPcmTail->used,
           data + copied,
           bytesThisCopy);
    ttsPcmTail->used += bytesThisCopy;
    ttsPcmBytes += bytesThisCopy;
    copied += bytesThisCopy;
  }

  return true;
}

void freeTtsChunkChain(TtsPcmChunk *chunk) {
  while (chunk != nullptr) {
    TtsPcmChunk *next = chunk->next;
    if (chunk->data != nullptr) {
      heap_caps_free(chunk->data);
    }
    heap_caps_free(chunk);
    chunk = next;
  }
}

bool readTtsByteAt(size_t offset, uint8_t &value) {
  TtsPcmChunk *chunk = ttsPcmHead;
  while (chunk != nullptr) {
    if (offset < chunk->used) {
      value = chunk->data[offset];
      return true;
    }
    offset -= chunk->used;
    chunk = chunk->next;
  }

  return false;
}

bool writeTtsByteAt(size_t offset, uint8_t value) {
  TtsPcmChunk *chunk = ttsPcmHead;
  while (chunk != nullptr) {
    if (offset < chunk->used) {
      chunk->data[offset] = value;
      return true;
    }
    offset -= chunk->used;
    chunk = chunk->next;
  }

  return false;
}

bool readTtsBytesAt(size_t offset, uint8_t *buffer, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (!readTtsByteAt(offset + i, buffer[i])) {
      return false;
    }
  }
  return true;
}

bool readTtsLittleEndian16At(size_t offset, uint16_t &value) {
  uint8_t bytes[2];
  if (!readTtsBytesAt(offset, bytes, sizeof(bytes))) {
    return false;
  }
  value = readLittleEndian16(bytes);
  return true;
}

bool readTtsLittleEndian32At(size_t offset, uint32_t &value) {
  uint8_t bytes[4];
  if (!readTtsBytesAt(offset, bytes, sizeof(bytes))) {
    return false;
  }
  value = readLittleEndian32(bytes);
  return true;
}

bool writeTtsLittleEndian16At(size_t offset, int16_t value) {
  const uint16_t unsignedValue = static_cast<uint16_t>(value);
  return writeTtsByteAt(offset, unsignedValue & 0xFF) &&
         writeTtsByteAt(offset + 1, (unsignedValue >> 8) & 0xFF);
}

void resetTtsPlaybackCursor() {
  playbackChunk = ttsPcmHead;
  playbackChunkOffset = 0;
  playbackCursor = 0;
}

bool discardTtsPrefix(size_t bytesToDiscard) {
  while (ttsPcmHead != nullptr && bytesToDiscard >= ttsPcmHead->used) {
    bytesToDiscard -= ttsPcmHead->used;
    ttsPcmBytes -= ttsPcmHead->used;
    TtsPcmChunk *next = ttsPcmHead->next;
    if (ttsPcmHead->data != nullptr) {
      heap_caps_free(ttsPcmHead->data);
    }
    heap_caps_free(ttsPcmHead);
    ttsPcmHead = next;
  }

  if (ttsPcmHead == nullptr) {
    ttsPcmTail = nullptr;
    resetTtsPlaybackCursor();
    return bytesToDiscard == 0;
  }

  if (bytesToDiscard > 0) {
    memmove(ttsPcmHead->data,
            ttsPcmHead->data + bytesToDiscard,
            ttsPcmHead->used - bytesToDiscard);
    ttsPcmHead->used -= bytesToDiscard;
    ttsPcmBytes -= bytesToDiscard;
  }

  resetTtsPlaybackCursor();
  return true;
}

void truncateTtsBytes(size_t newLength) {
  if (newLength >= ttsPcmBytes) {
    return;
  }

  if (newLength == 0) {
    freeTtsAudio();
    return;
  }

  size_t remaining = newLength;
  TtsPcmChunk *chunk = ttsPcmHead;
  TtsPcmChunk *previous = nullptr;

  while (chunk != nullptr && remaining > chunk->used) {
    remaining -= chunk->used;
    previous = chunk;
    chunk = chunk->next;
  }

  TtsPcmChunk *firstFree = nullptr;
  if (chunk != nullptr) {
    if (remaining == chunk->used) {
      firstFree = chunk->next;
      chunk->next = nullptr;
      ttsPcmTail = chunk;
    } else {
      chunk->used = remaining;
      firstFree = chunk->next;
      chunk->next = nullptr;
      ttsPcmTail = chunk;
    }
  } else if (previous != nullptr) {
    firstFree = previous->next;
    previous->next = nullptr;
    ttsPcmTail = previous;
  }

  freeTtsChunkChain(firstFree);
  ttsPcmBytes = newLength;
  resetTtsPlaybackCursor();
}

bool readPlaybackByte(uint8_t &value) {
  while (playbackChunk != nullptr && playbackChunkOffset >= playbackChunk->used) {
    playbackChunk = playbackChunk->next;
    playbackChunkOffset = 0;
  }

  if (playbackChunk == nullptr) {
    return false;
  }

  value = playbackChunk->data[playbackChunkOffset++];
  ++playbackCursor;
  return true;
}

bool readPlaybackSample(int16_t &sample) {
  uint8_t low = 0;
  uint8_t high = 0;
  if (!readPlaybackByte(low) || !readPlaybackByte(high)) {
    return false;
  }

  sample = static_cast<int16_t>(static_cast<uint16_t>(low) |
                                (static_cast<uint16_t>(high) << 8));
  return true;
}

void printTtsHeaderBytes() {
  Serial.print("TTS first bytes:");
  const size_t bytesToPrint = min(static_cast<size_t>(16), ttsPcmBytes);
  for (size_t i = 0; i < bytesToPrint; ++i) {
    uint8_t value = 0;
    if (readTtsByteAt(i, value)) {
      Serial.printf(" %02X", value);
    }
  }
  Serial.println();
}

void printTtsPcmStats() {
  if (ttsPcmBytes < sizeof(int16_t)) {
    Serial.println("TTS PCM stats: no samples");
    return;
  }

  TtsPcmChunk *savedPlaybackChunk = playbackChunk;
  const size_t savedPlaybackChunkOffset = playbackChunkOffset;
  const size_t savedPlaybackCursor = playbackCursor;
  playbackChunk = ttsPcmHead;
  playbackChunkOffset = 0;
  playbackCursor = 0;

  const size_t sampleCount = ttsPcmBytes / sizeof(int16_t);
  int16_t minSample = 32767;
  int16_t maxSample = -32768;
  uint64_t levelSum = 0;

  for (size_t i = 0; i < sampleCount; ++i) {
    int16_t sample = 0;
    if (!readPlaybackSample(sample)) {
      break;
    }
    minSample = min(minSample, sample);
    maxSample = max(maxSample, sample);
    levelSum += abs(static_cast<int32_t>(sample));
  }

  playbackChunk = savedPlaybackChunk;
  playbackChunkOffset = savedPlaybackChunkOffset;
  playbackCursor = savedPlaybackCursor;

  Serial.printf("TTS PCM stats: min=%d max=%d avg_abs=%lu\n",
                minSample,
                maxSample,
                static_cast<unsigned long>(levelSum / sampleCount));
}

bool ttsStartsWithRiffWave() {
  if (ttsPcmBytes < 12) {
    return false;
  }

  uint8_t bytes[12];
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    if (!readTtsByteAt(i, bytes[i])) {
      return false;
    }
  }

  return memcmp(bytes, "RIFF", 4) == 0 && memcmp(bytes + 8, "WAVE", 4) == 0;
}

bool normalizeTtsWavInPlace(uint32_t &sampleRate) {
  if (!ttsStartsWithRiffWave()) {
    return false;
  }

  uint16_t audioFormat = 0;
  uint16_t channelCount = 0;
  uint16_t bitsPerSample = 0;
  uint32_t wavSampleRate = 0;
  size_t dataOffset = 0;
  size_t dataBytes = 0;
  bool foundFmt = false;
  bool foundData = false;
  size_t offset = 12;

  while (offset + 8 <= ttsPcmBytes) {
    uint8_t chunkId[4];
    uint32_t chunkSize = 0;
    if (!readTtsBytesAt(offset, chunkId, sizeof(chunkId)) ||
        !readTtsLittleEndian32At(offset + 4, chunkSize)) {
      return false;
    }

    const size_t chunkDataOffset = offset + 8;
    if (chunkDataOffset + chunkSize > ttsPcmBytes) {
      Serial.println("TTS WAV chunk extends past decoded audio");
      return false;
    }

    if (memcmp(chunkId, "fmt ", 4) == 0) {
      if (chunkSize < 16) {
        Serial.println("TTS WAV fmt chunk is too short");
        return false;
      }
      uint16_t blockAlign = 0;
      if (!readTtsLittleEndian16At(chunkDataOffset, audioFormat) ||
          !readTtsLittleEndian16At(chunkDataOffset + 2, channelCount) ||
          !readTtsLittleEndian32At(chunkDataOffset + 4, wavSampleRate) ||
          !readTtsLittleEndian16At(chunkDataOffset + 12, blockAlign) ||
          !readTtsLittleEndian16At(chunkDataOffset + 14, bitsPerSample)) {
        return false;
      }
      if (blockAlign == 0) {
        Serial.println("TTS WAV block align is invalid");
        return false;
      }
      foundFmt = true;
    } else if (memcmp(chunkId, "data", 4) == 0) {
      dataOffset = chunkDataOffset;
      dataBytes = chunkSize;
      foundData = true;
    }

    offset = chunkDataOffset + chunkSize + (chunkSize % 2);
  }

  if (!foundFmt || !foundData) {
    Serial.println("TTS WAV audio is missing fmt or data chunk");
    return false;
  }

  if (audioFormat != 1 || bitsPerSample != 16) {
    Serial.printf("Unsupported TTS WAV format: format=%u bits=%u\n",
                  audioFormat,
                  bitsPerSample);
    return false;
  }

  if (channelCount != 1 && channelCount != 2) {
    Serial.printf("Unsupported TTS WAV channel count: %u\n", channelCount);
    return false;
  }

  if (channelCount == 1) {
    dataBytes -= dataBytes % sizeof(int16_t);
    if (!discardTtsPrefix(dataOffset)) {
      return false;
    }
    truncateTtsBytes(dataBytes);
  } else {
    const size_t stereoFrameBytes = 2 * sizeof(int16_t);
    const size_t stereoFrames = dataBytes / stereoFrameBytes;
    for (size_t i = 0; i < stereoFrames; ++i) {
      uint16_t leftRaw = 0;
      uint16_t rightRaw = 0;
      const size_t frameOffset = dataOffset + i * stereoFrameBytes;
      if (!readTtsLittleEndian16At(frameOffset, leftRaw) ||
          !readTtsLittleEndian16At(frameOffset + sizeof(int16_t), rightRaw)) {
        return false;
      }

      const int16_t left = static_cast<int16_t>(leftRaw);
      const int16_t right = static_cast<int16_t>(rightRaw);
      const int16_t mixed =
          static_cast<int16_t>((static_cast<int32_t>(left) + right) / 2);
      if (!writeTtsLittleEndian16At(i * sizeof(int16_t), mixed)) {
        return false;
      }
    }
    truncateTtsBytes(stereoFrames * sizeof(int16_t));
  }

  sampleRate = wavSampleRate;
  Serial.printf("TTS WAV normalized to mono PCM: %u bytes @ %lu Hz\n",
                static_cast<unsigned>(ttsPcmBytes),
                static_cast<unsigned long>(sampleRate));
  return ttsPcmBytes > 0 && sampleRate > 0;
}

bool decodeAndAppendBase64Quartet(char quartet[4]) {
  uint8_t decoded[3];
  size_t decodedLength = 0;
  const int result = mbedtls_base64_decode(
      decoded,
      sizeof(decoded),
      &decodedLength,
      reinterpret_cast<const unsigned char *>(quartet),
      4);
  if (result != 0) {
    Serial.println("Could not decode TTS base64 audio chunk");
    return false;
  }

  return appendTtsPcmBytes(decoded, decodedLength);
}

bool readGeminiTtsAudioResponse(WiFiClientSecure &client,
                                int &status,
                                String &audioMimeType) {
  bool chunked = false;
  int contentLength = -1;
  if (!readHttpHeaders(client, status, chunked, contentLength)) {
    return false;
  }

  if (status < 200 || status >= 300) {
    String errorBody;
    errorBody.reserve(contentLength > 0 ? min(contentLength, 1024) : 1024);
    HttpBodyReader errorReader = {
        &client, chunked, contentLength, 0, 0, false};
    char c = 0;
    while (readHttpBodyByte(errorReader, c) && errorBody.length() < 1024) {
      errorBody += c;
    }
    Serial.printf("Gemini TTS HTTP %d\n", status);
    Serial.println(errorBody);
    return false;
  }

  freeTtsAudio();

  HttpBodyReader reader = {&client, chunked, contentLength, 0, 0, false};
  enum class ParseState {
    Searching,
    SeekMimeColon,
    SeekMimeQuote,
    ReadMime,
    SeekDataColon,
    SeekDataQuote,
    ReadData,
    Done,
  };

  ParseState parseState = ParseState::Searching;
  String tail;
  tail.reserve(48);
  bool sawInlineData = false;
  bool escaping = false;
  char quartet[4] = {0, 0, 0, 0};
  size_t quartetLength = 0;
  char c = 0;

  while (readHttpBodyByte(reader, c)) {
    switch (parseState) {
      case ParseState::Searching:
        tail += c;
        if (tail.length() > 48) {
          tail.remove(0, tail.length() - 48);
        }
        if (tail.endsWith("\"inlineData\"") ||
            tail.endsWith("\"inline_data\"")) {
          sawInlineData = true;
        } else if (sawInlineData &&
                   (tail.endsWith("\"mimeType\"") ||
                    tail.endsWith("\"mime_type\""))) {
          parseState = ParseState::SeekMimeColon;
        } else if ((sawInlineData && tail.endsWith("\"data\"")) ||
                   tail.endsWith("\"audioContent\"") ||
                   tail.endsWith("\"audio_content\"")) {
          parseState = ParseState::SeekDataColon;
        }
        break;

      case ParseState::SeekMimeColon:
        if (c == ':') {
          parseState = ParseState::SeekMimeQuote;
        }
        break;

      case ParseState::SeekMimeQuote:
        if (c == '"') {
          audioMimeType = "";
          escaping = false;
          parseState = ParseState::ReadMime;
        }
        break;

      case ParseState::ReadMime:
        if (escaping) {
          audioMimeType += c;
          escaping = false;
        } else if (c == '\\') {
          escaping = true;
        } else if (c == '"') {
          parseState = ParseState::Searching;
          tail = "";
        } else {
          audioMimeType += c;
        }
        break;

      case ParseState::SeekDataColon:
        if (c == ':') {
          parseState = ParseState::SeekDataQuote;
        }
        break;

      case ParseState::SeekDataQuote:
        if (c == '"') {
          escaping = false;
          quartetLength = 0;
          parseState = ParseState::ReadData;
        }
        break;

      case ParseState::ReadData:
        if (escaping) {
          quartet[quartetLength++] = c;
          if (quartetLength == 4) {
            if (!decodeAndAppendBase64Quartet(quartet)) {
              return false;
            }
            quartetLength = 0;
          }
          escaping = false;
        } else if (c == '\\') {
          escaping = true;
        } else if (c == '"') {
          if (quartetLength != 0) {
            Serial.println("TTS base64 audio ended on a partial quartet");
            return false;
          }
          parseState = ParseState::Done;
          break;
        } else if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
          break;
        } else {
          quartet[quartetLength++] = c;
          if (quartetLength == 4) {
            if (!decodeAndAppendBase64Quartet(quartet)) {
              return false;
            }
            quartetLength = 0;
          }
        }
        break;

      case ParseState::Done:
        break;
    }

    if (parseState == ParseState::Done) {
      break;
    }
  }

  if (parseState != ParseState::Done) {
    Serial.println("Gemini TTS audio data was not fully received");
    Serial.printf("TTS response bytes read: %u\n",
                  static_cast<unsigned>(reader.bytesRead));
    return false;
  }

  Serial.printf("TTS response bytes read: %u, decoded audio bytes: %u\n",
                static_cast<unsigned>(reader.bytesRead),
                static_cast<unsigned>(ttsPcmBytes));
  return true;
}

bool startGeminiFileUpload(String &uploadUrl) {
  const size_t wavBytes = 44 + recordedSampleCount * sizeof(int16_t);
  char metadata[] = "{\"file\":{\"display_name\":\"esp32-question.wav\"}}";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  const String url = F("https://generativelanguage.googleapis.com/upload/v1beta/files");

  if (!https.begin(client, url)) {
    Serial.println("Could not begin Gemini upload start request");
    return false;
  }

  const char *headers[] = {"x-goog-upload-url"};
  https.collectHeaders(headers, 1);
  https.addHeader(F("Content-Type"), F("application/json"));
  https.addHeader(F("x-goog-api-key"), GEMINI_API_KEY);
  https.addHeader(F("X-Goog-Upload-Protocol"), F("resumable"));
  https.addHeader(F("X-Goog-Upload-Command"), F("start"));
  https.addHeader(F("X-Goog-Upload-Header-Content-Length"),
                  String(wavBytes));
  https.addHeader(F("X-Goog-Upload-Header-Content-Type"), F("audio/wav"));
  https.setTimeout(60000);

  const int status = https.POST(
      reinterpret_cast<uint8_t *>(metadata),
      strlen(metadata));
  uploadUrl = https.header("x-goog-upload-url");
  const String response = https.getString();
  https.end();

  if (status < 200 || status >= 300 || uploadUrl.isEmpty()) {
    Serial.printf("Gemini upload start HTTP %d\n", status);
    Serial.println(response);
    return false;
  }

  return true;
}

bool uploadRecordedWavFile(const String &uploadUrl, String &fileUri) {
  String host;
  String path;
  if (!parseHttpsUrl(uploadUrl, host, path)) {
    Serial.println("Could not parse Gemini upload URL");
    return false;
  }

  const size_t pcmBytes = recordedSampleCount * sizeof(int16_t);
  const size_t wavBytes = 44 + pcmBytes;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(60000);

  if (!client.connect(host.c_str(), 443)) {
    Serial.println("Could not connect to Gemini upload URL");
    return false;
  }

  client.print(F("POST "));
  client.print(path);
  client.print(F(" HTTP/1.1\r\nHost: "));
  client.print(host);
  client.print(F("\r\nContent-Length: "));
  client.print(wavBytes);
  client.print(F("\r\nContent-Type: audio/wav"));
  client.print(F("\r\nX-Goog-Upload-Offset: 0"));
  client.print(F("\r\nX-Goog-Upload-Command: upload, finalize"));
  client.print(F("\r\nConnection: close\r\n\r\n"));

  uint8_t wavHeader[44];
  buildWavHeader(wavHeader);

  if (!writeAll(client, wavHeader, sizeof(wavHeader))) {
    Serial.println("Gemini file upload failed while writing WAV header");
    client.stop();
    return false;
  }
  if (!writeAll(client,
                reinterpret_cast<const uint8_t *>(recordingBuffer),
                pcmBytes)) {
    Serial.println("Gemini file upload failed while writing recorded PCM");
    client.stop();
    return false;
  }

  int status = 0;
  String response;
  if (!readHttpBody(client, status, response)) {
    Serial.println("Could not read Gemini file upload response");
    client.stop();
    return false;
  }
  client.stop();

  if (status < 200 || status >= 300) {
    Serial.printf("Gemini file upload HTTP %d\n", status);
    Serial.println(response);
    return false;
  }

  if (!extractJsonStringAfter(response, "\"file\"", "\"uri\"", fileUri)) {
    Serial.println("Could not parse Gemini file URI");
    Serial.println(response);
    return false;
  }

  Serial.println("Gemini file uploaded:");
  Serial.println(fileUri);
  return true;
}

String geminiGenerateTextFromFile(const String &fileUri) {
  String payload;
  payload.reserve(fileUri.length() + 512);
  payload += F("{\"contents\":[{\"parts\":[");
  payload += F("{\"text\":\"You are a concise voice assistant. Listen to the user's audio and answer naturally in one or two short sentences.\"},");
  payload += F("{\"file_data\":{\"mime_type\":\"audio/wav\",\"file_uri\":\"");
  payload += jsonEscape(fileUri);
  payload += F("\"}}]}],\"generationConfig\":{\"maxOutputTokens\":120}}");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  const String url = String(F("https://generativelanguage.googleapis.com/v1beta/models/")) +
                     GEMINI_TEXT_MODEL + F(":generateContent");

  if (!https.begin(client, url)) {
    Serial.println("Could not begin Gemini text request");
    return String();
  }

  https.addHeader(F("Content-Type"), F("application/json"));
  https.addHeader(F("x-goog-api-key"), GEMINI_API_KEY);
  https.setTimeout(60000);

  const int status = https.POST(payload);
  const String response = https.getString();
  https.end();

  if (status < 200 || status >= 300) {
    Serial.printf("Gemini text HTTP %d\n", status);
    Serial.println(response);
    return String();
  }

  String text;
  if (!extractJsonStringAfter(response, nullptr, "\"text\"", text)) {
    Serial.println("Could not parse Gemini text response");
    Serial.println(response);
    return String();
  }

  text.trim();
  Serial.println("Gemini response:");
  Serial.println(text);
  return text;
}

String geminiGenerateText() {
  String uploadUrl;
  if (!startGeminiFileUpload(uploadUrl)) {
    return String();
  }

  String fileUri;
  if (!uploadRecordedWavFile(uploadUrl, fileUri)) {
    return String();
  }

  return geminiGenerateTextFromFile(fileUri);
}

String geminiGenerateTextInline() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(60000);

  if (!client.connect("generativelanguage.googleapis.com", 443)) {
    Serial.println("Could not connect to Gemini");
    return String();
  }

  constexpr char payloadPrefix[] =
      "{\"contents\":[{\"parts\":["
      "{\"text\":\"You are a concise voice assistant. Listen to the user's audio and answer naturally in one or two short sentences.\"},"
      "{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
  constexpr char payloadSuffix[] =
      "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":120}}";
  const size_t pcmBytes = recordedSampleCount * sizeof(int16_t);
  const size_t wavBytes = 44 + pcmBytes;
  const size_t wavBase64Bytes = ((wavBytes + 2) / 3) * 4;
  const size_t contentLength =
      strlen(payloadPrefix) + wavBase64Bytes + strlen(payloadSuffix);
  const String path = String(F("/v1beta/models/")) + GEMINI_TEXT_MODEL +
                      F(":generateContent");

  client.print(F("POST "));
  client.print(path);
  client.print(F(" HTTP/1.1\r\nHost: generativelanguage.googleapis.com\r\n"));
  client.print(F("Content-Type: application/json\r\nx-goog-api-key: "));
  client.print(GEMINI_API_KEY);
  client.print(F("\r\nContent-Length: "));
  client.print(contentLength);
  client.print(F("\r\nConnection: close\r\n\r\n"));

  uint8_t wavHeader[44];
  buildWavHeader(wavHeader);

  uint8_t carry[3];
  size_t carryLength = 0;
  if (!writeAll(client, payloadPrefix)) {
    Serial.println("Gemini stream failed while writing JSON prefix");
    client.stop();
    return String();
  }
  if (!writeBase64StreamChunk(client,
                              wavHeader,
                              sizeof(wavHeader),
                              carry,
                              carryLength)) {
    Serial.println("Gemini stream failed while writing WAV header");
    client.stop();
    return String();
  }
  if (!writeBase64StreamChunk(client,
                              reinterpret_cast<const uint8_t *>(recordingBuffer),
                              pcmBytes,
                              carry,
                              carryLength)) {
    Serial.println("Gemini stream failed while writing recorded PCM");
    client.stop();
    return String();
  }
  if (!finishBase64Stream(client, carry, carryLength)) {
    Serial.println("Gemini stream failed while finishing base64");
    client.stop();
    return String();
  }
  if (!writeAll(client, payloadSuffix)) {
    Serial.println("Gemini stream failed while writing JSON suffix");
    client.stop();
    return String();
  }

  int status = 0;
  String response;
  if (!readHttpBody(client, status, response)) {
    Serial.println("Could not read Gemini text response");
    client.stop();
    return String();
  }
  client.stop();

  if (status < 200 || status >= 300) {
    Serial.printf("Gemini text HTTP %d\n", status);
    Serial.println(response);
    return String();
  }

  String text;
  if (!extractJsonStringAfter(response, nullptr, "\"text\"", text)) {
    Serial.println("Could not parse Gemini text response");
    Serial.println(response);
    return String();
  }

  text.trim();
  Serial.println("Gemini response:");
  Serial.println(text);
  return text;
}

bool geminiTextToSpeech(const String &text) {
  String payload;
  payload.reserve(text.length() + 512);
  payload += F("{\"contents\":[{\"parts\":[{\"text\":\"Say clearly and conversationally: ");
  payload += jsonEscape(text);
  payload += F("\"}]}],\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],");
  payload += F("\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"");
  payload += GEMINI_TTS_VOICE;
  payload += F("\"}}}},\"model\":\"");
  payload += GEMINI_TTS_MODEL;
  payload += F("\"}");

  int status = 0;
  String audioMimeType;
  bool receivedAudio = false;
  const String path = String(F("/v1beta/models/")) + GEMINI_TTS_MODEL +
                      F(":generateContent");

  for (uint8_t attempt = 1; attempt <= GEMINI_TTS_MAX_ATTEMPTS; ++attempt) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(60000);

    if (!client.connect("generativelanguage.googleapis.com", 443)) {
      Serial.println("Could not connect to Gemini TTS");
      client.stop();
      if (attempt < GEMINI_TTS_MAX_ATTEMPTS) {
        delay(500 * attempt);
        continue;
      }
      freeTtsAudio();
      return false;
    }

    client.print(F("POST "));
    client.print(path);
    client.print(F(" HTTP/1.1\r\nHost: generativelanguage.googleapis.com\r\n"));
    client.print(F("Content-Type: application/json\r\nx-goog-api-key: "));
    client.print(GEMINI_API_KEY);
    client.print(F("\r\nContent-Length: "));
    client.print(payload.length());
    client.print(F("\r\nConnection: close\r\n\r\n"));
    client.print(payload);

    receivedAudio = readGeminiTtsAudioResponse(client, status, audioMimeType);
    client.stop();

    if (receivedAudio) {
      break;
    }

    freeTtsAudio();
    if (status < 500 || status >= 600 || attempt >= GEMINI_TTS_MAX_ATTEMPTS) {
      return false;
    }

    Serial.printf("Retrying Gemini TTS after HTTP %d, attempt %u of %u\n",
                  status,
                  static_cast<unsigned>(attempt + 1),
                  static_cast<unsigned>(GEMINI_TTS_MAX_ATTEMPTS));
    delay(750 * attempt);
  }

  if (!receivedAudio) {
    return false;
  }

  if (!audioMimeType.isEmpty()) {
    Serial.println("TTS MIME type:");
    Serial.println(audioMimeType);
  }

  const size_t decodedLength = ttsPcmBytes;
  size_t pcmBytes = decodedLength;
  uint32_t pcmSampleRate = sampleRateFromMimeType(audioMimeType,
                                                  TTS_SAMPLE_RATE);
  printTtsHeaderBytes();
  const bool ttsLooksLikeWav = ttsStartsWithRiffWave();

  if (ttsLooksLikeWav) {
    if (!normalizeTtsWavInPlace(pcmSampleRate)) {
      freeTtsAudio();
      return false;
    }
    pcmBytes = ttsPcmBytes;
  } else if (!isRawPcmMimeType(audioMimeType)) {
    Serial.println("TTS audio MIME type is not recognized as raw PCM");
    freeTtsAudio();
    return false;
  } else {
    Serial.printf("TTS audio is raw 16-bit little-endian PCM: %u bytes\n",
                  static_cast<unsigned>(decodedLength));
  }

  if (pcmBytes % sizeof(int16_t) != 0) {
    --pcmBytes;
  }

  if (pcmBytes == 0) {
    Serial.println("TTS audio did not contain playable 16-bit PCM");
    freeTtsAudio();
    return false;
  }
  if (pcmSampleRate == 0) {
    Serial.println("TTS audio did not provide a valid sample rate");
    freeTtsAudio();
    return false;
  }

  ttsPlaybackSampleRate = pcmSampleRate;
  ttsPcmBytes = pcmBytes;
  printTtsPcmStats();
  Serial.printf("TTS playback: %u bytes, %u samples, %lu Hz, %lu ms\n",
                static_cast<unsigned>(ttsPcmBytes),
                static_cast<unsigned>(ttsPcmBytes / sizeof(int16_t)),
                static_cast<unsigned long>(ttsPlaybackSampleRate),
                static_cast<unsigned long>(
                    ((ttsPcmBytes / sizeof(int16_t)) * 1000UL) /
                    ttsPlaybackSampleRate));
  return ttsPcmBytes > 0;
}

bool geminiTtsStreamToSpeaker(const String &text) {
  String payload;
  payload.reserve(text.length() + 512);
  payload += F("{\"contents\":[{\"parts\":[{\"text\":\"Say clearly and conversationally: ");
  payload += jsonEscape(text);
  payload += F("\"}]}],\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],");
  payload += F("\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"");
  payload += GEMINI_TTS_VOICE;
  payload += F("\"}}}},\"model\":\"");
  payload += GEMINI_TTS_MODEL;
  payload += F("\"}");

  const String reqPath = String(F("/v1beta/models/")) + GEMINI_TTS_MODEL +
                         F(":generateContent");
  int status = 0;
  bool succeeded = false;

  for (uint8_t attempt = 1; attempt <= GEMINI_TTS_MAX_ATTEMPTS; ++attempt) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(60000);

    if (!client.connect("generativelanguage.googleapis.com", 443)) {
      Serial.println("Could not connect to Gemini TTS (streaming)");
      client.stop();
      if (attempt < GEMINI_TTS_MAX_ATTEMPTS) {
        delay(500 * attempt);
        continue;
      }
      return false;
    }

    client.print(F("POST "));
    client.print(reqPath);
    client.print(F(" HTTP/1.1\r\nHost: generativelanguage.googleapis.com\r\n"));
    client.print(F("Content-Type: application/json\r\nx-goog-api-key: "));
    client.print(GEMINI_API_KEY);
    client.print(F("\r\nContent-Length: "));
    client.print(payload.length());
    client.print(F("\r\nConnection: close\r\n\r\n"));
    client.print(payload);

    bool chunked = false;
    int contentLength = -1;
    if (!readHttpHeaders(client, status, chunked, contentLength)) {
      client.stop();
      if (attempt < GEMINI_TTS_MAX_ATTEMPTS) {
        delay(500 * attempt);
        continue;
      }
      return false;
    }

    if (status < 200 || status >= 300) {
      String errorBody;
      errorBody.reserve(min(contentLength > 0 ? contentLength : 1024, 1024));
      HttpBodyReader errReader = {&client, chunked, contentLength, 0, 0, false};
      char ec;
      while (readHttpBodyByte(errReader, ec) &&
             errorBody.length() < 1024) {
        errorBody += ec;
      }
      Serial.printf("Gemini TTS HTTP %d (streaming)\n", status);
      Serial.println(errorBody);
      client.stop();
      if (status >= 500 && status < 600 &&
          attempt < GEMINI_TTS_MAX_ATTEMPTS) {
        Serial.printf("Retrying streaming TTS, attempt %u of %u\n",
                      static_cast<unsigned>(attempt + 1),
                      static_cast<unsigned>(GEMINI_TTS_MAX_ATTEMPTS));
        delay(750 * attempt);
        continue;
      }
      return false;
    }

    HttpBodyReader reader = {&client, chunked, contentLength, 0, 0, false};

    enum class ParseState {
      Searching,
      SeekMimeColon,
      SeekMimeQuote,
      ReadMime,
      SeekDataColon,
      SeekDataQuote,
      ReadData,
      Done,
    };

    ParseState ps = ParseState::Searching;
    String tail;
    tail.reserve(48);
    bool sawInlineData = false;
    bool jsonEsc = false;
    String audioMimeType;
    char quartet[4];
    size_t quartetLen = 0;

    constexpr size_t WAV_HDR_MAX = 128;
    uint8_t wavHdr[WAV_HDR_MAX];
    size_t wavHdrUsed = 0;
    bool fmtKnown = false;
    bool spkStarted = false;
    uint32_t spkRate = TTS_SAMPLE_RATE;
    uint16_t spkChannels = 1;
    size_t pcmStart = 0;
    size_t totalDec = 0;

    uint8_t frameBuf[4];
    size_t frameBufUsed = 0;
    size_t frameBufNeed = 2;
    size_t i2sSamples = 0;
    bool interrupted = false;
    bool parseOk = true;

    char ch;
    while (parseOk && !interrupted && readHttpBodyByte(reader, ch)) {
      switch (ps) {
        case ParseState::Searching:
          tail += ch;
          if (tail.length() > 48) {
            tail.remove(0, tail.length() - 48);
          }
          if (tail.endsWith("\"inlineData\"") ||
              tail.endsWith("\"inline_data\"")) {
            sawInlineData = true;
          } else if (sawInlineData &&
                     (tail.endsWith("\"mimeType\"") ||
                      tail.endsWith("\"mime_type\""))) {
            ps = ParseState::SeekMimeColon;
          } else if ((sawInlineData && tail.endsWith("\"data\"")) ||
                     tail.endsWith("\"audioContent\"") ||
                     tail.endsWith("\"audio_content\"")) {
            ps = ParseState::SeekDataColon;
          }
          break;

        case ParseState::SeekMimeColon:
          if (ch == ':') {
            ps = ParseState::SeekMimeQuote;
          }
          break;

        case ParseState::SeekMimeQuote:
          if (ch == '"') {
            audioMimeType = "";
            jsonEsc = false;
            ps = ParseState::ReadMime;
          }
          break;

        case ParseState::ReadMime:
          if (jsonEsc) {
            audioMimeType += ch;
            jsonEsc = false;
          } else if (ch == '\\') {
            jsonEsc = true;
          } else if (ch == '"') {
            ps = ParseState::Searching;
            tail = "";
          } else {
            audioMimeType += ch;
          }
          break;

        case ParseState::SeekDataColon:
          if (ch == ':') {
            ps = ParseState::SeekDataQuote;
          }
          break;

        case ParseState::SeekDataQuote:
          if (ch == '"') {
            jsonEsc = false;
            quartetLen = 0;
            ps = ParseState::ReadData;
          }
          break;

        case ParseState::ReadData: {
          char b64c = 0;
          bool haveB64 = false;
          if (jsonEsc) {
            b64c = ch;
            haveB64 = true;
            jsonEsc = false;
          } else if (ch == '\\') {
            jsonEsc = true;
          } else if (ch == '"') {
            if (quartetLen != 0) {
              Serial.println("Streaming TTS: partial base64 at end");
              parseOk = false;
            }
            ps = ParseState::Done;
          } else if (ch != '\n' && ch != '\r' && ch != '\t' && ch != ' ') {
            b64c = ch;
            haveB64 = true;
          }

          if (haveB64) {
            quartet[quartetLen++] = b64c;
            if (quartetLen == 4) {
              uint8_t decoded[3];
              size_t decLen = 0;
              if (mbedtls_base64_decode(
                      decoded,
                      sizeof(decoded),
                      &decLen,
                      reinterpret_cast<const unsigned char *>(quartet),
                      4) != 0) {
                Serial.println("Streaming TTS: base64 decode error");
                parseOk = false;
                break;
              }
              quartetLen = 0;

              for (size_t di = 0; di < decLen && parseOk && !interrupted;
                   ++di) {
                const uint8_t b = decoded[di];
                const size_t pos = totalDec++;

                if (!fmtKnown) {
                  if (wavHdrUsed < WAV_HDR_MAX) {
                    wavHdr[wavHdrUsed++] = b;
                  }
                  if (wavHdrUsed < 12) {
                    continue;
                  }

                  const bool isWav =
                      memcmp(wavHdr, "RIFF", 4) == 0 &&
                      memcmp(wavHdr + 8, "WAVE", 4) == 0;

                  if (!isWav && wavHdrUsed >= 44) {
                    fmtKnown = true;
                    pcmStart = 0;
                    spkRate = sampleRateFromMimeType(audioMimeType,
                                                    TTS_SAMPLE_RATE);
                    spkChannels = 1;
                    frameBufNeed = 2;
                    Serial.printf("Streaming raw PCM: %lu Hz\n",
                                  static_cast<unsigned long>(spkRate));

                    if (!setupSpeakerI2S(spkRate)) {
                      Serial.println("I2S setup failed (streaming)");
                      parseOk = false;
                      break;
                    }
                    digitalWrite(AMP_SD_PIN, HIGH);
                    delay(5);
                    i2s_zero_dma_buffer(I2S_NUM_0);
                    spkStarted = true;

                    for (size_t hi = 0; hi < wavHdrUsed; ++hi) {
                      frameBuf[frameBufUsed++] = wavHdr[hi];
                      if (frameBufUsed >= frameBufNeed) {
                        frameBufUsed = 0;
                        const int16_t s = static_cast<int16_t>(
                            static_cast<uint16_t>(frameBuf[0]) |
                            (static_cast<uint16_t>(frameBuf[1]) << 8));
                        const int16_t gs = applyPlaybackGain(s);
                        i2sOutputBuffer[i2sSamples * 2] = gs;
                        i2sOutputBuffer[i2sSamples * 2 + 1] = gs;
                        if (++i2sSamples >= MONO_SAMPLES_PER_CHUNK) {
                          writeSpeakerSamples(i2sOutputBuffer, i2sSamples);
                          i2sSamples = 0;
                        }
                      }
                    }
                    continue;
                  }

                  if (isWav) {
                    bool foundFmt = false;
                    bool foundData = false;
                    uint16_t audioFmt = 0;
                    uint16_t bps = 0;
                    size_t scanOff = 12;

                    while (scanOff + 8 <= wavHdrUsed) {
                      const uint32_t chunkSz =
                          readLittleEndian32(wavHdr + scanOff + 4);
                      const size_t chunkDataOff = scanOff + 8;

                      if (memcmp(wavHdr + scanOff, "fmt ", 4) == 0 &&
                          chunkDataOff + 16 <= wavHdrUsed && chunkSz >= 16) {
                        audioFmt = readLittleEndian16(wavHdr + chunkDataOff);
                        spkChannels =
                            readLittleEndian16(wavHdr + chunkDataOff + 2);
                        spkRate =
                            readLittleEndian32(wavHdr + chunkDataOff + 4);
                        bps = readLittleEndian16(wavHdr + chunkDataOff + 14);
                        foundFmt = true;
                      } else if (memcmp(wavHdr + scanOff, "data", 4) == 0) {
                        pcmStart = chunkDataOff;
                        foundData = true;
                        break;
                      }

                      const size_t nextOff =
                          chunkDataOff + chunkSz + (chunkSz % 2);
                      if (nextOff <= scanOff || nextOff > wavHdrUsed) {
                        break;
                      }
                      scanOff = nextOff;
                    }

                    if (!foundFmt || !foundData) {
                      if (wavHdrUsed >= WAV_HDR_MAX) {
                        Serial.println("WAV header too large for streaming");
                        parseOk = false;
                        break;
                      }
                      continue;
                    }

                    if (audioFmt != 1 || bps != 16) {
                      Serial.printf(
                          "Streaming TTS: unsupported WAV fmt=%u bps=%u\n",
                          audioFmt,
                          bps);
                      parseOk = false;
                      break;
                    }
                    if (spkChannels != 1 && spkChannels != 2) {
                      Serial.printf(
                          "Streaming TTS: unsupported channels=%u\n",
                          spkChannels);
                      parseOk = false;
                      break;
                    }

                    fmtKnown = true;
                    frameBufNeed = spkChannels * sizeof(int16_t);
                    Serial.printf("Streaming WAV: %lu Hz, %u ch, PCM@%u\n",
                                  static_cast<unsigned long>(spkRate),
                                  spkChannels,
                                  static_cast<unsigned>(pcmStart));

                    if (!setupSpeakerI2S(spkRate)) {
                      Serial.println("I2S setup failed (streaming WAV)");
                      parseOk = false;
                      break;
                    }
                    digitalWrite(AMP_SD_PIN, HIGH);
                    delay(5);
                    i2s_zero_dma_buffer(I2S_NUM_0);
                    spkStarted = true;

                    for (size_t hi = pcmStart; hi < wavHdrUsed; ++hi) {
                      frameBuf[frameBufUsed++] = wavHdr[hi];
                      if (frameBufUsed >= frameBufNeed) {
                        frameBufUsed = 0;
                        int16_t s;
                        if (spkChannels == 2) {
                          const int16_t l = static_cast<int16_t>(
                              static_cast<uint16_t>(frameBuf[0]) |
                              (static_cast<uint16_t>(frameBuf[1]) << 8));
                          const int16_t r = static_cast<int16_t>(
                              static_cast<uint16_t>(frameBuf[2]) |
                              (static_cast<uint16_t>(frameBuf[3]) << 8));
                          s = static_cast<int16_t>(
                              (static_cast<int32_t>(l) + r) / 2);
                        } else {
                          s = static_cast<int16_t>(
                              static_cast<uint16_t>(frameBuf[0]) |
                              (static_cast<uint16_t>(frameBuf[1]) << 8));
                        }
                        const int16_t gs = applyPlaybackGain(s);
                        i2sOutputBuffer[i2sSamples * 2] = gs;
                        i2sOutputBuffer[i2sSamples * 2 + 1] = gs;
                        if (++i2sSamples >= MONO_SAMPLES_PER_CHUNK) {
                          writeSpeakerSamples(i2sOutputBuffer, i2sSamples);
                          i2sSamples = 0;
                        }
                      }
                    }
                    continue;
                  }

                  continue;
                }

                if (pos < pcmStart) {
                  continue;
                }

                frameBuf[frameBufUsed++] = b;
                if (frameBufUsed < frameBufNeed) {
                  continue;
                }
                frameBufUsed = 0;

                int16_t sample;
                if (spkChannels == 2) {
                  const int16_t l = static_cast<int16_t>(
                      static_cast<uint16_t>(frameBuf[0]) |
                      (static_cast<uint16_t>(frameBuf[1]) << 8));
                  const int16_t r = static_cast<int16_t>(
                      static_cast<uint16_t>(frameBuf[2]) |
                      (static_cast<uint16_t>(frameBuf[3]) << 8));
                  sample = static_cast<int16_t>(
                      (static_cast<int32_t>(l) + r) / 2);
                } else {
                  sample = static_cast<int16_t>(
                      static_cast<uint16_t>(frameBuf[0]) |
                      (static_cast<uint16_t>(frameBuf[1]) << 8));
                }

                const int16_t gained = applyPlaybackGain(sample);
                i2sOutputBuffer[i2sSamples * 2] = gained;
                i2sOutputBuffer[i2sSamples * 2 + 1] = gained;
                if (++i2sSamples >= MONO_SAMPLES_PER_CHUNK) {
                  writeSpeakerSamples(i2sOutputBuffer, i2sSamples);
                  i2sSamples = 0;
                  if (digitalRead(BUTTON_INPUT_PIN) == LOW) {
                    interrupted = true;
                  }
                }
              }
            }
          }
          break;
        }

        case ParseState::Done:
          break;
      }

      if (ps == ParseState::Done) {
        break;
      }
    }

    if (i2sSamples > 0) {
      writeSpeakerSamples(i2sOutputBuffer, i2sSamples);
      i2sSamples = 0;
    }

    client.stop();

    if (spkStarted) {
      delay(50);
      digitalWrite(AMP_SD_PIN, LOW);
    }

    if (interrupted) {
      Serial.println("Streaming TTS interrupted by button");
      succeeded = true;
      break;
    }

    if (parseOk && ps == ParseState::Done) {
      Serial.printf("Streamed TTS: %u bytes decoded, %lu Hz\n",
                    static_cast<unsigned>(totalDec),
                    static_cast<unsigned long>(spkRate));
      succeeded = true;
      break;
    }

    Serial.println("Streaming TTS: audio not fully received");
    Serial.printf("Bytes read: %u, decoded: %u\n",
                  static_cast<unsigned>(reader.bytesRead),
                  static_cast<unsigned>(totalDec));

    if (attempt < GEMINI_TTS_MAX_ATTEMPTS) {
      Serial.printf("Retrying streaming TTS, attempt %u of %u\n",
                    static_cast<unsigned>(attempt + 1),
                    static_cast<unsigned>(GEMINI_TTS_MAX_ATTEMPTS));
      delay(750 * attempt);
    }
  }

  return succeeded;
}

void processAssistantRequest() {
  audioMode = AudioMode::Thinking;
  digitalWrite(AMP_SD_PIN, LOW);
  uninstallI2S();

  if (recordedSampleCount == 0) {
    showStatus(F("No recording"));
    delay(1000);
    enterMicrophoneMode(true);
    return;
  }

  printClipStats();
  showStatus(F("Thinking"), F("Sending to Gemini"));
  startThinkingAnimation();

  bool ok = connectWifi();
  String replyText;

  if (ok) {
    replyText = geminiGenerateText();
    ok = !replyText.isEmpty();
  }

  if (ok) {
    freeRecordingBuffer();
    showStatus(F("Thinking"), F("Making speech"));
    stopThinkingAnimation();
    setNeoPixel(0, 255, 0);
    ok = geminiTtsStreamToSpeaker(replyText);
  }

  stopThinkingAnimation();

  if (!ok) {
    setNeoPixel(255, 0, 0);
    showStatus(F("Gemini failed"), F("See Serial log"));
    delay(2000);
  }

  enterMicrophoneMode(true);
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

  if (audioMode == AudioMode::Microphone) {
    processAssistantRequest();
  } else if (audioMode == AudioMode::Speaker) {
    enterMicrophoneMode(true);
  }
}

int16_t applyPlaybackGain(int16_t sample) {
  const int32_t amplifiedSample = static_cast<int32_t>(sample) * PLAYBACK_GAIN;
  return constrain(amplifiedSample, -32768, 32767);
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

void printClipStats() {
  if (recordedSampleCount == 0) {
    return;
  }

  int16_t minSample = 32767;
  int16_t maxSample = -32768;
  uint64_t levelSum = 0;

  for (size_t i = 0; i < recordedSampleCount; ++i) {
    const int16_t sample = recordingBuffer[i];
    minSample = min(minSample, sample);
    maxSample = max(maxSample, sample);
    levelSum += abs(static_cast<int32_t>(sample));
  }

  Serial.printf("Clip stats: min=%d max=%d avg_abs=%lu\n",
                minSample,
                maxSample,
                static_cast<unsigned long>(levelSum / recordedSampleCount));
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
      "Mic raw: frames=%u Lnz=%u Rnz=%u Lmin=%ld Lmax=%ld Rmin=%ld Rmax=%ld first=%08lX,%08lX,%08lX,%08lX\n",
      static_cast<unsigned>(frames),
      static_cast<unsigned>(nonZeroLeft),
      static_cast<unsigned>(nonZeroRight),
      static_cast<long>(minLeft),
      static_cast<long>(maxLeft),
      static_cast<long>(minRight),
      static_cast<long>(maxRight),
      static_cast<unsigned long>(i2sInputBuffer[0]),
      static_cast<unsigned long>(i2sInputBuffer[1]),
      static_cast<unsigned long>(i2sInputBuffer[2]),
      static_cast<unsigned long>(i2sInputBuffer[3]));
}

void recordMicrophoneChunk() {
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
    const int16_t sample =
        chooseMicSample(i2sInputBuffer[i * 2], i2sInputBuffer[i * 2 + 1]);

    if (recordedSampleCount < MAX_RECORDING_SAMPLES) {
      recordingBuffer[recordedSampleCount++] = sample;
    }
  }
}

void playSpeakerChunk() {
  if (ttsPcmHead == nullptr || playbackCursor >= ttsPcmBytes) {
    digitalWrite(AMP_SD_PIN, LOW);
    enterMicrophoneMode(true);
    return;
  }

  const size_t remainingSamples = (ttsPcmBytes - playbackCursor) / sizeof(int16_t);
  const size_t samplesThisChunk =
      min(static_cast<size_t>(MONO_SAMPLES_PER_CHUNK), remainingSamples);

  size_t samplesReady = 0;
  for (size_t i = 0; i < samplesThisChunk; ++i) {
    int16_t rawSample = 0;
    if (!readPlaybackSample(rawSample)) {
      ttsPcmBytes = playbackCursor;
      break;
    }
    const int16_t sample = applyPlaybackGain(rawSample);
    i2sOutputBuffer[i * 2] = sample;
    i2sOutputBuffer[i * 2 + 1] = sample;
    ++samplesReady;
  }

  if (samplesReady > 0) {
    writeSpeakerSamples(i2sOutputBuffer, samplesReady);
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

  if (!allocateRecordingBuffer()) {
    setNeoPixel(0, 0, 0);
    return;
  }

  if (PLAY_TEST_TONE_ON_BOOT && setupSpeakerI2S(AUDIO_SAMPLE_RATE)) {
    Serial.println("Playing boot speaker test tone");
    digitalWrite(AMP_SD_PIN, HIGH);
    delay(5);
    playTestTone(AUDIO_SAMPLE_RATE);
    digitalWrite(AMP_SD_PIN, LOW);
    delay(50);
  }

  enterMicrophoneMode(true);

  Serial.printf("Recording up to %u seconds at %lu Hz\n",
                MAX_RECORDING_SECONDS,
                static_cast<unsigned long>(AUDIO_SAMPLE_RATE));
}

void loop() {
  handleButton();

  if (audioMode == AudioMode::Microphone) {
    if (recordingBuffer == nullptr && !allocateRecordingBuffer()) {
      delay(1000);
      return;
    }
    if (!i2sReady) {
      setupMicrophoneI2S();
    }
    recordMicrophoneChunk();
  } else if (audioMode == AudioMode::Speaker) {
    if (!i2sReady) {
      enterMicrophoneMode(true);
      return;
    }
    playSpeakerChunk();
  } else {
    delay(10);
  }
}
