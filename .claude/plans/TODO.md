# TODO

State of the session `env-esp32`. Plan: `261118-env-esp32.md`.

## Decisions waiting for the author

One at a time, in this order; each with a recommendation.

2. **Ownership**: this is a copy of `pgvalle-research/atmos-esp32` at
   `d56bcb0`; Pedro's repository goes on. How the two stay in sync.
3. **The first step** (asked on 2026-11-18): how to get there.
   Recommendation: `atm/init.atm` (PWM fade and IR) built by
   `ia-atmos/lang`, first on the host with the drivers mocked (nothing
   to install), the Lua port kept in the tree until the C version runs
   on the board, then removed.
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

- 2026-11-18, **the goal**: replace. The reference Atmos (the Lua
  implementation: the Lua VM, `lua/`, the bytecode build, the
  submodules) gives place to `ia-atmos/lang` (Atmos compiled to C) in
  this repository. His words: "nosso objetivo aqui é substituir o atmos
  referencia pelo ia-atmos daqui". (The session had recommended keeping
  both.)
