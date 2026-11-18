# TODO

State of the session `env-esp32`. Plan: `261118-env-esp32.md`.

## Decisions waiting for the author

One at a time, in this order; each with a recommendation.

1. **Build for the board here** (asked on 2026-11-18): install ESP-IDF
   v6 in the session (and perhaps Espressif's QEMU) to build `esp32/`,
   or leave the build to the author/Pedro with the board.
   Recommendation: install ESP-IDF (a few GB, redone in each new
   session unless saved as the setup script of the environment); it
   checks that `drv/esp32/*.c` and the runtime compile for Xtensa, and
   gives the sizes in flash. QEMU only after, if the build works.
2. **Ownership**: this is a copy of `pgvalle-research/atmos-esp32` at
   `d56bcb0`; Pedro's repository goes on. How the two stay in sync, and
   whether Pedro tests on the board.
3. **ultrat**: `atm/ultrat/` is unfinished in the Lua version
   (`ultrat.pins` vs `board.atm`, `motors` and `modes` undefined,
   `drv.move` vs `drv.Move`, `gpio.read(...) == true` where `read`
   gives 0/1). Port it as it is, or wait for Pedro.
4. **Removing the Lua implementation**: when the C one runs on the board
   (`lua/`, `main/`, `atm/`, `compile_lua.py`, `partitions.csv`, the
   submodules of `pgvalle-research`, the root ESP-IDF project; `esp32/`
   and `app/` move to the root).

## Known and not done

- `drv/esp32/*.c` and `esp32/` were never compiled: no ESP-IDF here.
- The board drivers duplicate `main/*.c` (and `drv/esp32/decoder.*`
  copies `main/ir/decoder.*`) until the Lua version is removed.
- On the board, a program that loops without awaiting starves the idle
  task, and the task watchdog warns (the Lua version yielded in a hook
  every 500 ms).
- The host does not reject an IR repeat with no frame before it (the
  board counts it as invalid).
- Reals are doubles, emulated in software on the ESP32 (no
  double-precision FPU): to measure on the board (compiler item 3).
- Compare on the board with the Lua version: flash, free heap, wake-ups
  per second (`debug.Mem`, `debug.Fps`). On the host, `app/init.atm`
  peaks at about 31 KB of heap (64-bit pointers).
- `sdkconfig.old` is committed (Lua project).

## Reported and not acknowledged

- (nothing)

## Reported and acknowledged

- 2026-11-18, to `atmos-lang`: `A_STK_MAX` fixed at 4 MB and an
  unchecked `malloc` in `atm_init`. Fixed in lang `36e36d8`. It notes other fixed sizes:
  `ATM_ENVS_MAX` (8 environments; this one uses 4) and `TRACE_MAX` (32),
  and waits for the measurement of reals on the board (its item 3).

## Sessions

- Compiler: `atmos-lang` (session_011eECogKVQ7ZzmWCoMST2wK).
- The rules of `pin`: `atmos-pin` (session_011eECogSDhkVfyt7qme5DLh).
- Environments in general: `atmos-envs` (session_011eECofYWTNPACTtUjHN4p6).
- `ext/lang` is at `8c9453c` (2026-11-18); the host tests pass there.

## Decided by the author

- 2026-11-18, **the goal**: replace. The reference Atmos (the Lua
  implementation: the Lua VM, `lua/`, the bytecode build, the
  submodules) gives place to `ia-atmos/lang` (Atmos compiled to C) in
  this repository. His words: "nosso objetivo aqui é substituir o atmos
  referencia pelo ia-atmos daqui". (The session had recommended keeping
  both.)
- 2026-11-18, **the first step**: "Faça tudo na ordem que preferir".
  Done: the drivers as plain C (`drv/`), the environment in Atmos
  (`env/`), a host simulation with tests (`make test`), `app/init.atm`
  ported, and an ESP-IDF project (`esp32/`) with ia-atmos/lang as the
  submodule `ext/lang`. The Lua version stays until the C one runs on
  the board.
