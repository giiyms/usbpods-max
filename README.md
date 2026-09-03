# USBPods Max

A **Pico 2 W** USB dongle that talks to AirPods the way iOS does: AAC-ELD playback **and** the proprietary hi-res microphone at the same time, then presents a normal USB headset to the PC. This repository is **USBPods Max** — MagicPods-class AACP control, a Windows Chrome/Edge settings page, and a mic gain boost — on top of the proven 2-way mic.

**This is our fork:** [giiyms/usbpods-pico2w-2-way-audio-implement](https://github.com/giiyms/usbpods-pico2w-2-way-audio-implement), forked from [han-um/USBPods-Pico2W-2-way-audio-implement](https://github.com/han-um/USBPods-Pico2W-2-way-audio-implement) (which itself extends [USBPods-Pico2W](https://github.com/wasdwasd0105/USBPods-Pico2W)). Issues belong **here**, not on han-um or wasdwasd0105.

## What you get

| Path | What the PC sees |
|------|------------------|
| Playback | USB Audio Class 2 speaker, 16-bit stereo **48 kHz** → AirPods AAC-ELD A2DP |
| Microphone | USB mic, 16-bit mono **64 kHz** ← AACP type `0x58` AAC-ELD (librepods / han-um) |
| Control | Vendor HID + CDC serial: mic gain, battery, noise control, pair/slot |

Both directions run **simultaneously**. The OS does not use Bluetooth. There is **no Windows Test Mode driver**.

The settings page lives in [`web/`](web/) (GitHub Pages after you enable it, or open via `http://localhost` — browsers block WebHID/Web Serial on `file://`). Protocol: [`web/PROTOCOL.md`](web/PROTOCOL.md). AACP opcode map: [`AACP-FEATURES.md`](AACP-FEATURES.md).

## Hardware already proven (2026-09-02)

- **Board:** official Raspberry Pi Pico 2 W (RP2350 + CYW43439). Flashed han-um v1.0.0 `PicoW_USB_BT_Audio.uf2` (sha256 `b9ff47d45aed5ba31692d1c7e0e137ad8d197a4dcc5531a5c50bf01b8db58d5a`).
- **Headset:** AirPods Max 2 (A3454, H2, MAC `70:F9:4A:8A:D8:54`). No LE Audio. **USB-C on the Max 2 is playback-only** — 2-way audio is this dongle.
- **Host:** Mac sees `TinyUSB BT` UAC2 (speakers 16-bit stereo 48 kHz, mic 16-bit mono 64 kHz). Daily driver target is also **Windows** (Chrome/Edge WebHID or Web Serial).
- **Serial proof:** A2DP Apple AAC-ELD; AACP L2CAP PSM `0x1001` OPEN; `[MIC] START`; `[DEC] sampleRate=48000 numChannels=1 frameSize=480 aot=39`; decoder `err=0`.

P0 is that `0x58` mic. It works. Firmware in this tree must not break it. Gain is applied **after** decode, on PCM.

### Host Bluetooth will steal the link

If the Mac or PC’s **own Bluetooth** owns the Max 2, A2DP on the dongle dies and **speakers go silent**. Forget the headset on the computer (and phone, if it grabs the buds) before using USBPods Max. Re-pair to the Pico (long-press BOOTSEL) if you flashed over older firmware — this stack advertises Apple’s vendor ID.

## Why this exists

A normal PC Bluetooth connection that needs the AirPods microphone drops A2DP and switches to HFP: both directions become telephone-grade. iOS keeps A2DP and pulls the mic through **AACP**. [librepods](https://github.com/librepods-org/librepods) reverse-engineered that ([PR #655](https://github.com/librepods-org/librepods/pull/655)). Windows user-mode cannot open that L2CAP channel beside A2DP, so the Pico is the Bluetooth radio and the PC only sees USB audio.

## Acknowledgments

- **[han-um/USBPods-Pico2W-2-way-audio-implement](https://github.com/han-um/USBPods-Pico2W-2-way-audio-implement)** — working AACP `0x58` mic + USB 64 kHz capture on Pico 2 W. USBPods Max starts from that firmware.
- **[USBPods-Pico2W](https://github.com/wasdwasd0105/USBPods-Pico2W)** by wasdwasd0105 — USB audio, BTstack, AAC-ELD A2DP to AirPods.
- **[librepods](https://github.com/librepods-org/librepods)** — AACP. Mic bytes and decoder config from #655 (LuanAdemi); battery / noise control / CA identifiers from librepods docs. We do not invent opcodes.
- TinyUSB `uac2_headset`, BTstack A2DP source demos, TinyUSB 0.18 RP2350 panic workaround (USBPods #29) — unchanged in spirit; the panic patch is still **required**.

## Scope

- **Pico 2 W / RP2350 only.** Mic decoder + encoder need the 520 KB SRAM (~55 KB leftover after decode). Not RP2040, not a radio-less Pico 2.
- **Validated headset: AirPods Max 2 (A3454).** han-um also tested AirPods Pro 3. Other AirPods that speak AACP `0x58` may work; they are untested here.
- **Codecs:** AirPods AAC-ELD is the supported path. Upstream LDAC/AAC/SBC code is still linked but was RAM-trimmed; do not expect USBPods-Pico2W behaviour on those sinks.
- SRAM is the hard limit. Do not pull an HTTP stack onto the Pico; the website is on the computer.

## Installation

1. Download the UF2 from this repo’s Releases (or build below).
2. Hold **BOOTSEL**, plug USB, copy the UF2.
3. Product name stays `TinyUSB BT`. This firmware also exposes **USBPods Max Console** (CDC) and a vendor HID interface (WebHID). Windows may treat it as a new device because the TinyUSB PID bit for HID is now set — unplug/replug once if the old cached UAC state looks dead.

## Usage

1. **Pair:** long-press BOOTSEL and release — LED blinks fast. Put the AirPods case in pairing mode. If you flashed over stock USBPods, **re-pair once** (Apple DID).
2. **Playback:** select `TinyUSB BT` as output.
3. **Microphone:** select `TinyUSB BT` as recording. START/STOP follows the USB recording alt-setting. First capture after plug-in can take a couple of seconds (one automatic START retry).
4. **Reconnect:** short-press BOOTSEL when not streaming. Volume: single press up, double press down while connected; double-press while idle switches slot.
5. **Mic gain:** 0 to +24 dB, default 0, persisted in flash, soft-clip limiter.
   - Windows Chrome/Edge: open the [settings page](web/index.html), **Connect device**, move the slider.
   - Windows recording volume (UAC Feature Unit) moves the same gain.
   - Mac: `screen /dev/cu.usbmodem*` (115200) → `h` for help, `gain 12`.
6. Two pairing slots are unchanged from upstream.

### Windows settings page

Chrome or Edge, HTTPS (GitHub Pages) or `http://localhost`. Click **Connect device**, pick **TinyUSB BT** (HID). If HID is missing, pick the serial port **USBPods Max Console**. No driver, no install, no Test Mode.

The page’s **Diagnostics / Scope** section is for cutouts (Teams/YouTube silent while the USB device stays present):

1. Connect HID. Watch **Why silent?** — especially Ear on-head vs off-head (off-head can HID-pause the host) and A2DP up/down.
2. **Speaker test** plays a tone/sweep from the page and scopes *that* waveform. Use `setSinkId` to TinyUSB BT when Chrome exposes it. **Browsers cannot tap Teams or YouTube PCM.** If the test is audible in the cans, the USB speaker path works; if not, check A2DP / off-head / Windows default device.
3. **Start mic monitor** opens the UAC input (TinyUSB/USBPods/Pico label, same picker as Record 3s) and shows a live oscilloscope + peak/RMS. Compare “mic stream open” with the HID mic flag.
4. When audio dies, **Copy log** or **Download log** and paste it into an issue. The log is transitions + a 10s heartbeat, not every 1s STATUS poll.

Enable Pages: repo Settings → Pages → GitHub Actions (workflow [`.github/workflows/pages.yml`](.github/workflows/pages.yml) publishes `web/`).

### Serial console

```
screen /dev/cu.usbmodem* 115200
```

`h` help · `s` status · `gain 0-24` · `pair` · `disconnect` · `reconnect` · `slot 1|2` · `anc off|anc|trans|adaptive` · `ca on|off`

Logs (`[AACP]` `[MIC]` `[DEC]`) share this port. Attach them when filing issues.

### Troubleshooting

- **Speakers silent, mic maybe still works:** host Bluetooth owns the Max 2. Forget the device on the Mac/PC.
- **Teams/YouTube silent, TinyUSB BT still listed:** the browser cannot tap system PCM. On the settings page run **Speaker test** (scopes the page’s own tone) and watch **Ear** / **A2DP** transitions in the diagnostic log. Off-head can HID-pause the host while the USB device remains. Copy the log when it cuts.
- **Mic level 0 after reflash:** unplug/replug so Windows drops the cached device.
- **Watchdog loop / audio dies seconds after play:** you skipped the TinyUSB 0.18 panic patch (below).

## Building from source

Prerequisites: [pico-sdk](https://github.com/raspberrypi/pico-sdk) **2.1.1** (submodules `btstack`, `tinyusb`, `cyw43-driver`, `lwip`, `mbedtls`), CMake, Ninja, `arm-none-eabi` GCC (14.2.Rel1 was used), picotool. **Pico 2 W only** (`PICO_BOARD pico2_w` in `CMakeLists.txt`).

### Required: patch TinyUSB 0.18 in the SDK

pico-sdk 2.1.1 bundles TinyUSB 0.18.0, which panics `ep %02X was already available` on RP2350 during A2DP ([USBPods #29](https://github.com/wasdwasd0105/USBPods-Pico2W/issues/29)). The firmware **builds without the patch and then crashes**.

```sh
git -C "$PICO_SDK_PATH/lib/tinyusb" apply /path/to/this/repo/patches/tinyusb-0.18-rp2350-panic-fix.patch
```

### Build

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

Output: `build/PicoW_USB_BT_Audio.uf2`. Do not ship a hand-made UF2; if the ARM toolchain is missing, build on a machine that has it.

`CMAKE_POLICY_VERSION_MINIMUM=3.5` is required on CMake 4.x (ldacBT).

## License

Combined work: **GNU GPL-3.0** ([LICENSE](LICENSE)) because the mic path is derived from librepods.

- Inherited USBPods-Pico2W files remain **Apache-2.0** ([LICENSE.Apache-2.0](LICENSE.Apache-2.0)); original headers kept.
- FDK-AAC, libldac / ldacBT, TinyUSB, BTstack: their own licenses, **unchanged**.
- New USBPods Max files (`mic_gain`, `control`, `web/`, this README, `AACP-FEATURES.md`): GPL-3.0-only.

## What changed from han-um (this Max tree)

- Mic **gain boost** 0–24 dB after decode, flash persist, UAC mic Feature Unit, serial + WebHID/Web Serial.
- **AACP notifications** parsed for battery (`0x0004`) and noise control (`0x09`/`0x0D`); set noise control and Conversation Awareness using documented librepods packets only.
- **Vendor HID** + CDC menu so Windows Chrome can set gain without a driver.
- Static **`web/`** settings page.
- han-um P0 mic path, CDC debug, TinyUSB panic patch, SRAM decoder budget: **kept**.
