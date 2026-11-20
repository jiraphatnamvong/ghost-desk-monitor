// Everything that leaves the gateway for the internet: Supabase rows (buffered in RAM while offline),
// the demo-mode setting, Discord warnings and the desk-photo check on the Claude API.
#pragma once
#include <Arduino.h>

enum AiClass : uint8_t { AI_NONE = 0, AI_EMPTY = 1, AI_PERSON = 2, AI_ITEMS = 3 };
const char *aiName(uint8_t ai);

struct Verdict {
  uint8_t node;
  uint8_t ai;  // AiClass
};

void cloudBegin(QueueHandle_t verdictQ, QueueHandle_t alertSentQ);  // starts the uplink and vision tasks

// Supabase: queue one row for POST /rest/v1/<table>. Never blocks; the oldest row is dropped when full.
void cloudQueueRow(const char *table, const String &json);
size_t cloudPendingRows();

// Post a warning to the team's Discord channel (webhook in secrets.h). Never blocks; sent by the uplink task,
// retried while it fails, dropped if it cannot go out within ALERT_MAX_AGE_MS.
void cloudQueueAlert(const String &text, uint8_t node = 0);

// settings.demo_mode as last read from Supabase (false until the first successful read).
bool cloudDemoMode();

// Ask the cloud whether the desk in this JPEG is empty, has a person, or has only belongings.
// Takes ownership of `jpeg` (allocated with ps_malloc). Returns false if a check is already running.
// The answer arrives later on verdictQ; every check is also logged as a photo_checks row (AI-accuracy metric).
bool cloudAskVision(uint8_t node, uint8_t *jpeg, size_t len, bool manual, bool demo);
