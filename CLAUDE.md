# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

GhostESP is an ESP-IDF (not Arduino) firmware for ESP32-family chips: Wi-Fi/BLE/802.15.4 security tooling, an LVGL UI, a serial CLI, a WebUI, and a native SD app runtime. One codebase builds ~61 *board targets* from ~63 Kconfig profiles in `configs/`.

## Building

There is no test suite. "Verifying a change" means compiling for the affected board target(s).

Two supported paths:

```bash
# Interactive/scripted helper (downloads ESP-IDF if missing, drives menuconfig, packages zips)
python build.py                      # interactive board picker
python build.py --targets 2 3        # by index from get_build_targets()
python build.py --targets all
python build.py --menuconfig --idf-path ~/esp-idf
```

```bash
# Docker, no local toolchain (see tools/docker/README.md)
tools/docker/build.sh m5core2_aws esp32     # <config suffix> <chip>
```

```bash
# Raw ESP-IDF, mirroring what CI does
. ~/esp-idf/export.sh
rm -f sdkconfig sdkconfig.defaults
cp configs/sdkconfig.<board> sdkconfig
cp configs/sdkconfig.<board> sdkconfig.defaults
export IDF_TARGET=esp32s3            # must match the config's chip
idf.py clean && idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Key build facts:

- **ESP-IDF v6.1** is what CI builds against (`git clone -b v6.1`). `build.py`'s auto-download offers v6.0.2/5.5.x instead — prefer 6.1 when reproducing CI.
- `sdkconfig` **and** `sdkconfig.defaults` must both be the board profile; copying only one silently builds the wrong config.
- `IDF_TARGET` is read from the environment by the root `CMakeLists.txt`. Always `idf.py fullclean` (or delete `build/`) when switching boards or chips.
- The root `CMakeLists.txt` injects `GIT_COMMIT_HASH`, `GIT_BRANCH`, and `GHOSTESP_BUILD_NUMBER` (from `GITHUB_RUN_NUMBER` in CI) as compile definitions; the OTA manifest compares on the build number. The hash is captured at *cmake configure* time, so **commit before building** — a build started with uncommitted changes stamps the previous commit into the banner, and any log a user sends back then names the wrong commit. `idf.py reconfigure` re-captures it without a full rebuild.
- `main/CMakeLists.txt` globs `main/**/*.c`, then *filters out* sources that must stay conditional (BLE on S2, camera managers, `infrared_view.c`, `i80_display.c`, most Flipper NFC parsers). A new file that must not always compile has to be excluded there, not just `#ifdef`-guarded.
- Managed components come from `main/idf_component.yml`, several gated by `rules: if: target in [...]`. Vendored components live in `components/`.

## CI (`.github/workflows/compile_all.yml`)

Runs on `pull_request` and `workflow_dispatch` (inputs: `build-only` / `prerelease` / `all`). It is **not** triggered by pushes to a branch — open a PR or dispatch it manually.

The job is a matrix, one entry per board:

```yaml
- { name: "...", idf_target: "esp32s3", sdkconfig_file: "configs/sdkconfig.<board>",
    zip_name: "<Board>.zip", ota: true, ota_slot_size: 4001792, board_key: "<board>" }
```

- `ota: true` adds a hard check that `build/Ghost_ESP_IDF.bin` fits `ota_slot_size`; `board_key` links the target to its entry in `firmware-manifest.json` for OTA.
- The same matrix is **duplicated** in `build.py:get_build_targets()`. Adding a board means editing both, plus `configs/` and the README board table.
- The workflow patches ESP-IDF **in place** before building (gdbstub compatibility, `ESP_WIFI_CACHE_TX_BUFFER_NUM`); it also appends `# CONFIG_FATFS_USE_DYN_BUFFERS is not set` / `# CONFIG_ESP_GDBSTUB_ENABLED is not set` to both sdkconfigs. A stock v6.1 checkout does **not** build without the gdbstub patch (`command_name_matches`, `portNUM_PROCESSORS`, `StaticTask_t` errors in `components/esp_gdbstub`), so run the workflow's patch step before building locally.
- Packaging reads the app offset out of the produced partition table rather than assuming `0x10000` — OTA layouts put the first app slot at `0x20000` or higher.

## Adding or changing a board target

1. `cp configs/sdkconfig.default.<chip> configs/sdkconfig.<board>` and edit (or `idf.py menuconfig` then copy `sdkconfig` back).
2. All board/feature switches live in `main/Kconfig.projbuild` (~1900 lines: Display, LED, SPI/MMC, GPS, NFC, NRF24, SubGHz, Misc). Board-specific behaviour is expressed as `CONFIG_*` symbols consumed with `#ifdef`, not as per-board source files.
3. Pick or add a partition CSV (`partitions*.csv`; `partitions_ota_*.csv` for OTA-capable boards) via `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`.
4. Add the matrix row in `.github/workflows/compile_all.yml` **and** `build.py:get_build_targets()`.
5. Update the board matrix table in `README.md` and add a `CHANGELOG.md` line.
6. Docs source is `docs/hugo docs/content/latest/**.md` (`docs/hugo docs/public/` is generated output — don't hand-edit).

## Architecture

Everything is C on ESP-IDF, started from `app_main()` in `main/main.c`, which brings up subsystems in a fixed order (serial → Wi-Fi/Ethernet → commands → settings → OTA → SD → managers → input → display) with each step wrapped in `MEASURE_INIT_RAM` for heap accounting. Headers mirror sources: `main/<dir>/x.c` ↔ `include/<dir>/x.h`.

Layers:

- **`main/core/`** — CLI and plumbing. `commandline.c` owns a command table populated by `register_commands()`; handlers live in `main/core/commands/cmd_*.c` and are declared in `include/core/commands.h`. Adding a command = handler in the right `cmd_*.c`, prototype in `commands.h`, one `register_command()` line. `serial_manager.c`/`esp_comm_manager.c` carry the same command surface over USB/UART, and the WebUI, Flipper app, and GhostLink peers all funnel into it — a command written once is available on every front end.
- **`main/managers/`** — long-lived subsystem owners (`wifi_manager`, `ble_manager`, `display_manager`, `sd_card_manager`, `settings_manager`, `ota_manager`, `plugin_manager`, `gps_manager`, NFC/IR/SubGHz/NRF24, audio, RGB…). Most expose `*_init()` called from `main.c` and a small task.
- **`main/managers/views/`** — one file per LVGL screen, each registering a `View` (see `include/managers/display_manager.h`) with `display_manager_register_view()`; `display_manager.c` (~5.5k lines) drives the LVGL loop, input routing, and view switching. `main/gui/` holds shared widgets, theming (`theme_palette.c`, `design_tokens.h`), menu layout and `menu_catalog.c`.
- **`main/attacks/`** and **`main/scans/`** — the radio work itself, split wifi/ble/ethernet; these are the functions CLI commands and views both call.
- **`main/vendor/`** — board/chip drivers (AXP PMU, AW9523, PCF8563, CH422G, ST7262, e-paper, M5GFX wrapper, GPS, PCAP writer).
- **`plugins/`** — native SD app SDK (`plugins/sdk/`), the `gbt` build tool and packaging scripts (`plugins/tools/`), and example apps. Apps need `CONFIG_SPIRAM`.
- **`webui/`** — small JS build (`node build.mjs`, then `html_to_header.py` to embed it into the firmware).
- **`ota_updater/`** — separate small ESP-IDF project for the two-stage Banshee C5 updater.

## UI gotchas (hard-won)

These are not obvious from the code and have each cost a hardware debugging round trip.

**Touch does not go through LVGL.** Only the JC3248W535EN registers a real `lv_indev` (`components/axs15231b/lv_port.c`). On every other touch board `hardware_input_task` in `display_manager.c` polls `touch_driver_read()` itself and pushes `INPUT_TYPE_TOUCH` events into `input_queue`, which `input_processing_task` hands to the current view's input callback. Consequences:

- **`LV_EVENT_CLICKED` never fires from touch.** `lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, …)` is dead code on those boards; a view must hit-test the touch point against `lv_obj_get_coords()` in its own input callback. Several views register both and only the manual path runs.
- `dm_update_manual_touch_pressed_state()` applies `LV_STATE_PRESSED` to the object under the finger before the event reaches the view. So **a button visibly highlighting on press proves nothing about the click being handled** — that feedback is cosmetic and independent of the view's logic.
- Move events only reach views listed in `touch_move_events_enabled_for_view_name()`; others see just press and release.

**A view that switches to itself.** `display_manager_switch_view(&self)` is how ~10 views rebuild themselves after changing internal state (wizard step, SD browser directory, options menu). It builds a bare `{GUI_ROUTE_VIEW, view}` route, which `gui_router_navigate_immediate()` would drop as equal to the top of the stack; `gui_router_legacy_switch_immediate()` special-cases it into a re-render. Don't "clean up" that branch.

**LVGL memory is a fixed pool by default, and it fails hard.**

- `CONFIG_LV_MEM_SIZE_KILOBYTES` is **silently ignored**: LVGL's Kconfig never defines `CONFIG_LV_MEM_SIZE`, so `lv_conf_internal.h` falls back to `LV_MEM_SIZE = 48KB` regardless of what the sdkconfig says.
- When that pool runs out there is no graceful failure. `lv_mem_alloc` → `LV_ASSERT_MALLOC` → `LV_ASSERT_HANDLER`, which defaults to `while(1);` — the device *freezes* with interrupts still running and needs a physical reset (the task watchdog can't reboot it: `CONFIG_ESP_TASK_WDT_PANIC` is off). `lv_mem_realloc` instead returns NULL, which `lv_obj_class.c:97` dereferences — a `StoreProhibited` panic. Same root cause, two very different symptoms.
- On boards without PSRAM those 48KB come out of internal RAM and leave very little heap. `CONFIG_LV_MEM_CUSTOM=y` (plus `CONFIG_LV_MEM_CUSTOM_INCLUDE="stdlib.h"`) drops the pool and routes LVGL to `malloc`/`realloc`/`free`. Set on `m5core2_aws`; the other non-PSRAM ESP32 boards are still exposed.
- Screens with many rows are where this bites — the setup wizard's theme list is `THEME_PALETTE_THEME_COUNT` = 21 rows.

**`pdMS_TO_TICKS()` truncates to zero.** At `CONFIG_FREERTOS_HZ=100` (all these boards) anything under 10ms becomes 0 ticks, and `vTaskDelay(0)` is `taskYIELD()` — which only yields to equal or higher priority. A poll loop in a high-priority task that "delays" 2ms therefore never lets a lower-priority task run. `display_manager.c` has `DM_DELAY_AT_LEAST_ONE_TICK()` for this; use it in any wait loop that a lower-priority task must make progress through.

## Debugging a panic from a user's serial log

`Backtrace:` lines are just addresses unless `idf.py monitor` decoded them. Keep the exact `build/Ghost_ESP_IDF.elf` that produced the flashed binary (check `ELF file SHA256` in the panic output matches), then:

```bash
xtensa-esp32-elf-addr2line -pfiaC -e Ghost_ESP_IDF.elf 0x401eb6b9 0x401f7ee9 ...
```

`EXCVADDR` is worth reading closely: on a NULL-pointer store it is the struct/array offset, which often identifies the exact element — e.g. `EXCVADDR 0x50` = 80 bytes = index 20 of a pointer array, i.e. the 21st child.

## Conventions

- `CHANGELOG.md` entries follow a strict attribution rule (see `CONTRIBUTING.md`): untagged lines are the core maintainer's, guest contributions end with `- @handle`, and ported code names the upstream project.
- Feature availability is chip- and board-conditional. Guard with the existing `CONFIG_*` symbols (`CONFIG_IDF_TARGET_ESP32S2` has no BLE, `CONFIG_IDF_TARGET_ESP32P4` uses ESP-Hosted for radios, `CONFIG_WITH_SCREEN` for UI code) rather than adding new build-time forks.
</content>
</invoke>
