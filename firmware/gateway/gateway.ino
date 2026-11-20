// Ghost-Desk gateway: Waveshare ESP32-S3-DEV-KIT-N8R8.
//
// Owns every decision: receives node batches over ESP-NOW, runs presence + the desk rule per desk
// (desk.cpp), asks the Claude API about a desk photo when a desk has been empty for 2 minutes,
// reads the room sensors (room.cpp), uploads to Supabase with a RAM buffer while offline and sends
// Discord warnings (cloud.cpp).
//
// Tasks: deskTask (3) · roomTask (2) · uplinkTask (1) · visionTask (1) · photoTask (1) · loop = Serial commands
//
// Upload: python firmware/flash.py gateway (board settings come from sketch.yaml), or in Arduino IDE pick board
// "ESP32S3 Dev Module", Flash Size "8MB", PSRAM "OPI PSRAM", USB CDC On Boot "Enabled",
// Partition "8M with spiffs (3MB APP/1.5MB SPIFFS)". Library: ArduinoJson. Build guide: docs/guide/.
//
// Serial Monitor (115200, newline): help · state · demo on|off|auto · log person|empty|nearby|off
//                                   cal <node> · snap <node> · face <node> <0|1|2>

#include <WiFi.h>
#include <ArduinoOTA.h>
#include "protocol.h"
#include "pins.h"
#include "config.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy secrets.h.example to secrets.h and fill in Wi-Fi, Supabase, Discord and Anthropic keys"
#endif
#include "cloud.h"
#include "desk.h"
#include "room.h"

static QueueHandle_t g_verdictQ;

// ---------- updates over Wi-Fi (E6) ----------
// On only when secrets.h defines OTA_PASSWORD. Then Arduino IDE -> Tools -> Port lists this board as a network
// port; pick it, upload as usual and type the password. No USB cable, so a node closed in its enclosure still updates.
static bool g_ota = false;

static void otaBegin(const char *host) {
#ifdef OTA_PASSWORD
  ArduinoOTA.setHostname(host);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("[ota] update starting: don't unplug"); });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] update done, restarting"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] update failed, error %u (wrong password?)\n", e); });
  ArduinoOTA.begin();
  g_ota = true;
  Serial.printf("[ota] ready: Arduino IDE -> Tools -> Port -> %s (network port)\n", host);
#else
  Serial.printf("[ota] off (%s): add #define OTA_PASSWORD \"...\" to secrets.h to update over Wi-Fi\n", host);
#endif
}

// ---------- photo server: the node POSTs one JPEG to /photo?node=N when asked ----------
// A tiny raw HTTP reader: the body is binary, so it goes straight into PSRAM without String copies.

static void reply(NetworkClient &c, int code, const char *text) {
  c.printf("HTTP/1.1 %d %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s", code, text, strlen(text), text);
}

static void handlePhoto(NetworkClient &c) {
  String request = c.readStringUntil('\n');  // POST /photo?node=1 HTTP/1.1
  size_t len = 0;
  for (;;) {
    String h = c.readStringUntil('\n');
    h.trim();
    if (h.isEmpty()) break;
    if (h.substring(0, 15).equalsIgnoreCase("content-length:")) len = h.substring(15).toInt();
  }
  int q = request.indexOf("/photo?node=");
  uint8_t node = q >= 0 ? request.substring(q + 12).toInt() : 0;
  if (!request.startsWith("POST") || q < 0 || len == 0 || len > 300000) {
    reply(c, 400, "bad request");
    return;
  }
  if (!deskExpectingPhoto(node)) {
    reply(c, 409, "no photo requested");
    return;
  }
  uint8_t *buf = (uint8_t *)ps_malloc(len);
  if (!buf) {
    reply(c, 500, "no memory");
    return;
  }
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < len && c.connected() && millis() - t0 < 4000) {
    int n = c.read(buf + got, len - got);
    if (n > 0) got += n;
    else vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (got != len || buf[0] != 0xFF || buf[1] != 0xD8) {  // JPEG starts with FF D8
    free(buf);
    reply(c, 400, "incomplete jpeg");
    return;
  }
  reply(c, 200, "ok");
  Serial.printf("[photo] D%02u %u bytes\n", node, len);
  if (!cloudAskVision(node, buf, len, deskCheckManual(node), deskDemoOn())) Serial.println("[photo] cloud check busy, dropped");
}

static void photoTask(void *) {
  NetworkServer server(GD_PHOTO_PORT);
  server.begin();
  for (;;) {
    NetworkClient c = server.accept();
    if (!c) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    handlePhoto(c);
    c.stop();
  }
}

// ---------- Serial commands ----------

static void handleCommand(String line) {
  line.trim();
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  arg.trim();

  if (cmd == "state") desksPrint();
  else if (cmd == "demo") {
    deskSetDemoOverride(arg == "on" ? 1 : arg == "off" ? 0 : -1);
    Serial.printf("[cmd] demo %s\n", arg == "on" || arg == "off" ? arg.c_str() : "follows Supabase settings");
  } else if (cmd == "log") {
    deskSetLogLabel(arg == "off" || arg.isEmpty() ? nullptr : arg.c_str());
  } else if (cmd == "cal") deskCalibrate(arg.toInt());
  else if (cmd == "snap") deskForceCheck(arg.toInt());
  else if (cmd == "discord") {  // setup check: one test post to the webhook, result printed as [discord] webhook HTTP ...
    cloudQueueAlert("Test from the Ghost-Desk gateway: desk warnings will appear in this channel.");
    Serial.println("[cmd] Discord test queued");
  }
  else if (cmd == "face") {
    int sp2 = arg.indexOf(' ');
    if (sp2 > 0) deskSetFace(arg.substring(0, sp2).toInt(), arg.substring(sp2 + 1).toInt());
  } else {
    Serial.println("commands: state | demo on|off|auto | log person|empty|nearby|off | cal <node> | snap <node> | face <node> <0 in use|1 away|2 free> | discord");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("\n[boot] Ghost-Desk gateway, reset reason %d, PSRAM %u bytes\n", esp_reset_reason(), ESP.getPsramSize());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep drops ESP-NOW frames
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[wifi] joining %s", WIFI_SSID);
  for (int i = 0; i < 40 && !WiFi.isConnected(); i++) {
    delay(500);
    Serial.print('.');
  }
  Serial.println(WiFi.isConnected() ? " ok" : " FAILED (will keep retrying; ESP-NOW needs the hotspot's channel)");
  Serial.printf("[wifi] gateway MAC %s ip=%s channel=%d\n", WiFi.macAddress().c_str(), WiFi.localIP().toString().c_str(),
                WiFi.channel());
  configTime(0, 0, "pool.ntp.org", "time.google.com");  // UTC; rows carry this time once it is set (a few seconds)

  g_verdictQ = xQueueCreate(4, sizeof(Verdict));
  cloudBegin(g_verdictQ);
  roomBegin();
  desksBegin(g_verdictQ);
  xTaskCreatePinnedToCore(photoTask, "photo", 6144, nullptr, 1, nullptr, 0);
  bool brownout = esp_reset_reason() == ESP_RST_BROWNOUT;
  otaBegin("ghost-desk-gateway");
  cloudQueueRow("desk_events", String("{\"node_id\":0,\"state\":\"BOOT\",\"prev_state\":\"BOOT\",\"reason\":\"") +
                                    (brownout ? "gateway_boot_brownout" : "gateway_boot") + "\"}");
  Serial.println("[boot] ready. Type help in the Serial Monitor.");
}

void loop() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) handleCommand(line);
      line = "";
    } else if (line.length() < 80) {
      line += c;
    }
  }
  if (g_ota) ArduinoOTA.handle();
  delay(20);
}
