# USBPods 2-Way Audio — AirPods Hi-Res Microphone on a Pico 2 W

A fork of [USBPods-Pico2W](https://github.com/wasdwasd0105/USBPods-Pico2W) that adds **2-way audio for AirPods**: high-quality playback *and* the AirPods' high-resolution microphone, at the same time, on any PC — no drivers, just a Raspberry Pi Pico 2 W acting as a USB sound card.

Plug in the dongle and Windows (or any OS) sees a normal USB headset:

- **Playback**: 48 kHz stereo, streamed to the AirPods as AAC-ELD over A2DP (upstream's work)
- **Microphone**: 64 kHz mono, received from the AirPods' hi-res mic stream and decoded on the Pico (this fork)
- Both run **simultaneously** — playback quality does not drop when the mic is in use


## Why this exists

When you use the AirPods microphone with a regular PC Bluetooth connection, the link has to switch from A2DP to the hands-free profile (HFP). That drops both directions to a narrow, telephone-grade codec — music playback and mic quality degrade badly. This is a Bluetooth profile limitation, not an AirPods one.

iOS devices don't have this problem: iOS keeps A2DP playback running and pulls the microphone audio through a **proprietary Apple channel (AACP)** as an AAC-ELD stream. The [librepods](https://github.com/librepods-org/librepods) project reverse-engineered this, and its Linux (Rust) branch implemented full 2-way audio ([PR #655](https://github.com/librepods-org/librepods/pull/655)).

On Windows, that approach can't be replicated in software — the Bluetooth stack doesn't let user code open the required L2CAP channel alongside A2DP. So this project moves the whole problem into hardware: a Pico 2 W speaks Bluetooth to the AirPods (based on USBPods-Pico2W) and presents itself to the PC as a plain USB audio device. The OS needs no Bluetooth involvement at all.


## Acknowledgments

This fork is an experiment in splicing two existing projects together — nearly all of the hard work happened elsewhere:

- **[USBPods-Pico2W](https://github.com/wasdwasd0105/USBPods-Pico2W)** by wasdwasd0105 — the entire foundation. USB audio, BTstack integration, and especially AAC-ELD A2DP streaming to AirPods on a Pico already worked before this fork added a single line. The mic path also reuses its architecture throughout.
- **[librepods](https://github.com/librepods-org/librepods)** — the AACP protocol. Specifically [PR #655](https://github.com/librepods-org/librepods/pull/655) by LuanAdemi, which implemented hi-res microphone support on Linux and is what this fork ports (the AACP byte sequences, the 0x58 stream layout, and the decoder configuration are direct translations of it). That PR in turn stands on the librepods community's accumulated reverse-engineering of the AirPods protocols — thanks to that whole community.
- Upstream's own acknowledgments (TinyUSB's `uac2_headset` example, BTstack's A2DP source demos) apply here unchanged.

The upstream author also found and fixed the TinyUSB 0.18 panic issue documented in the build section below — this fork merely bundles the fix.


## About this fork

This is an **experimental project**: two proven projects glued together to see if AirPods 2-way audio could work on a PC. It turned out to work well — but treat it as an experiment, not a product.

Most of the code in this fork was written with **Claude Code** (Anthropic's AI coding agent), directed and hardware-tested by me. I am not an experienced Bluetooth or embedded developer. Verification consisted of several days of iterative on-device testing during development, followed by about a week of daily use (simultaneous playback + mic) without failures — nothing more formal than that.

Accordingly:

- The upstream-bug diagnoses listed in *What changed from upstream* also came out of that AI-assisted debugging, not my own analysis. Each fix was verified on hardware (symptom present before, gone after), and the commit messages record the evidence in detail — treat those, not me, as the technical reference.
- This fork is **not** submitted as a PR to upstream, to avoid pushing a large, AI-assisted, single-purpose change onto the upstream maintainer. If any part of it is useful upstream, feel free to take it.
- **Please report issues on this repository, not upstream.** I will read everything, but this is a hobby project maintained on a best-effort basis — I may not be able to fix what you find. Logs from the debug console (see *Troubleshooting*) make a fix much more likely.


## Scope and tested environment

Please read this before using or building:

- **Board: Raspberry Pi Pico 2 W only.** The AAC-ELD mic decoder needs ~258 KB of heap on top of the encoder's ~122 KB; the whole audio path fits in the RP2350's 520 KB SRAM with roughly 55 KB to spare. It cannot fit on the original Pico W (RP2040, 264 KB), and no other board has been tried.
- **Earbuds: tested with AirPods Pro 3 only.** The AACP byte sequences come from librepods and may work on other recent AirPods models, but none have been tested.
- **Codecs: the AirPods path (AAC-ELD) is the only tested path.** Upstream also supports LDAC / AAC / SBC sinks; that code is still present but untested in this fork, and its jitter buffer was shortened to free RAM for the decoder (see *What changed*), so behavior may differ from upstream. The purpose of this fork is AirPods 2-way audio, nothing else.
- Windows 11 was the only tested host OS. The device is a standard USB Audio Class 2 composite, so other OSes should work, but are untested.


## Installation

1. **Download the UF2 file** from this repository's releases page.
2. **Enter bootloader mode**: hold the BOOTSEL button on the Pico while plugging it into USB. It appears as a mass-storage drive.
3. **Copy the UF2 file** onto the drive. The Pico reboots into the firmware automatically.


## Usage

Pairing and buttons work as in upstream:

1. **Pair**: long-press BOOTSEL and release — the LED blinks fast. Put the AirPods case into pairing mode; the dongle connects automatically.
   - If you flashed this fork over stock USBPods firmware, **re-pair once**: this fork advertises an Apple vendor ID (required for the mic protocol), and the AirPods treat it as a new device.
2. **Playback**: select `TinyUSB BT` as the output device and play audio.
3. **Microphone**: select `TinyUSB BT` as the recording device. The mic stream starts when an application opens the device and stops when it closes it. First capture after plug-in can take a couple of seconds (the firmware retries the mic start once automatically).
4. **Reconnect**: short-press BOOTSEL when not streaming. Volume: single press = up, double press = down while connected.
5. Two pairing slots (device A/B) and the LED status patterns are unchanged from upstream — see the [upstream README](https://github.com/wasdwasd0105/USBPods-Pico2W#usage) for details.

### Troubleshooting

- **Mic shows level 0 right after reflashing**: Windows' audio engine sometimes caches the old device state. Unplug and replug the dongle.
- **Attach logs when reporting issues**: the dongle exposes a serial port (`Pico Debug Console` / a COM port) next to the audio device. Open it with any terminal program (e.g. PuTTY, 115200 baud, any settings) — the firmware buffers its boot log and streams live status lines (`[AACP]`, `[MIC]`, `[DEC]`) once a terminal attaches. Nothing is sent unless you open the port.


## What changed from upstream

New functionality:

- `src/btstack/btstack_aacp.{c,h}` — **AACP channel** (L2CAP PSM 0x1001): init handshake, mic START/STOP, and demuxing of the type-0x58 audio SDUs into AAC-ELD access units. Ported from librepods.
- `src/btstack/aacp_mic_dec.{c,h}` — **AAC-ELD mic decoder** (fdk-aac, mono 480-sample frames, 64 kHz) feeding a lock-free PCM ring.
- `src/tinyusb/usb_descriptors.h`, `uac.c` — **USB microphone**: UAC2 input terminal + streaming interface + async isochronous IN endpoint, 16-bit mono 64 kHz with its own fixed clock entity (the speaker stays at 48 kHz). Opening/closing the recording stream (alt setting) is what starts/stops the AirPods mic.
- `src/tinyusb/debug_cdc.{c,h}` — **debug console**: printf goes into an interrupt-safe ring buffer drained over a CDC-ACM serial port. Replaces UART logging (see below).
- SDP now advertises an Apple Device ID record (vendor 0x004C) — several AACP features are gated on it.

Fixes and behavioral changes (all of these bit us during development):

- **UART stdout removed.** The Pico SDK's UART printf blocks the calling context at 115200 baud; log bursts from the BTstack run loop or timer IRQs caused audio stalls and stutter. All logging now goes through the non-blocking CDC ring.
- **Slot/MAC flash storage moved down one sector.** Upstream stored its pairing-slot data in the last flash sector, which is inside BTstack's link-key storage region — link-key writes could corrupt the slot MACs. Also falls back to the link-key address if a stored MAC looks erased.
- **AVRCP Target SDP record handle collision fixed** (0x10002 was reused, so the record silently failed to register and absolute-volume control from the PC side never advertised).
- **Duplicate HCI event-handler registration fixed** (two handlers shared one registration struct; only the last one ever ran).
- **Button actions moved out of IRQ context.** BOOTSEL button handling called BTstack APIs from a timer IRQ, which can deadlock against the async-context lock; presses are now latched and executed from the main loop.
- **A2DP jitter-buffer pool trimmed from 24 to 8 slots** (96 KB → 32 KB static RAM) and fdk-aac's decoder work buffers reduced from 8-channel to 2-channel size, to fit the mic decoder in RAM. The AirPods path uses 6 slots and is unaffected; other codecs get a shallower buffer.
- **Watchdog period is 8 s** (upstream: 2 s) — the decoder allocates ~258 KB in many small chunks at init, and all long-term stability testing was done at this value.


## Building from source

Prerequisites: [pico-sdk](https://github.com/raspberrypi/pico-sdk) **2.1.1** (with `btstack`, `tinyusb`, `cyw43-driver`, `lwip`, `mbedtls` submodules), CMake, Ninja, an `arm-none-eabi` GCC toolchain (14.2.Rel1 was used), and picotool.

### Required: patch the SDK's bundled TinyUSB first

pico-sdk 2.1.1 bundles TinyUSB 0.18.0, which has a spurious `panic("ep %02X was already available")` in `lib/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c`. On the Pico 2 W (RP2350) this guard trips during normal A2DP streaming and halts the firmware — the symptom is a watchdog reboot loop / audio cutting out seconds after playback starts. This is [USBPods issue #29](https://github.com/wasdwasd0105/USBPods-Pico2W/issues/29); the upstream maintainer's confirmed workaround is to remove the panic (the code below it already clears the AVAIL bit safely).

A ready-made patch is included in this repository:

```sh
git -C "$PICO_SDK_PATH/lib/tinyusb" apply /path/to/this/repo/patches/tinyusb-0.18-rp2350-panic-fix.patch
```

**The firmware builds fine without this patch — and then crashes at runtime.** Don't skip it. (If you build with the Raspberry Pi Pico VS Code extension, the same patch must be applied to the extension's copy of the SDK.)

### Build

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

The output is `build/PicoW_USB_BT_Audio.uf2`. The board (`pico2_w`) is set in `CMakeLists.txt`. The `CMAKE_POLICY_VERSION_MINIMUM` flag is needed with CMake 4.x, which otherwise rejects the older minimum-version declarations in the bundled ldacBT.


## License

This fork as a whole is distributed under the **GNU General Public License v3.0** ([LICENSE](LICENSE)), because the microphone implementation is derived from librepods (GPL-3.0).

- Code inherited from upstream USBPods-Pico2W remains under the **Apache License 2.0** ([LICENSE.Apache-2.0](LICENSE.Apache-2.0)); original file headers are retained.
- Third-party components keep their own licenses:
  - [libldac](https://android.googlesource.com/platform/external/libldac) — Apache-2.0, © Sony Corporation. Note Sony's [LDAC certification requirement](https://www.sony.net/Products/LDAC/aosp/) for products.
  - [ldacBT](https://github.com/EHfive/ldacBT) — Apache-2.0, © Huang-Huang Bao
  - [FDK AAC](https://github.com/mstorsjo/fdk-aac) — Software License for the Fraunhofer FDK AAC Codec Library for Android (see `3rd-party/fdk-aac/NOTICE`)
  - TinyUSB (MIT) and BTstack (BlueKitchen license) are used via the Raspberry Pi Pico SDK.
