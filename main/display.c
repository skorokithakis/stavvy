#include "display.h"

#include <stdatomic.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "faces.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "display";

// Verified against the schematic and AGENTS.md: Waveshare's user_config.h
// swaps the display chip select with the touch interrupt, so their GPIO5 is
// wrong and the panel is actually selected by GPIO15.
#define DISPLAY_SPI_HOST SPI2_HOST
#define DISPLAY_CLOCK_PIN 0
#define DISPLAY_DATA0_PIN 1
#define DISPLAY_DATA1_PIN 2
#define DISPLAY_DATA2_PIN 3
#define DISPLAY_DATA3_PIN 4
#define DISPLAY_CHIP_SELECT_PIN 15

#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 480

// One full frame is about 460 KB, which does not fit in the C6's 512 KB of
// SRAM alongside Wi-Fi and the audio buffers. LVGL therefore renders into a
// partial buffer of 40 lines (38.4 KB), which is large enough that scrolling
// text does not flush a hundred tiny strips.
#define DISPLAY_BUFFER_LINES 40
#define DISPLAY_BUFFER_PIXELS (DISPLAY_WIDTH * DISPLAY_BUFFER_LINES)

// 40 MHz is what Waveshare's working ESP-IDF example uses for this panel. The
// bus is shared with the SD card later, so the transfer size covers the whole
// draw buffer and the SD driver can add its own device without changing it.
#define DISPLAY_SPI_CLOCK_HZ (40 * 1000 * 1000)
#define DISPLAY_MAXIMUM_TRANSFER_BYTES (DISPLAY_BUFFER_PIXELS * sizeof(uint16_t))

// The panel is an AMOLED with no backlight pin, so brightness is a command
// rather than a PWM duty cycle. 60% is readable indoors without burning power
// or shortening the panel's life like full brightness would.
#define DISPLAY_BRIGHTNESS_FULL_PERCENT 60
// An idle face is mostly there to be glanced at, so a dim panel still shows the
// expression while using less current. The jump back to 60% is also the
// clearest "I am listening now" cue the screen has.
#define DISPLAY_BRIGHTNESS_DIM_PERCENT 25
// How long an idle or muted screen stays at full brightness before dimming.
#define DISPLAY_DIM_AFTER_MILLISECONDS (30 * 1000)

// A blink is one short frame every few seconds. 150 ms is long enough to see
// and short enough that it reads as a blink rather than a second expression.
#define DISPLAY_BLINK_DURATION_MILLISECONDS 150
#define DISPLAY_BLINK_MINIMUM_INTERVAL_MILLISECONDS (3 * 1000)
#define DISPLAY_BLINK_MAXIMUM_INTERVAL_MILLISECONDS (7 * 1000)

// A variety moment is another idle expression held for a few seconds, every
// few minutes, so the screen does not look frozen without becoming a
// distraction.
#define DISPLAY_VARIETY_MINIMUM_INTERVAL_MILLISECONDS (3 * 60 * 1000)
#define DISPLAY_VARIETY_MAXIMUM_INTERVAL_MILLISECONDS (6 * 60 * 1000)
#define DISPLAY_VARIETY_MINIMUM_DURATION_MILLISECONDS (3 * 1000)
#define DISPLAY_VARIETY_MAXIMUM_DURATION_MILLISECONDS (5 * 1000)

// Drift moves the face a few pixels every few minutes, so it is not pinned to
// the same pixel for hours. 20 px is about 4% of the screen: enough to feel
// alive, small enough that the face never looks misplaced.
#define DISPLAY_DRIFT_MINIMUM_INTERVAL_MILLISECONDS (2 * 60 * 1000)
#define DISPLAY_DRIFT_MAXIMUM_INTERVAL_MILLISECONDS (5 * 60 * 1000)
#define DISPLAY_DRIFT_MAXIMUM_STEP_PIXELS 4
#define DISPLAY_DRIFT_MAXIMUM_OFFSET_PIXELS 20

// The text sits below every face even at maximum downward drift. The lowest
// crop is the muted face, which ends at row 375; 20 px of drift takes it to
// 395, so the text starts at 396. That leaves 84 px: one transcript line and
// two reply lines of the 20 px font (line height 22 px). The user found 14 px
// too small to read; fewer, larger lines are fine because the reply is spoken.
#define DISPLAY_TEXT_TOP 396
#define DISPLAY_TEXT_MARGIN 16
#define DISPLAY_TEXT_WIDTH (DISPLAY_WIDTH - 2 * DISPLAY_TEXT_MARGIN)
#define DISPLAY_TRANSCRIPT_HEIGHT 24
#define DISPLAY_REPLY_TOP (DISPLAY_TEXT_TOP + DISPLAY_TRANSCRIPT_HEIGHT + 4)
#define DISPLAY_REPLY_HEIGHT 48

// QSPI command framing from the CO5300 datasheet: instruction 0x02 writes and
// 0x03 reads a 24-bit address of the form {0x00, command, 0x00}. The SH8601
// driver passes the same address in the upper 24 bits of a 32-bit command, so
// the display code does too.
#define DISPLAY_QSPI_WRITE_COMMAND_OPCODE 0x02
#define DISPLAY_QSPI_READ_COMMAND_OPCODE 0x03

// LVGL only renders a few labels here, not a demo, so the port's 7 KB default
// plus a little margin is enough. The extra kilobyte leaves room for the
// Montserrat text drawing path without wasting much of the 512 KB SRAM.
#define DISPLAY_LVGL_TASK_STACK_BYTES 8192
#define DISPLAY_LVGL_TASK_PRIORITY 4

// The LVGL port's task does not feed the task watchdog, so a timer running
// inside it does. One second is well below the watchdog timeout, and the port
// wakes at least every 500 ms to run timers, so a hang stops the feed.
#define DISPLAY_LVGL_WATCHDOG_RESET_MS 1000
#define DISPLAY_LVGL_TASK_NAME "taskLVGL"

// A touch is a manual recovery, not a gesture, so one re-init every two
// seconds is plenty and stops a held finger or a noisy controller from
// continuously resetting the panel.
#define DISPLAY_RECOVERY_MINIMUM_INTERVAL_US ((int64_t)2 * 1000 * 1000)
// The LVGL timer only copies an atomic flag most of the time, so it can run
// often enough to react to a touch without delaying rendering.
#define DISPLAY_RECOVERY_TIMER_PERIOD_MS 100

// The panel command set is SH8601-compatible, which is why Waveshare uses
// esp_lcd_sh8601 for a CO5300 board. This list is their 09_LVGL_V9_Test list,
// with two corrections for this panel's 480x480 geometry:
//  - both address windows run from 0 to 479. The XiaoZhi board file uses
//    0x16..0x1AF for columns and 0..0x1F5 for rows, which is the 2.06 inch
//    panel's geometry copied over; on a 480x480 panel it shifts and clips.
//  - 0x36 is 0x30 rather than 0xA0: the 0xA0 in the XiaoZhi file flips the
//    panel vertically to compensate for its wrong windows.
// The 0xFE 0x20, 0x19 and 0x1C commands switch to the CO5300 manufacturer page
// and configure the panel there. They stay because the working example uses
// them and the public user command set does not describe their role.
static const sh8601_lcd_init_cmd_t panel_init_commands[] = {
    {0x11, NULL, 0, 600},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0x30}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){DISPLAY_BRIGHTNESS_FULL_PERCENT * 255 / 100}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, NULL, 0, 100},
};

// Only used in logs now: the screen has no state line, but a state change is
// still the first thing to look for when checking the screen from a log.
static const char *const state_name[] = {
    [DISPLAY_STATE_IDLE] = "idle",
    [DISPLAY_STATE_LISTENING] = "listening",
    [DISPLAY_STATE_THINKING] = "thinking",
    [DISPLAY_STATE_SPEAKING] = "speaking",
    [DISPLAY_STATE_ERROR] = "error",
    [DISPLAY_STATE_NO_SERVER] = "no server",
    [DISPLAY_STATE_MUTED] = "muted",
};

// The face each state shows when no blink or variety moment is on screen.
// Error and no server keep the idle face and carry their message in the text
// area instead.
static const face_id_t state_face[] = {
    [DISPLAY_STATE_IDLE] = FACE_IDLE_DEFAULT,
    [DISPLAY_STATE_LISTENING] = FACE_LISTENING,
    [DISPLAY_STATE_THINKING] = FACE_THINKING,
    [DISPLAY_STATE_SPEAKING] = FACE_SPEAKING,
    [DISPLAY_STATE_ERROR] = FACE_IDLE_DEFAULT,
    [DISPLAY_STATE_NO_SERVER] = FACE_IDLE_DEFAULT,
    [DISPLAY_STATE_MUTED] = FACE_MUTED,
};

static lv_obj_t *face_image;
static lv_obj_t *transcript_label;
static lv_obj_t *reply_label;

// Everything that changes on its own runs on LVGL's timers, in the LVGL task.
// The session task therefore only ever changes the state; it does not have to
// start, stop or tick any of these.
static lv_timer_t *blink_timer;
static lv_timer_t *variety_timer;
static lv_timer_t *drift_timer;
static lv_timer_t *dim_timer;

static display_state_t current_state = DISPLAY_STATE_IDLE;
// The face on screen now, so a drift step can move it without looking it up
// from the state again (the state may be showing a blink or variety face).
static const face_asset_t *current_asset;
static int16_t drift_x;
static int16_t drift_y;
// Blink and variety are two-phase: one timer fire shows the temporary face and
// schedules the return, the next fire restores the state's face.
static bool blink_showing;
static bool variety_showing;
// What the panel was last told. Kept so a touch recovery, which resends the
// init list and its full-brightness command, can restore a dimmed panel.
static uint8_t brightness_percent = DISPLAY_BRIGHTNESS_FULL_PERCENT;

// Kept so the boot power-mode read and the brightness write can use the same
// io after panel creation.
static esp_lcd_panel_io_handle_t display_io;
static esp_lcd_panel_handle_t display_panel;

// The most recent DCS 0x0A read. The heartbeat task reads this from another
// task, and only display_init() writes it.
static atomic_int last_panel_power_mode = -1;

// The touch task sets this; the LVGL timer below consumes it. Keeping the SPI
// work out of the touch task means a touch never touches the panel bus itself.
static atomic_bool recovery_requested;
// Only read and written by the LVGL timer.
static int64_t last_recovery_microseconds;

// Both of Waveshare's working drivers round every invalidated area to even
// coordinates before it reaches the CO5300. LVGL invalidates arbitrary
// rectangles when labels change, so do the same here rather than rely on the
// panel tolerating an odd window. Every coordinate stays within the 0..479
// panel because 479 is itself odd.
static void round_area_to_even_coordinates(lv_area_t *area)
{
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void initialise_spi_bus(void)
{
    const spi_bus_config_t bus_configuration = {
        .sclk_io_num = DISPLAY_CLOCK_PIN,
        .data0_io_num = DISPLAY_DATA0_PIN,
        .data1_io_num = DISPLAY_DATA1_PIN,
        .data2_io_num = DISPLAY_DATA2_PIN,
        .data3_io_num = DISPLAY_DATA3_PIN,
        .max_transfer_sz = DISPLAY_MAXIMUM_TRANSFER_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &bus_configuration, SPI_DMA_CH_AUTO));
}

static esp_lcd_panel_io_handle_t initialise_panel_io(void)
{
    // A 32-bit command carries the QSPI write opcode in the top byte, followed
    // by the command byte. This is the framing the SH8601 driver expects for
    // both init parameters and pixel writes. One queued transaction is enough
    // because LVGL waits for each partial flush before rendering the next one.
    const esp_lcd_panel_io_spi_config_t io_configuration = {
        .cs_gpio_num = DISPLAY_CHIP_SELECT_PIN,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    esp_lcd_panel_io_handle_t io_handle;
    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_configuration, &io_handle));
    return io_handle;
}

static esp_lcd_panel_handle_t initialise_panel(esp_lcd_panel_io_handle_t io_handle)
{
    const sh8601_vendor_config_t vendor_configuration = {
        .init_cmds = panel_init_commands,
        .init_cmds_size = sizeof(panel_init_commands) / sizeof(panel_init_commands[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_configuration = {
        // There is no reset GPIO on this board; the panel is reset in software.
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_configuration,
    };

    esp_lcd_panel_handle_t panel_handle;
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_configuration, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    return panel_handle;
}

// The CO5300 datasheet frames a register read as DCS 0x03 followed by the
// 24-bit address {0x00, command, 0x00}, and esp_lcd_panel_io_rx_param builds
// exactly that 32-bit word. This is kept as the one attempt at boot: on this
// panel it returns 0xFF, so no display health check can be based on it.
static esp_err_t read_panel_power_mode(uint8_t *power_mode)
{
    const int command = (DISPLAY_QSPI_READ_COMMAND_OPCODE << 24) | (LCD_CMD_RDDPM << 8);
    return esp_lcd_panel_io_rx_param(display_io, command, power_mode, sizeof(*power_mode));
}

static esp_err_t write_panel_brightness(uint8_t percent)
{
    const uint8_t brightness = percent * 255 / 100;
    const int command = (DISPLAY_QSPI_WRITE_COMMAND_OPCODE << 24) | (LCD_CMD_WRDISBV << 8);
    return esp_lcd_panel_io_tx_param(display_io, command, &brightness, sizeof(brightness));
}

// Always writes the command, so the recovery path can use it to restore the
// dim value after re-sending the init list (which sets full brightness).
static void apply_brightness(uint8_t percent)
{
    const esp_err_t result = write_panel_brightness(percent);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Setting the panel brightness to %d%% failed (0x%x)", percent, result);
        return;
    }
    brightness_percent = percent;
}

// Skips a panel write when the value is already in effect, so the three state
// changes in a session do not each send a redundant command.
static void set_brightness(uint8_t percent)
{
    if (percent == brightness_percent) {
        return;
    }
    apply_brightness(percent);
}

void display_request_recovery(void)
{
    // Only a flag: the caller may be the touch task, and the panel bus belongs
    // to the LVGL context.
    atomic_store(&recovery_requested, true);
}

// Called from the LVGL timer, which the port runs with its lock held. Re-sends
// the whole init list because the panel has no reset line, restores the
// brightness and marks every pixel dirty so LVGL flushes the frame again.
static void recover_panel(void)
{
    const uint32_t uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000);
    ESP_LOGI(TAG, "Touch recovery: re-initialising the panel at %lu s", (unsigned long)uptime_seconds);

    const esp_err_t init_result = esp_lcd_panel_init(display_panel);
    if (init_result != ESP_OK) {
        ESP_LOGE(TAG, "Re-initialising the panel failed (0x%x)", init_result);
        return;
    }
    // The init list resends the brightness command at full brightness, so a
    // dimmed idle screen would come back bright. Restore the value in effect.
    apply_brightness(brightness_percent);

    lv_obj_invalidate(lv_screen_active());
    ESP_LOGI(TAG, "Touch recovery: panel re-initialised at %lu s, screen invalidated",
             (unsigned long)uptime_seconds);
}

static void handle_recovery_request(lv_timer_t *timer)
{
    (void)timer;
    if (!atomic_exchange(&recovery_requested, false)) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (last_recovery_microseconds != 0 &&
        now - last_recovery_microseconds < DISPLAY_RECOVERY_MINIMUM_INTERVAL_US) {
        ESP_LOGI(TAG, "Ignoring a touch recovery within 2 s of the previous one");
        return;
    }

    last_recovery_microseconds = now;
    recover_panel();
}

// Runs in the LVGL task. It is the only thing that proves that task is alive,
// because the managed port does not call the watchdog API itself.
static void reset_lvgl_watchdog(lv_timer_t *timer)
{
    (void)timer;
    esp_task_wdt_reset();
}

static uint32_t random_between(uint32_t minimum, uint32_t maximum)
{
    // esp_random() is the hardware RNG; the modulo bias over ranges of a few
    // seconds or pixels is irrelevant here.
    return minimum + esp_random() % (maximum - minimum + 1);
}

static int16_t clamp_drift(int16_t offset)
{
    if (offset > DISPLAY_DRIFT_MAXIMUM_OFFSET_PIXELS) {
        return DISPLAY_DRIFT_MAXIMUM_OFFSET_PIXELS;
    }
    if (offset < -DISPLAY_DRIFT_MAXIMUM_OFFSET_PIXELS) {
        return -DISPLAY_DRIFT_MAXIMUM_OFFSET_PIXELS;
    }
    return offset;
}

// Draws one face at its home position plus the current drift. Changing the
// source and moving the object invalidates only the image area, so neither a
// blink nor a drift step repaints the text or the rest of the screen.
static void show_face(face_id_t face)
{
    current_asset = faces_get(face);
    lv_image_set_src(face_image, current_asset->image);
    lv_obj_set_pos(face_image, current_asset->x + drift_x, current_asset->y + drift_y);
    // The object is sized from its content, and the new source's dimensions
    // only apply after a layout pass. Doing it here makes the object and the
    // log agree within the same timer call.
    lv_obj_update_layout(face_image);
}

// Puts the idle or muted screen on the 30 second dim countdown. A session
// pauses the timer instead, and error and no-server screens keep full
// brightness for their message, so when this timer fires the state is known to
// be one of these two.
static void restart_dim_timer(void)
{
    lv_timer_set_period(dim_timer, DISPLAY_DIM_AFTER_MILLISECONDS);
    lv_timer_reset(dim_timer);
    lv_timer_resume(dim_timer);
}

static void dim_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    ESP_LOGI(TAG, "Idle for %d s, dimming the display to %d%%",
             DISPLAY_DIM_AFTER_MILLISECONDS / 1000, DISPLAY_BRIGHTNESS_DIM_PERCENT);
    set_brightness(DISPLAY_BRIGHTNESS_DIM_PERCENT);
    lv_timer_pause(timer);
}

// Restarts the blink and variety cycles from a fresh random interval. Called on
// every state change so a session does not inherit a blink that was already
// due, and so an idle screen starts counting again when it comes back.
static void reset_expression_timers(void)
{
    blink_showing = false;
    variety_showing = false;
    lv_timer_set_period(blink_timer, random_between(DISPLAY_BLINK_MINIMUM_INTERVAL_MILLISECONDS,
                                                    DISPLAY_BLINK_MAXIMUM_INTERVAL_MILLISECONDS));
    lv_timer_reset(blink_timer);
    lv_timer_set_period(variety_timer, random_between(DISPLAY_VARIETY_MINIMUM_INTERVAL_MILLISECONDS,
                                                      DISPLAY_VARIETY_MAXIMUM_INTERVAL_MILLISECONDS));
    lv_timer_reset(variety_timer);
}

static void blink_timer_callback(lv_timer_t *timer)
{
    // Blinking belongs to the idle face only. A session, the muted face, an
    // error or a variety moment each have their own expression, and a blink
    // there would fight it. The timer still reschedules itself so the first
    // blink after the next return to idle comes at a random interval.
    if (current_state != DISPLAY_STATE_IDLE || variety_showing) {
        blink_showing = false;
        lv_timer_set_period(timer, random_between(DISPLAY_BLINK_MINIMUM_INTERVAL_MILLISECONDS,
                                                  DISPLAY_BLINK_MAXIMUM_INTERVAL_MILLISECONDS));
        return;
    }

    if (blink_showing) {
        blink_showing = false;
        show_face(FACE_IDLE_DEFAULT);
        lv_timer_set_period(timer, random_between(DISPLAY_BLINK_MINIMUM_INTERVAL_MILLISECONDS,
                                                  DISPLAY_BLINK_MAXIMUM_INTERVAL_MILLISECONDS));
        return;
    }

    blink_showing = true;
    show_face(FACE_BLINK);
    // The next fire is the return to the idle face, which is why the duration
    // is the period rather than a separate one-shot timer.
    lv_timer_set_period(timer, DISPLAY_BLINK_DURATION_MILLISECONDS);
    ESP_LOGI(TAG, "Blink");
}

static void variety_timer_callback(lv_timer_t *timer)
{
    const uint32_t waiting_period =
        random_between(DISPLAY_VARIETY_MINIMUM_INTERVAL_MILLISECONDS, DISPLAY_VARIETY_MAXIMUM_INTERVAL_MILLISECONDS);

    // Only the idle face has spare expressions. In every other state the timer
    // keeps counting down so the next idle period still starts with a fresh
    // interval.
    if (current_state != DISPLAY_STATE_IDLE) {
        variety_showing = false;
        lv_timer_set_period(timer, waiting_period);
        return;
    }

    if (variety_showing) {
        variety_showing = false;
        show_face(FACE_IDLE_DEFAULT);
        lv_timer_set_period(timer, waiting_period);
        ESP_LOGI(TAG, "Variety: back to the idle face");
        return;
    }

    const face_id_t face = (esp_random() & 1) ? FACE_IDLE_CURIOUS : FACE_IDLE_HAPPY_EYES;
    variety_showing = true;
    show_face(face);
    lv_timer_set_period(timer, random_between(DISPLAY_VARIETY_MINIMUM_DURATION_MILLISECONDS,
                                              DISPLAY_VARIETY_MAXIMUM_DURATION_MILLISECONDS));
    ESP_LOGI(TAG, "Variety: showing the %s face", face == FACE_IDLE_CURIOUS ? "curious" : "happy eyes");
}

static void drift_timer_callback(lv_timer_t *timer)
{
    // A small step in a random direction each way, clamped to the ±20 px box.
    // Steps rather than a new random offset keep the movement from jumping
    // across the whole box in one go.
    const int16_t step_x = (int16_t)(random_between(0, 2 * DISPLAY_DRIFT_MAXIMUM_STEP_PIXELS)) -
                           DISPLAY_DRIFT_MAXIMUM_STEP_PIXELS;
    const int16_t step_y = (int16_t)(random_between(0, 2 * DISPLAY_DRIFT_MAXIMUM_STEP_PIXELS)) -
                           DISPLAY_DRIFT_MAXIMUM_STEP_PIXELS;
    drift_x = clamp_drift((int16_t)(drift_x + step_x));
    drift_y = clamp_drift((int16_t)(drift_y + step_y));

    lv_obj_set_pos(face_image, current_asset->x + drift_x, current_asset->y + drift_y);
    ESP_LOGI(TAG, "Drift: (%d, %d) px from home", drift_x, drift_y);

    lv_timer_set_period(timer, random_between(DISPLAY_DRIFT_MINIMUM_INTERVAL_MILLISECONDS,
                                              DISPLAY_DRIFT_MAXIMUM_INTERVAL_MILLISECONDS));
}

// The transcript and reply stay up for a moment after the session returns to
// idle, so the end of the reply is still readable, then they clear: the user
// wants a bare face between sessions. Error and "No server" messages are not
// cleared, because they are the only explanation for the state.
#define DISPLAY_TEXT_CLEAR_AFTER_MILLISECONDS 2500

static lv_timer_t *text_clear_timer;

static void text_clear_timer_callback(lv_timer_t *timer)
{
    lv_timer_pause(timer);
    lv_label_set_text(transcript_label, "");
    lv_label_set_text(reply_label, "");
}

// The state line is gone, but an error or a missing server still has to say
// something. The session sets the reply text before returning to error on most
// failures, so only an empty reply gets the generic word; "No server" always
// wins because the session clears both fields before setting that state.
static void show_state_message(display_state_t state)
{
    if (state == DISPLAY_STATE_NO_SERVER) {
        lv_label_set_text(reply_label, "No server");
    } else if (state == DISPLAY_STATE_ERROR && lv_label_get_text(reply_label)[0] == '\0') {
        lv_label_set_text(reply_label, "Error");
    }
}

// The text starts below every face at maximum drift, which depends on the
// asset crops rather than on the drawing code, so check the assumption once at
// boot instead of trusting the constants to keep matching future images.
static void log_face_layout(void)
{
    int32_t lowest_row = 0;
    for (int index = 0; index < FACE_COUNT; index++) {
        const face_asset_t *asset = faces_get((face_id_t)index);
        const int32_t row = asset->y + asset->image->header.h - 1;
        if (row > lowest_row) {
            lowest_row = row;
        }
    }
    const int32_t drifted_row = lowest_row + DISPLAY_DRIFT_MAXIMUM_OFFSET_PIXELS;
    if (drifted_row >= DISPLAY_TEXT_TOP) {
        ESP_LOGE(TAG, "A face reaches row %d at maximum drift, overlapping the text at row %d", (int)drifted_row,
                 DISPLAY_TEXT_TOP);
    }
    ESP_LOGI(TAG, "Face area: lowest crop row %d, row %d at maximum drift; text starts at row %d",
             (int)lowest_row, (int)drifted_row, DISPLAY_TEXT_TOP);
}

static void create_face_screen(void)
{
    lv_obj_t *screen = lv_screen_active();

    // AMOLED pixels that are black are switched off, so a black background is
    // also the lowest-power option for a screen that is idle most of the time.
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    // The screen holds one fixed layout, so it should never scroll; keeping it
    // still also stops a stray swipe from moving the face or the text.
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(screen, LV_DIR_NONE);

    // One image object for the face. Swapping its source or moving it
    // invalidates only the image, which is what keeps blinks and drift steps
    // from repainting the screen.
    face_image = lv_image_create(screen);

    // The transcript is the user's last utterance and is usually a sentence or
    // two. Dots mode cuts an exceptionally long one with an ellipsis instead of
    // pushing the reply down.
    transcript_label = lv_label_create(screen);
    lv_obj_set_style_text_font(transcript_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(transcript_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_set_pos(transcript_label, DISPLAY_TEXT_MARGIN, DISPLAY_TEXT_TOP);
    lv_obj_set_size(transcript_label, DISPLAY_TEXT_WIDTH, DISPLAY_TRANSCRIPT_HEIGHT);
    lv_label_set_long_mode(transcript_label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(transcript_label, "");

    // A reply can be a full paragraph, but only three lines fit below the face;
    // dots mode shows that there is more without letting the text run into the
    // face or off the panel.
    reply_label = lv_label_create(screen);
    lv_obj_set_style_text_font(reply_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(reply_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_pos(reply_label, DISPLAY_TEXT_MARGIN, DISPLAY_REPLY_TOP);
    lv_obj_set_size(reply_label, DISPLAY_TEXT_WIDTH, DISPLAY_REPLY_HEIGHT);
    lv_label_set_long_mode(reply_label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(reply_label, "");

    show_face(state_face[current_state]);
}

void display_init(void)
{
    initialise_spi_bus();
    display_io = initialise_panel_io();
    display_panel = initialise_panel(display_io);

    // The init list already writes brightness, but sending it here from the
    // configured percentage keeps the value in one place that any recovery
    // path can reuse, at the cost of one extra command at boot.
    apply_brightness(DISPLAY_BRIGHTNESS_FULL_PERCENT);

    // The read returns 0xFF on this panel, which is the idle level of SIO0
    // rather than a power-mode value. It is logged and kept for the heartbeat
    // so the absence of a usable register read is visible in the log, but no
    // health check can be based on it.
    uint8_t power_mode = 0;
    const esp_err_t power_mode_result = read_panel_power_mode(&power_mode);
    if (power_mode_result == ESP_OK) {
        atomic_store(&last_panel_power_mode, power_mode);
        ESP_LOGW(TAG, "Panel power mode at boot: 0x%02X (register reads are not usable on this panel)", power_mode);
    } else {
        ESP_LOGE(TAG, "Could not read the panel power mode at boot (0x%x)", power_mode_result);
    }

    const lvgl_port_cfg_t lvgl_configuration = {
        .task_priority = DISPLAY_LVGL_TASK_PRIORITY,
        .task_stack = DISPLAY_LVGL_TASK_STACK_BYTES,
        .task_affinity = 0,
        .task_max_sleep_ms = 500,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_configuration));

    const lvgl_port_display_cfg_t display_configuration = {
        .io_handle = display_io,
        .panel_handle = display_panel,
        .buffer_size = DISPLAY_BUFFER_PIXELS,
        .double_buffer = false,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .rounder_cb = round_area_to_even_coordinates,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            // The panel's QSPI interface expects each 16-bit pixel high byte
            // first, while LVGL stores it in the CPU's little-endian order.
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    // esp_lvgl_port applies the rotation fields by calling swap_xy even when
    // they are false. The SH8601 driver does not implement swap_xy and logs an
    // error for that call, but it is harmless: this panel runs unrotated.
    ESP_ERROR_CHECK(lvgl_port_add_disp(&display_configuration) != NULL ? ESP_OK : ESP_FAIL);

    // LVGL objects may only be created while the port's lock is held.
    lvgl_port_lock(0);
    create_face_screen();
    lv_timer_create(handle_recovery_request, DISPLAY_RECOVERY_TIMER_PERIOD_MS, NULL);
    lv_timer_create(reset_lvgl_watchdog, DISPLAY_LVGL_WATCHDOG_RESET_MS, NULL);
    blink_timer = lv_timer_create(blink_timer_callback,
                                  random_between(DISPLAY_BLINK_MINIMUM_INTERVAL_MILLISECONDS,
                                                 DISPLAY_BLINK_MAXIMUM_INTERVAL_MILLISECONDS),
                                  NULL);
    variety_timer = lv_timer_create(variety_timer_callback,
                                    random_between(DISPLAY_VARIETY_MINIMUM_INTERVAL_MILLISECONDS,
                                                   DISPLAY_VARIETY_MAXIMUM_INTERVAL_MILLISECONDS),
                                    NULL);
    drift_timer = lv_timer_create(drift_timer_callback,
                                  random_between(DISPLAY_DRIFT_MINIMUM_INTERVAL_MILLISECONDS,
                                                 DISPLAY_DRIFT_MAXIMUM_INTERVAL_MILLISECONDS),
                                  NULL);
    dim_timer = lv_timer_create(dim_timer_callback, DISPLAY_DIM_AFTER_MILLISECONDS, NULL);
    text_clear_timer = lv_timer_create(text_clear_timer_callback, DISPLAY_TEXT_CLEAR_AFTER_MILLISECONDS, NULL);
    lv_timer_pause(text_clear_timer);
    // The board boots into the idle screen, so its dim countdown starts now.
    restart_dim_timer();
    log_face_layout();
    lv_mem_monitor_t lvgl_memory;
    lv_mem_monitor(&lvgl_memory);
    lvgl_port_unlock();

    // Subscribe the port's task now that the timer that feeds the watchdog
    // exists. The port does not expose its handle, but the task name is fixed
    // by the managed component.
    TaskHandle_t lvgl_task = xTaskGetHandle(DISPLAY_LVGL_TASK_NAME);
    ESP_ERROR_CHECK(lvgl_task != NULL ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_task_wdt_add(lvgl_task));

    ESP_LOGI(TAG, "CO5300 panel ready, %d x %d, brightness %d%%, LVGL heap %d KB",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_BRIGHTNESS_FULL_PERCENT, CONFIG_LV_MEM_SIZE_KILOBYTES);
    ESP_LOGI(TAG, "LVGL heap after creating the screen: %u of %u bytes used, %u bytes free",
             (unsigned)(lvgl_memory.total_size - lvgl_memory.free_size),
             (unsigned)lvgl_memory.total_size, (unsigned)lvgl_memory.free_size);
    ESP_LOGI(TAG, "Free heap after display init: %u bytes, minimum free heap: %u bytes, largest free block: %u bytes",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

int display_last_power_mode(void)
{
    return atomic_load(&last_panel_power_mode);
}

void display_set_state(display_state_t state)
{
    lvgl_port_lock(0);

    // Repeating the current state is normal during a session (two events may
    // both report "thinking"), and restarting the blink, variety and dim
    // cycles for it would keep them from ever firing.
    if (state != current_state) {
        ESP_LOGI(TAG, "Display state: %s -> %s", state_name[current_state], state_name[state]);
        current_state = state;

        // Every state starts at full brightness so a session or a message is
        // readable. Only the idle and muted screens then run the dim countdown;
        // the others pause it, so an idle screen cannot dim under a session or
        // an error message.
        set_brightness(DISPLAY_BRIGHTNESS_FULL_PERCENT);
        if (state == DISPLAY_STATE_IDLE || state == DISPLAY_STATE_MUTED) {
            restart_dim_timer();
        } else {
            lv_timer_pause(dim_timer);
        }

        reset_expression_timers();
        show_face(state_face[state]);

        if (state == DISPLAY_STATE_IDLE || state == DISPLAY_STATE_MUTED) {
            lv_timer_reset(text_clear_timer);
            lv_timer_resume(text_clear_timer);
        } else {
            lv_timer_pause(text_clear_timer);
        }
    }

    // The message is applied even when the state did not change. The session
    // clears both text fields before reporting "No server", so a second trigger
    // with no server would otherwise leave the idle face without its message:
    // the clears happen, then the state comparison above skips the redraw.
    show_state_message(state);

    lvgl_port_unlock();
}

void display_set_transcript(const char *text)
{
    lvgl_port_lock(0);
    lv_label_set_text(transcript_label, text);
    lvgl_port_unlock();
}

void display_set_reply(const char *text)
{
    lvgl_port_lock(0);
    lv_label_set_text(reply_label, text);
    lvgl_port_unlock();
}

void display_clear(void)
{
    display_set_transcript("");
    display_set_reply("");
    display_set_state(DISPLAY_STATE_IDLE);
}
