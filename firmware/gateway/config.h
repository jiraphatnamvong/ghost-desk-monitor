// Gateway settings: the desk rule, timeouts and the presence baseline.
#pragma once
#include <stdint.h>

#define MAX_DESKS 8              // node_id 1..MAX_DESKS

// The desk rule, in seconds. Demo mode (settings.demo_mode in Supabase, or "demo on" on Serial)
// shortens every time so a full sit / leave / warn / release cycle fits in a 1-minute video.
struct Timing {
  uint32_t seat;      // seen this long without a break -> IN_USE
  uint32_t away;      // not seen this long -> take a photo and ask the cloud
  uint32_t warn;      // not seen this long while AWAY -> Discord warning
  uint32_t release;   // not seen this long while AWAY -> FREE
  uint32_t recheck;   // while AWAY, take another photo this often
};
static const Timing TIMING_NORMAL = {60, 2 * 60, 20 * 60, 30 * 60, 5 * 60};
static const Timing TIMING_DEMO = {5, 10, 20, 30, 10};

#define OFFLINE_AFTER_MS   10000   // no batch for this long -> desk OFFLINE (sign keeps its face)
#define CHECK_TIMEOUT_MS   15000   // photo (<= 2 s) + cloud answer (<= 10 s); no answer -> treat as AWAY
#define CMD_RETRIES        3
#define RETURN_SEEN_S      5       // AWAY -> IN_USE only after someone is seen this long (a passer-by is not a return)
#define MANUAL_FACE_HOLD_MS 60000  // after "face <node> <f>" the gateway leaves the sign alone this long

// ---------- presence ----------
// The node's own AI model sends a 0-100 score in every batch; at or above this it counts as "someone seated".
#define NODE_AI_PERSON_PCT   50
// 1 = use the node's AI score when it sends one; 0 = always use the if-else baseline below (while the node's model
// does not match its current position: retrain it with ml/record.py at the new spot, then set this back to 1).
#define USE_NODE_AI          0

// ---------- presence baseline (the if-else rule the AI model has to beat; used while a node has no model) ----------
// Window = the last 5 s of samples (50 at 10 Hz). Tune these after a calibration run.
#define WINDOW_SAMPLES       50
#define NEAR_GATES           2     // gates 0-1 = 0 to 1.5 m from the radar: this desk's chair
#define STILL_ENERGY_ON      25    // mean still energy in the near gates that counts as "someone breathing"
#define MOVE_ENERGY_ON       30    // mean moving energy in the near gates that counts as "someone moving"
#define TOF_DROP_MM          150   // seat is at least this much closer than the empty-chair distance
#define TOF_EMPTY_DEFAULT_MM 700   // used until you run "cal <node>" with the chair empty

// ---------- cloud photo check (Claude API) ----------
#define VISION_MODEL "claude-opus-5-5"   // claude-haiku-5-5 is the cheaper option if cost matters
#define VISION_TIMEOUT_MS 10000

// ---------- uplink ----------
#define UPLINK_BUFFER_ROWS 300   // rows kept in RAM while the internet is down (F2), oldest dropped first
#define ROOM_EVERY_MS      60000 // room_env row every minute
#define SCD_EVERY_MS       60000 // one SCD41 single-shot measurement a minute; the mic is ignored while it runs
#define SCD_SHOT_MS        5000  // a single shot takes 5 s (Sensirion SCD4x datasheet, measure_single_shot)
#define SETTINGS_EVERY_MS  10000 // poll settings.demo_mode (each poll is a small HTTPS request on the hotspot)
#define LINK_STATS_EVERY_MS 600000 // link_stats row per desk every 10 min (packet-delivery metric)
#define ALERT_MAX_AGE_MS   120000 // a Discord warning not sent within 2 min is dropped (its numbers would be wrong)
