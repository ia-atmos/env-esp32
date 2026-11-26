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
| Board | `drv/esp32/*.c`, `esp32/` | ESP-IDF: GPIO, LEDC, ADC, RMT, ESP-NOW, Wi-Fi, Sleep/RTC, NVS |
| Host | `drv/host/host.c`, `Makefile` | a simulation with a virtual clock and scripted input |

There is one environment. Its step waits until the nearest timer of the
program, or until a driver queues an event, and emits the time that
passed; then it emits the events that the drivers queued, in the order
they happened, whatever driver queued them (each driver adds a source in
`drv/src.c`, and the oldest event of all goes first). On the board the
wait blocks on a semaphore that the interrupts give, so the CPU idles
between events. A module with events (`gpio`, `ir`, `espnow`, `wifi`) registers
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
| `wifi` | `connect(ssid [,password [,options]])`, `disconnect`, `stop`, `scan([options])`, `start_ap(ssid [,password [,options]])`, `stop_ap`, `status`, `info`, `get_mac`, `get_channel` | `:wifi.connected [ssid,bssid,channel,time]`, `:wifi.disconnected [ssid,bssid,reason,time]`, `:wifi.got_ip [ip,netmask,gateway,changed,time]`, `:wifi.lost_ip [time]`, `:wifi.scan [aps,time]`, `:wifi.ap.connected [mac,aid,time]`, `:wifi.ap.disconnected [mac,aid,reason,time]`, `:wifi.ap.sta_ip [mac,ip,time]` |
| `preferences` | `open`/`begin`, `close`, `commit`, `put_int`, `get_int`, `put_bool`, `get_bool`, `put_string`, `get_string`, `is_key`, `remove`, `clear` | |
| `sleep` | `deep(duration\|options)`, `light(duration\|options)`, `cause`, `causes`, `wakeup_pins`, `reset_reason`, `rtc_put_int`/`rtc_get_int`, `rtc_put_bool`/`rtc_get_bool`, `rtc_put_string`/`rtc_get_string`, `rtc_is_key`, `rtc_remove`, `rtc_clear` | `:sleep.before [mode,duration,time]`, `:sleep.wake [cause,causes,pins,slept,time]` |
| `debug` | `meminfo`, tasks `Mem` and `Fps` | |

The arguments and the results are those of [api.md](api.md). An invalid
argument, or a failure of ESP-IDF, raises an error with a message such
as `gpio.write : GPIO pin 34 cannot be used as output`, which `catch`
takes.

### Wi-Fi (`env/wifi.atm`) and ESP-NOW coexistence

`wifi` is non-blocking: `wifi.connect`, `wifi.scan`, and `wifi.start_ap`
return immediately and report state transitions through `:wifi.*` events
(and `:wifi.ap.*` for SoftAP clients, which are subtags of both `:wifi.ap`
and `:wifi`). Reconnection policy is left to the Atmos program (e.g., a
`loop` around `watching :wifi.disconnected { ... }` with a backoff timer).

`wifi` and `espnow` share the ESP32 Wi-Fi radio and can be included together:

- Both initialize `esp_netif`, the default event loop, and `esp_wifi`
  idempotently in whichever order they are started.
- Starting SoftAP (`wifi.start_ap`) while STA or ESP-NOW is active switches
  the radio to `WIFI_MODE_APSTA`; `wifi.stop_ap` drops back to
  `WIFI_MODE_STA` without interrupting STA or ESP-NOW.
- While STA is associated to an AP, the AP dictates the radio channel, so
  ESP-NOW peers must operate on that same channel (`wifi.get_channel()`,
  which reports the same physical radio channel as `espnow.get_channel()`).
  Pass `[power_save="none"]` to `wifi.connect` if you want low-latency
  ESP-NOW reception while connected to an AP.
- `wifi.stop()` powers down the shared Wi-Fi radio for both Wi-Fi and
  ESP-NOW.
- Programs that do not include `env/wifi.atm` or `env/espnow.atm` do not
  link the Wi-Fi stack.

Manual board test programs are provided in `app/`:
`app/wifi_scan.atm`, `app/wifi_sta.atm`, `app/wifi_ap.atm`, and
`app/wifi_espnow.atm`.

### Sleep (`env/sleep.atm`)

`sleep` provides both **deep sleep** (CPU and SRAM power off; wakes by
rebooting from `app_main`) and **light sleep** (CPU pauses; RAM and all
Atmos tasks resume where they left off). Multiple wakeup sources
(`duration`, `ext0`, `ext1`, and `gpio` for light sleep) act as a hardware
`par_or`—whichever trigger fires first wakes the chip:

- Durations use Atmos clock literals (e.g. `sleep.deep(30s)`,
  `sleep.light(500ms)`) and scale automatically with `A_CLOCK_US`.
- Both `sleep.deep` and `sleep.light` validate all options first, then
  broadcast `:sleep.before` globally (`emit @:global`) so concurrent tasks
  can flush state or run `defer` cleanup before sleeping.
- `sleep.rtc_*` stores up to 32 keys (int, bool, or strings up to 63 bytes)
  in checksummed `RTC_DATA_ATTR` slow SRAM—surviving deep sleep with zero
  flash wear and auto-clearing on cold boot.
- On the original ESP32, `ext0`, `ext1`, and `isolate` require RTC GPIOs
  (`0, 2, 4, 12–15, 25–27, 32–39`).
- `sleep.deep` and `sleep.light` enter sleep immediately without waiting for
  peripherals to drain: any recent or same-reaction serial output (`print`,
  including inside `:sleep.before`) or in-flight peripheral activity
  (Wi-Fi/ESP-NOW, PWM, RMT) may be cut off. Keep `:sleep.before` handlers to
  synchronous state or shutdown work (e.g. `sleep.rtc_put_*`, `wifi.stop()`).

Manual board test programs are provided in `app/`: `app/sleep_deep.atm` and
`app/sleep_light.atm`.

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
| reals of 32 bits | `ATMOS_REAL_FLOAT` | `ATMOS_FLOAT=32` | a real is a C `float`, in the FPU of the ESP32 |
| integers of 32 bits | `ATMOS_INT_32` | `ATMOS_INTEGER=32` | with 32-bit reals, a value has 8 bytes instead of 16 |
| time in milliseconds | `ATMOS_TIME_MS` | `ATMOS_CLOCK=ms` | timers, the time emitted and the `time` of the events are in ms |

The checks of `pin` are an option too: `idf.py -DATMOS_PIN=unchecked`
or `-DATMOS_PIN=off` (`ATMOS_PIN=...` on the host). With `off`, `pin`
only holds tasks and pools, and cycles are not collected. For
`app/ultrat.atm`, `off` shrinks our component from 215219 to 198269
bytes (with the three options above too, to 161172).

With 32-bit integers, use milliseconds: a time since boot in
microseconds wraps after 35 minutes (in ms, after 24 days), and the
`raw` of an IR frame above `0x7FFFFFFF` wraps to a negative number. For
`app/init.atm`, the three options together shrink our component from
159642 to 134224 bytes and the static RAM by 912 bytes.

## Reactions that do not await

A reaction runs until every task awaits again. On the board it runs in
the main task, at the same priority as the idle task of FreeRTOS, so a
loop with no `await` (or a long computation) keeps the idle task from
running: after 5 seconds the task watchdog prints a warning (it does not
reset the board). The Lua version has the same limit. A long loop should
await a short time from time to time, as `await(1ms)`, which blocks the
main task for a tick of FreeRTOS (`await(0)` does not: it ends at once,
in the same reaction).

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
0     wifi ap_add LabNet aa:bb:cc:dd:ee:01 -45 6 wpa2   # visible AP for scan
250   wifi connected LabNet aa:bb:cc:dd:ee:01 6         # STA associated
300   wifi got_ip 192.168.1.42 255.255.255.0 192.168.1.1 [changed]
400   wifi lost_ip                                      # STA lost IP
450   wifi disconnected 8 [LabNet aa:bb:cc:dd:ee:01]    # STA disconnected
500   wifi ap_connected de:ad:be:ef:00:01 1             # client joined SoftAP
520   wifi ap_sta_ip de:ad:be:ef:00:01 192.168.4.2      # SoftAP client got IP
600   wifi ap_disconnected de:ad:be:ef:00:01 1 8        # client left SoftAP
0     sleep reset deepsleep                             # boot reset reason
0     sleep cause ext1                                  # boot wakeup cause
0     sleep wakeup_pin 32                               # ext1 wakeup pin
0     sleep rtc_int boots 4                             # pre-populated RTC key
21000 quit                                  # nothing more happens
```

With `ESP32_HOST_TRACE` set, each action of the program on a driver is
printed as `[drv <ms>] ...`; with `ESP32_HOST_REALTIME` set, the clock
also sleeps for real.
