# TODO

State of the session `env-esp32`. Plan: `261118-env-esp32.md`.

Items are `#N` (standing rule of the author, 2026-11-18, `atmos-lang#71`):
unique in this session, in order, never reused or changed; options and
steps of an item are letters (`#3a`). From another session an item is
`session#N`.

## Index

| # | Item | Kind | State |
|---|---|---|---|
| #1 | The goal: replace the Lua implementation by ia-atmos/lang | decision | decided 2026-11-18 |
| #2 | The first step: drivers in C, environment in Atmos, host tests, `app/init.atm`, `esp32/` | work | done 2026-11-18 |
| #3 | Build for the board in this session (install ESP-IDF v6) | decision | decided 2026-11-19: #3a, install here |
| #4 | Ownership: sync with Pedro's `pgvalle-research/atmos-esp32`, who tests on the board | decision | decided 2026-11-20: follow his repository one way (driver fixes come here), nothing goes there; Pedro or the author runs on the board |
| #5 | `ultrat`: port it, or wait for Pedro | decision | decided 2026-11-20 (port); done in `app/ultrat/`, with 11 fixes and 2 questions in its README |
| #6 | Remove the Lua implementation, once the C one runs on the board | work | open: needs the board, steps in doc/board.md |
| #7 | `drv/esp32/*.c` and `esp32/` never compiled | work | done 2026-11-19: builds with ESP-IDF v6.1.1, no warning |
| #8 | Board drivers duplicate `main/*.c` until #6 | remark | open |
| #9 | A loop with no await starves the idle task on the board (watchdog warning) | remark | documented in doc/env.md; sent to atmos-envs |
| #10 | The host does not reject an IR repeat with no frame before it | defect | fixed 2026-11-20: a repeat repeats the last frame, as on the board; test in `tst/ir` |
| #11 | Reals are doubles, emulated on the ESP32: measure (`atmos-lang#3`) | work | `-DATMOS_REAL_FLOAT=ON` builds; size measured (image -4.6 KB); speed needs a board |
| #12 | Compare with the Lua version on the board: flash, heap, wake-ups | work | flash done 2026-11-19; heap and wake-ups need a board |
| #13 | `sdkconfig.old` is committed (Lua project) | remark | open, goes with #6 |
| #14 | `A_STK_MAX` fixed at 4 MB, unchecked `malloc` (to `atmos-lang`) | defect | fixed in lang `36e36d8` |
| #15 | The Lua version queues ESP-NOW events with `xQueueSendFromISR` from the Wi-Fi task (not an interrupt); `drv/esp32/espnow.c` uses `xQueueSend` | remark | open (Pedro's code) |
| #16 | One environment for the board instead of one per driver; modules included only when used | decision | decided 2026-11-20 (#16a), done: image of init 989 to 357 KB |
| #17 | A native result written over an Atmos variable leaked the default it replaced (`get_int`, `get_bool` with a string or table default) | defect | fixed 2026-11-19 (`a_set`), test under `ATMOS_ASAN=1 make test` |
| #18 | The board image filled 94% of the default 1 MB app partition | defect | fixed 2026-11-19: own table, app of 4032 KB, 76% free |
| #19 | Save the ESP-IDF install as the setup script of the environment `ceu-atmos`, only for sessions with `env-esp32` | work | approved 2026-11-20; the card was interrupted twice by restarts; waits for him to answer it |
| #21 | `atmos-float` branch `float` (32-bit integers, time in ms): measured on the board build in a scratch copy, not adopted (the branch may change) | work | reported 2026-11-20: AVal 16 to 8 bytes, our component -16%; a Makefile defect of the branch reported |
| #22 | Soft-float cost on a board with no FPU (esp32c3), asked by atmos-float (#7 there) | work | done 2026-11-20: soft-float arithmetic in ROM (0 B flash), libm 15.2 KB vs 10.9 KB on the ESP32; sent |
| #23 | atmos-float renames the host build variables (ATMOS_REAL/INT/TIME to ATMOS_FLOAT/INTEGER/CLOCK; old names refused): update Makefile and doc/env.md when it merges | work | waiting for the merge in lang (atmos-lang tells us) |
| #20 | To `atmos-lang`: `nanosleep` in `rt/main.c`, `$x = ...` in the manual, warnings of the generated C | defect | fixed in lang (`ee17162`, `dc94c6f`, `7b0bf06`); `ext/lang` at `826e219`, built with `ATM_NO_CLOCK` |

## Open items

- **#4** This is a copy of `pgvalle-research/atmos-esp32` at `d56bcb0`;
  Pedro's repository goes on. Nothing goes to `pgvalle-research`.
- **#5** `atm/ultrat/` is unfinished in the Lua version: `ultrat.pins`
  vs `board.atm`, `motors` and `modes` undefined, `drv.move` vs
  `drv.Move`, `gpio.read(...) == true` where `read` gives 0/1.
- **#6** Remove `lua/`, `main/`, `atm/`, `compile_lua.py`,
  `partitions.csv`, `sdkconfig.old`, the submodules of
  `pgvalle-research`, the root ESP-IDF project; move `esp32/` and
  `app/` to the root.
- **#11, #12** On the host, `app/init.atm` peaks at about 31 KB of heap
  (64-bit pointers) and needs fewer than 64 entries of value stack.
  Flash, both built with ESP-IDF v6.1.1 and `app/init.atm` /
  `atm/init.atm`: C image 989 KB (our component 152 KB: runtime about
  105 KB, generated C of the program and the environment 79 KB, drivers
  17 KB, before `--gc-sections`); Lua image 1012 KB (Lua VM 114 KB,
  bindings 14 KB) plus 66 KB of bytecode in LittleFS. The Wi-Fi stack is
  about 330 KB of both, linked even when ESP-NOW is not used (#16).

## Sessions

- Compiler: `atmos-lang` (session_011eECogKVQ7ZzmWCoMST2wK).
- The rules of `pin`: `atmos-pin` (session_011eECogSDhkVfyt7qme5DLh).
- Numeric types per system: `atmos-float` (session_011eECog5kQDX9i1eN1gKzDJ).
- Environments in general: `atmos-envs` (session_011eECofYWTNPACTtUjHN4p6).
- `ext/lang` is at `55b1fee` (2026-11-20); host tests pass (also with ASan), the board builds with no warning.

## Decided by the author

- 2026-11-20: "Ok para todos. Vá em frente até onde conseguir": #16
  (#16a, one environment), #19, #4, #5, with the recommendations.
- **#3** 2026-11-19: "#3 instale aqui". ESP-IDF v6.1.1 in `/opt/esp`
  (outside the repository), from the mirror of Espressif.
- **#1** 2026-11-18: replace. The Lua implementation (the Lua VM,
  `lua/`, the bytecode build, the submodules) gives place to
  ia-atmos/lang in this repository. His words: "nosso objetivo aqui é
  substituir o atmos referencia pelo ia-atmos daqui". (The session had
  recommended keeping both.)
- **#2** 2026-11-18: "Faça tudo na ordem que preferir". Done: the
  drivers as plain C (`drv/`), the environment in Atmos (`env/`), a host
  simulation with tests (`make test`), `app/init.atm` ported, and an
  ESP-IDF project (`esp32/`) with ia-atmos/lang as the submodule
  `ext/lang`. The Lua version stays until the C one runs on the board.

## Reported to other sessions

- **#14** to `atmos-lang`, 2026-11-18: acknowledged and fixed in
  `36e36d8`. It notes other fixed sizes: `ATM_ENVS_MAX` (8; this uses 4)
  and `TRACE_MAX` (32), and waits for #11.
