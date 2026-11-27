# ESP32 Atmos Library API Documentation

This document describes the Atmos hardware wrapper libraries for the ESP32. All libraries are packaged under the `esp32` namespace.

You can import all modules at once:
```lua
local esp32 = require "esp32"
local clock, gpio, espnow, ir = esp32.clock, esp32.gpio, esp32.espnow, esp32.ir
```

---

## Table of Contents
* **[1. esp32.clock](#1-esp32clock)**
  * [`clock.now`](#clocknow)
* **[2. esp32.gpio](#2-esp32gpio)**
  * [`gpio.setup`](#gpiosetup)
  * [`gpio.read`](#gpioread)
  * [`gpio.write`](#gpiowrite)
  * [`gpio.watch`](#gpiowatch)
  * [`gpio.unwatch`](#gpiounwatch)
  * [`gpio.read_adc`](#gpioread_adc)
  * [`gpio.setup_pwm`](#gpiosetup_pwm)
  * [`gpio.attach_pwm`](#gpioattach_pwm)
  * [`gpio.detach_pwm`](#gpiodetach_pwm)
  * [`gpio.stop_pwm`](#gpiostop_pwm)
  * [`gpio.write_pwm`](#gpiowrite_pwm)
  * [`gpio.fade_pwm`](#gpiofade_pwm)
* **[3. esp32.espnow](#3-esp32espnow)**
  * [`espnow.init`](#espnowinit)
  * [`espnow.stop`](#espnowstop-c-environment) (C environment)
  * [`espnow.get_mac`](#espnowget_mac)
  * [`espnow.add_peer`](#espnowadd_peer)
  * [`espnow.send`](#espnowsend)
* **[4. esp32.ir](#4-esp32ir)**
  * [`ir.start`](#irstart)
  * [`ir.stop`](#irstop)
  * [`ir.stats`](#irstats)
* **[5. esp32.preferences](#5-esp32preferences)**
  * [`preferences.open`](#preferencesopen)
  * [`preferences.put_int`](#preferencesput_int)
  * [`preferences.get_int`](#preferencesget_int)
  * [`preferences.put_bool`](#preferencesput_bool)
  * [`preferences.get_bool`](#preferencesget_bool)
  * [`preferences.put_string`](#preferencesput_string)
  * [`preferences.get_string`](#preferencesget_string)
  * [`preferences.is_key`](#preferencesis_key)
  * [`preferences.remove`](#preferencesremove)
  * [`preferences.clear`](#preferencesclear)
  * [`preferences.close`](#preferencesclose)
* **[6. wifi (C environment)](#6-wifi-c-environment)**
  * [`wifi.connect`](#wificonnect)
  * [`wifi.disconnect`](#wifidisconnect)
  * [`wifi.stop`](#wifistop)
  * [`wifi.scan`](#wifiscan)
  * [`wifi.start_ap`](#wifistart_ap)
  * [`wifi.stop_ap`](#wifistop_ap)
  * [`wifi.status`](#wifistatus)
  * [`wifi.info`](#wifiinfo)
  * [`wifi.get_mac`](#wifiget_mac)
  * [`wifi.get_channel`](#wifiget_channel)
* **[7. sleep (C environment)](#7-sleep-c-environment)**
  * [`sleep.deep`](#sleepdeep)
  * [`sleep.light`](#sleeplight)
  * [`sleep.cause` / `sleep.causes`](#sleepcause--sleepcauses)
  * [`sleep.wakeup_pins`](#sleepwakeup_pins)
  * [`sleep.reset_reason`](#sleepreset_reason)
  * [`sleep.rtc_*`](#sleeprtc_)
* **[8. Atmos Hardware Events](#8-atmos-hardware-events)**
  * [`gpio` Event](#gpio-event)
  * [`gpio.pwm_fade` Event](#gpiopwm_fade-event)
  * [`espnow.recv` Event](#espnowrecv-event)
  * [`espnow.send` Event](#espnowsend-event)
  * [`espnow.stop` Event](#espnowstop-event-c-environment) (C environment)
  * [`ir` Event](#ir-event)
  * [`wifi` Events](#wifi-events)
  * [`sleep` Events](#sleep-events)

---

## 1. `esp32.clock`
The clock module manages logical time and high-precision timing.

### `clock.now`
* **Syntax:** `clock.now` (field)
* **Returns:** `number` (Microseconds since microcontroller boot).
* **Example:**
  ```lua
  local start_time = clock.now
  -- do work
  local elapsed = clock.now - start_time
  print("Elapsed time in us: " .. elapsed)
  ```

---

## 2. `esp32.gpio`
The GPIO module handles all physical pin interactions including digital I/O, edge interrupts, ADC oneshot reads, and hardware-controlled PWM.

### `gpio.setup`
Configures a physical pin's digital routing and buffers (`gpio.mode` is available as a backwards-compatible alias). If the pin was previously attached to a PWM channel, changing its mode (e.g. to `"input"` or `"output"`) automatically detaches it from the PWM peripheral.
* **Syntax:** `gpio.setup(pin, mode)`
* **Arguments:**
  * `pin`: `integer` (GPIO pin number).
  * `mode`: `string`. Supported values:
    * `"input"`: Digital input.
    * `"input_pullup"`: Digital input with internal pull-up resistor.
    * `"input_pulldown"`: Digital input with internal pull-down resistor.
    * `"output"`: Digital push-pull output.
    * `"open_drain"`: Digital open-drain output.
* **Example:**
  ```lua
  -- Configure pin 22 as a normal output
  gpio.setup(22, "output")

  -- Configure pin 34 as a digital input with pull-up
  gpio.setup(34, "input_pullup")
  ```

### `gpio.read`
Reads the digital state of a pin.
* **Syntax:** `gpio.read(pin)`
* **Returns:** `integer` (`0` for LOW, `1` for HIGH).
* **Example:**
  ```lua
  local state = gpio.read(4)
  if state == 1 then
      print("Pin is HIGH")
  end
  ```

### `gpio.write`
Sets the digital state of an output pin.
* **Syntax:** `gpio.write(pin, value)`
* **Arguments:**
  * `value`: `boolean` or `integer` (`true`/`1` sets HIGH, `false`/`0` sets LOW).
* **Example:**
  ```lua
  gpio.write(22, 1) -- High
  gpio.write(22, false) -- Low
  ```

### `gpio.watch`
Enables hardware interrupts on a pin. Edge transitions are queued and emitted as **[`gpio` events](#gpio-event)**.
* **Syntax:** `gpio.watch(pin, edge)`
* **Arguments:**
  * `edge`: `string` (`"rising"`, `"falling"`, or `"both"`/`"change"`).
* **Cross Reference:** Configures the hardware to trigger **[`gpio` events](#gpio-event)**.
* **Example:**
  ```lua
  gpio.mode(4, "input_pullup")
  gpio.watch(4, "falling")

  spawn(task(function()
      while true do
          local ev = await({ tag = "gpio", pin = 4, edge = "falling" })
          print("Button pressed at: " .. ev.time)
      end
  end))
  ```

### `gpio.unwatch`
Disables interrupts on the specified pin and detaches its ISR handler.
* **Syntax:** `gpio.unwatch(pin)`

### `gpio.read_adc`
Reads the raw analog voltage on a pin.
* **Syntax:** `gpio.read_adc(pin, [attenuation])`
* **Arguments:**
  * `attenuation`: `string` or `integer`. Configures internal voltage dividers to support different input ranges:
    * `"0db"` or `0`: $0\text{V}$ to $1.1\text{V}$ range.
    * `"2.5db"` or `2`: $0\text{V}$ to $1.25\text{V}$ range.
    * `"6db"` or `6`: $0\text{V}$ to $1.75\text{V}$ range.
    * `"12db"` / `"11db"` or `12` / `11`: $0\text{V}$ to $3.3\text{V}$ range (Default).
* **Returns:** `integer` (Raw reading from `0` to `4095` for 12-bit resolution).
* > [!WARNING]
  > **Wi-Fi vs. ADC2 Hardware Conflict Gotcha (ESP32 Limitation)**
  >
  > The ESP32's internal Wi-Fi hardware controller shares physical circuitry with the **ADC2** pins. When Wi-Fi (or ESP-NOW) is active, reading from any ADC2 pin will fail or return corrupt noise.
  >
  > * **DO NOT USE ADC2 PINS:** GPIO 0, 2, 4, 12, 13, 14, 15, 25, 26, 27.
  > * **ALWAYS USE ADC1 PINS:** **GPIO 32, 33, 34, 35, 36, 37, 38, 39**. (For example, `gpio.read_adc(34)` is perfectly safe and unaffected by ESP-NOW).
* **Example:**
  ```lua
  local raw = gpio.read_adc(34, "12db")
  local volts = (raw / 4095) * 3.3
  print("Sensor Voltage: " .. volts)
  ```

### `gpio.setup_pwm`
Configures a hardware LEDC PWM channel and its underlying timer.
* **Syntax:** `gpio.setup_pwm(channel, frequency, resolution)`
* **Arguments:**
  * `channel`: `integer` (LEDC channel `0` to `7`). Hardware timers `0` to `3` are assigned automatically via `channel % 4`.
  * `frequency`: `integer` (Frequency in Hz).
  * `resolution`: `integer` (Duty resolution in bits, `1` to `20`).
* **Example:**
  ```lua
  -- Configure Channel 0 at 25kHz with 10-bit resolution (0-1023 duty range)
  gpio.setup_pwm(0, 25000, 10)
  ```

### `gpio.attach_pwm`
Attaches an output-capable GPIO pin to a configured PWM channel. Multiple pins can be bound to the same PWM channel. If the pin was previously attached to a different PWM channel, its previous binding is detached first.
* **Syntax:** `gpio.attach_pwm(pin, channel)`
* **Arguments:**
  * `pin`: `integer` (Output-capable GPIO pin number).
  * `channel`: `integer` (Configured LEDC channel `0` to `7`).
* **Example:**
  ```lua
  -- Attach pin 25 to Channel 0
  gpio.attach_pwm(25, 0)
  ```

### `gpio.detach_pwm`
Detaches a pin from its PWM channel and resets the pin back to default digital GPIO state. The PWM channel itself and any other pins bound to the same channel continue running uninterrupted.
* **Syntax:** `gpio.detach_pwm(pin)`
* **Arguments:**
  * `pin`: `integer` (GPIO pin number).
* **Example:**
  ```lua
  -- Detach pin 25 from PWM (pin returns to GPIO input)
  gpio.detach_pwm(25)
  ```

### `gpio.stop_pwm`
Stops the PWM output on a channel, driving and holding the output at level 0.
* **Syntax:** `gpio.stop_pwm(channel)`
* **Arguments:**
  * `channel`: `integer` (LEDC channel `0` to `7`).
* **Example:**
  ```lua
  gpio.stop_pwm(0)
  ```

### `gpio.write_pwm`
Sets the static duty cycle on a configured PWM channel.
* **Syntax:** `gpio.write_pwm(channel, duty)`
* **Arguments:**
  * `duty`: `integer` (Value from `0` to $2^{\text{resolution}} - 1$).

### `gpio.fade_pwm`
Triggers a non-blocking hardware duty cycle fade over a duration of time. Once completed, a **[`gpio.pwm_fade` event](#gpiopwm_fade-event)** is emitted.
* **Syntax:** `gpio.fade_pwm(channel, target_duty, duration_ms)`
* **Cross Reference:** Triggers a **[`gpio.pwm_fade` event](#gpiopwm_fade-event)** upon completion.
* **Example:**
  ```lua
  -- Fade LED on Channel 0 to 100% (8191) over 1000ms, then fade back to 0% over 500ms
  spawn(task(function()
      while true do
          gpio.fade_pwm(0, 8191, 1000)
          await({ tag = "gpio.pwm_fade", channel = 0 })

          gpio.fade_pwm(0, 0, 500)
          await({ tag = "gpio.pwm_fade", channel = 0 })
      end
  end))
  ```

---

## 3. `esp32.espnow`
The ESP-NOW module enables low-latency connectionless packet communication over the Wi-Fi radio.

### `espnow.init`
Initializes the Wi-Fi Station stack and boots the ESP-NOW communication service.
* **Syntax:** `espnow.init()`
* **Under the hood configurations:**
  * **Power Saving:** Automatically disables Wi-Fi Power Saving (`WIFI_PS_NONE`). This keeps the radio receiver awake at all times to prevent dropped packets.
  * **Channel Lock:** Locks the physical Wi-Fi channel to **Channel 1** to ensure matching transmission frequencies with peer devices.
* **Cross Reference:** Allows incoming packets to trigger **[`espnow.recv` events](#espnowrecv-event)**.
* **C environment:** starts the Wi-Fi radio again if it is off (after `espnow.stop`, or after a sleep stopped it). Does nothing if ESP-NOW is already running.

### `espnow.stop` (C environment)
Stops ESP-NOW. The Wi-Fi radio, shared with the `wifi` module, turns off only if Wi-Fi is not using it.
* **Syntax:** `espnow.stop()`
* **Behavior:**
  * Emits **[`:espnow.stop [reason="stopped"]`](#espnowstop-event-c-environment)** if ESP-NOW was running; does nothing otherwise.
  * Forgets every peer: call `espnow.add_peer` again after the next `espnow.init`.
  * Messages that arrived before the stop are still delivered, before `:espnow.stop`; none arrives after it. Messages sent to the board while ESP-NOW is stopped are lost, and the result of a send still in flight is dropped.
  * `sleep.deep` and `sleep.light` call it, with `reason="sleep"`.

### `espnow.get_mac`
Gets the local MAC address of the board.
* **Syntax:** `espnow.get_mac()`
* **Returns:** `string` (The formatted MAC address, e.g., `"24:0a:c4:05:06:07"`).

### `espnow.get_channel`
Gets the current operational Wi-Fi radio channel used by ESP-NOW.
* **Syntax:** `espnow.get_channel()`
* **Returns:** `integer` (Channel number, `1` to `14`).

### `espnow.set_channel`
Sets the operational Wi-Fi radio channel used by ESP-NOW.
* **Syntax:** `espnow.set_channel(channel)`
* **Arguments:**
  * `channel`: `integer` (Wi-Fi channel `1` to `14`).

### `espnow.add_peer`
Registers a target peer device. This must be called before sending unicast messages to a peer.
* **Syntax:** `espnow.add_peer(mac_str)`
* **Arguments:**
  * `mac_str`: `string` (Standard MAC format, e.g. `"24:0a:c4:05:06:07"`).

### `espnow.send`
Sends an asynchronous message to the specified MAC address (unicast or broadcast). Does not wait for confirmation.
* **Syntax:** `espnow.send(mac_str, payload)`
* **Arguments:**
  * `mac_str`: `string` (Target peer MAC or `"ff:ff:ff:ff:ff:ff"` for broadcast).
  * `payload`: `string` (Message string up to 250 bytes).
* **Cross Reference:** Triggers an **[`espnow.send` event](#espnowsend-event)** upon packet delivery outcome (immediate for broadcast).
* **Reliability Check Observation:**
  This function is non-blocking and fires the packet asynchronously. If you need link-layer delivery confirmation (ensuring the receiver ACKed the packet), you should explicitly `await` the corresponding **[`espnow.send` event](#espnowsend-event)** matching the target MAC:
  ```lua
  espnow.send(peer_mac, "Hello")
  
  -- Suspend explicitly until transmission resolves
  local ev = await({ tag = "espnow.send", mac = peer_mac })
  if ev.status == "success" then
      print("Delivered successfully!")
  else
  end
  ```
* > [!NOTE]
  > **Transmission Status Semantics (Unicast vs. Broadcast)**
  >
  > * **Unicast (Specific MAC):** A subsequent `espnow.send` event returning `status = "success"` guarantees that the target device **successfully received** the packet and returned a hardware Link-layer ACK.
  > * **Broadcast (`"ff:ff:ff:ff:ff:ff"`):** A subsequent `espnow.send` event returning `status = "success"` **only** confirms the packet was successfully **transmitted into the air** by the local antenna. It does **not** indicate whether any remote device actually heard it.

---

## 4. `esp32.ir`

The IR module receives demodulated remote-control signals through the ESP32 RMT
peripheral. Modeled after popular libraries such as `IRremote` and `IRremoteESP8266`,
it decodes NEC (standard and extended), Samsung, Sony SIRC, and LG frames. The
module emits neutral frame data and does not assign application-specific meanings to commands.

### `ir.start`

Starts the single IR receiver.

* **Syntax:** `ir.start(pin, [options])`
* **Arguments:**
  * `pin`: GPIO connected to the IR receiver output (active-low demodulated sensor such as TSOP4838 or VS1838B).
  * `options.glitch_ns`: Pulses shorter than this are discarded by the hardware filter. Default: `1250` (hardware limit on ESP32 is `3187` ns; set to `0` to disable filtering).
  * `options.idle_ns`: A level lasting this long ends the frame. Default: `15000000` (15 ms).

```lua
local ir = require("esp32").ir
ir.start(15)
```

### `ir.stop`

Stops the receiver and releases its RMT channel, task, and queues.

* **Syntax:** `ir.stop()`

### `ir.stats`

Returns diagnostic counters:

```lua
{
    captured = integer,
    decoded = integer,
    invalid = integer,
    dropped = integer
}
```

`invalid` counts captured waveforms that were not valid supported frames.
`dropped` counts frames lost because an internal queue was full.

### Events

When an IR frame is successfully decoded, an event is emitted into the Atmos / Lua environment with `tag = "ir"`:

```lua
{
    tag = "ir",
    protocol = "nec" | "samsung" | "sony" | "lg",
    address = integer,
    command = integer,
    raw = string,      -- e.g. "0x20DF10EF"
    bits = integer,    -- bit length (e.g. 32 for NEC/Samsung, 12/15/20 for Sony, 28 for LG)
    repeat = boolean,  -- true if frame is a repeated code
    time = integer     -- capture timestamp in microseconds
}
```

In Atmos, you can pattern-match or await IR events directly:

```atmos
;; wait for any non-repeat IR event with a specific command
val ev = await :ir [command=0x34, repeat=false]

;; match on command code
match ev.command {
    0x10 => print("Command 1")
    0x20 => print("Command 2")
}
```

---

## 5. `esp32.preferences`

The preferences module provides persistent key-value storage in ESP32 Non-Volatile Storage (NVS) flash memory. Changes are committed to flash immediately upon write.

Namespaces and keys must be between 1 and 15 characters in length. Both module-level singleton usage and instance/handle usage are supported with a single clean `snake_case` API.

### `preferences.open`
Opens a storage namespace. Sets the active namespace for module-level methods and returns a preferences handle object.
* **Syntax:** `preferences.open(namespace, [read_only], [partition])`
* **Arguments:**
  * `namespace`: `string` (1 to 15 characters).
  * `read_only`: `boolean` (Default: `false`).
  * `partition`: `string` (Optional, defaults to `"nvs"` partition).
* **Returns:** `Preferences` handle object.

### `preferences.put_int`
Stores a 32-bit signed integer in flash. By default, changes are committed immediately. Pass `false` for `commit` to batch multiple writes and commit once via `preferences.commit()`.
* **Syntax:** `preferences.put_int(key, value[, commit])` or `handle:put_int(key, value[, commit])`
* **Arguments:**
  * `key`: `string` (1 to 15 characters).
  * `value`: `integer` (32-bit signed integer).
  * `commit`: `boolean` (Optional, default: `true`).
* **Returns:** `boolean` (`true` on success).

### `preferences.get_int`
Retrieves a stored integer from flash.
* **Syntax:** `preferences.get_int(key, [default])` or `handle:get_int(key, [default])`
* **Arguments:**
  * `key`: `string` (1 to 15 characters).
  * `default`: `integer` (Returned if the key is not found. Default: `0`).
* **Returns:** `integer`

### `preferences.put_bool`
Stores a boolean in flash and commits.
* **Syntax:** `preferences.put_bool(key, value)`

### `preferences.get_bool`
Retrieves a stored boolean from flash.
* **Syntax:** `preferences.get_bool(key, [default])` (Default: `false`).

### `preferences.put_string`
Stores a string in flash and commits.
* **Syntax:** `preferences.put_string(key, value)`

### `preferences.get_string`
Retrieves a stored string from flash.
* **Syntax:** `preferences.get_string(key, [default])` (Default: `""`).

### `preferences.is_key`
Checks if a key exists in the current namespace.
* **Syntax:** `preferences.is_key(key)`
* **Returns:** `boolean`

### `preferences.remove`
Removes a specific key from flash and commits.
* **Syntax:** `preferences.remove(key)`
* **Returns:** `boolean`

### `preferences.clear`
Erases all keys in the open namespace and commits.
* **Syntax:** `preferences.clear()`
* **Returns:** `boolean`

### `preferences.commit`
Explicitly commits any pending uncommitted changes to flash.
* **Syntax:** `preferences.commit()` or `handle:commit()`
* **Returns:** `boolean`

### `preferences.close`
Closes the open namespace handle and releases resources.
* **Syntax:** `preferences.close()` or `handle:close()`

### Example: Storing and Loading 3 Integers
```lua
local esp32 = require "esp32"
local prefs = esp32.preferences

-- Open namespace "app" in read/write mode
prefs.open("app")

-- Save 3 integers separately
prefs.put_int("cmd_start", 100)
prefs.put_int("cmd_stop", 200)
prefs.put_int("cmd_prepare", 300)

-- Read them back (with default 0 if not previously stored)
local start_cmd   = prefs.get_int("cmd_start", 0)
local stop_cmd    = prefs.get_int("cmd_stop", 0)
local prepare_cmd = prefs.get_int("cmd_prepare", 0)

print("Loaded commands:", start_cmd, stop_cmd, prepare_cmd)

prefs.close()
```

---

## 6. `wifi` (C environment)

The `wifi` module (`include "../env/wifi.atm"`) provides non-blocking Wi-Fi Station (STA), SoftAP, and asynchronous AP scanning over ESP-IDF. It can coexist with `espnow` on the shared Wi-Fi radio (see [doc/env.md](env.md)).

### `wifi.connect`
Lazily initializes Wi-Fi in STA mode (or `APSTA` if SoftAP is active) and initiates an asynchronous connection to an access point.
* **Syntax:** `wifi.connect(ssid [, password [, options]])`
* **Arguments:**
  * `ssid`: `string` (1 to 32 characters).
  * `password`: `string` or `nil` (up to 64 characters; `nil` or `""` connects to an open network).
  * `options`: optional table with any of:
    * `bssid`: `string` (`"aa:bb:cc:dd:ee:ff"`) to lock to a specific AP MAC.
    * `channel`: `integer` (`1` to `14`).
    * `hostname`: `string` DHCP hostname.
    * `power_save`: `string` (`"none"`, `"min"`, or `"max"`).
* **Cross Reference:** Emits **[`:wifi.connected`](#wifi-events)** and **[`:wifi.got_ip`](#wifi-events)** on success, or **[`:wifi.disconnected`](#wifi-events)** on failure/drop.

### `wifi.disconnect`
Disconnects the station from the current AP while keeping the Wi-Fi driver initialized. Emits `:wifi.disconnected`.
* **Syntax:** `wifi.disconnect()`

### `wifi.stop`
Stops STA, SoftAP, and any scan in progress. The radio, shared with ESP-NOW, turns off only if ESP-NOW is not using it: `wifi.stop` does not end ESP-NOW.
* **Syntax:** `wifi.stop()`
* **Events:** tells the program of everything it ends, exactly once (events that ESP-IDF posts afterwards for the same are dropped):
  * **[`:wifi.scan [status="stopped"]`](#wifi-events)** for a scan in progress;
  * **[`:wifi.ap.disconnected [reason=8]`](#wifi-events)** for each SoftAP client;
  * **[`:wifi.disconnected [reason=8]`](#wifi-events)** if STA was connecting or connected.
* `sleep.deep` and `sleep.light` call it.

### `wifi.scan`
Starts a non-blocking scan for nearby Wi-Fi access points. Lazily starts Wi-Fi in STA (or APSTA) mode if stopped. Raises an error if a scan is already in progress.
* **Syntax:** `wifi.scan([options])`
* **Arguments:**
  * `options`: optional table with any of:
    * `ssid`: `string` to filter for a specific SSID.
    * `channel`: `integer` (`1` to `14`, `0` or omitted for all channels).
    * `show_hidden`: `boolean` (default `false`).
    * `max`: `integer` maximum APs to return (default `20`, capped at `32`, sorted by strongest RSSI).
* **Cross Reference:** Emits **[`:wifi.scan`](#wifi-events)** when the scan ends: check its `status` before reading `aps`, which is `nil` unless `status` is `"success"`.

### `wifi.start_ap`
Starts a SoftAP access point (default gateway IP `192.168.4.1`). If STA or ESP-NOW is already active, switches automatically to `APSTA` mode so both remain active.
* **Syntax:** `wifi.start_ap(ssid [, password [, options]])`
* **Arguments:**
  * `ssid`: `string` (1 to 32 characters).
  * `password`: `string` or `nil` (8 to 64 characters for WPA2-PSK; `nil` or `""` for an open AP).
  * `options`: optional table with any of:
    * `channel`: `integer` (`1` to `13`, default `1`).
    * `max_conn`: `integer` (`1` to `10`, default `4`).
    * `hidden`: `boolean` (default `false`).
* **Cross Reference:** Emits **[`:wifi.ap.connected`](#wifi-events)**, **[`:wifi.ap.sta_ip`](#wifi-events)**, and **[`:wifi.ap.disconnected`](#wifi-events)** as clients join and leave.

### `wifi.stop_ap`
Stops the SoftAP interface. If STA or ESP-NOW is active, switches back to STA mode without disconnecting it; otherwise stops the Wi-Fi radio.
* **Syntax:** `wifi.stop_ap()`
* **Events:** emits **[`:wifi.ap.disconnected [reason=8]`](#wifi-events)** for each client still connected.

### `wifi.status`
Returns the current STA connection state string.
* **Syntax:** `wifi.status()`
* **Returns:** `"stopped" | "disconnected" | "connecting" | "connected" | "got_ip"`.

### `wifi.info`
Returns a snapshot table of the current Wi-Fi STA and SoftAP state.
* **Syntax:** `wifi.info()`
* **Returns:** Table with:
  * `state`: same string as `wifi.status()`.
  * `ap_active`: `boolean`.
  * `mac`, `ap_mac`: local STA and SoftAP MAC address strings.
  * `ssid`, `bssid`, `rssi`: present while STA is connected (`nil` otherwise).
  * `channel`: active radio channel while STA or SoftAP is active (`nil` when stopped).
  * `ip`, `netmask`, `gateway`: present while STA has an IPv4 address (`nil` otherwise).
  * `ap_ssid`, `ap_ip`: present while SoftAP is active (`nil` otherwise).

### `wifi.get_mac`
Returns the local STA MAC address.
* **Syntax:** `wifi.get_mac()`
* **Returns:** `string` (e.g. `"24:0a:c4:05:06:07"`).

### `wifi.get_channel`
Returns the current primary Wi-Fi radio channel (shared with ESP-NOW).
* **Syntax:** `wifi.get_channel()`
* **Returns:** `integer` (`1` to `14`). Raises an error if Wi-Fi has not been initialized yet.

---

## 7. `sleep` (C environment)

The `sleep` module (`include "../env/sleep.atm"`) exposes ESP32 deep sleep, light sleep, wakeup/reset inspection, and RTC slow-memory retention storage.

### `sleep.deep`
Enters deep sleep (CPU and main SRAM power off; waking reboots into `app_main`). Validates arguments first, emits `:sleep.before` globally, configures wakeup triggers + optional RTC pin isolation, and powers down immediately. Never returns on the board.
* **Note:** `sleep.deep` powers down immediately without draining peripherals—recent or same-reaction serial output (`print`, including inside `:sleep.before`) and in-flight I/O (Wi-Fi/ESP-NOW, PWM, RMT) may be cut off. Keep `:sleep.before` to synchronous state or shutdown work (e.g. `sleep.rtc_put_*`).
* **Radios:** right after `:sleep.before` (in which they still work), stops every radio the program includes (`wifi.stop`, `espnow.stop`), as ESP-IDF requires. Their events are lost with the reboot.
* **Syntax:** `sleep.deep(duration)` or `sleep.deep(options)`
* **Arguments:**
  * `duration`: positive Atmos clock duration (e.g. `30s`, `5min`) — shorthand for `[duration=...]`.
  * `options`: table combining any wakeup sources (acted on as hardware `par_or`):
    * `duration`: positive Atmos clock duration for RTC timer wakeup.
    * `ext0`: `[pin=int, level=0|1, pull="none"|"up"|"down"]` — single RTC GPIO wakeup (`0, 2, 4, 12–15, 25–27, 32–39` on ESP32).
    * `ext1`: `[pins=[int, ...], mode="any_high"|"all_low"]` — multi-pin RTC GPIO wakeup (`any_high` = OR across pins; `all_low` = AND across pins on ESP32).
    * `isolate`: `[int, ...]` — RTC GPIOs to pass to `rtc_gpio_isolate()` before sleeping to reduce leakage current.
    * `force`: `boolean` (default `false`) — must be `true` to allow deep sleep with no wakeup source configured.

### `sleep.light`
Enters light sleep (CPU pauses; SRAM and all Atmos tasks stay intact and resume on wake). Emits `:sleep.before`, sleeps immediately until the first wakeup source fires, resynchronizes the Atmos clock, emits `:sleep.wake`, and returns the wakeup table.
* **Note:** `sleep.light` gates peripheral clocks immediately without draining hardware FIFOs—recent or same-reaction serial output (`print`, including inside `:sleep.before`) and in-flight peripheral activity may be cut off.
* **Radios:** right after `:sleep.before` (in which they still work), stops every radio the program includes (`wifi.stop`, `espnow.stop`), as ESP-IDF requires. After waking, their `:wifi.disconnected`, `:wifi.ap.disconnected`, `:wifi.scan [status="stopped"]` and `:espnow.stop [reason="sleep"]` come after `:sleep.wake`; the program starts the radios again (`wifi.connect`, `espnow.init`).
* **Syntax:** `sleep.light(duration)` or `sleep.light(options)`
* **Arguments:**
  * `duration` or `options` table with `duration`, `ext0`, `ext1`, and/or `gpio` (`[pin=int, level=0|1]` or list `[[pin=..., level=...], ...]`, supporting any valid digital GPIO).
* **Returns:** `[cause=string, causes=[string, ...], pins=[int, ...], slept=number]`.

### `sleep.cause` / `sleep.causes`
Returns the wakeup cause(s) from the most recent deep or light sleep.
* **Syntax:** `sleep.cause()` -> `string` (`"none"`, `"timer"`, `"ext0"`, `"ext1"`, `"gpio"`, `"touch"`, `"ulp"`, `"uart"`, `"wifi"`, `"bt"`, `"other"`).
* **Syntax:** `sleep.causes()` -> vector of all active wakeup cause strings.

### `sleep.wakeup_pins`
Returns a vector of GPIO pin numbers that triggered an `ext1` wakeup (`[]` otherwise).
* **Syntax:** `sleep.wakeup_pins()`

### `sleep.reset_reason`
Returns the chip reset reason at boot (`"poweron"`, `"ext"`, `"sw"`, `"panic"`, `"int_wdt"`, `"task_wdt"`, `"wdt"`, `"deepsleep"`, `"brownout"`, `"sdio"`, `"usb"`, `"jtag"`, `"efuse"`, `"pwr_glitch"`, `"cpu_lockup"`, `"unknown"`).
* **Syntax:** `sleep.reset_reason()`

### `sleep.rtc_*`
Checksummed key-value storage in `RTC_DATA_ATTR` slow SRAM (up to 32 keys, key length 1–15 chars, strings up to 63 bytes). Survives deep sleep with zero flash wear and automatically clears on cold boot/power loss:
* `sleep.rtc_put_int(key, value)` / `sleep.rtc_get_int(key [, default])`
* `sleep.rtc_put_bool(key, value)` / `sleep.rtc_get_bool(key [, default])`
* `sleep.rtc_put_string(key, value)` / `sleep.rtc_get_string(key [, default])`
* `sleep.rtc_is_key(key)` / `sleep.rtc_remove(key)` / `sleep.rtc_clear()`

---

## 8. Atmos Hardware Events

Secondary environments process physical interrupts and emit them as Atmos events. You can capture them in tasks using the `await()` pattern.

### `gpio` Event
* **Created/Configured by:** [`gpio.watch`](#gpiowatch)
* **Structure:**
  ```lua
  {
      tag = "gpio",
      pin = integer,       -- GPIO pin number
      value = integer,     -- Pin state: 0 (LOW) or 1 (HIGH)
      edge = string,       -- "rising" or "falling"
      time = integer       -- Microseconds since boot of transition
  }
  ```
* **Atmos Syntax:**
  ```atmos
  await :gpio [pin=4, edge="falling"]
  ```

### `gpio.pwm_fade` Event
* **Created/Triggered by:** [`gpio.fade_pwm`](#gpiofade_pwm)
* **Structure:**
  ```lua
  {
      tag = "gpio.pwm_fade",
      channel = integer,   -- PWM channel number (0 to 7)
      time = integer       -- Microseconds since boot when fade completed
  }
  ```
* **Atmos Syntax:**
  ```atmos
  await :"gpio.pwm_fade" [channel=0]
  ```

### `espnow.recv` Event
* **Allowed/Enabled by:** [`espnow.init`](#espnowinit)
* **Structure:**
  ```lua
  {
      tag = "espnow.recv",
      mac = string,        -- Sender's MAC address (e.g. "24:0a:c4:05:06:07")
      data = string,       -- Received payload data (string)
      time = integer       -- Microseconds since boot when received
  }
  ```
* **Atmos Syntax:**
  ```atmos
  await :"espnow.recv" [data="PING"]
  ```

### `espnow.send` Event
* **Created/Triggered by:** [`espnow.send`](#espnowsend)
* **Structure:**
  ```lua
  {
      tag = "espnow.send",
      mac = string,        -- Target peer's MAC address
      status = string,     -- "success" or "fail"
      time = integer       -- Microseconds since boot when result resolved
  }
  ```
* **Atmos Syntax:**
  ```atmos
  await :"espnow.send" [status="success"]
  ```
* > [!NOTE]
  > **Transmission Status Semantics (Unicast vs. Broadcast)**
  >
  > * **Unicast (Specific MAC):** A `status = "success"` outcome guarantees that the target device **successfully received** the packet and returned a hardware Link-layer ACK.
  > * **Broadcast (`"ff:ff:ff:ff:ff:ff"`):** A `status = "success"` outcome **only** confirms the packet was successfully **transmitted into the air** by the local antenna. It does **not** indicate whether any remote device actually heard it.

### `espnow.stop` Event (C environment)
* **Created/Triggered by:** [`espnow.stop`](#espnowstop-c-environment), or a sleep (`sleep.deep`, `sleep.light`).
* **Structure:** `:espnow.stop [reason=string, time=integer]`, where `reason` is `"stopped"` (`espnow.stop`) or `"sleep"`.
* **Purpose:** ESP-NOW has no connection, so this is its only sign of an end. A task that waits for messages should watch it, or it waits forever after a stop or a light sleep:
  ```atmos
  watching :espnow.stop {
      loop {
          val r = await :espnow.recv
          print(r.mac, r.data)
      }
  }
  ```

---

### `ir` Event

Emitted once for each valid decoded IR frame:

```lua
{
    tag = "ir",
    protocol = "nec" | "samsung" | "sony" | "lg",
    address = integer,
    command = integer,
    raw = "0x20DF10EF",
    bits = integer,
    repeat = boolean,
    time = integer
}
```

* **Atmos Syntax:**
  ```atmos
  await :ir [command=0x45, repeat=false]
  ```

`raw` is a hexadecimal string because this firmware configures Lua with
32-bit integers. The event describes only the received frame; button mappings
and state machines belong to application code.

```lua
spawn(task(function()
    while true do
        local ev = await({ tag = "ir" })
        print(ev.protocol, ev.address, ev.command, ev.repeat)
    end
end))
```

### Compatibility and Decoded Protocols

The event is intentionally comparable to Arduino-IRremote's
`IrReceiver.decodedIRData`: it reports a decoded protocol, address, command,
raw value, bit count, and repeat status:

* **NEC**: Standard 8-bit address, Extended 16-bit address, 8-bit command, and repeat frames.
* **Samsung**: 32-bit frame format and repeat codes.
* **Sony SIRC**: 12, 15, and 20-bit variations.
* **LG**: 28-bit frame format with 4-bit checksum and repeat codes.
* Samsung's ordinary 8-bit command layout is supported. A valid Samsung
  16-bit command is currently reduced to its first 8 bits.
* A duplicated Samsung 8-bit address is returned as the complete 16-bit wire
  value. Arduino-IRremote normally collapses equal address bytes to one byte.
* The SamsungLG short repeat format used by current Arduino-IRremote is not
  decoded yet. Full repeated Samsung frames are decoded normally.
* Repeat detection for complete frames compares equal raw frames received
  within 120 ms. Arduino-IRremote instead uses protocol-specific preceding-gap
  limits by default.
* The default 15 ms RMT idle threshold can merge repeated Sony20 frames whose
  inter-frame gap is shorter than 15 ms. Single frames and typical Sony12
  repetitions are unaffected.
* Samsung inverse-byte parity is not currently reported or rejected.
* Unknown protocols and malformed frames do not emit events; they increment
  `ir.stats().invalid`.
* Only one receiver can be active. Decoded events use a fixed queue of 16;
  queue overflow increments `ir.stats().dropped`.
* `raw` is a hexadecimal string, rather than an integer, to preserve the value
  in the firmware's 32-bit Lua configuration.

---

### `wifi` Events

All Wi-Fi events are hierarchical subtags of `:wifi`, and SoftAP client events are subtags of `:wifi.ap`:

* **`:wifi.connected`** — STA associated with AP:
  `[ssid=string, bssid=string, channel=integer, time=integer]`
* **`:wifi.disconnected`** — STA disconnected, connection failed, or `wifi.stop` (`reason=8`):
  `[ssid=string, bssid=string, reason=integer, time=integer]`
* **`:wifi.got_ip`** — STA obtained IPv4 address:
  `[ip=string, netmask=string, gateway=string, changed=boolean, time=integer]`
* **`:wifi.lost_ip`** — STA lost IPv4 lease:
  `[time=integer]`
* **`:wifi.scan`** — Async AP scan ended:
  `[aps=[[ssid, bssid, rssi, channel, auth], ...]|nil, status=string, time=integer]`
  where `status` is `"success"` (the scan finished: `aps` is the list, and an empty list means that no network was found), `"stopped"` (`wifi.stop` or a sleep ended it) or `"fail"` (ESP-IDF failed it); `aps` is `nil` unless `"success"`, and `auth` is `"open"`, `"wep"`, `"wpa"`, `"wpa2"`, `"wpa_wpa2"`, `"enterprise"`, `"wpa3"`, `"wpa2_wpa3"`, `"wapi"`, `"owe"`, or `"unknown"`.
* **`:wifi.ap.connected`** — Client joined SoftAP:
  `[mac=string, aid=integer, time=integer]`
* **`:wifi.ap.sta_ip`** — SoftAP DHCP server assigned an IP to a client:
  `[mac=string, ip=string, time=integer]`
* **`:wifi.ap.disconnected`** — Client left SoftAP, or `wifi.stop_ap`/`wifi.stop` disconnected it (`reason=8`):
  `[mac=string, aid=integer, reason=integer, time=integer]`

```atmos
include "../env/wifi.atm"

loop {
    wifi.connect("MySSID", "secret")
    watching :wifi.disconnected {
        val ip = await :wifi.got_ip
        print("Online at", ip.ip)
        await(false)
    }
    await(5s)
}
```

---

### `sleep` Events

* **`:sleep.before`** — Broadcast globally (`emit @:global`) immediately before entering deep or light sleep (after validating options):
  `[mode="deep"|"light", duration=number|nil, time=integer]`
  *(Runs synchronously in the same reaction as the sleep call—use it for state persistence or peripheral shutdown, not serial `print`. The radios still work here; sleep stops them right after.)*
* **`:sleep.wake`** — Broadcast globally immediately after returning from `sleep.light`:
  `[cause=string, causes=[string, ...], pins=[int, ...], slept=number, time=integer]`

```atmos
include "../env/sleep.atm"

spawn {
    watching :sleep.before {
        defer { sleep.rtc_put_int("clean", 1) }
        await(false)
    }
}

;; Wake after 30s OR when GPIO 0 (BOOT) is pressed LOW
sleep.deep([duration=30s, ext0=[pin=0, level=0, pull="up"]])
```
