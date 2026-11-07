#pragma once

// The assistant state the face screen shows. The names match the states the
// session layer reports, so the mapping is obvious.
typedef enum {
    DISPLAY_STATE_IDLE,
    DISPLAY_STATE_LISTENING,
    DISPLAY_STATE_THINKING,
    DISPLAY_STATE_SPEAKING,
    DISPLAY_STATE_ERROR,
    DISPLAY_STATE_NO_SERVER,
    // The microphone is muted. It is a distinct idle state rather than an
    // error, because it is the user's own switch and not a failure.
    DISPLAY_STATE_MUTED,
} display_state_t;

// Brings up the QSPI panel, LVGL and the face screen. Must run after
// board_power_init(): the AMOLED has no power until the PMIC enables ALDO3.
// The panel shares SPI2 with the SD card, so the bus is created here and left
// ready for another device to be added later.
void display_init(void);

// Every setter takes LVGL's lock, so it may be called from any task. The text
// is copied into LVGL's own buffers; the caller's string does not need to stay
// alive after the call. Setting the same state again is a no-op.
void display_set_state(display_state_t state);
void display_set_transcript(const char *text);
void display_set_reply(const char *text);

// Clears both text fields and returns the screen to idle.
void display_clear(void);

// The DCS 0x0A value read at boot, or -1 if the read failed. Used by the
// diagnostics heartbeat. Register reads return 0xFF on this panel, so the
// value is reported for diagnosis rather than used to drive a health check.
int display_last_power_mode(void);

// Asks the display module to re-send the panel init and redraw everything.
// Safe to call from any task: it only sets a flag, and the LVGL task performs
// the SPI work under its own lock. Calls within two seconds of a previous
// recovery are ignored.
void display_request_recovery(void);
