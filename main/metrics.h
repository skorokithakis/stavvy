#pragma once

#include "board_power.h"

// Counters and the periodic Prometheus push. They live in one module so that
// the heartbeat has a single place to read them, and so that the tasks that
// produce them never have to know how the values are exported.
//
// The push itself is compiled out when the build had no credentials (see
// main/CMakeLists.txt), in which case every function here is a no-op.

typedef enum {
    // The session ended after a reply played, or with RUN_END, without a
    // failure before that.
    METRICS_SESSION_OUTCOME_REPLY,
    // A wake word trigger arrived while no API client was subscribed.
    METRICS_SESSION_OUTCOME_NO_SERVER,
    // A server ERROR event, or a local failure (send, microphone, playback).
    METRICS_SESSION_OUTCOME_ERROR,
    // A session deadline was reached.
    METRICS_SESSION_OUTCOME_TIMEOUT,
    // The API client disconnected in the middle of a session.
    METRICS_SESSION_OUTCOME_DISCONNECTED,
    METRICS_SESSION_OUTCOME_COUNT,
} metrics_session_outcome;

// Logs once whether the push is compiled in. Call it during boot; it never
// mentions the credentials themselves.
void metrics_init(void);

// Formats one Prometheus snapshot and POSTs it to VictoriaMetrics. It blocks
// for up to the HTTP timeout, so it belongs in the heartbeat task, and it does
// nothing at all while a voice session is running so the session keeps its
// heap, Wi-Fi and CPU.
//
// power is the heartbeat's single AXP2101 reading, or NULL when that read
// failed, in which case the three power series are left out.
void metrics_push(const board_power_status *power);

// Safe from any task: each is one atomic increment and never blocks.
void metrics_count_wake_word(void);
void metrics_count_session(metrics_session_outcome outcome);
void metrics_count_key_press(void);
void metrics_count_display_recovery(void);
