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
| #4 | Ownership: sync with Pedro's `pgvalle-research/atmos-esp32`, who tests on the board | decision | open |
| #5 | `ultrat`: port it, or wait for Pedro | decision | open |
| #6 | Remove the Lua implementation, once the C one runs on the board | work | open, after #3 and a board run |
| #7 | `drv/esp32/*.c` and `esp32/` never compiled | work | open, needs #3 |
| #8 | Board drivers duplicate `main/*.c` until #6 | remark | open |
| #9 | A loop with no await starves the idle task on the board (watchdog warning) | remark | open |
| #10 | The host does not reject an IR repeat with no frame before it | defect | open (host only) |
| #11 | Reals are doubles, emulated on the ESP32: measure (`atmos-lang#3`) | work | open, needs a board |
| #12 | Compare with the Lua version on the board: flash, heap, wake-ups | work | open, needs a board |
| #13 | `sdkconfig.old` is committed (Lua project) | remark | open, goes with #6 |
| #14 | `A_STK_MAX` fixed at 4 MB, unchecked `malloc` (to `atmos-lang`) | defect | fixed in lang `36e36d8` |
| #15 | The Lua version queues ESP-NOW events with `xQueueSendFromISR` from the Wi-Fi task (not an interrupt); `drv/esp32/espnow.c` uses `xQueueSend` | remark | open (Pedro's code) |
| #16 | One environment for the board instead of one per driver; modules included only when used | decision | asked 2026-11-19 |

## Open items

- **#4** This is a copy of `pgvalle-research/atmos-esp32` at `d56bcb0`;
  Pedro's repository goes on. Nothing goes to `pgvalle-research`.
- **#5** `atm/ultrat/` is unfinished in the Lua version: `ultrat.pins`
  vs `board.atm`, `motors` and `modes` undefined, `drv.move` vs
  `drv.Move`, `gpio.read(...) == true` where `read` gives 0/1.
- **#16** Asked by the author (2026-11-19): "pq vários envs? Não estão
  todos na placa? Há como dissocia-los?". Today the clock is the primary
  and `gpio`, `ir`, `espnow` are secondaries, as in the Lua version.
  - #16a (recommended) one environment `esp32`: one C queue that every
    driver fills, one step that waits and drains it in arrival order;
    `gpio`, `ir`, `espnow`, `preferences` become plain modules, each in
    its own file, included only by a program that uses it (so a program
    with no `espnow` does not link the Wi-Fi stack)
  - #16b keep one environment per driver
- **#6** Remove `lua/`, `main/`, `atm/`, `compile_lua.py`,
  `partitions.csv`, `sdkconfig.old`, the submodules of
  `pgvalle-research`, the root ESP-IDF project; move `esp32/` and
  `app/` to the root.
- **#11, #12** On the host, `app/init.atm` peaks at about 31 KB of heap
  (64-bit pointers) and needs fewer than 64 entries of value stack.

## Sessions

- Compiler: `atmos-lang` (session_011eECogKVQ7ZzmWCoMST2wK).
- The rules of `pin`: `atmos-pin` (session_011eECogSDhkVfyt7qme5DLh).
- Environments in general: `atmos-envs` (session_011eECofYWTNPACTtUjHN4p6).
- `ext/lang` is at `907fee9` (2026-11-19); the host tests pass there, with no warnings.

## Decided by the author

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
