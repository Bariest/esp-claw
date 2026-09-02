# mp4-claw

Firmware for the **MangDang MP4 ESP32 CORE** quadruped, built on
[ESP-Claw](https://github.com/espressif/esp-claw).

ESP-Claw supplies the agent runtime — conversations, memory, skills, Lua
extensions, capability dispatch, MCP. This repository adds the robot: the servo
driver boards, the gait generator and inverse kinematics, the WASM skill
sandbox, the board definition, the `/v1` API, and the MPX-Dog PWA.

The thing that makes it a *robot* rather than a dev board is that the agent has
the robot's own tools. Ask it to walk, or to look sad, and it calls
`robot_move` or `display_show_emotion` itself — the same way it would search
the web.

---

## Quick start

```bash
source /path/to/esp-idf/export.sh
python -m pip install -r requirements.txt
git submodule update --init --recursive

idf.py bmgr -c ./boards -b mp4_esp32_core   # generate board code
idf.py build
idf.py -p PORT flash monitor
```

`idf.py set-target` is not needed — the target comes from `chip:` in
`boards/mp4_esp32_core/board_info.yaml`. The console is the CH340K on UART0,
not USB-Serial-JTAG.

Two rules that cost real time when forgotten, both covered in `AGENTS.md`:

- **Rerun `idf.py bmgr` after editing anything under `boards/`** — including
  `sdkconfig.defaults.board`, which is copied into a generated file that is the
  one actually on `SDKCONFIG_DEFAULTS`.
- **Config defaults only apply to a fresh `sdkconfig`.** An existing one wins,
  silently.

---

## The one rule

**Nothing under `third-party/esp-claw/` is ever edited.**

It is a pinned submodule. Every extension goes through a documented seam:

| Seam | Used for |
|---|---|
| `claw_cap_register_group()` | registering tools the agent can call |
| `app_capabilities_register_external_group()` | announcing those groups at boot |
| `cap_lua_register_module()` | adding Lua modules |
| `claw_event_router_register_outbound_binding()` | routing agent replies to a channel |

Every ESP-Claw component enters the build as an IDF Component Manager `path:`
dependency declared in `main/idf_component.yml` or a component's own manifest.
Taking an upstream update is a submodule bump, not a merge.

`tools/check_paths.py` enforces the boundary: every `path:` dependency must
resolve inside the repo, no relative path may escape it, and no source file may
be zero bytes. Run it before committing.

---

## Layout

```
CMakeLists.txt              project root; partition/model staging, esp-sr patch
boards/mp4_esp32_core/      board definition — pins, peripherals, devices
components/                 product-owned components (below)
main/                       entry point, service wiring, boot order
fatfs_image/                files baked into the SYSTEM and DATA partitions
third-party/esp-claw/       ESP-Claw, pinned submodule — never edited
tools/                      build helpers, check_paths.py, gif_to_mpxa.py
docs/                       animations, SDK connection, bring-up order
```

### Components

| Component | Lines | What it is |
|---|---:|---|
| `mpx_robot` | 5161 | Servos, gait, kinematics, IMU |
| `http_server` | 6149 | The whole web layer: 76 routes plus static serving |
| `mpx_wasm` | 5030 | WASM skill sandbox, MPXE decryption |
| `cap_display` | 919 | Display tools and the drawn face |
| `app_config` | 664 | NVS-backed settings |
| `cap_robot` | 431 | Robot tools for the agent |
| `cmd_wifi` | 315 | Console Wi-Fi commands |
| `cap_mpx_skill` | 223 | Skill tools for the agent |
| `mpx_util` | 555 | Log and trace rings |
| `gen_bmgr_codes` | 682 | **Generated** by `idf.py bmgr` — do not edit |

---

## How it works

### Boot, in order

`main/app_main()` runs this sequence. The order is load-bearing in several
places, noted where it matters.

```
mpx_log_ring_init()          first statement — before anything can log
init_nvs()
app_config_init/load()       settings from NVS
esp_board_manager_init()     brings up I2C, I2S, SPI, LEDC from the board YAML
app_fs_init()                mounts the SYSTEM and DATA partitions
claw_paths_set()             tells ESP-Claw where those live

mpx_robot_init()             SPI3, four AT32 boards, BMI270. Non-fatal.
mpx_wasm_init()              WAMR runtime. Non-fatal.

wifi_manager_init()
app_claw_ui_start()          starts the display service
cap_display_face_start()     ← face appears here, before Wi-Fi
http_server_init()
wifi_manager_start()
main_apply_ap_ip()           moves the AP to 192.168.2.1
http_server_start()
captive_dns_start()          after the AP is renumbered, not before

app_register_robot_capabilities()   ← must precede app_claw_start()
app_claw_start()             the agent comes up
http_server_webim_bind_im()
mpx_wasm_start_skills()      last — after the web UI can report failures
register_wifi_command()
```

Two of those orderings are not stylistic. `app_register_robot_capabilities()`
must run before `app_claw_start()`, because that is when external capability
groups are collected. And `main_apply_ap_ip()` must run before
`captive_dns_start()`, because renumbering restarts the DHCP server, which
discards the DNS option the captive portal writes into it.

The face starting before Wi-Fi is deliberate: on a robot that takes seconds to
find a network, eyes that are already blinking are the difference between
"booting" and "broken".

### Talking to it

This is the path that replaced the cloud backend.

```
browser ──ws──▶ /v1/chat/ui ──▶ cap_im_local_emit_user_message("web", session, text)
                                              │
                                        claw_core  (the agent loop)
                                              │
                          ┌───────────────────┼───────────────────┐
                     robot_move        display_show_emotion    web_search …
                                              │
                     claw_event_router ──▶ local_send_message
                                              │
        webim's cap_im_local callback ──▶ mirrored to the PWA socket ──ws──▶ browser
```

`http_server_mpx_chat_api.c` owns the browser end. Inbound, a
`user_chat_input` frame becomes `cap_im_local_emit_user_message()` on channel
`"web"` with the PWA's session UUID as the chat_id — the same door
`/api/webim/send` uses, so a conversation is one conversation whichever screen
opened it.

Outbound has a wrinkle worth knowing. `cap_im_local` has exactly **one**
outbound callback slot, and `http_server_webim_api.c` holds it. Rather than
fight for it, that callback mirrors every `"web"` message into the chat module,
which delivers only to sockets watching that chat_id. One owner, two views.

Agent progress notes ("Round 2: display_show_text(…)") arrive through the same
callback, because `claw_core` publishes them as ordinary out-messages. The
event's "stage" kind does not survive the send_message cap, so the only thing
left to match on is the prefix the stage publisher stamps on them — that is how
they become `step` frames rather than answers.

Replies over 2 KB are chunked, and **the split backs off to a UTF-8 character
boundary**. Each chunk is its own JSON text frame; a frame carrying half a
character is invalid UTF-8, the browser's `JSON.parse` rejects it, and
reassembly happens after decoding, so it can never repair a split the encoder
made.

### Moving

`mpx_robot` is the port of the original firmware's robot layer.

```
robot_move tool ──▶ mpx_robot_c_api ──▶ robot.cc ──▶ stanford_gait
                                                          │
                                              stanford_kinematics (IK)
                                                          │
                                             driver_board.c  (SPI3)
                                                          │
                            CN3 / CN4 / CN5 / CN6 → four AT32F413 boards, 3 servos each
```

Chip selects are GPIO 15, 7, 4, 5 on connectors CN3–CN6, with MOSI 6, CLK 16,
MISO 17 — all verified against the schematic.

Two things to know:

- **Two position frames exist.** The gait frame and the AT32 frame are related
  by `at32_raw == 1024 - gait_raw`. Mixing them silently inverts a joint.
- **`MP4_ROBOT_SERVO_BOARD_VARIANT` (1–5)** selects how logical servo IDs map to
  physical channels, because legs are not always plugged in the same order.
  Nothing validates it. A wrong variant drives the wrong joint, which is why the
  bring-up order says to power the servo rail off for the first boot.

`mpx_robot` deliberately exposes `src/` as an include directory — a documented
exception, because 45 `robot::` symbols form the skill ABI that `mpx_wasm`
compiles against.

### Running skills

`mpx_wasm` is WAMR plus the MPX ABI: **version 4, 74 host functions, frozen.**
Skills built with the MPX SDK against ABI 4 run unchanged.

Encrypted `.mpxe` skills are AES-256-GCM, hardware-bound to
`CONFIG_MP4_ROBOT_UUID` as the AAD. Changing that UUID makes every purchased
skill fail to decrypt.

The skill filesystem is a **chroot into `<DATA>/mpx_skills`**, and any path
containing `..` is refused outright rather than normalised — the input comes
from a marketplace, so the strict rule is the safe one.

`mpx_robot` and `mpx_wasm` would be a dependency cycle (the robot needs to know
whether a skill owns the pose; skills need to move the robot). It is broken with
a hook table that `mpx_wasm` registers into `mpx_robot` at init.

### The web layer

`http_server` serves 76 routes in 88 handler slots. Registration goes through
`http_server_register_uri_table()`, which **logs the URI of anything that fails
to register** — httpd silently refuses registrations once the table is full, and
the original firmware lost a day to exactly that.

| Group | Routes | File |
|---|---:|---|
| Robot control | 9 | `http_server_mpx_robot_api.c` |
| Skills | 12 | `http_server_mpx_skills_api.c` |
| Servo Studio | 12 | `http_server_mpx_studio_api.c` |
| Filesystem | 4 | `http_server_mpx_fs_api.c` |
| Lua | 6 | `http_server_mpx_lua_api.c` |
| Wi-Fi | 5 | `http_server_mpx_wifi_api.c` |
| Marketplace proxy | 5 | `http_server_mpx_market_api.c` |
| Chat | 1 | `http_server_mpx_chat_api.c` |
| Static + catch-all | 2 | `http_server_mpx_web.c` |

Four of these are not simple ports:

**`/v1/lua`** is rewired onto ESP-Claw's `cap_lua` and shares its script store
(`<DATA>/scripts`), so a script saved from the PWA is one the agent can also
run. `enqueue` keeps a serialising worker task, because `cap_lua`'s async runner
rejects same-group jobs rather than queuing them, and the deploy flow posts a
skill's scripts in order.

**`/v1/wifi`** applies changes to the radio, not just NVS — the phone doing the
setup is usually on the robot's own AP, so a reboot mid-flow drops it off the
network. Disconnect is spelled "reapply with no station SSID", because
`esp_wifi_disconnect()` alone gets undone by wifi_manager's reconnect timer.

**The marketplace proxy** runs on `esp_http_client` with a 15 s GET cache and a
three-in-flight cap. Both exist because each proxied request costs two of the
sixteen LWIP sockets, and the Store screen fires several at once.

**Static serving** streams both web bundles from the read-only system partition
rather than embedding them as `.rodata`. One wildcard GET handler covers the
whole PWA — assets and client-side routes — registered last, because httpd hands
a request to the first entry that matches in registration order.

### The face

`cap_display` draws two rounded-rectangle eyes: 59×114 at radius 8, `#FFE631`
on black, centres at (82, 120) and (236, 120), blinking every 2.5–3.5 s with a
~15 % widening as they close. Geometry measured from the reference GIF.

Drawn, not decoded. Every frame of that animation is two rectangles, so two
LVGL objects with an animated height reproduce it for ~2 KB of code and no
assets — against ~150 KB of RAM for a decoded frame buffer. And unlike a GIF it
is programmable, which is what lets `display_show_emotion` animate between ten
expressions.

**The face is the home screen**, and is also the display service's *default*
screen — what the panel falls back to when no session owns it. That matters
because the agent can write Lua that takes the display; when the script calls
`display.deinit()`, the panel returns to the face rather than to ESP-Claw's
launcher, which this board has no touch controller to operate.

`display_show_text` borrows the screen for eight seconds and hands it back.

### Voice — partial

Wake word and offline command recognition via esp-sr. **The models are
installed; the runtime component is not written yet.**

esp-sr is *not* speech-to-text. WakeNet detects a wake word; MultiNet matches a
fixed list of phrases compiled into the firmware. Free-form speech needs a cloud
transcription service.

The planned seam is a single entry point, `mpx_voice_submit_text()`: recognised
commands go straight to `claw_cap_call()` — offline, no LLM round trip — and
anything else routes to `cap_im_local_emit_user_message()`, the same door the
chat bridge uses. Adding cloud STT later becomes a matter of calling that one
function with different text.

---

## Memory and partitions

16 MB flash, filled exactly:

| Partition | Offset | Size | Holds |
|---|---|---:|---|
| `ota_0` | `0x020000` | 4352 K | the app |
| `ota_1` | `0x460000` | 4352 K | OTA slot |
| `system` | `0x8A0000` | 2600 K | fonts, skills, both web UIs |
| `storage` | `0xB2A000` | 2048 K | user data, installed skills |
| `model` | `0xD2A000` | 2904 K | esp-sr models |

Last measured:

| | Used | Free |
|---|---|---|
| App image | 3,549,872 B of 4,456,448 | 20 % |
| Speech models | 2,756,427 B of 2,973,696 | 7 % |
| Internal DIRAM | 206,819 B of 341,760 | 134,941 B |

Internal DIRAM is the scarce resource, not flash. Anything large belongs in
PSRAM: `http_server_alloc_scratch_buffer()` prefers it, skill uploads prefer it,
and esp-sr's AFE will want ~49 KB internal plus ~776 KB PSRAM when it lands.

The web bundles live on the system partition specifically to keep them out of
the app image — as `.rodata` they would cost 199 KB of the app slot, paid twice
because `ota_1` is the same size.

---

## The board

Verified against `MP4ESP32_CORE_SCH_0827` sheets 2 and 3.

| Function | Pins |
|---|---|
| I²C (IMU) | SDA 1, SCL 2 — 1 kΩ pull-ups |
| SPI display | MOSI 40, SCK 41, CS 9, DC 39, BL 42 |
| I²S audio | MCK 38, BCK 14, WS 13, DOUT 45, DIN 12 |
| Servo SPI | MOSI 6, CLK 16, MISO 17; CS 15, 7, 4, 5 |
| IMU INT1 | GPIO 10 (R48 pulls it to 3V3) |
| Buttons | BOOT 0, WAKE 21 |

- **BMI270 is at `0x68`.** Pin 1 (SDO) ties to GND. Do not copy M5 CoreS3, which
  straps SDO high and uses `0x69`.
- **The BMM150 is not on the I²C bus.** Its SCK/SDI go to the BMI270's
  ASCX/ASDX — the accelerometer's own auxiliary master. It is deliberately not
  declared as a board device; `board_devices.yaml` records how to add it.
- **LCD reset is tied to the board reset net**, so `reset_gpio_num: -1`.
- **The backlight FET is P-channel** (Q2, SI2301), hence `output_invert: true`.
- **MIC3 is the amplifier's own output** fed back through a divider — the AEC
  reference channel.
- No touch controller. No SD card, so the data root is always `/fatfs`.

Three values are *not* schematic-derivable and remain unverified: `spi_mode`,
`pclk_hz`, and the panel orientation flags.

---

## Status

Everything builds. **Nothing has run on hardware.**

Working and untested: the whole `/v1` API, the chat bridge, the PWA, the face,
the robot stack, the skill runtime.

Not written: the voice runtime component, the MPXA animation player (the
converter exists and is verified — see `docs/animations.md`), and the BMM150
magnetometer, deferred until accel and gyro are confirmed.

Known rough edges:

- The PWA's marketplace **Deploy** button still posts generated Lua to
  `/v1/lua/enqueue`, which now runs a different interpreter. `mpx-cli install`
  does the same job over `/v1/skills/upload` and is unaffected.
- **No authentication** on the `/v1` endpoints, same as the old firmware.
- `MP4_GATEWAY_HOST` is empty, so the marketplace is off and those routes answer
  503. The local `mpx-cli` loop works without it.

`docs/mpx-sdk.md` has the bring-up order. The first step is *power the servo
rail off*.
