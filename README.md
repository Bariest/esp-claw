# mp4-claw

Firmware for the **MangDang MP4 ESP32 CORE** quadruped, built on
[ESP-Claw](https://github.com/espressif/esp-claw).

ESP-Claw supplies the agent runtime — conversations, memory, skills, Lua
extensions, capability dispatch, MCP. This repository adds the robot: the
servo driver boards, the gait generator, the WASM skill runtime, the board
definition for the MP4 ESP32 CORE, and the MPX-Dog PWA.

## Layout

```
CMakeLists.txt            project root
boards/mp4_esp32_core/    board definition (ST7789, ES7210, MAX98357A, BMI270)
components/               product-owned components
main/                     application entry point and service wiring
fatfs_image/              resources baked into the SYSTEM and DATA partitions
third-party/esp-claw/     ESP-Claw, as a pinned git submodule
tools/cmake/              build helpers (partition selection, IDF patches)
.agents/                  design notes, including the integration plan
```

**ESP-Claw is never edited.** Every one of its components enters the build as
an IDF Component Manager `path:` dependency declared in `main/idf_component.yml`,
and everything this repo adds hangs off documented extension points
(`app_capabilities_register_external_group`, `cap_lua_register_module`,
`claw_cap_register_group`, `claw_event_router_register_outbound_binding`).
Taking an upstream update is a submodule bump, not a merge.

## Build

Requires Python 3 and an ESP-IDF 5.x environment with ESP32-S3 support.

```bash
source /path/to/esp-idf/export.sh
python -m pip install -r requirements.txt
git submodule update --init --recursive

idf.py bmgr -c ./boards -b mp4_esp32_core
idf.py build
idf.py -p PORT flash monitor
```

`idf.py set-target` is not needed — the target comes from `chip:` in
`boards/mp4_esp32_core/board_info.yaml`.

> **Rerun `idf.py bmgr` after every edit to a board YAML file.** Otherwise the
> stale generated C under `components/gen_bmgr_codes/` is silently compiled
> instead.

The console is the CH340K on UART0, not USB-Serial-JTAG.

## Plan

`.agents/mp4-integration-plan.md` is the phase-by-phase integration plan, with
the hardware map, the pin table, and the reasoning behind the structure.
