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
| 15 | flags2: bit0 owns, bit1 CA duck active, bit2 auto-conn, bit3 ear-detect enable, **bit4 reclaim**, **bit5 paused** (anti-ping-pong), **bit6 USB speaker alt≠0**, **bit7 USB PCM streaming** |
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

Name/model/serial/fw and last-19 hex also appear on the CDC `@STATUS` line (WebHID has no room for strings). AVRCP `vol` is **serial-only** (not in the 32-byte HID report). HID byte 14 is **UAC mic mute**, not speaker mute. The Pages UI shows both when present.

Speaker-path keys on `@STATUS` (USB ISO PCM **after** `spk_frame_align`, **before** A2DP encode — not Teams/YouTube, not a browser tap). Integer **dBFS** (`0` = full-scale 16-bit, `-96` = silence; last completed ~100 ms RMS window):

| Key | Meaning |
|-----|---------|
| `spk_misalign=` | Lifetime leftover 1–3 byte events. `0` is healthy; a **rising** count means ISO packets are not 4-byte aligned. |
| `spk_rem=` | Current sticky remainder length `0..3`. `2` is one-channel leftover (`spk_swap=1`). |
| `spk_half=` | Times remainder was `2` (classic L/R sticky-swap size). Lifetime counter. |
| `spk_swap=` | `1` while `spk_rem=2` (live one-channel remainder). Coarse swap-suspect — **not** an AACP opcode. |
| `spk_l=` `spk_r=` | Rolling RMS, integer dBFS. Serial-only; HID has no room. |

Cross-correlation lag between L and R is **not** measured (deferred — too much CPU on the Pico). Dual-connect keys on `@STATUS` (same firmware state as flags2 / 0x0E parse — no invented opcodes): `reclaim=` steal-reclaim armed, `paused=` `we_paused_after_giveup`, `spk=` USB speaker open, `stream=` USB PCM streaming, `peer=` last `0x0E` audio-src MAC (`-` until the first parse), **`softexcl=`** `1` on / `0` off (default on), **`sxphase=`** `idle|hold|grace`, **`sxkick=`** `1` when HOLD kick dropped or refused a Pico phone ACL. Soft exclusive: while USB wants the sink (`spk` or `stream`), firmware tries HCI-disconnect of extra Pico ACLs that are not the Max / not self; iPhone stays **paired**. After USB idle (`spk=0` and `stream=0`) a 2 s GRACE, then the phone may reconnect. **Reclaim / `0x10` skip only when `sxkick=1`.** Max-only dual-connect (no Pico ACL to the iPhone) logs `no Pico ACL to phone` and **still fights** (AVDTP reclaim + `0x10` + host session wake). After iPhone steal/reclaim, CDC may log `[A2DP] softexcl HOLD, no Pico phone ACL — fight reclaim/0x10`, `[A2DP] steal → HID Pause`, `[A2DP] reclaim stream → host session wake`, `[A2DP] host session wake → HID Play`, `[USB] speaker FU unmute interrupt`, `[USB] speaker iso nudge` — Windows should resume TinyUSB PCM without switching the default device. If `spk=1 stream=0` persists, the old recovery is still: switch default audio device away from TinyUSB and back.

## Settings page diagnostics

GitHub Pages (`web/index.html`) can connect WebHID / Web Serial and:

- Log STATUS **transitions** (not the 1 s HID poll) plus a 10 s heartbeat
- Highlight ear on-head ↔ off-head (off-head may HID-pause the host)
- Scope **getUserMedia** (mic monitor) and a **Speaker test** Oscillator via `setSinkId` when the browser allows it
- **CDC console** (diagnostic log): attach **USBPods Max Console** beside WebHID, or as the Web Serial fallback. Every CDC text line is mirrored into the log (same `LOG_CAP` 800). Consecutive duplicate `@STATUS` lines from the status poll and the human `USBPods Max status` banner are collapsed so the cap is not burned. WebHID STATUS transitions still log when HID is up. Quiet writes CDC as `Uint8Array` (`TextEncoder`); a raw JS string throws `The provided value is not of type 'ArrayBuffer' or 'ArrayBufferView'`.
- AACP hex / known opcodes are annotated from `AACP-FEATURES.md` (e.g. `0x0E` audio-src, `0x10` smart-routing, `0x11` SetOwnershipToFalse, `0x2E` connected devices, `0x06` OWNS, `0x20` autocon). Dual-connect lines (owns / they-own / reclaim / `0x11`) are highlighted.
- **Export .aacp**: download a dump-replay fixture for `tests/aacp_dump_replay_test`. Only **full** AACP frames (`04 00 04 00 …`); truncated CDC previews (`…` / `...`) are skipped. `USB_SPK_OPEN` / `USB_STREAMING` are stubs from last `@STATUS` `spk=` / `stream=` (or HID flags2). `EXPECT` lines are omitted — fill by hand after a steal capture.
- **Dual-connect** strip: owns, AACP, A2DP, duck, reclaim, paused (anti-ping-pong), **softexcl / SX phase**, USB speaker / streaming, last `0x0E` peer. CDC **softexcl on|off** button (default on).
- **Balance / Align** strip: ear L/R (HID or `@STATUS`), `spk_misalign` / `spk_rem` / `spk_half` / `spk_swap`, and USB L vs R dBFS bars from `spk_l=` `spk_r=`. **Connect HID, then CDC console** so the meters update (`status` every 2 s beside HID). Warn styling: misalign climbing, ear R off while L on (or vice versa), `|L−R| ≥ 6 dB` while both channels are louder than `-50 dBFS`. HID-only: ear chips work; L/R USB PCM bars stay `—` until CDC is attached.

**How to capture a steal dump:** Connect TinyUSB BT (HID), then **CDC console** → pick USBPods Max Console. Quiet sends `aacpdump on`. Play audio on the dongle, steal from the iPhone, then **Export .aacp**. Drop the file into `tests/fixtures/` and add `EXPECT` lines.

**Browsers cannot tap Teams/YouTube PCM.** The speaker canvas is the page’s own test tone, not system playback. USB output activity on the page is A2DP / AVRCP vol / UAC mic mute / CA duck inferred from HID or `@STATUS`. The Balance / Align meters are dongle USB speaker PCM (after frame-align) plus AirPods ear sensors — not a tap of other apps.

**“Left-weighted” patterns (Balance / Align):**

| What you see | Likely |
|--------------|--------|
| Ear L on, ear R off (or vice versa) | One cup off-head — spatial/level feel, not a PCM bug |
| `spk_misalign` climbing, `spk_rem=2` / `spk_swap=1`, `spk_half` rising | USB ISO leftover / one-channel sticky — classic L/R swap path (firmware carries remainder; still a host packet-size issue) |
| Both ears on, `spk_l` much hotter than `spk_r` (gap ≥ 6 dB, both > `-50 dBFS`) | Host USB speaker PCM itself is left-weighted (app mix / Windows balance / source) |
| Bars ~equal, ears on, A2DP up, quality still “left” | Not USB PCM or ear-off — codec/headset/ANC; Quiet cannot prove encoded A2DP L/R |

## CDC text

Line-oriented, `\r` or `\n`. Extra verbs: `rename`, `crown`, `autoans`, `chime`, `adaptvol`, `sleep`, `listen`, `ear`, `gestures`, `hold`, `autocon`, **`softexcl`**, **`aacpdump`**.

`softexcl on|off` — soft exclusive (default **on**). While USB wants the sink, try to drop extra Pico HCI ACLs that are not the AirPods Max; keep the iPhone **paired**. USB idle + 2 s grace allows the phone back. Reclaim / `0x10` run unless kick actually dropped a Pico phone ACL (`sxkick=1`). Max-only dual-connect still fights. Quiet’s Dual-connect strip has a CDC toggle. HID flags2 is full — this is serial/`@STATUS` only.

`aacpdump on|off` — full hex for non-dual AACP packets. Dual-connect / smart-routing opcodes **`0x0E` / `0x10` / `0x11` / `0x2E`** (and control `0x06` OWNS / `0x20`) always print **complete** frames, even when `aacpdump` is off. Other hex dumps stay at a 24-byte preview unless `aacpdump on`. Quiet’s CDC console turns `aacpdump on` when the port opens. No UF2 on Pages; dump-replay wants these full lines, not the old 24-byte preview.

```
@STATUS a2dp=1 aacp=1 mic=0 gain=6 slot=1 mute=0 … owns=1 duck=0 autocon=1 allowauto=0 earen=1 reclaim=0 paused=0 spk=1 stream=1 peer=aa:bb:cc:dd:ee:ff softexcl=1 sxphase=hold sxkick=0 … last19=05 01 name=AirPods Max … spk_misalign=0 spk_rem=0 spk_half=0 spk_swap=0 spk_l=-12 spk_r=-13
@GAIN 6
[AACP] 0x000E audio-src-resp n=13: 04 00 04 00 0E 00 FF EE DD CC BB AA 02
[AACP] tx n=11: 04 00 04 00 09 00 06 01 00 00 00
[SX] phase idle → hold (spk=1 stream=1)
```

## Mic gain

Software gain on **decoded PCM** after AAC-ELD. Range 0..+24 dB. Persist skipped while the AACP mic is active **or** AACP is connected (`flash_safe_execute` mid-stream kills A2DP). Magic `0x5A`, ~750 ms debounce. UAC `0x8000` is mute, not 0 dB.
