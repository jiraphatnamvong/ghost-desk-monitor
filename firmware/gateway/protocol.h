// Ghost-Desk message format, version 3 (v3 added ai_score: the node's own presence model). The same file lives in firmware/node/ and firmware/gateway/:
// edit both copies together (firmware/check-protocol.sh tells you if they drift apart).
#pragma once
#include <stdint.h>

#define GD_PROTO_VER 3
#define GD_GATES 9          // LD2410 range gates of 0.75 m
#define GD_BATCH 10         // samples per batch: 10 Hz, one batch a second

// Sign faces. The servo angle for each face is calibrated in the node's config.h.
enum : uint8_t { FACE_IN_USE = 0, FACE_AWAY = 1, FACE_FREE = 2, FACE_COUNT = 3 };

// Node -> gateway, ESP-NOW, one batch a second (sooner when the radar's OUT pin changes).
typedef struct __attribute__((packed)) {
  uint8_t  ver;          // = GD_PROTO_VER
  uint8_t  node_id;      // 1, 2, ... in every message
  uint16_t seq;          // ack / retry and packet-loss count
  uint32_t uptime_ms;    // time of the first sample
  uint8_t  sign_face;    // current sign face
  uint8_t  n;            // samples in this batch (10, fewer on an early send)
  uint8_t  ai_score;     // node's presence model over the last 5 s: 0-100 = % sure someone is seated,
                         // GD_AI_NONE = no model on the node or not 5 s of samples yet
  struct {
    uint8_t  move_e[GD_GATES];   // LD2410 moving energy per gate, 0-100
    uint8_t  still_e[GD_GATES];  // LD2410 still energy (breathing) per gate, 0-100
    uint16_t tof_mm;             // VL53L1X distance to the seat, 0 = no valid reading
  } s[GD_BATCH];
} node_batch_t;          // 211 bytes, ESP-NOW allows 250

static_assert(sizeof(node_batch_t) == 211, "node_batch_t must stay 211 bytes");

#define GD_AI_NONE 255

// Gateway -> node, ESP-NOW.
enum : uint8_t { CMD_SET_SIGN = 1, CMD_SNAP = 2 };

typedef struct __attribute__((packed)) {
  uint8_t  ver;          // = GD_PROTO_VER
  uint8_t  node_id;      // which node this is for
  uint16_t seq;
  uint8_t  cmd;          // CMD_SET_SIGN or CMD_SNAP
  uint8_t  face;         // CMD_SET_SIGN: face to show
  uint32_t gw_ip;        // CMD_SNAP: gateway IPv4 (network byte order) to POST the photo to
} gw_cmd_t;              // 10 bytes

static_assert(sizeof(gw_cmd_t) == 10, "gw_cmd_t must stay 10 bytes");

// The node POSTs the JPEG to http://<gw_ip>:GD_PHOTO_PORT/photo?node=<id>
#define GD_PHOTO_PORT 80
