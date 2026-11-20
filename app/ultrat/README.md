# ultrat

A sumo robot: the port of `atm/ultrat/` (the Lua version) to ia-atmos/lang.
`app/ultrat.atm` is the program; it includes the files here, which include
the modules of `env/` that they use.

| File | What |
|---|---|
| `board.atm` | the pins; `BlinkLed` |
| `drv8833.atm` | the motor driver: `setup`, `set`, `reset`, `Move` |
| `pid.atm` | `Pid`: reads the line sensors every 10ms, emits `:target` and `:pid` to its parent |
| `modes.atm` | `ISeeYou`, `Calibration`, `Backwards`, `Whiplash`, `Sharingan` |
| `setup.atm` | learns the commands of the IR remote (start, prepare, stop) with BOOT held at reset, or loads them |

Build for the board with `idf.py -DATMOS_APP=$PWD/../app/ultrat.atm build`
(in `esp32/`): the image is 439 KB, with 19 KB of static RAM. On the host,
`make test` runs it with `tst/ultrat.script`: it learns the commands, then
fights from start to stop while a sensor sees the target.

## Differences from the Lua version

The Lua version did not run as it was. The port fixes:

| Lua | Here |
|---|---|
| `pid.atm` requires `ultrat.pins`, which does not exist | `board.atm` |
| the sensors are read with no setup | `pid.setup` sets them as inputs |
| `Pid` awaits `:clock` ("as fast as possible") | every 10ms (its `MS`, unused in Lua), with that `dt` |
| `motors.stop()` (undefined) | `drv8833.reset()` |
| `modes.I_see_you()` (undefined) | `modes.ISeeYou()` |
| `drv.move(v1, v2)` (undefined, no duration) | `drv8833.set(v1, v2)`, then the `await 150ms` that followed |
| `gpio.read(LEARN_PIN) == true` (a level is a number) and no pull-up | `== 0`, with `input_pullup`: BOOT is low while pressed |
| the stop command is learned with no timeout and may repeat another | the same `AwaitButton` as the others |
| `prefs.begin("commands")` at start, never closed | `preferences.open` and `close` in `save` and `load` |
| the saved commands are never loaded | `setup.load` |
| `modes.Select` and `modes.Run` are empty | the program always fights in `ISeeYou` |

Kept as in Lua, to check with the authors:

- The PID weights `[-4, -2, 2, 4]` apply to S3, S2, S4, S6; S5 and S8 are
  read but not weighted, and `board.S_ESQ` and the others are unused.
- `KP = 450` with a weight of 4 gives 1800, beyond the 1023 of a motor.
