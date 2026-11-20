# Running on the board

What only a board can answer, in order. Each step says what to send back
(`#N` is the item in `.claude/plans/TODO.md`).

Build with ESP-IDF v6.1.1, in `esp32/` (`. $IDF_PATH/export.sh` first):

```
idf.py build flash monitor                                  # app/init.atm
idf.py -DATMOS_APP=$PWD/../app/ultrat.atm build flash monitor
```

A `-D` option stays in the cache of `esp32/build/`: remove that folder
(`rm -rf build sdkconfig`) to change the program or the options.

## 1. It runs (#6)

Flash `app/init.atm`, with LEDs on pins 19, 18, 4 and 23 and an IR
receiver on pin 15.

- The first LED fades up in 2 s; a key of the remote fades it down; the
  next key goes to the next LED. A held key (repeat) does nothing.
- Every second the monitor prints `[MEM] ...` and `FPS: n`, where `n` is
  0 to 2 while idle (the clock steps only when something happens).

Send: whether it does that, and the monitor log of the first minute.

## 2. Memory (#11, #12)

From the same log: `Free Heap`, `Min Free` and `Stack Free` after a few
fades. On the host the program peaks at about 31 KB of heap, with 64-bit
pointers; on the board it should be less.

Send: the three numbers after 1 minute and after 10 minutes (a leak shows
as `Min Free` going down).

## 3. Against Lua (#6)

Flash the Lua version (the root project, `atm/init.atm`) and repeat 1 and
2.

Send: the same numbers, and whether the behaviour differs.

## 4. Numeric options (#11)

Remove `build` and `sdkconfig`, then:

```
idf.py -DATMOS_REAL_FLOAT=ON -DATMOS_INT_32=ON -DATMOS_TIME_MS=ON build flash monitor
```

Send: whether 1 still holds, and the numbers of 2.

## 5. ultrat (#5)

On the robot, flash `app/ultrat.atm`.

- Reset with BOOT held: the monitor asks for the start, prepare and stop
  keys; press each one. Reset without BOOT: it prints the three saved
  codes.
- Start key: the robot turns to a target in front of the sensors, and
  goes ahead when it is centered; stop key: it stops.

Send: the log, and what the robot does. The questions about the PID
weights are in `app/ultrat/README.md`.

## 6. Then

If 1 to 3 hold, the Lua version can go (#6, with #8 and #13).
