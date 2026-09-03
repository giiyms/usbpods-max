# USBPods Max control protocol

The Pico does **not** run an HTTP stack. The website on the computer talks to the dongle over a tiny USB protocol. Two transports carry the same commands:

| Transport | Windows Chrome / Edge | macOS | Pico interface |
|-----------|----------------------|-------|----------------|
| **WebHID** (preferred) | `navigator.hid` — pick **TinyUSB BT** / *USBPods Max Control* | Chrome | vendor HID, usage page `0xFFA0` |
| **Web Serial** (fallback) | `navigator.serial` — pick **USBPods Max Console** | Chrome, or `screen /dev/cu.usbmodem*` | CDC-ACM |

VID `0xCafe`. Product string stays `TinyUSB BT` so the host still sees the same USB audio device name. Adding HID changes the TinyUSB auto-PID (HID bit) — Windows treats a freshly flashed dongle as a new composite device; unplug/replug once.

No WinUSB driver, no Test Mode, no kernel driver.

## HID reports

32-byte reports, **no report IDs**. `device.sendReport(0, buf)` / `inputreport`.

### Host → device (output)

| Offset | Field |
|--------|--------|
| 0 | command |
| 1+ | arguments |

| cmd | name | arg0 |
|-----|------|------|
| `0x01` | GET_STATUS | — |
| `0x02` | SET_GAIN | dB `0..24` |
| `0x03` | PAIR | long-press BOOTSEL equivalent (scan / re-pair) |
| `0x04` | DISCONNECT | drop A2DP, **keep** pairing |
| `0x05` | RECONNECT | reconnect last slot |
| `0x06` | SET_SLOT | `1` or `2` |
| `0x07` | SET_NOISE | `1` Off, `2` ANC, `3` Transparency, `4` Adaptive (AACP `0x09` / `0x0D`, librepods) |
| `0x08` | SET_CA | `1` on, `2` off (AACP `0x09` / `0x28`, librepods) |

### Device → host (input / GET_REPORT)

Byte 0 = `0x01` (STATUS).

| Offset | Field |
|--------|--------|
| 1 | flags: bit0 A2DP, bit1 AACP, bit2 mic active |
| 2 | mic gain dB `0..24` |
| 3 | pairing slot `1` or `2` |
| 4 | battery left `%` or `255` unknown |
| 5 | battery right |
| 6 | battery case |
| 7 | battery headset (Max may report a single component) |
| 8 | noise mode `0` unknown, `1..4` as above |
| 9 | ear L raw (`255` unknown) |
| 10 | ear R raw |
| 11 | conversation awareness `0` unk, `1` on, `2` off |
| 12 | protocol minor (`0`) |
| 13 | protocol major (`1`) |
| 14 | UAC mute `0/1` |
| 16..21 | last headset BD_ADDR |

## CDC text (serial menu + Web Serial)

Line-oriented, `\n` terminated. Same verbs as USBPods Lite-style consoles:

```
h / help
s / status
g / gain [0-24]
p / pair
x / disconnect
r / reconnect
1 / 2 / slot N
n / anc off|anc|trans|adaptive
ca on|off
```

`status` also emits a machine line the web page parses:

```
@STATUS a2dp=1 aacp=1 mic=0 gain=6 slot=1 mute=0 bat_l=255 bat_r=255 bat_c=255 bat_h=80 noise=2 ear_l=0 ear_r=0 ca=2 addr=70:f9:4a:8a:d8:54
@GAIN 6
```

Debug logs (`[AACP]`, `[MIC]`, `[DEC]`) share this port. The page ignores lines that do not start with `@`.

## Mic gain

Software gain on **decoded PCM** after AAC-ELD, before the USB mic endpoint. Range 0..+24 dB, default 0, persisted in flash. Soft-clip/limiter so boost cannot wrap 16-bit. Optional UAC2 Feature Unit on the mic path so Windows' own recording slider moves the same gain.

There is **no** documented Apple AACP "mic gain" opcode in librepods; this is not HFP.
