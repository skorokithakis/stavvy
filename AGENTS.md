# Hardware: Waveshare ESP32-C6-Touch-AMOLED-2.16

All firmware in this repository targets this one board. The project goal is not decided yet.

Toolchain: ESP-IDF (C). Do not use Arduino, ESPHome or MicroPython. Waveshare's ESP-IDF examples target ESP-IDF v5.5.3.

- SKU 34202: ESP32-C6-Touch-AMOLED-2.16 (with lithium battery).
- SKU 34201: ESP32-C6-Touch-AMOLED-2.16-EN (without battery).

## Links

- Wiki: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16
- Resources (schematic, datasheets): https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16/Resources-And-Documents
- Arduino guide: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16/Arduino
- ESP-IDF guide: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16/ESP-IDF
- FAQ: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16/FAQ
- Example code (ESP-IDF v5.5.3, Arduino core v3.3.3, XiaoZhi voice assistant): https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16
- Schematic: https://files.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-2.16/ESP32-C6-Touch-AMOLED-2.16-Schematic.pdf
- Product page: https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm
- ESP32-C6 datasheet: https://documentation.espressif.com/esp32-c6_datasheet_en.pdf
- ESP32-C6 technical reference manual: https://documentation.espressif.com/esp32-c6_technical_reference_manual_en.pdf
- Datasheets: [CO5300](https://files.waveshare.com/wiki/common/Co5300_Datasheet.pdf), [AXP2101](https://files.waveshare.com/wiki/common/X-power-AXP2101_SWcharge_V1.0.pdf), [ES8311](https://files.waveshare.com/wiki/common/ES8311.DS.pdf), [QMI8658](https://files.waveshare.com/wiki/common/QMI8658C_datasheet_rev_0.9.pdf), [PCF85063](https://files.waveshare.com/wiki/common/Pcf85063atl1118-NdPQpTGE-loeW7GbZ7.pdf)

## Specs

| Part | Chip | Notes |
| --- | --- | --- |
| SoC | ESP32-C6 | Single-core RISC-V, 160 MHz. Wi-Fi 6 (2.4 GHz), BLE 5, 802.15.4 (Zigbee, Thread). |
| RAM | On-chip | 512 KB HP SRAM, 16 KB LP SRAM. **No PSRAM, and the C6 cannot use PSRAM.** |
| Flash | NOR | 16 MB external. |
| Display | CO5300 driver | 2.16 inch AMOLED, 480 × 480, 16.7M colours, 600 cd/m², QSPI. No backlight pin (AMOLED; brightness is a panel command, 0x51). |
| Touch | CST9220 | Capacitive, I2C. |
| Power | AXP2101 PMIC | I2C address 0x34. Battery charge/discharge, rails (ALDO1 to ALDO4 power the peripherals, see the Waveshare examples). Drives the PWR button. |
| IMU | QMI8658 | 3-axis accelerometer and 3-axis gyroscope, I2C. |
| RTC | PCF85063 | I2C. AXP2101 backs it up from the battery. |
| Audio out | ES8311 codec + speaker amplifier | 2-pin speaker pads. |
| Audio in | ES7210 ADC | Dual microphone array, echo-cancellation reference. |
| Storage | TF (microSD) slot | FAT32. SPI mode. |
| Battery | MX1.25 2-pin | 3.7 V lithium. |
| Antenna | Onboard chip antenna | IPEX1 connector is available after you move an onboard resistor. |
| USB | Type-C | Native ESP32-C6 USB (USB-Serial-JTAG) for flashing and logs. |
| Buttons | BOOT, PWR, KEY | BOOT: steps the full brightness through 40, 60, 80 and 100%, and holding it at power-on enters download mode. PWR: long press off, short press on (board powers on when power connects). KEY: cycles the microphone mute. |
| Expansion | Solder pads | 1× I2C, 1× UART, 1× USB. |

## Pin map

Verified against the schematic unless marked otherwise.

| Function | GPIO |
| --- | --- |
| Display QSPI CLK | 0 |
| Display QSPI D0 to D3 | 1, 2, 3, 4 |
| Display CS | 15 |
| Display reset | Not connected to a GPIO |
| Touch INT | 5 |
| Touch RST | 11 |
| Shared I2C SDA / SCL (PMIC, touch, IMU, RTC, codecs) | 8 / 7 |
| IMU INT1 / INT2 | 16 / 17 |
| I2S MCLK / BCLK / WS / DOUT (to ES8311) / DIN (from ES7210) | 19 / 20 / 22 / 23 / 21 |
| SD card CLK / MOSI / MISO (shared with display QSPI bus) | 0 / 1 / 2 |
| SD card CS | 6 |
| BOOT button | 9 |
| KEY button | 10 |
| PWR button (readable state; the AXP2101 also handles power on and off) | 18 |
| USB D- / D+ | 12 / 13 |
| Speaker amplifier enable | Not a GPIO in Waveshare's code (`GPIO_NUM_NC`). Not confirmed how it is enabled. |

## Build and flash

The ESP-IDF toolchain is not installed on the host. Use Espressif's v5.5.3 Docker image and run it as the host user, so `build/` and `sdkconfig` are not left owned by root:

```sh
docker pull espressif/idf:v5.5.3

docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
    espressif/idf:v5.5.3 idf.py build
```

The first build downloads the mDNS component from the ESP component registry, so it needs network access.

Flash and watch the log over the board's USB port. `--group-add dialout` lets the container open the port, and the device may enumerate as something other than `ttyACM0`:

```sh
docker run --rm -it --device=/dev/ttyACM0 --group-add dialout -v "$PWD":/project -w /project \
    -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5.3 \
    idf.py -p /dev/ttyACM0 flash monitor
```

Wi-Fi credentials are set at build time. `menuconfig` writes them to the git-ignored `sdkconfig`, so they never enter the repository:

```sh
docker run --rm -it -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
    espressif/idf:v5.5.3 idf.py menuconfig
```

## ESPHome native API server

The firmware runs a plaintext ESPHome native API server on TCP port 6053, limited to the messages `havpe-server` uses for its voice pipeline. The wire protocol lives in `main/proto/esphome_api.proto`, a reduced copy of ESPHome's `api.proto` with identical field numbers.

nanopb is used without dynamic allocation. Its runtime is vendored in `components/nanopb` (0.4.9.1, zlib licence). The generated `main/proto/esphome_api.pb.c` and `.pb.h` are committed because the ESP-IDF Docker image has no protobuf toolchain; regenerate them on the host after editing the `.proto`:

```sh
./main/proto/regenerate.sh
```

The script creates a virtualenv in `$TMPDIR` and installs `nanopb==0.4.9.1` (which bundles protoc), so `python3` is the only host requirement.

## Wake word

The trigger module (`main/trigger.c`) reads 10 ms frames of 16 kHz mono audio and
hands them to the detector in `main/wake_word.cpp`, which runs the custom
microWakeWord model "Stavrobot". The model and its microWakeWord manifest are
`main/wake_word/stavrobot.tflite` and `stavrobot.json`.

The manifest's four values are copied into constants at the top of
`wake_word.cpp`: `probability_cutoff` 0.97 (quantised to 247 the way ESPHome does
it), `sliding_window_size` 5, `feature_step_size` 10 ms and `tensor_arena_size`
38000. Replacing the model means editing those constants as well.

The spectrogram frontend is `esphome/esp-micro-speech-features` and the inference
is `espressif/esp-tflite-micro`. esp-nn is pinned to 1.1.2 because 1.2.x gave
wrong outputs and needed a larger arena; the same versions are what ESPHome's
`micro_wake_word` uses. The streaming inference, op list, arena handling and
sliding-window averaging are adapted from that component (GPLv3, like this
firmware), and `wake_word.cpp` keeps its copyright notice. The model is in
`EMBED_FILES`, so TFLite Micro reads it straight from flash and a normal
`idf.py flash` writes it as part of the application; there is no model partition.

The tensor arena is 38 KB and 22.9 KB of it is in use on this chip, so the
manifest's size is generous here. On boot the detector logs the arena in use, the
inference period (the model's input stride times the feature step, 20 ms with the
model as shipped) and the free heap. Every 60 seconds of detection it logs the
average and maximum inference time, about 3.6 ms and 5.4 ms of that 20 ms period
on this board, plus the trigger task's stack high-water mark. The trigger reads
one feature step per call so that this timing is per inference rather than a
burst inside one read.

The trigger starts detection with a reset, which clears the frontend's window,
the model's streaming state and the probability history, so the words just before
a session cannot fire the next one. For the same reason a detection clears the
probability history, and the model has to hear a second of audio below the cutoff
before it can fire again, so one utterance cannot trigger repeatedly while
detection stays enabled. The first 100 ms of audio after capture starts are still
discarded: the codec and amplifier rails make a transient that would otherwise
hide or fake a wake word.

The KEY button is on GPIO10. The schematic shows a 10K pull-up (R18) to 3.3 V
and the switch to ground, so it reads low when pressed. The 100 nF debounce
capacitor (C22) is marked not fitted, so the firmware debounces the press. KEY
toggles the microphone mute; it is polled by its own task in `trigger.c` rather
than by the detection loop, because it has to work while the microphone is
muted and while a session owns the capture device.

BOOT is on GPIO9 and is polled by that same task with the same 50 ms debounce.
The schematic shows a 10K pull-up (R8) to 3.3 V and a 100 nF capacitor (C14) on
the pin, with the switch to ground. A press advances the full panel brightness
(see the display section), and the level is stored in NVS. GPIO9 is a strapping
pin, so holding BOOT through a reset still enters the ROM's download mode;
nothing in the firmware works around that, and a press while the firmware runs
is unaffected.

## Drawn faces

The faces are not images. `main/faces.c` builds a list of drawing primitives
for one face and `main/display.c` paints it with LVGL's `lv_draw_rect`,
`lv_draw_arc` and `lv_draw_line` from the face object's `LV_EVENT_DRAW_MAIN`
callback. Nothing is decoded and no pixel data is embedded, which makes the
application about 1 MB smaller than the old RGB565A8 face images did.

`faces_build(face, elapsed_ms, context, shapes)` fills a caller-provided array
of at most `FACE_MAX_SHAPES` and returns its length. It is pure: it has no LVGL
state, and the same arguments always produce the same shapes. `elapsed_ms` is
the time since the face appeared, so every animation starts from its first
keyframe when the face changes. The context carries the blink time and the
conversation geometry: the reply bubble height, the wrapped reply height, the
scroll offset and the user transcript bubble's size. The full-size faces ignore
the conversation fields. Display code adds drift when it draws, so the list
itself does not depend on where the face currently sits.

LVGL 9.5 has no ellipse or Bezier primitive, so the design is approximated:

- An SVG ellipse becomes a pill of the same bounding box.
- A quadratic curve `M x0 y0 Q cx cy x1 y1` becomes the circular arc through
  `(x0,y0)`, the curve's midpoint `((x0+2cx+x1)/4, (y0+2cy+y1)/4)` and `(x1,y1)`.
  The stroke width and round ends stay the same. `faces.c` keeps the original
  control points in the face definitions; `add_quadratic_arc()` converts them.
- A group opacity is mixed into the colour in advance against the black
  background, because overlapping translucent strokes would brighten at their
  crossings. The listening waves and the thinking dots keep a per-shape
  opacity, because they animate it.
- The listening tilt moves the centre of each eye and mouth the way a rotation
  about (240,240) would, and leaves the shapes upright. A real rotation would
  need an off-screen layer that does not fit in RAM.

`FACE_IDLE_DEFAULT` is the pill-eyed face with a 5 s breathe. `FACE_LISTENING`
adds the 3 s tilt and six fading waves. `FACE_SPEAKING` has bobbing eyes and
five talk bars. `FACE_MUTED` has dimmed eyes, a zipper and a crossed-out
microphone. `FACE_IDLE_HAPPY_EYES` is the pink cheeks and arcs, and
`FACE_IDLE_CURIOUS` is the big whites with the shifting look group.
`FACE_SURPRISED` has wide eyes with jittering pupils, raised brows, a quivering
zigzag mouth, four shock lines and a falling sweat drop, and the whole face
shakes for the first 450 ms of each 3 s cycle. `FACE_RELIEVED` has closed eyes,
blush, a small outlined mouth and three puffs that drift away, and it sighs
once over the 4 s it is shown. The conversation faces add both chat bubbles
(the filled robot bubble and the outlined user transcript bubble), the thinking
dots, the scrollbar and the mini faces. The geometry, colours and keyframes are
in the design sections of the `amo-wvlew`, `amo-ndndv`, `amo-zxkxh` and
`amo-rmixv` tickets, which follow the user's `docs/cute-robot-faces.html`
design. That file is a Claude artifact bundle: the boards are nested
gzip+base64 pages inside its `__bundler` manifest, and the SVG in the ticket
design sections is the reference, so it does not need to be unpacked.

## Face screen

`main/display.c` draws one face on a black background. The state word line is
gone. `voice_session` logs the transcript and passes it to
`display_set_transcript()` at STT_END; it is shown only in the conversation
screen, where it sits in the user bubble. The full-size layouts show neither
the transcript nor the conversation bubbles.

Which screen each state uses:

- Thinking: C1, a small bubble with three bouncing dots and the mini thinking
  face. When the transcript arrives, the user bubble appears above it and the
  dots bubble moves down.
- Speaking with reply text: C2, a chat bubble with the scrolling reply, a
  scrollbar and the mini speaking face, with the user transcript bubble above
  it when the transcript is non-empty.
- Speaking without text (shortcuts and error sounds): the full-size speaking
  face.
- Idle, listening, muted, error and no server: the full-size faces, as in
  `amo-wvlew`. Error and no server show their message in the bottom line.
- After speaking: the C2 bubble and the user bubble stay for 2.5 s with the
  mini idle face, at the final scroll position. Then the full-size face of the
  current state returns and both texts clear. An error switches to the full
  layout immediately, and any state change during the linger (a new wake word,
  for example) ends it.
- The full-size layouts never show the transcript. Empty transcripts produce
  no user bubble, so the layout is exactly the pre-transcript one.

The bottom message line starts at row 396 and is 84 px of Montserrat 20. It is
the space the transcript used to occupy and is only shown for error and no
server. The lowest full-size shape is the speaking bars, which end at row 356,
and 20 px of drift takes that to 376, so the message never covers the face. A
long message uses the same static-prefix ellipsis as the transcript, but with
three full stops because Montserrat 20 has no U+2026.

The conversation screen:

- The bubble starts at x28 y28, is 424 px wide, and its height is the wrapped
  reply height plus 52 px, capped at 262 px. The corners are radius 24 except
  for a radius-6 bottom-left corner. C1 uses a fixed 120x60 bubble.
- The reply is an LVGL label inside a clipping viewport at x50..422, from y34
  to 6 px above the bubble's bottom. The viewport is bubble height minus 12.
  The label starts 20 px below the viewport top at scroll 0 and is positioned
  every 33 ms from the same scroll offset as the face's scrollbar thumb.
- The label text is static, in `reply_text` in `display.c`, and set with
  `lv_label_set_text_static()`, so the reply is never copied into LVGL's heap.
  It uses DM Sans SemiBold 26, 4 bpp, line height 34 and colour #DCDCFF.
- Two 28 px gradient widgets fade the text into the bubble at the viewport's
  top and bottom edges. The gradient stops carry their own opacity, which the
  simple vertical gradient path supports without complex-gradient support.
- The scroll holds for 1.5 s after speaking starts, then moves at 16 px/s to
  the end and stops. The thumb's height is
  `track height * viewport height / (text height + 40)` and it moves over the
  scroll distance. Both track and thumb are drawn by `faces.c`; there is no
  scrollbar when the text fits.
- C1's dots are radius-6 circles at (70,60), (88,60) and (106,60) in #8A38F5,
  with a 1.2 s bounce and opacity cycle and delays of 0, 0.2 and 0.4 s. Its mini
  face is two pills, a brow line and a mouth line, with the eyes shifting as a
  group. C2's mini face is two pills and three bars; the linger's mini face is
  two pill eyes only.

The user transcript bubble:

- It is outline only: a 2 px #3A3456 border, no fill, text in #9A98C0, with a
  6 px tail at the bottom right. The width is the natural text width plus
  padding and border, capped at 340, and the right edge is at x452. An empty
  transcript means no bubble at all.
- C1 uses DM Sans Italic 24 (line height 32), 20x14 padding and radius 22. The
  text wraps to at most three lines and ends in an ellipsis. Its width is the
  natural text width plus 44 px, capped at 340, and the height is the used
  lines times 32 plus 32. The dots bubble moves to y = 28 + user height + 14.
- C2 and the linger use DM Sans Italic 18, one line with an ellipsis, 16x8
  padding, radius 18 and a fixed 44 px height. The reply bubble moves from y28
  to y84, and the reply viewport, fades and scrollbar move down by 56 px. The
  one-line label is 24 px high; the generated font's line height is 22, so the
  extra two pixels are the design's line box.
- The tail cannot use the filled-square trick because the bubble has no fill.
  The big bottom-right corner is erased with a black square, then a 12x12
  border-only rounded rectangle with only its bottom and right sides and a
  6 px radius forms the tail. Two more black squares erase the two corners that
  side selection still draws, and two filled straight edges rejoin the tail to
  the remaining big outline. It uses only rectangles, no arcs.
- The transcript is an LVGL label on top of the face's outline, static in
  `transcript_text` in `display.c` and set with `lv_label_set_text_static()`,
  so it is never copied into LVGL's heap. It is hidden in every full-size
  layout and cleared when the linger ends or a new session starts.
- LVGL's dots long mode refuses static text, so the shown text is built in a
  second static buffer: the longest UTF-8-safe prefix, preferably cut at a
  nearby space, plus U+2026. A binary search with `lv_text_get_size()` at the
  label's font and width finds the longest prefix that fits three lines in C1
  or one line in C2. It is recomputed whenever the layout changes between C1
  and C2. C1 uses WRAP and C2 uses CLIP, not DOTS.

The fonts are generated by `main/fonts/regenerate_font.sh` from the static
`DMSans-SemiBold.ttf` and `DMSans-Italic.ttf` at
`https://raw.githubusercontent.com/googlefonts/dm-fonts/4412393b7d2de9fe7a92064c2dce9b5af5d7fd26/Sans/fonts/ttf/`
with `lv_font_conv` 1.5.3, 4 bpp, and the ranges 0x20-0x7E, 0xA0-0xFF,
U+2013-2014, U+2018-2019, U+201C-201D and U+2026. The generated
`dm_sans_semibold_26.c`, `dm_sans_italic_24.c`, `dm_sans_italic_18.c` and the
OFL 1.1 text in `main/fonts/OFL.txt` are committed.

The face is one unstyled `lv_obj` covering the 480x480 screen and created
before the labels, so the text is drawn above it. Everything that changes on
its own runs on LVGL timers in the LVGL task, so there is no extra task:

- Animation: every 33 ms it rebuilds the current shape list and compares it
  with the previous one. A face that does not move after rounding causes no
  repaint. The faces that contain arcs (idle, listening, muted, happy eyes,
  curious, surprised and relieved) merge all changed shapes into one
  invalidation box: LVGL redraws the face once per invalidated area, and every
  arc re-initialises its circle masks, so splitting them into many small areas
  multiplies that work. An arc invalidates its whole circle rather than a band
  because LVGL's software arc renderer can spin forever in `draw_buf_flush()`
  when a large arc is clipped to a band; the listening waves (radius about
  160 px) reproduced a watchdog reset that way. The speaking and conversation
  faces contain no arcs and invalidate each changed shape separately, so a
  moving scrollbar does not repaint the mini face and the mini face does not
  repaint the scrollbar. A face change, a drift step on a full-size face, or a
  change in the number of shapes invalidates the whole face object.
- Blink: the eyes of the current face close from full height to 0.08 over
  90 ms and reopen over 160 ms, 250 ms in total, every 4 to 8 seconds, in
  idle and muted. Each segment uses the same smoothstep as the other
  animations. The design's own periodic blink is not used and a variety
  moment does not blink.
- Variety: every 3 to 6 minutes, idle only, `idle_happy_eyes` for 3 to 5
  seconds or `idle_curious` for exactly 6 seconds. The 6 seconds are one full
  look cycle (`FACE_CURIOUS_LOOK_PERIOD_MILLISECONDS` in `faces.h`), so every
  curious moment looks left and right and ends with the eyes centred.
- Drift: a step of up to 4 px every 2 to 5 minutes, added to every shape of
  the full-size faces and clamped to ±20 px from home. The conversation screen
  is fixed, because its text cannot move with the face canvas; a step while it
  is shown only updates the saved offset that the next full-size face uses.
- Linger: 2.5 seconds after speaking with a reply and returning to idle or
  muted, the bubble, text and scrollbar clear and the full face returns. Error
  and no-server messages stay.
- Dimming: after 30 seconds in idle or muted the panel goes from the chosen
  full level to 25%. BOOT steps the full level through 40, 60, 80 and 100%,
  wrapping to 40, applies it at once even when dimmed, and writes it to NVS;
  the default is 60%. A press restarts the countdown in idle or muted. Any
  state change restores the full level and restarts the countdown, and a
  session pauses it.
- Motion: picking the board up in idle or muted replaces the state's face with
  the surprised face, at the full brightness and with the dim countdown
  paused. A blink or variety moment is dropped. Putting it down shows the
  relieved face for 4 s, then the state's face returns and the countdown
  restarts; moving again during those 4 s returns to surprised. No blink or
  variety moment runs during the override, and drift applies as on any
  full-size face. Any state change ends it and shows the new state's face, and
  a session's conversation linger ignores it as if the session were still
  running. The last motion report is kept even when it cannot be shown, so
  becoming idle or muted while the board is already moving starts with the
  surprised face and a put-down during a session is not drawn as relieved
  afterwards.

LVGL's own heap is 24 KB (`CONFIG_LV_MEM_SIZE_KILOBYTES` in both
`sdkconfig.defaults` and the existing `sdkconfig`, since the latter overrides
the defaults). Sixteen kilobytes left only about 1.8 KB free at the listening
face's peak, and the conversation screen adds two permanent static labels, a
clipping viewport and two gradient widgets. Both text buffers are static and
the font data is in flash, so neither lives in this heap. The 38.4 KB draw
buffer is still allocated separately by esp_lvgl_port.

The labels change only when their text changes, so LVGL invalidates just those
areas and the scrolling label is clipped to its viewport. The one full-screen
invalidation is the touch recovery, which also restores the brightness in
effect (the init list resends the full level and the current level is applied
again afterwards).

A state change logs at info, as do drift steps, brightness presses, dimming and
the start and end of a variety moment and of the motion override. Each face also
logs the LVGL heap it leaves free and its peak use, once, a second after it
appears. Blinks log at debug, which the default INFO log level compiles out.


## Motion

`main/motion.c` owns the QMI8658 on the I2C bus `board_i2c_init()` creates. It
polls at 40 Hz and calls its callback, from the motion task, only on a
transition: moving after 200 ms above the thresholds, still after a second
below them. Those hold times are what separate a pick-up from a screen tap or a
knock on the desk. The moving thresholds are 0.5 rad/s of rotation and 0.15 g
of deviation from a running average of the acceleration vector; the still ones
are 0.25 rad/s and 0.06 g. The still thresholds sit lower than the moving ones
so a movement that ends slowly does not oscillate, and the gyroscope's initial
offset may be up to ±10 dps (0.17 rad/s), so the still rotation threshold has to
stay above the board's measured 0.07 rad/s of resting noise rather than just
above it.

The average is per axis, seeded with the first sample, and updated after the
deviation is taken, with a 0.5 s time constant (20 samples, 0.05 per sample). It
follows the board into whatever orientation it is put down in, which a value
measured once at boot could not: the resting magnitude is 1.097 g upright and
0.968 g lying on its left side, so a board booted in one orientation and put down
in another never came back below the still threshold. A failed read leaves the
average as it is rather than reseeding it, so a gap on the bus cannot hide a
movement that started while it was down.

Movement start and stop log at info with the peak rotation and acceleration
deviation seen, which is what the thresholds should be tuned against. If the
IMU does not answer at 0x6B or reports an unexpected identity, `motion_init()`
logs an error and starts no task, so the callback never fires and the rest of
the firmware runs as before. The callback must not block; that is why
`display_set_moving()` only takes LVGL's lock and updates state.


## Voice session

`main/voice_session.c` connects the trigger, the API server, the microphone and
the player into the flow `havpe-server` drives. One session task owns all of it:
the trigger, API and player callbacks only post messages to its queue, because
`trigger_disable()` and `player_stop()` wait for those callbacks' tasks and
calling them from a callback would stall or deadlock that task.

States are idle, awaiting response, streaming, thinking and speaking. A trigger
with no subscribed API client shows "No server" and leaves the trigger enabled.
Otherwise the trigger is disabled, `VoiceAssistantRequest(start=true)` goes out
and the screen shows the listening face at full brightness. The client answers
with port 0, after which the session reads 512 samples (32 ms, 1024 bytes) at a
time and sends each as one `VoiceAssistantAudio` frame; it handles its own queue
between chunks.
`STT_VAD_END`, `ERROR` and `RUN_END` stop the stream. `TTS_END` plays the URL and
the player completion returns to idle; `RUN_END` alone returns to idle once it
arrives, unless playback is still running. A client disconnect stops streaming
and playback. Streaming is cancelled with `VoiceAssistantRequest(start=false)`
after 20 seconds without a stop event; the response and thinking waits have
their own shorter timeouts so a wedged client cannot keep the trigger disabled.

KEY toggles the microphone mute and does not start a session. Muting disables
detection and releases the capture device; the idle screen shows the muted
face. Unmuting re-enables detection. A press during a session only records the
requested state, which `return_to_idle()` applies when the session ends, so the
reply is never cut off. Mute starts false on every boot and is not stored.

`CONFIG_SESSION_SIMULATED_TRIGGER` raises a simulated wake word trigger three
seconds after an API client connects, for bench checks without speaking to the
board. It is off by default and is ignored while muted.

## Robot body

`body/` holds the 3D-printable EVE-style body that the board sits on. It is an
OpenSCAD model, not firmware, and the ESP-IDF build ignores it.
`body/eve_robot.scad` is the whole model: `part = "assembly"` (the default)
shows the assembled robot with a dummy screen, and `print_body`, `print_neck`,
`print_arm_left` and `print_arm_right` give each part in its print orientation.
`body/export.sh` writes `body/stl/*.stl` and three `preview_*.png` renders and
fails if a part does not sit on the bed. It needs `openscad` (or `OPENSCAD`
set) and `python3`. The outputs are git-ignored. `body/README.txt` has the
print settings, the assembly steps and the untested assumptions, for example
that the USB-C plug alone holds the screen.

## Gotchas

- **Freeing the serial port.** If flashing fails because `/dev/ttyACM0` is busy, find the holder with `fuser -v /dev/ttyACM0` (or `docker ps` for a leftover IDF container) and stop only that process. Never use `pkill -f` with a short pattern: `pkill -f tio` once killed the user's whole graphical session.

- **Console is USB only.** ESP-IDF's default UART0 console uses GPIO16/GPIO17, which are the IMU INT1/INT2 lines here. `sdkconfig.defaults` moves the console to the USB-Serial-JTAG port. An existing `sdkconfig` overrides `sdkconfig.defaults`, so after changing defaults, change `sdkconfig` too or delete it (you then have to set Wi-Fi credentials again).

- **Frame buffer does not fit in RAM.** One 480 × 480 frame at 16 bits per pixel is about 460 KB, nearly all of the 512 KB SRAM. Draw in partial strips (for example, LVGL partial render mode with a buffer of a few tens of lines). Full-screen animation and video will be slow.
- **Waveshare's ESP-IDF and Arduino examples swap two pins.** Their `user_config.h` sets LCD CS = GPIO5 and touch INT = GPIO15. The schematic and Waveshare's XiaoZhi board config (`main/boards/waveshare/esp32-c6-touch-amoled-2.16/config.h`) use LCD CS = GPIO15 and touch INT = GPIO5. Trust the schematic.
- **SD card and display share one SPI bus** (GPIO0 to GPIO2). Use one SPI host with two devices, and do not access both at the same time without bus locking.
- **The AXP2101 must be set up before peripherals work.** The Waveshare examples enable its ALDO rails at boot. The display, microphones and other parts may not have power until then.
- **Display connector wiring (J4).** `LCD_RESET` is not on a GPIO: it is pulled up to the ALDO3 rail through R16 (10K), so the panel only resets on an ALDO3 power-up or a dip/noise on that line. The schematic's summary table says GPIO10 = `LCD_RESET`; that is wrong (pressing KEY on GPIO10 does not affect the panel). `LCD_TE` goes only to test pad TP20, not to a GPIO. `DSI_PWR_EN` is pulled up to 3.3 V (always on).
- **Unsolved: the screen sometimes goes black while idle,** a few minutes after boot, while everything else keeps running. Touching the screen (which re-sends the panel init list) brings it back, so the panel has lost its configuration. Leading hypothesis: a short ALDO3 dip or noise on the reset line, possibly from the charger cycling at its 4.1 V end point (the battery sits at about 4.09 V in CV charging). Not proven; see the open ticket.
- **Unsolved: mute sometimes toggles by itself,** about 44 to 45 seconds after boot. It was seen in 3 of 5 boots during the microWakeWord work (2026-11-08). The log shows "Key button pressed" with nobody at the board, and in two of the three cases there was no touch event near it. The firmware accepts a KEY (GPIO10) press only after the pin reads low for a stable 50 ms, so this is probably electrical: a real low pulse on GPIO10, not contact bounce. It may share a cause with the black screen above, for example a rail dip, but that is a guess. Not investigated yet. Check whether it also happens on the WakeNet build (commit 493b5ae) and log the GPIO10 level and the AXP2101 status at the moment it happens.
- **The CO5300 does not answer DCS reads over QSPI on this board.** Reading 0x0A (Read Display Power Mode) through `esp_lcd_panel_io_rx_param` with the 32-bit form `{0x03, 0x00, command, 0x00}` returns 0xFF. Reading SIO0 in 3-wire mode, reading MISO, multi-byte reads with dummy bytes and a 1 MHz clock all returned 0xFF, so the panel is not driving either line. Do not base a display health check on register reads without proving one works first.
- Unverified: the CO5300 init sequence in the XiaoZhi board file sets a column window of 0x16 to 0x1AF (410 columns). This looks copied from the 2.06 inch board. Check it against the panel if the image is offset or clipped.
