# AirPods Max dual-connect: control boundary and recovery

## Diagnosis

There are three endpoints but only two Bluetooth ACLs:

```
Windows --USB--> Pico 2 W --ACL/A2DP+AACP--> AirPods Max <--ACL-- iPhone
```

The Pico owns only its ACL to the Max. It can inspect, suspend, disconnect, and
reconnect profiles on that link. It can also send the ownership and smart
routing packets implemented by LibrePods over its AACP channel.

The iPhone-to-Max ACL is terminated by the Max. It is not visible in the Pico
controller's HCI connection table, so the Pico cannot HCI-disconnect it, block
its reconnection, change its iOS Bluetooth policy, or guarantee which source
the Max chooses. A MAC reported by AACP `0x0E` or `0x2E` identifies the other
source; it does not turn that source into a Pico ACL.

Soft-exclusive therefore tries to disconnect an extra, non-Max **Pico ACL**
only when one really exists. In the normal topology above it logs
`no Pico ACL to phone` and must continue into Max-side recovery. Pairing keys
are never erased and the Pico-to-Max ACL is never deliberately kicked.

## Primary architecture: cooperative Max-side takeover

When Windows opens or streams to the USB speaker, firmware holds ownership:

1. Enter soft-exclusive HOLD and try the extra-Pico-ACL kick.
2. If no kick succeeds, keep the existing Max link and reassert LibrePods
   control `0x06` (owns) and `0x20` (automatic connection).
3. On an explicit Max-side ownership signal (`owns=00`, `0x0E` peer
   MEDIA/CALL, or `0x11 SetOwnershipToFalse`), immediately enter the existing
   steal recovery instead of waiting for a later AVDTP event.
4. Pause the Windows media session, drop/rebuild AVDTP signaling, and send the
   LibrePods `0x10` takeover sequence after the ownership claim.
5. Once A2DP START succeeds, clear UAC mute and send delayed HID Play. One
   bounded retry nudges the Windows USB stream if it did not restart.

Unexpected AVDTP SUSPEND and START rejection remain equivalent fallback
triggers. Resource status `0x81` backs off before retrying. USB idle enters a
two-second grace period and then releases HOLD.

This is the best firmware-only behavior on Pico 2 W. It preserves iPhone
pairing and does not require Mac Bluetooth to be disabled. It is not proof of
Apple-native multipoint: the Max may ignore `0x10`, especially if it did not
accept the Pico's Apple DID record, and an actively playing iPhone may keep
`owns=00`.

## Operational fallback: disconnect, do not forget

If the Max repeatedly retains the iPhone while the iPhone is actively playing,
disconnect the Max from the iPhone (Control Center/Settings) or set **Connect
to This iPhone** to **When Last Connected**, then start Windows playback.
This keeps the pairing and is reversible. There is no documented LibrePods
opcode that lets the Pico force the Max to tear down the separate iPhone ACL.

A re-pair to the Pico may be required once after flashing firmware that first
advertises the Apple DID SDP record. DID acceptance improves the chance of
native-like takeover, but does not give the Pico authority over the iPhone ACL.

## Windows + Max + iPhone test plan

Use CDC `s`/`status` and keep a timestamped log.

1. **Baseline:** iPhone Bluetooth on and paired; Max connected to Pico and
   iPhone; Windows output is `TinyUSB BT`. Confirm A2DP and AACP are up,
   `softexcl=1`, and `sxphase=hold` during Windows PCM.
2. **Max-only ACL proof:** confirm the Pico logs only the Max HCI ACL.
   Trigger idle iPhone playback. Expect `no Pico ACL to phone`, then
   `fight reclaim/0x10`; never expect a disconnect of the Max address.
3. **Explicit ownership signals:** capture each available `owns=00`, `0x0E`
   peer MEDIA/CALL, and `0x11 SetOwnershipToFalse`. Recovery must begin on that
   signal without waiting for SUSPEND/START rejection.
4. **Windows recovery:** expect HID Pause, OWNS/`0x20`, AVDTP signaling
   rebuild, `0x10` when a `0x2E` peer is known, successful START, UAC unmute,
   then delayed HID Play. Audio must return without a default-device bounce.
5. **Busy-phone limit:** repeat while the iPhone is actively playing. Record
   whether the Max holds `owns=00` or returns status `0x81`. If bounded retries
   cannot reclaim, disconnect the Max on iPhone without forgetting it and
   verify Windows recovers.
6. **Idle release:** stop Windows PCM and close the speaker alt setting.
   Expect HOLD -> GRACE -> IDLE after about two seconds; verify the iPhone can
   use the Max again.
7. **Kick branch (only if reproducible):** establish a genuine non-Max ACL to
   the Pico, start Windows PCM, and verify only that ACL is disconnected,
   `sxkick=1`, pairing remains, and the Max ACL stays up.
8. **Regression:** exercise Windows speaker + 64 kHz microphone together,
   off-head pause/resume, explicit USB disconnect/reconnect, and at least ten
   alternating phone/Windows takeover cycles. Check for HID Play storms,
   reconnect tight loops, USB disappearance, and A2DP underruns.

Host policy tests validate packet parsing and state transitions, but they
cannot prove radio behavior, Apple DID acceptance, or Windows USB recovery.
