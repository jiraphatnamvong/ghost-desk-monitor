#include "cloud.h"
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <deque>
#include <sys/time.h>
#include "mbedtls/base64.h"
#include "config.h"
#include "secrets.h"

const char *aiName(uint8_t ai) {
  switch (ai) {
    case AI_EMPTY: return "empty";
    case AI_PERSON: return "person";
    case AI_ITEMS: return "items_only";
    default: return "none";
  }
}

// ---------- queues ----------

struct Row {
  const char *table;
  String json;
};

struct AlertMsg {
  String text;
  uint32_t queued;
};

static std::deque<Row> s_rows;        // Supabase rows waiting to go up
static std::deque<AlertMsg> s_alerts; // Discord warnings waiting to go out
static uint32_t s_alertNextTry = 0;   // the next warning is not tried before this millis() (set after a failed post)
static SemaphoreHandle_t s_mutex;
static volatile bool s_demo = false;

struct VisionJob {
  uint8_t node;
  uint8_t *jpeg;
  size_t len;
  bool manual, demo;
};
static QueueHandle_t s_visionQ;
static QueueHandle_t s_verdictQ;

// Stamp each row with the gateway's own clock (NTP, set in setup) when it is known, so a row that waited out an
// internet outage keeps the time it really happened. Before the clock is set, Supabase's now() is used.
static String stamped(const String &json) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000 || !json.startsWith("{") || json.length() < 3) return json;
  struct tm t;
  gmtime_r(&tv.tv_sec, &t);
  char ts[48];
  snprintf(ts, sizeof(ts), "{\"created_at\":\"%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ\",", t.tm_year + 1900, t.tm_mon + 1,
           t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (long)(tv.tv_usec / 1000));
  return String(ts) + json.substring(1);
}

void cloudQueueRow(const char *table, const String &json) {
  String row = stamped(json);
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (s_rows.size() >= UPLINK_BUFFER_ROWS) s_rows.pop_front();
  s_rows.push_back({table, row});
  xSemaphoreGive(s_mutex);
}

size_t cloudPendingRows() {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  size_t n = s_rows.size();
  xSemaphoreGive(s_mutex);
  return n;
}

void cloudQueueAlert(const String &text) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (s_alerts.size() >= 20) s_alerts.pop_front();
  s_alerts.push_back({text, millis()});
  xSemaphoreGive(s_mutex);
}

bool cloudDemoMode() { return s_demo; }

bool cloudAskVision(uint8_t node, uint8_t *jpeg, size_t len, bool manual, bool demo) {
  VisionJob job = {node, jpeg, len, manual, demo};
  if (xQueueSend(s_visionQ, &job, 0) != pdTRUE) {
    free(jpeg);
    return false;
  }
  return true;
}

// ---------- Supabase + Discord (one task, one TLS connection kept open for Supabase) ----------

static NetworkClientSecure s_supaTls;
static HTTPClient s_supa;  // lives for ever: its connection stays open between rows (no handshake per row)

static void supabaseHeaders(HTTPClient &http) {
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", "Bearer " SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
}

static bool postRow(const Row &row) {
  s_supa.setTimeout(8000);
  if (!s_supa.begin(s_supaTls, String(SUPABASE_URL) + "/rest/v1/" + row.table)) return false;
  supabaseHeaders(s_supa);
  s_supa.addHeader("Prefer", "return=minimal");
  int code = s_supa.POST(row.json);
  if (code != 201 && code != 200) {
    Serial.printf("[supabase] %s HTTP %d %s\n", row.table, code, s_supa.getString().substring(0, 160).c_str());
    if (code == 401 || code == 403)
      Serial.println("[supabase] check SUPABASE_URL / SUPABASE_ANON_KEY in secrets.h and that cloud/schema.sql was run");
  }
  s_supa.end();
  if (code == 200 || code == 201) return true;
  // 429 (slow down), 5xx and no answer: keep the row and try again. Any other 4xx means the row itself is wrong
  // (a column the table doesn't have, RLS): retrying will not help, so drop it.
  return code >= 400 && code < 500 && code != 429;
}

static void pollSettings() {
  s_supa.setTimeout(5000);
  if (!s_supa.begin(s_supaTls, String(SUPABASE_URL) + "/rest/v1/settings?id=eq.1&select=demo_mode")) return;
  supabaseHeaders(s_supa);
  int code = s_supa.GET();
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, s_supa.getString()) && doc.is<JsonArray>() && doc.size() > 0) {
      bool demo = doc[0]["demo_mode"] | false;
      if (demo != s_demo) Serial.printf("[settings] demo_mode -> %s\n", demo ? "on" : "off");
      s_demo = demo;
    }
  }
  s_supa.end();
}

// Discord webhook post: anyone who can read the channel sees it. Discord has no idempotency key (LINE's
// X-Line-Retry-Key had one), so if the answer is lost after Discord already posted (a timeout), the retry
// posts the same warning a second time. Rare, and a repeated warning does no harm.
enum SendResult : uint8_t { SEND_OK, SEND_DROP, SEND_RETRY };

static SendResult sendDiscord(const AlertMsg &m, uint32_t &retryMs) {
  retryMs = 3000;
  // A warning sign emoji (UTF-8) in front of the gateway's own text, so it stands out in the channel.
  String content = String("\xE2\x9A\xA0\xEF\xB8\x8F ") + m.text;
  // Discord allows 2000 characters. Cut at 2000 bytes (never more characters than that), on a UTF-8 boundary.
  if (content.length() > 2000) {
    unsigned int cut = 2000;
    while (cut > 0 && ((uint8_t)content[cut] & 0xC0) == 0x80) cut--;
    content = content.substring(0, cut);
  }
  JsonDocument doc;
  doc["content"] = content;                          // ArduinoJson escapes quotes, backslashes, control characters
  doc["allowed_mentions"]["parse"].to<JsonArray>();  // empty list: the text can never ping @everyone or anyone
  String body;
  serializeJson(doc, body);

  String url = DISCORD_WEBHOOK_URL;
  url += url.indexOf('?') < 0 ? "?wait=true" : "&wait=true";  // wait=true: Discord answers 200 once it has posted
  NetworkClientSecure tls;
  tls.useBuiltinCACertBundle();
  tls.setHandshakeTimeout(10);
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(tls, url)) return SEND_RETRY;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String();
  http.end();
  Serial.printf("[discord] webhook HTTP %d\n", code);

  if (code == 200 || code == 204) return SEND_OK;
  if (code == 429) {  // rate limited: the JSON body says how long to wait, in seconds
    JsonDocument rl;
    float wait = deserializeJson(rl, resp) ? 0.0f : (rl["retry_after"] | 0.0f);
    retryMs = constrain((uint32_t)(wait * 1000.0f) + 250, 1000UL, 60000UL);
    Serial.printf("[discord] rate limited: trying again in %lu ms\n", (unsigned long)retryMs);
    return SEND_RETRY;
  }
  if (code == 401 || code == 404) {  // wrong or deleted webhook: a retry will not fix it
    Serial.println("[discord] check DISCORD_WEBHOOK_URL in secrets.h");
    return SEND_DROP;
  }
  if (code >= 400 && code < 500) {  // the message itself is wrong: retrying will not help
    Serial.printf("[discord] %s\n", resp.substring(0, 160).c_str());
    return SEND_DROP;
  }
  return SEND_RETRY;  // no answer (negative code) or 5xx: keep it and try again
}

static void uplinkTask(void *) {
  s_supaTls.useBuiltinCACertBundle();
  s_supaTls.setHandshakeTimeout(10);
  s_supa.setReuse(true);
  uint32_t lastSettings = 0;
  for (;;) {
    if (!WiFi.isConnected()) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    bool idle = true;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool haveAlert = !s_alerts.empty();
    AlertMsg alert = haveAlert ? s_alerts.front() : AlertMsg{};
    xSemaphoreGive(s_mutex);
    if (haveAlert && millis() - alert.queued > ALERT_MAX_AGE_MS) {
      Serial.println("[discord] warning waited too long (internet down?): dropped, its times would be wrong");
      xSemaphoreTake(s_mutex, portMAX_DELAY);
      if (!s_alerts.empty()) s_alerts.pop_front();
      xSemaphoreGive(s_mutex);
      haveAlert = false;
    }
    if (haveAlert && (int32_t)(millis() - s_alertNextTry) >= 0) {
      idle = false;
      uint32_t retryMs;
      if (sendDiscord(alert, retryMs) == SEND_RETRY) {
        s_alertNextTry = millis() + retryMs;  // wait before the next try; Supabase rows keep going up meanwhile
      } else {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (!s_alerts.empty()) s_alerts.pop_front();
        xSemaphoreGive(s_mutex);
      }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool haveRow = !s_rows.empty();
    Row row = haveRow ? s_rows.front() : Row{nullptr, String()};
    xSemaphoreGive(s_mutex);
    if (haveRow) {
      idle = false;
      if (postRow(row)) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (!s_rows.empty()) s_rows.pop_front();
        xSemaphoreGive(s_mutex);
      } else {
        vTaskDelay(pdMS_TO_TICKS(3000));  // internet down: keep the row, try again later
      }
    }

    if (millis() - lastSettings >= SETTINGS_EVERY_MS) {
      lastSettings = millis();
      pollSettings();
    }
    if (idle) vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ---------- desk-photo check on the Claude API ----------

// The model must answer with exactly one of the three labels (structured output, enforced by the API).
static const char VISION_PREFIX_1[] = "{\"model\":\"" VISION_MODEL "\",\"max_tokens\":2048,\"fallbacks\":\"default\","
  "\"output_config\":{\"effort\":\"low\",\"format\":{\"type\":\"json_schema\",\"schema\":{\"type\":\"object\","
  "\"properties\":{\"desk\":{\"type\":\"string\",\"enum\":[\"empty\",\"person\",\"items_only\"]}},"
  "\"required\":[\"desk\"],\"additionalProperties\":false}}},"
  "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"source\":{\"type\":\"base64\","
  "\"media_type\":\"image/jpeg\",\"data\":\"";
static const char VISION_SUFFIX[] = "\"}},{\"type\":\"text\",\"text\":\""
  "This photo looks down at one library desk and its chair. Classify the desk. "
  "person: someone is sitting at the desk or on the chair (any part of a seated person counts). "
  "items_only: nobody is there, but personal belongings are left on the desk or chair "
  "(bag, laptop, book, notebook, pencil case, bottle, jacket, phone). "
  "empty: the desk and chair are clear. Fixed furniture, lamps, sockets and the desk sign do not count as belongings."
  "\"}]}]}";

struct VisionResult {
  uint8_t ai;
  int code;
  uint32_t ms;
};

static VisionResult classifyPhoto(const uint8_t *jpeg, size_t len) {
  VisionResult r = {AI_NONE, 0, 0};
  size_t b64Len = 0;
  mbedtls_base64_encode(nullptr, 0, &b64Len, jpeg, len);  // b64Len = size needed, including the 0 terminator
  size_t pre = sizeof(VISION_PREFIX_1) - 1, suf = sizeof(VISION_SUFFIX) - 1;
  size_t total = pre + b64Len + suf;
  uint8_t *body = (uint8_t *)ps_malloc(total);
  if (!body) return r;
  memcpy(body, VISION_PREFIX_1, pre);
  size_t written = 0;
  mbedtls_base64_encode(body + pre, b64Len, &written, jpeg, len);
  memcpy(body + pre + written, VISION_SUFFIX, suf);
  size_t bodyLen = pre + written + suf;

  NetworkClientSecure tls;
  tls.useBuiltinCACertBundle();
  tls.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(VISION_TIMEOUT_MS);
  uint8_t ai = AI_NONE;
  if (http.begin(tls, "https://api.anthropic.com/v1/messages")) {
    http.addHeader("x-api-key", ANTHROPIC_API_KEY);
    http.addHeader("anthropic-version", "2026-10-21");
    http.addHeader("anthropic-beta", "server-side-fallback-2026-07-01");
    http.addHeader("content-type", "application/json");
    uint32_t t0 = millis();
    int code = http.POST(body, bodyLen);
    String resp = http.getString();
    r.code = code;
    r.ms = millis() - t0;
    Serial.printf("[vision] %u-byte photo, HTTP %d in %lu ms\n", len, code, r.ms);
    if (code == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, resp)) {
        const char *stop = doc["stop_reason"] | "";
        if (strcmp(stop, "refusal") == 0) {
          Serial.println("[vision] request declined (refusal): using the no-AI rule");
        } else {
          for (JsonObject block : doc["content"].as<JsonArray>()) {
            if (strcmp(block["type"] | "", "text") != 0) continue;  // skip thinking blocks
            JsonDocument answer;
            if (deserializeJson(answer, block["text"].as<const char *>())) continue;
            const char *desk = answer["desk"] | "";
            if (!strcmp(desk, "empty")) ai = AI_EMPTY;
            else if (!strcmp(desk, "person")) ai = AI_PERSON;
            else if (!strcmp(desk, "items_only")) ai = AI_ITEMS;
          }
        }
      }
    } else {
      Serial.printf("[vision] %s\n", resp.substring(0, 200).c_str());
    }
    http.end();
  }
  free(body);
  r.ai = ai;
  return r;
}

static void visionTask(void *) {
  for (;;) {
    VisionJob job;
    if (xQueueReceive(s_visionQ, &job, portMAX_DELAY) != pdTRUE) continue;
    VisionResult r = WiFi.isConnected() ? classifyPhoto(job.jpeg, job.len) : VisionResult{AI_NONE, 0, 0};
    free(job.jpeg);  // the photo is never stored
    Verdict v = {job.node, r.ai};
    Serial.printf("[vision] D%02u -> %s\n", job.node, aiName(r.ai));
    xQueueSend(s_verdictQ, &v, pdMS_TO_TICKS(100));
    // one row per check, answered or not, for the AI-accuracy and latency metrics (truth is filled in by hand)
    String row = "{\"node_id\":" + String(job.node) + ",\"answer\":\"" + aiName(r.ai) + "\",\"http_code\":" + String(r.code) +
                 ",\"ms\":" + String(r.ms) + ",\"photo_bytes\":" + String(job.len) + ",\"manual\":" + (job.manual ? "true" : "false") +
                 ",\"demo\":" + (job.demo ? "true" : "false") + "}";
    cloudQueueRow("photo_checks", row);
  }
}

void cloudBegin(QueueHandle_t verdictQ) {
  s_mutex = xSemaphoreCreateMutex();
  s_visionQ = xQueueCreate(2, sizeof(VisionJob));
  s_verdictQ = verdictQ;
  xTaskCreatePinnedToCore(uplinkTask, "uplink", 10240, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(visionTask, "vision", 10240, nullptr, 1, nullptr, 0);
}
