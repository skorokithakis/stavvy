#pragma once

// The assistant state shown on the top line of the screen. The names match the
// states the session layer will report, so the mapping is obvious.
typedef enum {
    DISPLAY_STATE_IDLE,
    DISPLAY_STATE_LISTENING,
    DISPLAY_STATE_THINKING,
    DISPLAY_STATE_SPEAKING,
    DISPLAY_STATE_ERROR,
    DISPLAY_STATE_NO_SERVER,
} display_state_t;

// Brings up the QSPI panel, LVGL and the status screen. Must run after
// board_power_init(): the AMOLED has no power until the PMIC enables ALDO3.
// The panel shares SPI2 with the SD card, so the bus is created here and left
// ready for another device to be added later.
void display_init(void);

// Every setter takes LVGL's lock, so it may be called from any task. The text
// is copied into LVGL's own buffers; the caller's string does not need to stay
// alive after the call.
void display_set_state(display_state_t state);
void display_set_transcript(const char *text);
void display_set_reply(const char *text);

// Clears both text fields and returns the state line to idle.
void display_clear(void);

// Cycles every state once, leaving the screen idle. This is a hardware check
// for the boot before the session layer exists; it blocks the caller for about
// nine seconds and should not be called by the finished firmware.
void display_run_boot_demo(void);
