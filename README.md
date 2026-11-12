# Pico ToyBox (firmware for the Raspberry Pi Pico 2 W)

Plugs into a console over USB and pretends to be **one** of: Disney Infinity base, Skylanders
portal, LEGO Dimensions toy pad. You pick which from a phone over Bluetooth LE, and manage
figures from the same phone. **No jumper wires needed**: the log is shown on the phone.

> **Status: written, never run on a Pico.** The portable logic (flash store, all three modes,
> LEGO commands, phone protocol, log) passes host tests (`tests/`, `make test`, with
> AddressSanitizer). The TinyUSB and BTstack glue (`usb_modes.c`, `ble_link.c`) has only been
> compiled against hand-written stub headers, so expect to fix API mismatches on the first real
> build. The reused Flipper protocol code is unchanged and was proven on your consoles.

## Build

```
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential python3 git
git clone https://github.com/raspberrypi/pico-sdk.git && cd pico-sdk && git submodule update --init
export PICO_SDK_PATH=$PWD            # needs pico-sdk 2.x (Pico 2 W support, TinyUSB, BTstack submodules)
cd /path/to/pico_toybox && mkdir build && cd build && cmake .. && make -j4
```
Flash: hold BOOTSEL while plugging in, copy `build/pico_toybox.uf2` to the drive that appears.
First check: the Pico's LED blinks N times every 2 s (N = 1 Infinity, 2 Skylanders, 3 LEGO) until
a console enumerates it, then stays on.

## Use

1. Open `web/pico.html` on your phone (Chrome on Android, Bluefy on iPhone), tap **Connect**,
   choose **PicoToyBox**. Serving the `web/` folder from any web host or opening the files
   locally both work as with your Flipper pages.
2. Pick the base **before** starting the game. Switching makes the Pico unplug and re-plug USB
   (about 1 s apart). Never switch mid-game. The choice is remembered across power-ups.
3. Switch the mode in `pico.html`, disconnect, then open `infinity.html` / `skylanders.html` /
   `lego_dimensions.html` (your existing pages, unchanged) and Connect. (`pico.html` also has a
   small built-in Infinity loader.) They talk to the Pico exactly as they talked to the Flipper.
4. The Pico remembers loaded figures (and the LEGO token layout) in its flash, so after a
   power-up they are already on the base without the phone. Game progress is saved too and sent
   back to the phone when it next connects.

## The log (no cables)

`pico.html` → **Log**: the buffered log loads on connect, new lines stream live, **Level**
selects errors / events / everything (every USB command and reply: chatty, use for debugging),
**Save as file** downloads it, **Info** prints counters (USB packets, flash saves, MTU...).
The log lives in RAM (12 KB), so it survives a phone disconnect but not a power cycle. If the
Pico hung and the watchdog rebooted it, the first log line says so.
When the console can't see the Pico at all, the log still works (Bluetooth is independent of
USB): the LED pattern plus `usb: ... configured by host` lines tell you how far enumeration got.

## What is reused vs new

Unchanged from your Flipper project (`vendor/`): Infinity core + crypto + figure table, Skylanders
core + phone handler, LEGO phone handler, TEA/burtle, minifigure names, frame parser.
New: `usb_modes.c` (TinyUSB driver + re-enumeration), `ble_link.c` (BTstack Nordic UART),
`mode_*.c` (per-base glue), `ld_pad.c` (toy pad port), `store.c`/`persist.c` (flash),
`ctl.c`/`log.c` (mode switch, log), `web/pico.html`.

## Differences from the Flipper app you should know about

- **No Infinity web page was in the zip** (only Skylanders and LEGO). `pico.html` has a basic
  Infinity loader; your separate PC "infinity_base_gui" should still work (same protocol).
- LEGO: no Emulate screen, so the pad is ready as soon as the console wakes it. The Flipper's
  150 ms blocking pauses between pad changes became a timed queue. LEGO "Save on Flipper" /
  saved-vehicle list are unavailable (no SD); the web page keeps its own copies.
- LEGO USB strings: the Flipper reported its own manufacturer/product; the Pico reports
  "Raspberry Pi" / "Pico ToyBox" / board id. If a console refuses the pad, edit `mode_ld.c`.
- Two bugs fixed while porting LEGO: the Flipper code read/wrote past the 180-byte token and
  trusted USB frame lengths. The UID's last random byte keeps the Flipper's value (0).
- Skylanders: Trap Team speaker audio, lights: dropped. Bluetooth has no pairing PIN.
- The existing pages `CLEAR` the base when they connect. Saved figures are removed from the base
  then (the page reloads its own), but saved **game progress the phone hasn't received** is never
  erased and is re-sent on connect (`SAVE`, before the CLEAR). Untested with the real pages.

## Known risks (test in this order)

1. First build: TinyUSB/BTstack API names or versions may differ slightly (`tusb_init()`, the
   `usbd_class_driver_t` fields, `pico_btstack_make_gatt_header`). Fix and rebuild.
2. USB + Bluetooth together on the Pico W is the least proven part. Test with `pico.html` and
   a console connected at the same time.
3. Skylanders reply timing was tuned on the Flipper (22 ms IN interval): `SKY_IN_INTERVAL_MS`.
4. Flash writes stall the CPU for ~50 ms. They happen 5 s after the last change
   (`PERSIST_DELAY_MS` in `persist.h`) and on phone loads, so mid-game hiccups are possible.
5. Flash layout: the last 256 KiB hold saves; the firmware must stay under 3.75 MiB (it will be
   ~0.7 MiB). A log line errors if they ever overlap.
6. Back up your dumps; **Factory reset** = send message 0x36 0xA5 (not in the page on purpose).

Licence: GPL-2.0-only, from the RPCS3-derived Infinity code; the LEGO and Skylanders parts came
from other authors with no licence file in your archive, so check upstream before publishing.
