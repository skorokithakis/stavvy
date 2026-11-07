#include "display.h"

#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
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
#define DISPLAY_BRIGHTNESS_PERCENT 60

// LVGL only renders a few labels here, not a demo, so the port's 7 KB default
// plus a little margin is enough. The extra kilobyte leaves room for the
// Montserrat text drawing path without wasting much of the 512 KB SRAM.
#define DISPLAY_LVGL_TASK_STACK_BYTES 8192
#define DISPLAY_LVGL_TASK_PRIORITY 4

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
    {0x51, (uint8_t[]){DISPLAY_BRIGHTNESS_PERCENT * 255 / 100}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, NULL, 0, 100},
};

static const char *const state_text[] = {
    [DISPLAY_STATE_IDLE] = "Idle",
    [DISPLAY_STATE_LISTENING] = "Listening",
    [DISPLAY_STATE_THINKING] = "Thinking",
    [DISPLAY_STATE_SPEAKING] = "Speaking",
    [DISPLAY_STATE_ERROR] = "Error",
    [DISPLAY_STATE_NO_SERVER] = "No server",
};

static const lv_color_t state_color[] = {
    [DISPLAY_STATE_IDLE] = LV_COLOR_MAKE(0xB0, 0xB0, 0xB0),
    [DISPLAY_STATE_LISTENING] = LV_COLOR_MAKE(0x4C, 0xAF, 0x50),
    [DISPLAY_STATE_THINKING] = LV_COLOR_MAKE(0xFF, 0xC1, 0x07),
    [DISPLAY_STATE_SPEAKING] = LV_COLOR_MAKE(0x21, 0x96, 0xF3),
    [DISPLAY_STATE_ERROR] = LV_COLOR_MAKE(0xF4, 0x43, 0x36),
    [DISPLAY_STATE_NO_SERVER] = LV_COLOR_MAKE(0xFF, 0x98, 0x00),
};

static lv_obj_t *state_label;
static lv_obj_t *transcript_label;
static lv_obj_t *reply_label;
static lv_obj_t *reply_container;

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

static void create_status_screen(void)
{
    lv_obj_t *screen = lv_screen_active();

    // AMOLED pixels that are black are switched off, so a black background is
    // also the lowest-power option for a screen that is idle most of the time.
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xE0E0E0), 0);
    // The screen holds one fixed layout, so it should never scroll; keeping it
    // still also stops a stray swipe from moving the state line later.
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(screen, LV_DIR_NONE);

    state_label = lv_label_create(screen);
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(state_label, 16, 16);

    // The transcript is the user's last utterance and is usually a sentence or
    // two. Three lines at 16 px is enough; DOT mode makes an exceptionally long
    // one end in an ellipsis instead of pushing the reply off the screen.
    transcript_label = lv_label_create(screen);
    lv_obj_set_style_text_font(transcript_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(transcript_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_set_pos(transcript_label, 16, 44);
    lv_obj_set_size(transcript_label, DISPLAY_WIDTH - 32, 66);
    lv_label_set_long_mode(transcript_label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(transcript_label, "");

    // A reply can be a full paragraph, so it gets its own scrollable panel and
    // a 14 px font. If it still does not fit, the panel scrolls instead of
    // clipping the end. The pad leaves room for the scrollbar over the text.
    reply_container = lv_obj_create(screen);
    lv_obj_set_pos(reply_container, 16, 118);
    lv_obj_set_size(reply_container, DISPLAY_WIDTH - 32, DISPLAY_HEIGHT - 134);
    lv_obj_set_style_bg_opa(reply_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(reply_container, 0, 0);
    lv_obj_set_style_radius(reply_container, 0, 0);
    lv_obj_set_style_shadow_width(reply_container, 0, 0);
    lv_obj_set_style_outline_width(reply_container, 0, 0);
    lv_obj_set_style_pad_all(reply_container, 0, 0);
    lv_obj_set_style_pad_right(reply_container, 10, 0);
    lv_obj_set_scroll_dir(reply_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(reply_container, LV_SCROLLBAR_MODE_AUTO);

    reply_label = lv_label_create(reply_container);
    lv_obj_set_style_text_font(reply_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(reply_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_width(reply_label, LV_PCT(100));
    lv_label_set_long_mode(reply_label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(reply_label, "");

    lv_label_set_text(state_label, state_text[DISPLAY_STATE_IDLE]);
    lv_obj_set_style_text_color(state_label, state_color[DISPLAY_STATE_IDLE], 0);
}

void display_init(void)
{
    initialise_spi_bus();
    esp_lcd_panel_io_handle_t io_handle = initialise_panel_io();
    esp_lcd_panel_handle_t panel_handle = initialise_panel(io_handle);

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
        .io_handle = io_handle,
        .panel_handle = panel_handle,
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
    create_status_screen();
    lv_mem_monitor_t lvgl_memory;
    lv_mem_monitor(&lvgl_memory);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "CO5300 panel ready, %d x %d, brightness %d%%, LVGL heap %d KB",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_BRIGHTNESS_PERCENT,
             CONFIG_LV_MEM_SIZE_KILOBYTES);
    ESP_LOGI(TAG, "LVGL heap after creating the screen: %u of %u bytes used, %u bytes free",
             (unsigned)(lvgl_memory.total_size - lvgl_memory.free_size),
             (unsigned)lvgl_memory.total_size, (unsigned)lvgl_memory.free_size);
    ESP_LOGI(TAG, "Free heap after display init: %u bytes, minimum free heap: %u bytes, largest free block: %u bytes",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

void display_set_state(display_state_t state)
{
    lvgl_port_lock(0);
    lv_label_set_text(state_label, state_text[state]);
    lv_obj_set_style_text_color(state_label, state_color[state], 0);
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
    // New replies are read from the start; if the previous one was scrolled to
    // the end, leaving the offset there would show the middle of the new text.
    lv_obj_scroll_to_y(reply_container, 0, LV_ANIM_OFF);
    lvgl_port_unlock();
}

void display_clear(void)
{
    display_set_transcript("");
    display_set_reply("");
    display_set_state(DISPLAY_STATE_IDLE);
}

void display_run_boot_demo(void)
{
    display_set_transcript("What is the weather like in Reykjavik tomorrow?");
    display_set_reply("It will be cloudy in the morning with a chance of light rain, "
                      "turning brighter in the afternoon. The temperature will be "
                      "around seven degrees, with a light breeze from the southwest. "
                      "The evening should stay dry, so it is a good day for a walk "
                      "along the harbour.");

    static const display_state_t demo_states[] = {
        DISPLAY_STATE_LISTENING,
        DISPLAY_STATE_THINKING,
        DISPLAY_STATE_SPEAKING,
        DISPLAY_STATE_ERROR,
        DISPLAY_STATE_NO_SERVER,
        DISPLAY_STATE_IDLE,
    };
    for (size_t index = 0; index < sizeof(demo_states) / sizeof(demo_states[0]); index++) {
        display_set_state(demo_states[index]);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}
