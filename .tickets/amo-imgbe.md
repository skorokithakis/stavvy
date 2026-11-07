---
id: amo-imgbe
status: closed
deps: [amo-pmgdr]
links: []
created: 2026-11-06T23:13:23Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# Display: CO5300 panel and LVGL status screen

Ready for implementation.

Objective: the AMOLED shows one screen with the assistant state, the last transcript and the last reply text. Other modules update it through a small thread-safe API.

Scope:
- QSPI panel on SPI2: CLK GPIO0, D0 to D3 GPIO1 to GPIO4, CS GPIO15, no reset GPIO. Driver chip CO5300, 480 x 480. Waveshare's XiaoZhi board file and 09_LVGL_V9_Test show a working setup (they use the SH8601 QSPI panel driver with a custom init command list). Waveshare's examples set CS to GPIO5; that is wrong, use GPIO15.
- LVGL v9 through esp_lvgl_port. Partial render with a small draw buffer (about 40 lines). There is no PSRAM and a full frame does not fit.
- Brightness via panel command 0x51. Set a fixed, moderate level.
- UI: a state line (idle, listening, thinking, speaking, error, no server), transcript text, reply text. Long text wraps or scrolls. Use a built-in LVGL font with English glyphs.
- API: set state, set transcript, set reply, clear. Safe to call from any task.
- A boot demo that shows each state for a moment, so the user can check the panel before the session task exists.
- Report free heap with the display running.

Non-goals: touch, animations, images, custom fonts, the SD card (it shares the SPI bus; do not block its future use).

## Design

Check the init command list against the panel. The XiaoZhi list sets a column window of 0x16 to 0x1AF (410 columns), which looks copied from the 2.06 inch board. If the image is offset or clipped, the window is the first suspect.

