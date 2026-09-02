#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <math.h>
#include <pgmspace.h>
#include "kick.h"
#include "snare_on.h"
#include "snare_off.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define KICK_PIN 34
#define SNARE_PIN 35
#define SNARE_MODE_PIN 32
#define THRESHOLD 100
#define DEBOUNCE_TIME 200
#define VOICES_PER_DRUM 4

#define I2S_BCLK_PIN 4
#define I2S_LRC_PIN 15
#define I2S_DOUT_PIN 2

#define SAMPLE_RATE 22050
#define SAMPLE_BYTES_PER_FRAME 2

// 1. WIFI ACCESS POINT SETTINGS
const char* ssid = "ALEKTRONIC DRUM";
const char* password = "drum1234";
IPAddress staticIP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

struct Sample {
  uint8_t* data;
  size_t size;
  volatile size_t pos;
  volatile bool playing;
  volatile bool started;
  uint8_t volume; // 0-100
};

Sample kickVoices[VOICES_PER_DRUM];
Sample snareOnVoices[VOICES_PER_DRUM];
Sample snareOffVoices[VOICES_PER_DRUM];
uint8_t kickVolume = 80;
uint8_t snareVolume = 80;
bool snareModeOn = true;

unsigned long lastKickHit = 0;
unsigned long lastSnareHit = 0;

// FIX: Static buffer so it doesn't crash
static int16_t mixBuffer[256];

void playSine(int freq, int duration_ms, int vol_percent);

void initVoicePool(Sample* voices, int count, uint8_t volume) {
  for (int i = 0; i < count; ++i) {
    voices[i].data = NULL;
    voices[i].size = 0;
    voices[i].pos = 0;
    voices[i].playing = false;
    voices[i].started = false;
    voices[i].volume = volume;
  }
}

Sample* getActiveSnareVoicePool() {
  return snareModeOn ? snareOnVoices : snareOffVoices;
}

Sample* getFreeVoice(Sample* voices, int count) {
  for (int i = 0; i < count; ++i) {
    if (!voices[i].playing) {
      return &voices[i];
    }
  }
  return &voices[0];
}

void triggerVoicePool(Sample* voices, int count) {
  const char* poolName = "unknown";
  if (voices == kickVoices) poolName = "kick";
  else if (voices == snareOnVoices) poolName = "snare_on";
  else if (voices == snareOffVoices) poolName = "snare_off";
  Serial.printf("triggerVoicePool: selecting pool %s\n", poolName);

  Sample* slot = getFreeVoice(voices, count);
  slot->pos = 0;
  slot->playing = true;
  slot->started = true;
}

void initPCM5102A() {
  i2s_config_t i2s_config = {
    (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    SAMPLE_RATE,
    I2S_BITS_PER_SAMPLE_16BIT,
    I2S_CHANNEL_FMT_RIGHT_LEFT,
    I2S_COMM_FORMAT_STAND_I2S,
    ESP_INTR_FLAG_LEVEL1,
    4,
    256,
    false,
    true,
    0,
    I2S_MCLK_MULTIPLE_DEFAULT,
    I2S_BITS_PER_CHAN_DEFAULT
  };

  i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRC_PIN,
    .data_out_num = I2S_DOUT_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
}

void writeToPCM5102A(const int16_t* samples, size_t count) {
  size_t written = 0;
  if (count == 0) {
    return;
  }
  // PCM5102A expects stereo frames; duplicate mono samples into L/R interleaved
  if (count > 256) count = 256; // safety cap for our static buffer sizes
  static int16_t stereoBuf[512];
  for (size_t i = 0; i < count; ++i) {
    stereoBuf[i*2] = samples[i];
    stereoBuf[i*2 + 1] = samples[i];
  }
  i2s_write(I2S_NUM_0, stereoBuf, count * 2 * sizeof(int16_t), &written, portMAX_DELAY);
}

void sendSerial(String msg) {
  DynamicJsonDocument doc(512);
  doc["type"] = "serial";
  doc["data"] = msg;
  String output;
  serializeJson(doc, output);
  webSocket.broadcastTXT(output);
}

void pollSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 't') {
      Serial.println("Serial tone test (440 Hz, 500 ms)");
      playSine(440, 500, 80);
    } else if (c == 'T') {
      Serial.println("Serial long tone test (440 Hz, 1500 ms)");
      playSine(440, 1500, 80);
    }
  }
}

void sendDrumList() {
  DynamicJsonDocument doc(512);
  doc["type"] = "drumlist";
  JsonArray arr = doc.createNestedArray("data");

  JsonObject k = arr.createNestedObject();
  k["name"] = "kick"; k["vol"] = kickVolume;
  JsonObject s = arr.createNestedObject();
  s["name"] = "snare"; s["vol"] = snareVolume;

  String output;
  serializeJson(doc, output);
  webSocket.broadcastTXT(output);
}

bool loadSamplePool(const uint8_t* data, size_t size, Sample* voices, int count, uint8_t volume) {
  if (!data || size == 0) {
    return false;
  }

  // Allocate a single shared buffer for the sample data to save RAM
  uint8_t* shared = (uint8_t*) malloc(size);
  if (!shared) {
    Serial.printf("malloc failed for sample pool shared buffer (%u bytes)\n", (unsigned)size);
    return false;
  }
  memcpy_P(shared, data, size);

  for (int i = 0; i < count; ++i) {
    voices[i].data = shared;
    voices[i].size = size;
    voices[i].pos = 0;
    voices[i].playing = false;
    voices[i].started = false;
    voices[i].volume = volume;
  }

  Serial.printf("Loaded sample pool %u bytes, %d voices (shared)\n", (unsigned)size, count);
  return true;
}

void mixVoiceIntoBuffer(Sample* sp) {
  if (!sp || !sp->playing || sp->data == NULL) {
    return;
  }

  if (sp->started) {
    Serial.printf("Audio start: %s\n", (sp >= kickVoices && sp < kickVoices + VOICES_PER_DRUM) ? "kick" : "snare");
    sp->started = false;
  }

  for (int i = 0; i < 256; ++i) {
    size_t sampleBytePos = sp->pos + (size_t)i * SAMPLE_BYTES_PER_FRAME;
    if (sampleBytePos + SAMPLE_BYTES_PER_FRAME <= sp->size) {
      int16_t sampleVal = (int16_t)(
        ((uint16_t)sp->data[sampleBytePos]) |
        ((uint16_t)sp->data[sampleBytePos + 1] << 8)
      );
      int32_t scaled = (sampleVal * sp->volume) / 100;
      int32_t mixed = (int32_t)mixBuffer[i] + scaled;
      mixBuffer[i] = (mixed > 32767) ? 32767 : ((mixed < -32768) ? -32768 : mixed);
    } else {
      sp->playing = false;
      sp->pos = 0;
      break;
    }
  }

  if (sp->playing) {
    sp->pos += 256 * SAMPLE_BYTES_PER_FRAME;
  }
}

void audioTask(void *param) {
  while (1) {
    memset(mixBuffer, 0, sizeof(mixBuffer));

    for (int i = 0; i < VOICES_PER_DRUM; ++i) {
      mixVoiceIntoBuffer(&kickVoices[i]);
    }

    Sample* activeSnare = getActiveSnareVoicePool();
    for (int i = 0; i < VOICES_PER_DRUM; ++i) {
      mixVoiceIntoBuffer(&activeSnare[i]);
    }

    writeToPCM5102A(mixBuffer, 256);
    vTaskDelay(1);
  }
}
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if(type == WStype_CONNECTED) {
    sendDrumList();
  }
  if(type == WStype_TEXT) {
    DynamicJsonDocument doc(256);
    deserializeJson(doc, payload);
    String t = doc["type"];
    if(t == "volume") {
      String name = doc["name"];
      int vol = doc["value"];
      if(name == "kick") {
        kickVolume = vol;
        for (int i = 0; i < VOICES_PER_DRUM; ++i) {
          kickVoices[i].volume = kickVolume;
        }
      }
      if(name == "snare") {
        snareVolume = vol;
        for (int i = 0; i < VOICES_PER_DRUM; ++i) {
          snareOnVoices[i].volume = snareVolume;
          snareOffVoices[i].volume = snareVolume;
        }
      }
    }
    if(t == "test") {
      String name = doc["name"];
      if(name == "kick") triggerVoicePool(kickVoices, VOICES_PER_DRUM);
      if(name == "snare") triggerVoicePool(getActiveSnareVoicePool(), VOICES_PER_DRUM);
    }
    if(t == "snare_mode") {
      snareModeOn = doc["value"] | false;
      Serial.printf("webSocket: snare_mode set to %s\n", snareModeOn ? "ON" : "OFF");
    }
    if(t == "tone") {
      int freq = doc["freq"] | 440;
      int dur = doc["dur"] | 500;
      int vol = doc["vol"] | 80;
      extern void playSine(int freq, int duration_ms, int vol_percent);
      playSine(freq, dur, vol);
    }
  }
}

// Simple blocking sine-wave test generator (writes directly to built-in DAC)
void playSine(int freq, int duration_ms, int vol_percent) {
  const int bufSamples = 256;
  static int16_t toneBuf[bufSamples];
  int sr = SAMPLE_RATE;
  int totalSamples = (duration_ms * sr) / 1000;
  if (totalSamples < bufSamples) totalSamples = bufSamples;
  int chunks = (totalSamples + bufSamples - 1) / bufSamples;
  for (int c = 0; c < chunks; c++) {
    for (int i = 0; i < bufSamples; i++) {
      int idx = c * bufSamples + i;
      double t = (double)idx / (double)sr;
      double s = sin(2.0 * M_PI * (double)freq * t);
      int32_t v = (int32_t)(s * 32767.0 * ((double)vol_percent / 100.0));
      toneBuf[i] = (int16_t)v;
    }
    writeToPCM5102A(toneBuf, bufSamples);
  }
}
void handleRoot() {
  File file = LittleFS.open("/index.html", "r");
  if (file) {
    Serial.printf("handleRoot: serving /index.html, size=%u\n", (unsigned)file.size());
    server.streamFile(file, "text/html");
    file.close();
  } else {
    Serial.println("handleRoot: /index.html not found in LittleFS");
    server.send(404, "text/plain", "404: File not found");
  }
}
void setup() {
  Serial.begin(115200);
  delay(1000);

  if(!LittleFS.begin(false)){
    Serial.println("LittleFS Mount Failed");
    while(1) delay(1000);
  }

  // List LittleFS files for debugging
  Serial.println("Listing LittleFS root:");
  File root = LittleFS.open("/");
  if (root) {
    File file = root.openNextFile();
    while (file) {
      Serial.printf("  %s  (%u bytes)\n", file.name(), (unsigned)file.size());
      file = root.openNextFile();
    }
  } else {
    Serial.println("  (failed to open root)");
  }

  // WIFI ACCESS POINT WITH STATIC IP
  WiFi.mode(WIFI_AP);
  if(!WiFi.softAPConfig(staticIP, gateway, subnet)) {
    Serial.println("Failed to configure AP static IP");
  }
  if(!WiFi.softAP(ssid, password)) {
    Serial.println("Failed to start AP");
    while(1) delay(1000);
  }
  Serial.println("AP started! SSID: " + String(ssid) + " IP: " + WiFi.softAPIP().toString());

  server.on("/", handleRoot);
  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  initPCM5102A();

  analogSetPinAttenuation(KICK_PIN, ADC_11db);
  analogSetPinAttenuation(SNARE_PIN, ADC_11db);
  pinMode(SNARE_MODE_PIN, INPUT_PULLUP);

  initVoicePool(kickVoices, VOICES_PER_DRUM, kickVolume);
  initVoicePool(snareOnVoices, VOICES_PER_DRUM, snareVolume);
  initVoicePool(snareOffVoices, VOICES_PER_DRUM, snareVolume);

  loadSamplePool(kick, sizeof(kick), kickVoices, VOICES_PER_DRUM, kickVolume);
  loadSamplePool(snare_on, sizeof(snare_on), snareOnVoices, VOICES_PER_DRUM, snareVolume);
  loadSamplePool(snare_off, sizeof(snare_off), snareOffVoices, VOICES_PER_DRUM, snareVolume);

  xTaskCreatePinnedToCore(audioTask, "AudioTask", 8192, NULL, 2, NULL, 0); // 8KB stack
  Serial.println("ALEKTRONIC DRUM Ready");
  Serial.println("Serial commands: t = short tone, T = long tone");
}
void loop() {
  pollSerialCommands();
  server.handleClient();
  webSocket.loop();

  unsigned long now = millis();
  snareModeOn = (digitalRead(SNARE_MODE_PIN) == LOW);

  if (analogRead(KICK_PIN) > THRESHOLD && now - lastKickHit > DEBOUNCE_TIME) {
    lastKickHit = now;
    triggerVoicePool(kickVoices, VOICES_PER_DRUM);
  }
  if (analogRead(SNARE_PIN) > THRESHOLD && now - lastSnareHit > DEBOUNCE_TIME) {
    lastSnareHit = now;
    triggerVoicePool(getActiveSnareVoicePool(), VOICES_PER_DRUM);
  }
}