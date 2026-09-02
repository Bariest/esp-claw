# AGENTS.md

Firmware for the MangDang MP4 ESP32 CORE quadruped, built on ESP-Claw.

## The one rule

**Never edit anything under `third-party/esp-claw/`.** It is a pinned git
submodule. Everything this repo adds hangs off documented extension points, so
taking an upstream update is a submodule bump rather than a merge. If you find
yourself wanting to change an ESP-Claw file, the answer is almost always one
of: a capability group, a Lua module, a skill, a router rule, a board overlay,
or a component that shadows the ESP-Claw one by name.

ESP-Claw's own agent guide lives at `third-party/esp-claw/AGENTS.md` and is
worth reading for anything about the agent runtime itself.

## Layout

```
CMakeLists.txt            project root
boards/mp4_esp32_core/    board definition (ST7789, ES7210, MAX98357A, BMI270)
components/               product-owned components
  app_config/             persisted settings, projected into app_claw_config_t
  http_server/            device config server + embedded UI (shadows ESP-Claw's)
  cmd_wifi/               console wifi command
  mpx_robot/              the quadruped: servos, gait, IK, IMU
main/                     app entry point and service wiring
fatfs_image/              baked into the SYSTEM (read-only) and DATA (seed) partitions
third-party/esp-claw/     ESP-Claw, pinned submodule -- DO NOT EDIT
tools/cmake/              partition selection, ESP-IDF patches
.agents/                  design notes, including the integration plan
```

## Build

```bash
source /path/to/esp-idf/export.sh
python -m pip install -r requirements.txt
git submodule update --init --recursive

idf.py bmgr -c ./boards -b mp4_esp32_core
idf.py build
idf.py -p PORT flash monitor
```

`idf.py set-target` is not needed -- the target comes from `chip:` in
`boards/mp4_esp32_core/board_info.yaml`.

**Rerun `idf.py bmgr` after editing anything under `boards/`** -- not only the
YAML. `sdkconfig.defaults.board` is copied into the generated
`components/gen_bmgr_codes/board_manager.defaults`, and only that generated
file is on `SDKCONFIG_DEFAULTS`. Edit the board defaults without rerunning
bmgr and your settings are simply absent, with nothing to say so.

Config defaults only apply to a **fresh** `sdkconfig`; an existing one wins.
`bmgr` handles that for you when the *board changes*, but **not** when you edit
`sdkconfig.defaults.board` for the board already selected -- it logs
"Board unchanged, preserving sdkconfig" and your new defaults silently do
nothing. That case needs the delete:

```bash
idf.py bmgr -c ./boards -b mp4_esp32_core   # regenerate board_manager.defaults
rm sdkconfig                                # only needed when the board is the same
idf.py build
```

Either way, losing `sdkconfig` discards anything set through menuconfig -- the
LLM API key and `MP4_GATEWAY_HOST` being the ones that matter here. The backup
`bmgr` leaves at `components/gen_bmgr_codes/sdkconfig.bmgr_board.old` is where
to go looking for them.

The console is the CH340K on UART0, not USB-Serial-JTAG.

### On Windows: box-drawing characters

esp-sr's `model/movemodel.py` prints a box-drawing rule in its model report.
Under the default cp1252 console codec that raises `UnicodeEncodeError` and
fails the build at "Move and Pack models" -- before any of our code, with an
error that looks nothing like the cause. It is a bug in esp-sr, still present
on their master, so there is no version to bump to.

**Already handled.** The top-level `CMakeLists.txt` patches that script at
configure time to substitute unencodable characters rather than raise. It
re-applies on every configure, so a re-downloaded component cannot bring the
bug back. Nothing to set per shell.

Optional, for output that renders properly rather than as `ΓöîΓöÇ`:

```powershell
chcp 65001
```

That affects only how tables from `idf.py size` are drawn; the numbers are
correct either way.

### Switching between boards

```bash
idf.py fullclean                                    # recommended, see below
idf.py bmgr -c ./boards -b mp4_esp32_core           # or generic_esp32s3
idf.py build flash monitor
```

No `rm sdkconfig` here: **when the board changes, `bmgr` deletes `sdkconfig`
itself**, backing it up to
`components/gen_bmgr_codes/sdkconfig.bmgr_board.old`. It clears `CMakeCache.txt`
too, so no separate reconfigure is needed either.

`bmgr` also regenerates `components/gen_bmgr_codes/board_manager.defaults`,
which is the only place a board's settings reach `SDKCONFIG_DEFAULTS` -- and
where `flash_partition_defaults.cmake` scrapes `CONFIG_ESPTOOLPY_FLASHSIZE_*`
to pick `partitions_<size>.csv`. So the partition table follows the board
automatically.

`fullclean` is not strictly required, but a board switch changes the partition
table, whether esp-sr is in the build at all, and the staged filesystem images.
A full rebuild costs ten minutes and removes every question about stale
artefacts -- worth it when the point of the exercise is to trust the result.

`idf.py flash` writes the bootloader, partition table, app and all three data
partitions, which is what a changed layout needs.

The console moves with the board: the MP4 uses the CH340K on UART0
(`ESP_CONSOLE_UART_DEFAULT`), the generic devkit uses USB-Serial-JTAG. Expect a
different port, and a board that appears to say nothing if you open the old one.

## How ESP-Claw components enter the build

Through IDF Component Manager `path:` dependencies in `main/idf_component.yml`,
not `EXTRA_COMPONENT_DIRS`. Two idioms:

- `rules:` -- hard requirement gated on a Kconfig symbol; the dependency is
  absent when the condition is false.
- `matches:` -- optional inclusion when the condition holds.

## Extension points

These are the seams. Prefer them in this order.

| Want to | Use |
|---|---|
| Give the agent a new tool | a capability group, registered with `app_capabilities_register_external_group()` from `app_main` **before** `app_claw_start()` |
| Give Lua a new module | `cap_lua_register_module(name, luaopen_fn)`, called from the module's own component |
| Give the agent long instructions | a `skills/<id>/SKILL.md` in your component; the build syncs it into the SYSTEM image automatically |
| Receive agent replies on your channel | `claw_event_router_register_outbound_binding(channel, "<send_handler>")` |
| Describe hardware | the board YAML, not C |

Capability descriptions are truncated at **256 bytes** (`CLAW_CAP_TOOL_DESCRIPTION_MAX`).
Long instructions belong in a SKILL.md, not the descriptor.

## Runtime paths

Never hard-code `/fatfs` or `/system`. Compose paths with
`claw_paths_join(CLAW_PATH_DATA, ...)` / `claw_paths_join(CLAW_PATH_SYSTEM, ...)`.
DATA may live on flash or an SD card depending on the board.

## Board YAML gotchas

- The generator emits `custom` device struct fields **in YAML order** and
  silently omits absent keys. A consuming C mirror struct must match field for
  field; use `-1` for unused GPIOs rather than dropping the key. A mismatch
  shows up as `ESP_ERR_INVALID_SIZE` at runtime if the consumer checks
  `cfg_size`, and as garbage if it does not.
- A scalar's C type is chosen from the literal's magnitude. Keep masks as
  strings.
- Device names are load-bearing: `display_lcd`, `lcd_touch`, `audio_dac`,
  `audio_adc`, `imu_sensor`, `magnetometer_sensor`, `fs_sdcard`,
  `lcd_brightness`.

## This board, specifically

Five things that are not obvious and have already cost time once:

1. The LCD reset pin is the board `RESET` net, not a GPIO (`reset_gpio_num: -1`).
2. There is no servo power-enable pin; GPIO 8 is on the expansion header.
3. The console is UART0 via CH340K, not USB-Serial-JTAG.
4. There is no touch controller.
5. ES7210 MIC3 is an echo-cancellation reference fed from the amplifier, not a
   microphone. Labelled `RE` in `board_devices.yaml`.

`boards/mp4_esp32_core/README.md` has the full pin table and the list of values
that still need confirming on hardware.

## Silent boot loop: read this before debugging anything else

Symptom: the terminal repeats the ROM banner (`ESP-ROM:esp32s3-...`, `load:`,
`entry 0x403c8920`) and nothing else. No `I (31) boot:` line, no app log, no
backtrace.

The missing **second-stage bootloader banner is the tell**. The ROM bootloader
always prints on UART0 no matter how the project is configured; everything
after `entry` prints on whatever `CONFIG_ESP_CONSOLE_*` selects. If you see one
and not the other, the console is on a port you are not watching -- almost
always USB-Serial-JTAG while you are on the UART bridge. The chip is not hung;
it is panicking and rebooting with the evidence on the other port.

`rst:0xc (RTC_SW_CPU_RST)` with `Saved PC: ... esp_restart_noos` confirms it:
that is a *software* restart, so app code ran and called it -- the panic
handler, in practice.

Both boards now default to UART0 primary with USB-Serial-JTAG as a secondary
sink, so this should not recur. If you change a console setting, change it in
`boards/<board>/sdkconfig.defaults.board` and remember the rule below.

## Editing sdkconfig.defaults.board does nothing on its own

`idf.py bmgr` deletes `sdkconfig` **only when the board changes**. On an
unchanged board it prints `Board unchanged, preserving sdkconfig` and your edit
never reaches the build -- the old value is already in `sdkconfig` and wins.

    idf.py bmgr -c ./boards -b generic_esp32s3
    del sdkconfig            # rm sdkconfig on POSIX -- this is the step people skip
    idf.py build

**`-c ./boards` is not optional.** Without it bmgr scans only
`managed_components/` and `components/`, finds the thirteen Espressif dev
boards and none of ours, and fails with `Board "generic_esp32s3" not found`
followed by a list that looks authoritative and is not. Passing the board as a
positional argument (`idf.py bmgr generic_esp32s3`) is also rejected as of
esp-bmgr-assist 0.8.2 -- it prints a usage hint and exits **0** having done
nothing, which reads like success.

This is what made the esp-sr model options appear to be ignored for an
afternoon. Check `components/gen_bmgr_codes/board_manager.defaults` if you want
to see what was actually handed to the build.

## The plan

`.agents/mp4-integration-plan.md` is the phase-by-phase integration plan, with
the hardware map and the reasoning behind the structure. Phases 0-2 are done.
