// The desk rule lives here (Appendix C of the report), one copy per node_id:
//
//   FREE   --someone seen for T.seat------------------------------------------> IN_USE
//   IN_USE --nobody for T.away--> photo -> cloud: empty      -> FREE   (released at once)
//                                                 items_only -> AWAY
//                                                 person     -> stay IN_USE (camera beats the sensors)
//                                                 no answer  -> AWAY   (system still works without AI)
//   AWAY   --someone seen---------------------------------------------------> IN_USE
//          --photo every T.recheck: empty -> FREE, person -> IN_USE
//          --nobody for T.warn: Discord warning · nobody for T.release: FREE (+ staff list if items were left)
//   OFFLINE: no batch for OFFLINE_AFTER_MS; the sign keeps its face, the timers keep running.
//
// Only deskTask touches desk state. Serial commands and verdicts reach it through queues.
#include "desk.h"
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include "config.h"
#include "cloud.h"

// Presence: each node runs its own trained model (firmware/node/presence_ai.h, E7) and sends a 0-100 score in every
// batch. When a node has no model yet (ai_score = GD_AI_NONE) the gateway falls back to the if-else baseline below.

enum State : uint8_t { FREE = 0, IN_USE = 1, AWAY = 2 };
static const char *STATE_NAME[] = {"FREE", "IN_USE", "AWAY"};
static uint8_t faceFor(State s) { return s == IN_USE ? FACE_IN_USE : (s == AWAY ? FACE_AWAY : FACE_FREE); }

struct Sample {
  uint8_t move[GD_GATES];
  uint8_t still[GD_GATES];
  uint16_t tof;
};

struct Desk {
  bool known;
  uint8_t mac[6];
  Sample win[WINDOW_SAMPLES];
  uint16_t head, count;  // ring buffer: head = next write slot
  uint32_t lastRxMs, lastUptime, rx, lost, dup, reboots;
  uint32_t rxWin, lostWin, dupWin, rebootWin;  // since the last link_stats row
  uint16_t lastSeq, cmdSeq;
  bool haveSeq, offline;

  State state;
  bool person, hasPresent, hasAbsent, warned;
  volatile bool checking;
  float score;
  uint32_t presentSince, absentSince, checkStart, lastCheck;
  uint8_t lastAi, reportedFace;
  uint32_t lastFaceCmd, holdFaceUntil;
  bool manualCheck;
  uint16_t emptyTofMm;
  uint8_t nodeAi;  // latest ai_score from the node (GD_AI_NONE = no model on the node)
};

static Desk s_desks[MAX_DESKS + 1];  // index = node_id, 0 unused

struct RxBatch {
  uint8_t mac[6];
  node_batch_t b;
};
enum ReqKind : uint8_t { REQ_CAL, REQ_CHECK, REQ_FACE };
struct Req {
  uint8_t kind, node, arg;
};

static QueueHandle_t s_rxQ, s_reqQ, s_verdictQ, s_alertSentQ;
static TaskHandle_t s_deskTask;
static volatile bool s_sendOk;
static volatile int s_demoOverride = -1;
static char s_logLabel[16] = "";
static Preferences s_prefs;

// ---------- ESP-NOW ----------

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(node_batch_t)) return;
  RxBatch r;
  memcpy(r.mac, info->src_addr, 6);
  memcpy(&r.b, data, sizeof(r.b));
  xQueueSend(s_rxQ, &r, 0);
}

static void onSent(const esp_now_send_info_t *, esp_now_send_status_t status) {
  s_sendOk = (status == ESP_NOW_SEND_SUCCESS);
  xTaskNotifyGive(s_deskTask);
}

static bool sendCmd(Desk &d, uint8_t node, uint8_t cmd, uint8_t face) {
  gw_cmd_t c = {GD_PROTO_VER, node, d.cmdSeq++, cmd, face, (uint32_t)WiFi.localIP()};
  for (int attempt = 0; attempt <= CMD_RETRIES; attempt++) {
    ulTaskNotifyTake(pdTRUE, 0);
    if (esp_now_send(d.mac, (const uint8_t *)&c, sizeof(c)) != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) && s_sendOk) return true;
  }
  Serial.printf("[espnow] D%02u cmd %u failed after %d retries\n", node, cmd, CMD_RETRIES);
  return false;
}

// ---------- logging ----------

static bool demoOn() { return s_demoOverride >= 0 ? s_demoOverride == 1 : cloudDemoMode(); }

static uint32_t secondsSince(bool running, uint32_t since) { return running ? (millis() - since) / 1000 : 0; }

static void logEvent(Desk &d, uint8_t node, const char *state, const char *prev, const char *reason) {
  String j = "{\"node_id\":" + String(node);
  j += ",\"state\":\"" + String(state) + "\"";
  j += ",\"prev_state\":\"" + String(prev) + "\"";
  j += ",\"reason\":\"" + String(reason) + "\"";
  j += ",\"absent_s\":" + String(secondsSince(d.hasAbsent, d.absentSince));
  j += ",\"ai_class\":\"" + String(aiName(d.lastAi)) + "\"";
  j += ",\"presence\":" + String(d.score, 2);
  j += ",\"demo\":" + String(demoOn() ? "true" : "false") + "}";
  cloudQueueRow("desk_events", j);
  Serial.printf("[desk] D%02u %s -> %s (%s)\n", node, prev, state, reason);
}

static String humanTime(uint32_t s) {
  if (s >= 60) return String(s / 60) + " นาที";
  return String(s) + " วินาที";
}

// ---------- presence ----------

static uint16_t medianTof(const Desk &d, uint16_t &valid) {
  static uint16_t v[WINDOW_SAMPLES];
  valid = 0;
  for (uint16_t i = 0; i < d.count; i++)
    if (d.win[i].tof > 0) v[valid++] = d.win[i].tof;
  if (!valid) return 0;
  std::sort(v, v + valid);
  return v[valid / 2];
}

// If-else baseline: someone is seated when the radar sees breathing or movement close to the chair
// AND the ToF sees something on the seat. A bag on the chair fails the radar test; a person at the
// next desk fails the ToF test. A very still person the radar misses is caught by the camera check.
static bool classifyBaseline(Desk &d) {
  if (d.count < 10) return false;
  uint32_t still = 0, move = 0;
  for (uint16_t i = 0; i < d.count; i++)
    for (uint8_t g = 0; g < NEAR_GATES; g++) {
      still += d.win[i].still[g];
      move += d.win[i].move[g];
    }
  float stillMean = (float)still / (d.count * NEAR_GATES);
  float moveMean = (float)move / (d.count * NEAR_GATES);
  bool radarOn = stillMean >= STILL_ENERGY_ON || moveMean >= MOVE_ENERGY_ON;

  uint16_t valid;
  uint16_t med = medianTof(d, valid);
  bool tofUsable = valid >= d.count / 2;
  bool tofOn = tofUsable && med + TOF_DROP_MM < d.emptyTofMm;

  float radarScore = constrain(max(stillMean / STILL_ENERGY_ON, moveMean / MOVE_ENERGY_ON), 0.0f, 2.0f) / 2.0f;
  float tofScore = tofUsable ? constrain((float)(d.emptyTofMm - min(med, d.emptyTofMm)) / (2.0f * TOF_DROP_MM), 0.0f, 1.0f) : 0.5f;
  d.score = (radarScore + tofScore) / 2.0f;
  return radarOn && (tofOn || !tofUsable);  // no ToF at all: fall back to the radar alone
}

// The node's own model when it sends a score, else the baseline. d.score feeds the presence column in Supabase.
static bool classify(Desk &d) {
  if (USE_NODE_AI && d.nodeAi <= 100) {
    d.score = d.nodeAi / 100.0f;
    return d.nodeAi >= NODE_AI_PERSON_PCT;
  }
  return classifyBaseline(d);
}

// ---------- state machine ----------

static void setState(Desk &d, uint8_t node, State s, const char *reason) {
  State prev = d.state;
  d.state = s;
  d.warned = false;
  logEvent(d, node, STATE_NAME[s], STATE_NAME[prev], reason);
  if (sendCmd(d, node, CMD_SET_SIGN, faceFor(s))) d.lastFaceCmd = millis();
}

static void applyVerdict(Desk &d, uint8_t node, uint8_t ai) {
  d.checking = false;
  d.lastCheck = millis();
  d.lastAi = ai;
  if (d.person) {  // someone is there (they came back, or a snap of a seated person): nothing to decide
    if (ai == AI_PERSON) d.hasAbsent = false;
    return;
  }
  if (d.state == IN_USE) {
    if (ai == AI_EMPTY) setState(d, node, FREE, "empty");
    else if (ai == AI_ITEMS) setState(d, node, AWAY, "items_only");
    else if (ai == AI_PERSON) {
      d.hasAbsent = false;  // someone is there after all: the absence clock starts again
      logEvent(d, node, "IN_USE", "IN_USE", "camera_sees_person");
    }
    else setState(d, node, AWAY, "no_ai");
  } else if (d.state == AWAY) {
    if (ai == AI_EMPTY) setState(d, node, FREE, "empty");
    else if (ai == AI_PERSON) {
      d.hasAbsent = false;
      setState(d, node, IN_USE, "camera_sees_person");
    }
  }
}

static void startCheck(Desk &d, uint8_t node, bool manual) {
  d.lastCheck = millis();
  d.checkStart = millis();
  d.manualCheck = manual;
  d.checking = true;  // before the command: the photo may arrive before sendCmd returns
  if (!WiFi.isConnected() || !sendCmd(d, node, CMD_SNAP, 0)) {
    applyVerdict(d, node, AI_NONE);
    return;
  }
  Serial.printf("[desk] D%02u photo requested\n", node);
}

static void step(Desk &d, uint8_t node, const Timing &T) {
  uint32_t now = millis();
  if (!d.offline && now - d.lastRxMs > OFFLINE_AFTER_MS) {
    d.offline = true;
    logEvent(d, node, "OFFLINE", STATE_NAME[d.state], "no_reports");
  }
  if (d.offline) return;

  bool p = classify(d);
  d.person = p;
  if (p) {
    d.hasAbsent = false;
    if (!d.hasPresent) d.hasPresent = true, d.presentSince = now;
  } else {
    d.hasPresent = false;
    if (!d.hasAbsent) d.hasAbsent = true, d.absentSince = now;
  }
  uint32_t presentS = secondsSince(d.hasPresent, d.presentSince);
  uint32_t absentS = secondsSince(d.hasAbsent, d.absentSince);
  uint32_t sinceCheckS = (now - d.lastCheck) / 1000;

  // Back before the answer: drop the desk rule's check. A check asked for with "snap" still runs, so the team
  // can photograph a seated person for the AI-accuracy metric (its answer then changes nothing).
  if (d.checking && p && !d.manualCheck) d.checking = false;
  if (d.checking && now - d.checkStart > CHECK_TIMEOUT_MS) {
    Serial.printf("[desk] D%02u no photo/answer in %d s\n", node, CHECK_TIMEOUT_MS / 1000);
    applyVerdict(d, node, AI_NONE);
  }

  switch (d.state) {
    case FREE:
      if (p && presentS >= T.seat) setState(d, node, IN_USE, "seated");
      break;
    case IN_USE:
      if (!p && absentS >= T.away && !d.checking && sinceCheckS >= T.away) startCheck(d, node, false);
      break;
    case AWAY:
      if (p && presentS >= min((uint32_t)RETURN_SEEN_S, T.seat)) {
        setState(d, node, IN_USE, "returned");
      } else if (absentS >= T.release) {
        setState(d, node, FREE, d.lastAi == AI_ITEMS ? "timeout_items_left" : "timeout");
      } else if (!p) {
        if (absentS >= T.warn && !d.warned) {
          d.warned = true;
          char id[8];
          snprintf(id, sizeof(id), "D%02u", node);
          cloudQueueAlert(String("โต๊ะ ") + id + ": ไม่มีคนนั่งมา " + humanTime(absentS) +
                         " แล้ว ถ้ายังไม่กลับ ระบบจะปล่อยโต๊ะให้คนอื่นในอีก " + humanTime(T.release - absentS), node);
        }
        if (!d.checking && sinceCheckS >= T.recheck) startCheck(d, node, false);
      }
      break;
  }

  // Keep the sign in step with the state: covers a lost SET_SIGN or a node that rebooted.
  uint8_t want = faceFor(d.state);
  bool held = (int32_t)(now - d.holdFaceUntil) < 0;  // a "face" command from the Serial Monitor wins for a minute
  if (!held && d.reportedFace != want && now - d.lastFaceCmd > 3000) {
    sendCmd(d, node, CMD_SET_SIGN, want);
    d.lastFaceCmd = now;
  }
}

// ---------- incoming batches ----------

static void ingest(const RxBatch &r) {
  const node_batch_t &b = r.b;
  uint8_t node = b.node_id;
  if (b.ver != GD_PROTO_VER || node < 1 || node > MAX_DESKS || b.n > GD_BATCH) return;
  Desk &d = s_desks[node];
  bool first = !d.known;
  bool rebooted = d.known && b.uptime_ms < d.lastUptime;

  if (first || memcmp(d.mac, r.mac, 6) != 0) {
    memcpy(d.mac, r.mac, 6);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, r.mac, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    if (!esp_now_is_peer_exist(r.mac)) esp_now_add_peer(&peer);
    if (!first) Serial.printf("[espnow] D%02u now comes from a different board: are two nodes set to NODE_ID %u?\n", node, node);
    d.known = true;
    Serial.printf("[espnow] D%02u is %02X:%02X:%02X:%02X:%02X:%02X\n", node, r.mac[0], r.mac[1], r.mac[2], r.mac[3],
                  r.mac[4], r.mac[5]);
  }
  // The same seq again = the node resent a batch whose ACK got lost: count it once.
  if (d.haveSeq && !rebooted && b.seq == d.lastSeq) {
    d.dup++;
    d.dupWin++;
    d.lastRxMs = millis();
    return;
  }
  if (d.haveSeq && !rebooted) {
    uint16_t gap = (uint16_t)(b.seq - d.lastSeq - 1);
    if (gap > 0 && gap < 1000) {
      d.lost += gap;
      d.lostWin += gap;
    }
  }
  d.haveSeq = true;
  d.lastSeq = b.seq;
  d.rx++;
  d.rxWin++;
  d.lastRxMs = millis();
  d.lastUptime = b.uptime_ms;
  d.reportedFace = b.sign_face;
  d.nodeAi = b.ai_score;
  if (d.offline) {
    d.offline = false;
    d.count = d.head = 0;  // old samples say nothing about now
    logEvent(d, node, STATE_NAME[d.state], "OFFLINE", "back_online");
  }

  for (uint8_t i = 0; i < b.n; i++) {
    Sample &s = d.win[d.head];
    memcpy(s.move, b.s[i].move_e, GD_GATES);
    memcpy(s.still, b.s[i].still_e, GD_GATES);
    s.tof = b.s[i].tof_mm;
    d.head = (d.head + 1) % WINDOW_SAMPLES;
    if (d.count < WINDOW_SAMPLES) d.count++;

    if (s_logLabel[0]) {  // CSV training data for ml/train.py (the last column is the node's own score)
      Serial.printf("%s,%u,%lu", s_logLabel, node, b.uptime_ms + i * 100UL);
      for (uint8_t g = 0; g < GD_GATES; g++) Serial.printf(",%u", s.move[g]);
      for (uint8_t g = 0; g < GD_GATES; g++) Serial.printf(",%u", s.still[g]);
      Serial.printf(",%u,%u\n", s.tof, b.ai_score);
    }
  }

  // First contact after the gateway (re)started: trust the face the sign already shows, so a gateway restart
  // doesn't flip every occupied desk to FREE. The timers start again from now.
  if (first) d.state = b.sign_face == FACE_IN_USE ? IN_USE : (b.sign_face == FACE_AWAY ? AWAY : FREE);
  if (rebooted) {
    d.reboots++;
    d.rebootWin++;
    logEvent(d, node, STATE_NAME[d.state], STATE_NAME[d.state], "node_rebooted");
  }
  // Handshake: the node learns our MAC from the first command and switches to unicast.
  if (first || rebooted) {
    if (sendCmd(d, node, CMD_SET_SIGN, faceFor(d.state))) d.lastFaceCmd = millis();
    if (first) logEvent(d, node, STATE_NAME[d.state], STATE_NAME[d.state], "node_joined");
  }
}

static void handleReq(const Req &q) {
  if (q.node < 1 || q.node > MAX_DESKS || !s_desks[q.node].known) {
    Serial.printf("[cmd] D%02u has not reported yet\n", q.node);
    return;
  }
  Desk &d = s_desks[q.node];
  if (q.kind == REQ_CAL) {
    uint16_t valid;
    uint16_t med = medianTof(d, valid);
    if (valid < 10) {
      Serial.println("[cal] not enough valid ToF readings: is the sensor aimed at the seat?");
      return;
    }
    d.emptyTofMm = med;
    char key[8];
    snprintf(key, sizeof(key), "tof%u", q.node);
    s_prefs.putUShort(key, med);
    Serial.printf("[cal] D%02u empty chair = %u mm (saved)\n", q.node, med);
  } else if (q.kind == REQ_CHECK) {
    startCheck(d, q.node, true);
  } else if (q.kind == REQ_FACE) {
    d.holdFaceUntil = millis() + MANUAL_FACE_HOLD_MS;
    sendCmd(d, q.node, CMD_SET_SIGN, q.arg);
    Serial.printf("[cmd] D%02u sign to face %u; the desk rule takes the sign back in %d s\n", q.node, q.arg, MANUAL_FACE_HOLD_MS / 1000);
  }
}

// Packet delivery for the report: what arrived, what went missing, repeats and node restarts per 10 min.
static void postLinkStats() {
  for (uint8_t n = 1; n <= MAX_DESKS; n++) {
    Desk &d = s_desks[n];
    if (!d.known) continue;
    String j = "{\"node_id\":" + String(n) + ",\"rx\":" + String(d.rxWin) + ",\"lost\":" + String(d.lostWin) +
               ",\"dup\":" + String(d.dupWin) + ",\"reboots\":" + String(d.rebootWin) + ",\"offline\":" + (d.offline ? "true" : "false") + "}";
    cloudQueueRow("link_stats", j);
    d.rxWin = d.lostWin = d.dupWin = d.rebootWin = 0;
  }
}

static void deskTask(void *) {
  uint32_t lastStep = 0, lastStats = millis();
  for (;;) {
    RxBatch r;
    if (xQueueReceive(s_rxQ, &r, pdMS_TO_TICKS(50)) == pdTRUE) ingest(r);
    Verdict v;
    while (xQueueReceive(s_verdictQ, &v, 0) == pdTRUE)
      if (v.node >= 1 && v.node <= MAX_DESKS && s_desks[v.node].checking) applyVerdict(s_desks[v.node], v.node, v.ai);
    uint8_t alertNode;
    while (xQueueReceive(s_alertSentQ, &alertNode, 0) == pdTRUE)
      if (alertNode >= 1 && alertNode <= MAX_DESKS && s_desks[alertNode].known) {
        Desk &d = s_desks[alertNode];
        const char *state = d.offline ? "OFFLINE" : STATE_NAME[d.state];
        logEvent(d, alertNode, state, state, "warning_sent");
      }
    Req q;
    while (xQueueReceive(s_reqQ, &q, 0) == pdTRUE) handleReq(q);

    if (millis() - lastStep >= 1000) {
      lastStep = millis();
      const Timing &T = demoOn() ? TIMING_DEMO : TIMING_NORMAL;
      for (uint8_t n = 1; n <= MAX_DESKS; n++)
        if (s_desks[n].known) step(s_desks[n], n, T);
    }
    if (millis() - lastStats >= LINK_STATS_EVERY_MS) {
      lastStats = millis();
      postLinkStats();
    }
  }
}

// ---------- public ----------

void desksBegin(QueueHandle_t verdictQ, QueueHandle_t alertSentQ) {
  s_verdictQ = verdictQ;
  s_alertSentQ = alertSentQ;
  s_rxQ = xQueueCreate(8, sizeof(RxBatch));
  s_reqQ = xQueueCreate(4, sizeof(Req));
  s_prefs.begin("ghostdesk", false);
  for (uint8_t n = 1; n <= MAX_DESKS; n++) {
    char key[8];
    snprintf(key, sizeof(key), "tof%u", n);
    s_desks[n].emptyTofMm = s_prefs.getUShort(key, TOF_EMPTY_DEFAULT_MM);
    s_desks[n].state = FREE;
    s_desks[n].reportedFace = FACE_FREE;
  }
  Serial.println(USE_NODE_AI ? "[desk] presence: the node's own AI model when it sends a score, else the if-else baseline" : "[desk] presence: the if-else baseline (USE_NODE_AI is 0)");

  if (esp_now_init() != ESP_OK) {
    Serial.println("[espnow] init failed, restarting");
    delay(1000);
    ESP.restart();
  }
  xTaskCreatePinnedToCore(deskTask, "desk", 8192, nullptr, 3, &s_deskTask, 1);
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
}

bool deskExpectingPhoto(uint8_t node) { return node >= 1 && node <= MAX_DESKS && s_desks[node].checking; }
bool deskCheckManual(uint8_t node) { return node >= 1 && node <= MAX_DESKS && s_desks[node].manualCheck; }
bool deskDemoOn() { return demoOn(); }

void desksPrint() {
  Serial.printf("demo=%s pending_rows=%u wifi=%s ch=%d ip=%s heap_internal=%u psram=%u\n", demoOn() ? "on" : "off", cloudPendingRows(),
                WiFi.isConnected() ? "up" : "DOWN", WiFi.channel(), WiFi.localIP().toString().c_str(),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  for (uint8_t n = 1; n <= MAX_DESKS; n++) {
    const Desk &d = s_desks[n];
    if (!d.known) continue;
    Serial.printf("D%02u %-6s%s person=%d score=%.2f (%s) present=%lus absent=%lus face=%u ai=%s tof_empty=%umm rx=%lu lost=%lu dup=%lu reboots=%lu%s\n",
                  n, STATE_NAME[d.state], d.offline ? " OFFLINE" : "", d.person, d.score,
                  (USE_NODE_AI && d.nodeAi <= 100) ? "node AI" : "baseline",
                  secondsSince(d.hasPresent, d.presentSince), secondsSince(d.hasAbsent, d.absentSince), d.reportedFace,
                  aiName(d.lastAi), d.emptyTofMm, d.rx, d.lost, d.dup, d.reboots, d.checking ? " (checking)" : "");
  }
}

static void request(uint8_t kind, uint8_t node, uint8_t arg) {
  Req q = {kind, node, arg};
  xQueueSend(s_reqQ, &q, pdMS_TO_TICKS(100));
}

void deskCalibrate(uint8_t node) { request(REQ_CAL, node, 0); }
void deskForceCheck(uint8_t node) { request(REQ_CHECK, node, 0); }
void deskSetFace(uint8_t node, uint8_t face) { request(REQ_FACE, node, face); }
void deskSetDemoOverride(int on) { s_demoOverride = on; }

void deskSetLogLabel(const char *label) {
  if (!label) {
    s_logLabel[0] = 0;
    return;
  }
  strncpy(s_logLabel, label, sizeof(s_logLabel) - 1);
  s_logLabel[sizeof(s_logLabel) - 1] = 0;
  Serial.print("label,node,t_ms");
  for (int g = 0; g < GD_GATES; g++) Serial.printf(",move%d", g);
  for (int g = 0; g < GD_GATES; g++) Serial.printf(",still%d", g);
  Serial.println(",tof_mm,ai");
}
