# AACP features — MagicPods / librepods → USBPods Max

Source of truth for opcodes: [librepods](https://github.com/librepods-org/librepods) (`docs/opcodes.md`, `docs/control_commands.md`) and the AACP 0x58 mic path from [PR #655](https://github.com/librepods-org/librepods/pull/655). Windows KMDF [PR #716](https://github.com/librepods-org/librepods/issues/716) is **reference only** — this firmware is not a Windows driver.

**Rule: no invented opcodes.** If a row says “not in librepods”, we do not send a guessed packet.

Packet header for every control AACP PDU: `04 00 04 00` + opcode u16le + payload. Channel: L2CAP PSM `0x1001` (`src/btstack/btstack_aacp.c`).

| Feature | librepods | MagicPods-class intent | USBPods Max status | File / notes |
|---------|-----------|------------------------|--------------------|--------------|
| AACP L2CAP PSM 0x1001 | linux 2-way #655 | required | **P0 done** (han-um) | `btstack_aacp.c` `AACP_PSM` |
| Handshake + SET_FEATURE_FLAGS `0x004D` + REQUEST_NOTIFICATIONS `0x000F` | opcodes.md, host-capabilities.md | required | **P0 done** | `aacp_handshake`, `aacp_set_feature_flags` (`FF 00…`), `aacp_request_notifications` (`FF FF FF FF`) |
| Apple DID SDP (vendor `0x004C`) | needed for several AACP features | required | **P0 done** | `btstack_avdtp_source.c` Device ID server |
| Hi-res mic START/STOP type `0x58` | #655 `aacp_audio.rs` | required | **P0 done — do not regress** | `aacp_mic_start_bytes` / `aacp_mic_stop_bytes`; USB alt-setting in `uac.c` |
| AAC-ELD mic decode (ASC `F8 E6 30 00`, AOT 39, 480 samples, 64 kHz USB) | #655 | required | **P0 done** | `aacp_mic_dec.c` — gain is applied **after** decode on PCM, not here |
| Mic digital gain | **no AACP opcode** | MagicPods host UX only | **P0.5 software path** | `mic_gain.c` after `aacp_mic_pcm_read()` in `uac.c`. Uplink EQ `0x3E`/`0x3F` exist as control ids but are **not** a documented dB gain; unused |
| Precise battery | opcode `0x0004` | P2.1 | **parse in this PR** | AAP Definitions: `[count] ([component] 01 [level] [status] 01)*`. Components: Right=`0x02`, Left=`0x04`, Case=`0x08`. Headset-only devices (Max) may use another component id — stored as `headset`. `aacp_handle_control()` |
| Noise control Off / ANC / Transparency / Adaptive | control `0x09` id `0x0D` | P2.2 | **parse + set in this PR** | Values: `1` Off, `2` ANC, `3` Transparency, `4` Adaptive (`docs/control_commands.md`). Packet `04 00 04 00 09 00 0D [mode] 00 00 00`. Max 2 has all four |
| ListeningModeConfigs bitmask | id `0x1A` | which modes the buds allow | **documented, not sent** | bitmask Off=`0x01` ANC=`0x02` Trans=`0x04` Adaptive=`0x08` |
| Ear detection | opcode `0x0006` | P2.3 → USB HID consumer play/pause | **parse raw bytes in this PR; HID consumer not wired** | Payload layout from AAP Definitions / librepods (left, right). Play/pause HID is a separate consumer report — skipped this PR to keep the vendor HID report tiny |
| Conversation Awareness config | control id `0x28` | P2.4 | **set + parse in this PR** | `1` enabled, `2` disabled. Packet `04 00 04 00 09 00 28 [val] 00 00 00` |
| Conversation Awareness speaking event | opcode `0x004B` | duck volume while speaking | **opcode known; payload not decoded** | opcodes.md. Do not guess the speaking-level bytes; config is `0x28` |
| Button / stem / Digital Crown | ids `0x14`–`0x16`, `0x1C`, opcode `0x0019` stem press | P2.5 | **documented, not implemented** | `0x1C` CrownRotationDirection `1`=reversed `2`=default. Stem press `0x0019` — need librepods `docs/stem-press.md` before sending or mapping Max 2 crown. **No guessed packets** |
| Mic Mode (which bud) | id `0x01` | not P0 | **documented, not sent** | Automatic/Right/Left — this is bud selection, **not** analog/digital gain |
| Host capabilities `0x004D` byte6 `0xFF` vs `0xD7` | host-capabilities.md | unlocks Adaptive + CA on some firmware | **already sending `0xFF`** (han-um init sequence) | Do not change without a mic regression test |
| EQ `0x0053` / uplink EQ `0x3E` `0x3F` | opcodes.md / control_commands.md | not a mic-gain substitute | **not used** | |
| MagicPods tray / VoiceOver / Live Tiles / HandsFree disable | Windows host app | **out of scope** | skipped | We are a USB headset + web page, not a Windows shell extension |

## Mic path (P0) — leave it alone

```
AirPods --L2CAP 0x58 AU--> aacp_handle_audio_sdu
       --> aacp_mic_dec_decode (fdk-aac, unchanged)
       --> PCM ring
       --> aacp_mic_pcm_read
       --> mic_gain_apply   ← only new audio math
       --> USB UAC2 IN 16-bit mono 64 kHz
```

USB speaker remains 48 kHz stereo AAC-ELD A2DP. Mic START/STOP still follows the recording alt-setting.

## What this firmware will not do

- Windows KMDF / Test Mode / HandsFree profile disable
- Guessed AACP mic-gain opcode
- RP2040 / Pico W (not enough SRAM)
- Changing the AAC-ELD decoder working set in this PR
