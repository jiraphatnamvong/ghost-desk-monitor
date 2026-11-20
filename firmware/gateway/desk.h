// One state machine per desk, the ESP-NOW link to the nodes, and the presence classifier.
#pragma once
#include <Arduino.h>
#include "protocol.h"

void desksBegin(QueueHandle_t verdictQ, QueueHandle_t alertSentQ);  // ESP-NOW + deskTask

// Called by the photo server: true if this node owes us a photo right now.
bool deskExpectingPhoto(uint8_t node);
bool deskCheckManual(uint8_t node);       // that photo was asked for with "snap", not by the desk rule
bool deskDemoOn();

// Serial commands (run from loop()).
void desksPrint();
void deskCalibrate(uint8_t node);          // store the current ToF median as this chair's "empty" distance
void deskForceCheck(uint8_t node);         // take a photo and ask the cloud now
void deskSetFace(uint8_t node, uint8_t face);
void deskSetLogLabel(const char *label);   // CSV logging for the AI dataset: "person", "empty", "nearby", or nullptr = off
void deskSetDemoOverride(int on);          // -1 = follow Supabase, 0 = off, 1 = on
