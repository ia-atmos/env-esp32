# The ESP32 environment of ia-atmos/lang

The environment `env/esp32.atm` connects an Atmos program, compiled to C
by [ia-atmos/lang](https://github.com/ia-atmos/lang), to the ESP32. It
replaces the Lua implementation (`lua/esp32/`, documented in
[api.md](api.md)), with the same modules, functions and events, except
for the differences listed at the end.

A program includes the modules it uses (each includes the core
`env/esp32.atm`), and only their drivers are linked:

```
include "../env/gpio.atm"
include "../env/ir.atm"

ir.start(15)
val k = await :ir [repeat=false]
```

```
$ make -C ext/lang                          # the compiler
$ make test                                 # the tests, on the host
$ cd esp32 && idf.py build flash monitor    # the board (ESP-IDF v6)
```

## Layers

| Layer | Where | What |
|---|---|---|
| Program | `app/*.atm` | Atmos; includes the modules it uses |
| Environment | `env/esp32.atm` | the one environment of the board: the clock and the events of all drivers |
| Modules | `env/<module>.atm` | functions in Atmos; native literals call the drivers |
| Drivers | `drv/drv.h` | plain C, no Lua and no Atmos; a failure is a message |
| Board | `drv/esp32/*.c`, `esp32/` | ESP-IDF: GPIO, LEDC, ADC, RMT, ESP-NOW, NVS |
| Host | `drv/host/host.c`, `Makefile` | a simulation with a virtual clock and scripted input |

There is one environment. Its step waits until the nearest timer of the
program, or until a driver queues an event, and emits the time that
passed; then it emits the events that the drivers queued, in the order
they happened, whatever driver queued them (each driver adds a source in
`drv/src.c`, and the oldest event of all goes first). On the board the
wait blocks on a semaphore that the interrupts give, so the CPU idles
between events. A module with events (`gpio`, `ir`, `espnow`) registers
in `esp32.drain` the function that emits the oldest event of its driver.

For `app/init.atm` (`gpio`, `ir`, `debug`, no `espnow`), the image is
357 KB with 18 KB of static RAM; a program that also includes `espnow`
links the Wi-Fi stack, and its image is about 980 KB.

## Modules

| Module | Functions | Events |
|---|---|---|
| `clock` | `clock.now` (µs since boot) | the time |
| `gpio` | `setup`/`mode`, `read`, `write`, `watch`, `unwatch`, `read_adc`, `setup_pwm`, `attach_pwm`, `detach_pwm`, `stop_pwm`, `write_pwm`, `fade_pwm` | `:gpio [pin,value,edge,time]`, `:gpio.pwm_fade [channel,time]` |
| `ir` | `start(pin [,options])`, `stop`, `stats` | `:ir [protocol,address,command,raw,bits,repeat,time]` |
| `espnow` | `init`, `add_peer`, `send`, `get_mac`, `get_channel`, `set_channel` | `:espnow.recv [mac,data,time]`, `:espnow.send [mac,status,time]` |
| `preferences` | `open`/`begin`, `close`, `commit`, `put_int`, `get_int`, `put_bool`, `get_bool`, `put_string`, `get_string`, `is_key`, `remove`, `clear` | |
| `debug` | `meminfo`, tasks `Mem` and `Fps` | |

The arguments and the results are those of [api.md](api.md). An invalid
argument, or a failure of ESP-IDF, raises an error with a message such
as `gpio.write : GPIO pin 34 cannot be used as output`, which `catch`
takes.

## Differences from the Lua version

- There is no `require`: the program includes the modules it uses, whose
  names are globals.
- `:gpio.pwm_fade` is a subtag of `:gpio`: `await :gpio` also awakes on
  a fade (a pattern with a field, as `:gpio [pin=4]`, does not).
- `:ir` gives `raw` as an integer, not as a hexadecimal string: integers
  have 64 bits.
- `preferences` has one namespace open at a time, used through the
  module; `open` gives `true`, not a handle with methods.
- `debug.Fps` counts the steps of the clock in a second. The Lua clock
  stepped all the time; this one waits for the next timer or event, so
  the count is low when nothing happens.
- `pin` is a keyword of Atmos, and cannot name a variable or parameter.

## Numeric options

The compiler has three build options for small targets; the runtime, the
generated C and the drivers must be built with the same ones:

| Option | Board (`idf.py -D...=ON`) | Host (`make test`) | Effect |
|---|---|---|---|
| reals of 32 bits | `ATMOS_REAL_FLOAT` | `ATMOS_REAL=float` | a real is a C `float`, in the FPU of the ESP32 |
| integers of 32 bits | `ATMOS_INT_32` | `ATMOS_INT=32` | with 32-bit reals, a value has 8 bytes instead of 16 |
| time in milliseconds | `ATMOS_TIME_MS` | `ATMOS_TIME=ms` | timers, the time emitted and the `time` of the events are in ms |

With 32-bit integers, use milliseconds: a time since boot in
microseconds wraps after 35 minutes (in ms, after 24 days), and the
`raw` of an IR frame above `0x7FFFFFFF` wraps to a negative number. For
`app/init.atm`, the three options together shrink our component from
159642 to 134224 bytes and the static RAM by 912 bytes.

## Testing on the host

`make test` (or `ATMOS_ASAN=1 make test`, which also fails on a leak)
builds each `tst/<t>.atm` (and `app/init.atm`, as `init`)
with the drivers of `drv/host/`, runs it with `tst/<t>.script` as its
input, and compares what it prints with `tst/<t>.out`. The clock is
virtual: a wait jumps to the next timer, fade or line of the script, so
a test runs at once and prints the same every time. A script has one
line per input:

```
# <ms since boot> <input>
100   pin 4 1                               # a pin goes to a level
2500  ir nec 0x00 0x45 [repeat]             # an IR frame
300   espnow 24:0a:c4:05:06:07 PING         # an ESP-NOW message
21000 quit                                  # nothing more happens
```

With `ESP32_HOST_TRACE` set, each action of the program on a driver is
printed as `[drv <ms>] ...`; with `ESP32_HOST_REALTIME` set, the clock
also sleeps for real.
