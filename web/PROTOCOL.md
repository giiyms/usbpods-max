# USBPods Max control protocol

The Pico does **not** run an HTTP stack. The website on the computer talks to the dongle over a tiny USB protocol. Two transports carry the same commands:

| Transport | Windows Chrome / Edge | macOS | Pico interface |
|-----------|----------------------|-------|----------------|
| **WebHID** (preferred) | `navigator.hid` — pick **TinyUSB BT** / *USBPods Max Control* | Chrome | vendor HID, usage page `0xFFA0` |
| **Web Serial** (fallback) | `navigator.serial` — pick **USBPods Max Console** | Chrome, or `screen /dev/cu.usbmodem*` | CDC-ACM |

VID `0xCafe`. Product string stays `TinyUSB BT` so the host still sees the same USB audio device name. Adding HID (including a second Consumer Control interface) does **not** change the TinyUSB auto-PID bitmap (HID bit already set). Windows may still need unplug/replug after a new interface layout.

No WinUSB driver, no Test Mode, no kernel driver.

A second HID interface is **Consumer Control** (usage page `0x0C`, string *USBPods Max Media*): Play/Pause, Next/Prev, Volume Up/Down, Mute. TinyUSB instance 1. Vendor settings stay on instance 0 / page `0xFFA0`.

CDC lines end on **`\r` or `\n`** (Mac `screen` is often CR-only).

## HID reports (vendor)

32-byte reports, **no report IDs**. `device.sendReport(0, buf)` / `inputreport`.

Protocol **major 1 / minor 1**. Bytes 0–14 and 16–21 are unchanged from minor 0. Minor 1 fills byte 15 and 22–31.

### Host → device (output)

| cmd | name | arg |
|-----|------|------|
| `0x01` | GET_STATUS | — |
| `0x02` | SET_GAIN | dB `0..24` |
| `0x03` | PAIR | long-press BOOTSEL equivalent |
| `0x04` | DISCONNECT | drop A2DP, **keep** pairing (sets reclaim hold) |
| `0x05` | RECONNECT | reconnect last slot (clears reclaim hold) |
| `0x06` | SET_SLOT | `1` or `2` |
| `0x07` | SET_NOISE | `1` Off, `2` ANC, `3` Transparency, `4` Adaptive |
| `0x08` | SET_CA | `1` on, `2` off |
| `0x09` | SET_CROWN | `1` reverse, `2` default (`0x1C`) |
| `0x0A` | SET_AUTOANS | `1` on, `2` off (`0x1E`) |
| `0x0B` | SET_CHIME | `0..100` (`0x1F`) |
| `0x0C` | SET_ADAPT | `1` on, `2` off (`0x26`) |
| `0x0D` | SET_SLEEP | `1` on, `2` off (`0x35`) |
| `0x0E` | SET_LISTEN | bitmask (`0x1A`) |
| `0x0F` | RENAME | `buf[1]=len`, `buf[2..]` UTF-8 (opcode `0x001A`) |
| `0x10` | SET_EAR_DET | `1` on, `2` off (`0x0A`) |
| `0x11` | SET_GESTURES | bitmask (`0x39`) |
| `0x12` | SET_HOLD | `0x01` noise, `0x05` Siri (`0x16`) |
| `0x13` | SET_AUTOCONN | `1` on, `2` off (`0x20`) |

### Device → host (input / GET_REPORT)

Byte 0 = `0x01` (STATUS).

| Offset | Field |
|--------|--------|
| 1 | flags: bit0 A2DP, bit1 AACP, bit2 mic active |
| 2 | mic gain dB `0..24` |
| 3 | pairing slot `1` or `2` |
| 4–7 | battery L/R/case/headset (`255` unknown) |
| 8 | noise mode `0` unknown, `1..4` |
| 9–10 | ear L/R raw (`0` = on-head) |
| 11 | CA config `0` unk, `1` on, `2` off |
| 12 | protocol minor (`1`) |
| 13 | protocol major (`1`) |
| 14 | UAC mute `0/1` |
| 15 | flags2: bit0 owns, bit1 CA duck active, bit2 auto-conn, bit3 ear-detect enable |
| 16..21 | last headset BD_ADDR |
| 22 | chime `0..100` |
| 23 | crown dir |
| 24 | listen mask |
| 25 | gestures mask |
| 26 | click-hold mode |
| 27–28 | last `0x0019` type, bud |
| 29 | auto-answer |
| 30 | adaptive volume |
| 31 | sleep detection |

Name/model/serial/fw and last-19 hex also appear on the CDC `@STATUS` line (WebHID has no room for strings). AVRCP `vol` is **serial-only** (not in the 32-byte HID report). HID byte 14 is **UAC mic mute**, not speaker mute. The Pages UI shows both when present. `@STATUS` also reports `spk_misalign=` (USB speaker leftover 1–3 byte events; 0 is healthy).

## Settings page diagnostics

GitHub Pages (`web/index.html`) can connect WebHID / Web Serial and:

- Log STATUS **transitions** (not the 1 s poll) plus a 10 s heartbeat
- Highlight ear on-head ↔ off-head (off-head may HID-pause the host)
- Scope **getUserMedia** (mic monitor) and a **Speaker test** Oscillator via `setSinkId` when the browser allows it

**Browsers cannot tap Teams/YouTube PCM.** The speaker canvas is the page’s own test tone, not system playback. USB output activity on the page is A2DP / AVRCP vol / UAC mic mute / CA duck inferred from HID or `@STATUS`.

## CDC text

Line-oriented, `\r` or `\n`. Extra verbs: `rename`, `crown`, `autoans`, `chime`, `adaptvol`, `sleep`, `listen`, `ear`, `gestures`, `hold`, `autocon`.

```
@STATUS a2dp=1 aacp=1 mic=0 gain=6 slot=1 mute=0 … owns=1 duck=0 autocon=1 allowauto=0 earen=1 … last19=05 01 name=AirPods Max …
@GAIN 6
```

## Mic gain

Software gain on **decoded PCM** after AAC-ELD. Range 0..+24 dB. Persist skipped while the AACP mic is active **or** AACP is connected (`flash_safe_execute` mid-stream kills A2DP). Magic `0x5A`, ~750 ms debounce. UAC `0x8000` is mute, not 0 dB.
