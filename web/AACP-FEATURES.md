# AACP features — LibrePods → USBPods Max 2

Source of truth: [librepods](https://github.com/librepods-org/librepods) `docs/control_commands.md`, `docs/opcodes.md`, `docs/AAP Definitions.md`, and working Android/Linux send/parse. **If docs conflict, trust working LibrePods code.** Unknown RX is dumped as hex on CDC. **No invented opcodes or payloads.**

Packet header: `04 00 04 00` + opcode u16le + payload. Channel: L2CAP PSM `0x1001` (`src/btstack/btstack_aacp.c`).

Control command frame (opcode `0x0009`): `04 00 04 00 09 00 [id] [data1] [data2] 00 00`. Android `sendControlCommand(Boolean)`: enabled=`0x01`, disabled=`0x02`.

Handshake replay runs after handshake + `0x004D` + `0x000F` on **every new AACP session**.

## Status legend

- **implemented** — we send and/or act
- **parsed** — we decode notifications
- **dumped** — hex on CDC only; no reply
- **skipped** — reason in the row

| Feature | Opcode / id | USBPods Max | Notes |
|---------|-------------|-------------|-------|
| AACP L2CAP PSM 0x1001 | — | implemented | already working |
| Handshake | raw 16-byte | implemented | already working |
| SET_FEATURE_FLAGS | `0x004D` `FF 00…` | implemented | already working |
| REQUEST_NOTIFICATIONS | `0x000F` `FF FF FF FF` | implemented | already working |
| Apple DID `004C` type `0x58` AAC-ELD mic | SDP / `0x58` | implemented | **do not edit** `aacp_mic_dec.c` or START/STOP bytes |
| Mic software gain 0–24 dB | no AACP opcode | implemented | after decode; persist skip while `aacp_mic_active()` or `aacp_is_connected()`; magic `0x5A`; ~750 ms debounce; UAC slider **not** capped at 12 dB; `0x8000` is mute |
| Battery | `0x0004` | parsed | AAP Definitions components |
| Noise control | `0x0009` / `0x0D` | implemented + parsed | 1 Off, 2 ANC, 3 Trans, 4 Adaptive |
| Conversation Awareness config | `0x0009` / `0x28` | implemented + parsed | 1 on, 2 off |
| **Owns connection** | `0x0009` / `0x06` | implemented + parsed | handshake **`0x01` own** (LibrePods `takeOver`) |
| **Connect Automatically** | `0x0009` / `0x20` | implemented + parsed | handshake **`0x01` enabled**. Android `automaticConnectionEnabled = true`. Keeps **this** host. Not `0x02`. |
| **Allow Auto Connect** | `0x0009` / `0x36` | skipped (not sent) | Android: “AUTOMATIC_CONNECTION is the only one used”. Parsed if the headset echoes it. |
| Ear Detection enable | `0x0009` / `0x0A` | implemented + parsed | handshake `0x01` |
| Raw Gestures | `0x0009` / `0x39` | implemented + parsed | handshake bitmask **`0x0F`** (single\|double\|triple\|long) so `0x0019` flows |
| ListeningModeConfigs | `0x0009` / `0x1A` | implemented + parsed | handshake **`0x0F`** (Off\|ANC\|Trans\|Adaptive) |
| Allow Off | `0x0009` / `0x34` | implemented | handshake **`0x01`** (LibrePods default off-listening-mode on) |
| ClickHoldMode | `0x0009` / `0x16` | implemented + parsed | handshake **`0x01, 0x01`** Noise control both sides. Docs: `0x01`=Noise, `0x05`=Siri |
| SingleClick / DoubleClick | `0x14` / `0x15` | skipped (not sent) | LibrePods has **no packet values**. Host maps `0x0019` instead (StemAction.kt: play/pause, next, prev) |
| Click intervals | `0x17` / `0x18` | implemented | handshake **`0x00` default** (Android `byteArrayOf(0x00)`) |
| Button Send Mode | `0x05` | skipped | enum only; LibrePods never sends a “send buttons to this host” value |
| CrownRotationDirection | `0x0009` / `0x1C` | implemented + parsed | default **`0x02`**; UI can set `0x01` reverse |
| AutoAnswer | `0x0009` / `0x1E` | implemented + parsed + persist | default **off `0x02`** |
| Chime volume | `0x0009` / `0x1F` | implemented + parsed + persist | default **50** (docs: 0–100 single byte). Android demo stores `0x46,0x50` two bytes — we send data1=saved/50 |
| Adaptive Volume | `0x0009` / `0x26` | implemented + parsed + persist | default **off `0x02`** (user spec; Android demo default is on) |
| Sleep Detection | `0x0009` / `0x35` | implemented + parsed + persist | default **on `0x01`**. If Max 2 has no event, still send enable and dump other RX |
| Ear detection report | `0x0006` | parsed + HID | `04 00 04 00 06 00 [primary] [secondary]`. **`0x00` = In Ear = ON HEAD**. `0x01` Out, `0x02` In Case. L=0 R=0 while worn is ON. **Off-head = either cup not 0x00** (Apple Max: pause if you lift one earphone; LibrePods linux default `PauseWhenOneRemoved`). HID Consumer **Pause** (`0xB1`) on take-off if A2DP streaming (~200 ms). **Play** (`0xB0`) on put-on only if we paused **and both cups stay 0x00 for 1.5 s** (Max sensors bounce L=0 R=0 after take-off; 200 ms resume fired Play immediately). Never Play/Pause toggle. First packet after connect: no HID. Max 2 is a headset. |
| Stem / crown / button events | `0x0019` | parsed + HID | Android `parseStemPressResponse`: size 8, type `pkt[6]`, bud `pkt[7]`. Types: single `0x05`, double `0x06`, triple `0x07`, long `0x08`. Bud L=`0x01` R=`0x02`. Map: single→Play/Pause, double→Next, triple→Prev, long→headset noise cycle via `0x16` (do not double-send `0x0D` when hold=noise). Unknown payloads dumped hex. **Crown rotation volume is AVRCP Absolute Volume**, not a `0x0019` type — applied to UAC speaker + HID Vol Up/Down. |
| CA speaking | `0x004B` | parsed | `04 00 04 00 4B 00 02 00 01 [level]`. `01/02` duck USB speaker a lot (~15%); `03` restore; `08/09` normal; 4–7 interpolate. **Mic path untouched.** Config remains `0x28`. |
| Device info | `0x001D` | parsed | Unsolicited host-only; do not request. Null-terminated strings: name, model, manufacturer, serial, fw. |
| Audio source req/resp | `0x000D` / `0x000E` | dumped | Android parses `0x0E` MAC+type. Hijack is `OWNS=1` + `sendMediaInformation` + `sendHijackRequest` (MAC-specific blobs). **No reply invented.** USB speaker alt≠0 still `control_request_reconnect()` for A2DP. |
| Connected devices | `0x002D` / `0x002E` | dumped | LibrePods list of connected devices. Live dual-connect steal shows the iPhone MAC in `0x2E`. **No reply invented.** |
| Smart routing | `0x0010` / `0x0011` | dumped | Same: no invented reply. |
| Rename | opcode **`0x001A`** | implemented | Docs `opcodes.md` lists `0x001E` but that id is AutoAnswer. LibrePods send path: `04 00 04 00 1A 00 01 [size] 00 [name]`. CDC `rename <str>` + WebHID cmd `0x0F`. |
| Hearing aid `0x2C/0x33/0x3D` | — | skipped | user: not Max 2 scope |
| HRM `0x30` | — | skipped | |
| PPE `0x37/0x38` | — | skipped | |
| In-case tone | — | skipped | Max 2 case is not Pro 2 speaker-case |
| Uplink EQ `0x3E/0x3F`, EQ `0x0053` | — | skipped | not mic gain |
| Transparency customization, Find My, spatial | — | skipped | |
| Head gestures | — | skipped | Android ATT; Linux will-not-implement. No Classic AACP packet found. |
| Volume swipe `0x25/0x23` | — | skipped | Pro stems, not Max crown |

## Dual-connect hunch (protocol side)

iPhone can keep the AACP/HFP session so crown volume hits the phone. Firmware always claims **`0x06` own** and LibrePods **`0x20=0x01`** on every AACP session, and **re-sends `0x06`/`0x20` (LibrePods takeOver)** when A2DP is stolen. Dual-connect may still need the user to set iPhone “Connect only When Last Connected”. Hijack `0x0E` blobs are not invented.

## HID

- Vendor usage page **`0xFFA0`**, 32-byte status report, protocol minor **1** (bytes 15, 22–31 extras; 0–14 and 16–21 unchanged).
- Separate HID Consumer Control (usage page `0x0C`): Play/Pause, Next/Prev, Volume Up/Down, Mute. TinyUSB instance 1. `control.c` must not include `tusb.h` (duplicate `hid_report_type_t` with BTstack). Callbacks live in `src/control_usb.c`.

## AVDTP

- On `AVDTP_SUBEVENT_SIGNALING_CONNECTION_RELEASED`, wait 400 ms then retry, unless `reclaim_hold` (explicit disconnect / pair-scan). Do **not** stack a new `avdtp_source_connect` until the previous attempt completes.
- Dual-connect steal (unexpected `AVDTP_SI_SUSPEND` / `AVDTP_SI_START` reject / streaming released while USB speaker alt ≠ 0): reassert `0x06` owns, drop signaling, wait **2 s**, then reclaim. Status **129** (`0x81`, phone still holds the sink) backs off 5–10 s instead of 2 s. Max 40 tries.
- USB speaker alt ≠ 0 and A2DP down **or signaling up but not streaming** → `control_request_reconnect()`.
- AAC-ELD send-fail: suspend threshold 8; **do not** reset FDK every 100 ms; do not tight-loop restart on suspend fail. Local recovery still auto-STARTs; unexpected pause does not.
