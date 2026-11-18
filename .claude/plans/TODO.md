# TODO

State of the session `env-esp32`. Plan: `261118-env-esp32.md`.

## Decisions waiting for the author

One at a time, in this order; each with a recommendation.

1. **The goal** (asked on 2026-11-18): keep the Lua port and add the C
   backend beside it, replace it, or only study it.
   Recommendation: keep and add beside. The Lua port is the only
   version that runs on the board and is the baseline for the
   comparison (flash, free heap, events per second); the drivers
   (ISRs, queues, the IR decoder) are shared by both.
2. **Ownership**: this is a copy of `pgvalle-research/atmos-esp32` at
   `d56bcb0`; Pedro's repository goes on. How the two stay in sync.
3. **The first step**: `atm/init.atm` (PWM fade and IR) built by
   `ia-atmos/lang` into the ESP-IDF application.
4. **Testing with no board**: a host build with mocked bindings,
   Espressif's QEMU, or the author/Pedro with the hardware. Installing
   ESP-IDF or QEMU here needs permission.
5. **The submodules**: SSH URLs of `pgvalle-research`; they do not
   clone in a cloud session.

## Known and not done

- Nothing was built: the container has `gcc`, `cmake`, `ninja` and
  `python3`, and no ESP-IDF, no cross compiler, no QEMU, no Lua; the
  submodules are empty.
- Each binding mixes the driver with the Lua API (lines that touch
  `lua_*`: `gpio.c` 114 of 660, `preferences.c` 97 of 351, `espnow.c`
  55 of 344, `ir/ir.c` 55 of 371, `ir/decoder.c` 0 of 238). A C backend
  needs the driver split from the Lua glue: argument checks,
  `luaL_error`, and `next_event` building a Lua table.
- `main/main.c` is all Lua (VM, LittleFS, package path, hook, GC); the
  C backend needs its own `app_main` over `atm_init`/`atm_main`, with
  the clock from `esp_timer_get_time` and `vTaskDelay` in place of
  `clock_gettime`/`nanosleep` of `rt/main.c`.
- `atm/ultrat/` is unfinished and in the Lua dialect: `require`,
  `ultrat.pins` (the file is `board.atm`), `motors` and `modes`
  undefined in `modes.atm`, `drv.move` vs `drv.Move`. Not checked
  against the C compiler.
- `sdkconfig.old` is committed.

## Reported and not acknowledged

- (nothing)

## Decided by the author

- (nothing)
