# Synthseqr v3 — Firmware Architecture

Status: **planning**. Written 2026-09-01, before v3 PCBs returned from fab.

This document covers the port of Synthseqr 2.x firmware onto v3 hardware: the
ownership model between the two processors, the link protocol, and the phasing.

## 1. Hardware context

v2 ran on an Adafruit Grand Central M4 (SAMD51P20, 1 MB flash, onboard SD, ~50
GPIO) driving a serial 16x2 character LCD.

v3 replaces that with two processors:

- **Adafruit Feather M4 Express** (SAMD51J19, 512 KB flash, **no SD card**, ~20
  usable GPIO). Owns the sequencer.
- **CYD** ("Cheap Yellow Display" — ESP32 + 320x240 resistive touchscreen).
  Owns the display, persistent storage, and the radios.

The pin shortage and the missing SD card are what force storage off the MCU.

### v3 pin map

Derived from `hardware/synthseqr_v3.kicad_pcb` (pad→net), not from the v2
source. Authoritative as of the first fab run.

| Feather M4 | Net | Notes |
|---|---|---|
| A0 | `FADER_SIGNALS` | U1 mux COM — 16 slide pots |
| SDA / SCL | `SIG_BTNA` / `SIG_BTNB` | U2/U3 mux COM. Plain GPIO — **I2C is unavailable** |
| D4 / D6 / D9 / D12 | `S0`–`S3` | shared select lines, all three muxes |
| D5 | `LED_DATA` | → 74AHCT125 level shifter → 33x SK6812 |
| A2 / A3 / A4 | `ENC_A` / `ENC_B` / `ENC_SW` | EC11 encoder |
| D13 | `BTN33` | direct; shares the onboard LED |
| D0 / D1 | `MIDI_RX` / `MIDI_TX` | `Serial1`. DIN in via H11L1 opto (J6), DIN out (J7) |
| D10 / D11 | `CYD_TX` / `CYD_RX` | J5, 4-pin to CYD. **Not a hardware UART by default** |
| — | free | A1, A5, SCK, MOSI, MISO |

Buttons are active LOW with 10k pull-ups (RN1–4).

> **Known discrepancy:** `tests/synthseqr_bringup/synthseqr_bringup.ino` sets
> `PIN_BTN_33 = A5`, but the PCB routes BTN33 to **D13** and leaves A5
> unconnected. Test 7 currently reads a floating pin.

## 2. Ownership model

**The Feather is authoritative.** It is a server; the CYD is a client that
renders and proposes.

- **Feather owns** pattern data, transport state, tempo, clock, and all
  sequencer config. It is the only thing that mutates state.
- **CYD owns** pixels, storage media, and radios. It holds a *mirror* of
  sequencer state and is never an authority on it.

Every change follows one path, regardless of origin:

```
input → CYD sends Event → Feather validates & applies
      → Feather broadcasts new state → CYD updates mirror → CYD renders
```

A web-browser edit round-trips through the Feather exactly like a touch or a
physical fader move. At 1 Mbaud over a few centimetres of trace the latency is
inaudible, and it means no conflict-resolution logic is ever needed between a
browser and a fader.

### Consequences

- **There is exactly one canonical serialization format**, defined by the
  Feather and versioned. The CYD stores it verbatim. It may parse it read-only
  to drive the web UI, but never writes it back directly.
- **The Feather must boot, sequence, and play DIN MIDI with the CYD
  unplugged.** v2's `FlashAsEEPROM_SAMD` path is retained as a 4-pattern local
  fallback. The CYD is an enhancement, not a dependency — which also keeps
  bring-up and debugging tractable.

## 3. Link protocol

### Framing

COBS with a `0x00` delimiter, wrapping:

```
[type:1][seq:1][payload:N][crc16:2]
```

COBS rather than SLIP because resync after a garbled byte is unambiguous and
overhead is constant. The CRC is there because fader-noise corrupting a stored
pattern is a bug that would be very expensive to chase.

Baud: 115200 for bring-up, then 921600 / 1 M once stable.

### Message classes

| Range | Direction | Purpose |
|---|---|---|
| `0x0x` | both | link mgmt: hello, protocol version, ping, ack/nak |
| `0x1x` | F→C | display (phase 1: raw 16x2 backpack passthrough) |
| `0x2x` | C→F | input events: touch, slider-mode select, web/BLE edits |
| `0x3x` | F→C | state broadcast (the mirror) |
| `0x4x` | both | storage, chunked with offset/len |
| `0x5x` | both | MIDI relay for BLE |

### Timing rules

These govern everything else:

- **The sequencer ISR touches nothing but flags.** v2 already does this
  correctly in `sequencer_timer.ino` (TC4 sets `_hw_clock_pending` /
  `_hw_step_pending`, main loop acts). Follow that precedent.
- **All UART I/O is ring-buffered and serviced from the main loop.**
- **Storage transfers are chunked** (~128 B payloads, per-chunk ack) so a
  pattern save cannot monopolize the link or stall a display update.

### Two decisions locked in early

- **MIDI clock does not travel over BLE.** DIN out is direct from the Feather
  and tight. BLE adds a UART hop plus the ESP32 BLE stack — acceptable for note
  data, not for clock. External clock *in* likewise arrives via DIN on
  `Serial1`, straight into the Feather, never via the CYD.
- **The protocol version is exchanged in the `hello` handshake** and a mismatch
  fails loudly. The two boards will be flashed independently for months; silent
  version skew is the most expensive available failure mode.

## 4. Repository layout

```
synthseqr/
  firmware/feather/    ported v2 sequencer core
  firmware/cyd/        ESP32 + LVGL
  protocol/            shared header — ONE definition, both sides compile against it
  hardware/            KiCad (gitignored contents)
  tests/               bring-up sketch + host-side protocol tests
  docs/
```

`protocol/` as a single shared header consumed by both builds is what keeps the
two codebases from drifting.

## 5. Porting notes from the v2 source

Three findings that shape the phasing:

**The display is already a single choke point.** v2's `config.h:426` is
`#define lcd Serial1`. All ~92 display call sites across `config_menu.ino`,
`diagnostics.ino` and others are `Serial1.print()` through that one macro.
Pointing `lcd` at a CYD transport that speaks the same backpack command set
(`?f`, `?x00?y0`, `?D0…` custom glyphs) brings the entire v2 menu system across
untouched. This is why 16x2 emulation comes first.

**Storage is cleanly abstracted.** Only three call sites exist outside the
storage files: `boot_load()` in the main sketch, and `save_to_sd()` /
`save_to_eeprom()` in `config_menu.ino`. Retargeting to the CYD is contained.

**The sequencer core ports as-is.** TC4 exists on the SAMD51J19. The external
clock path is pure software. `chords.ino`, `scales.ino`, `clock_div.ino`,
`transport.ino` and `midi_note_sending.ino` need little or no change.

The real cost is `config_menu.ino` (1,573 lines, 69 direct display writes),
which is written against a two-line character cell paradigm. Deferring its
rewrite to phase 4 is the main reason the phasing below works.

## 6. Phasing

| Phase | Scope |
|---|---|
| **0** | Link bring-up: SERCOM UART proof, COBS framing, handshake. No PCB needed. |
| **1** | 16x2 emulation. Redirect the `lcd` macro at a CYD transport; ESP32 renders a 16x2 grid. Feather standalone on the EEPROM fallback. |
| **2** | Input layer: mux scanning, encoder, LEDs. The bring-up sketch is most of this. |
| **3** | Storage moves to the CYD. Retarget the three call sites; keep EEPROM fallback. |
| **4** | Native UI: touch, slider modes. Retire the 16x2 emulation per-screen, not all at once. |
| **5** | Radios: BLE MIDI, WiFi web interface. |

Phases 1–3 are the port proper. Phase 4 is where `config_menu.ino` is finally
rewritten.

## 7. Open risks and questions

**Risk — the CYD link is not a hardware UART.** On v2 the LCD lived on
`Serial1`. On v3 `Serial1` (D0/D1) is DIN MIDI, and the CYD sits on D10/D11,
where the Feather M4 has no `Serial2`. This needs a hand-instantiated SERCOM
UART. It looks feasible — D10 is PA20 (SERCOM PAD[2] → TX) and D11 is PA21
(PAD[3] → RX), i.e. `UART_TX_PAD_2` + `SERCOM_RX_PAD_3` — but this is
**unverified against the SAMD51 datasheet mux table and must be proven on the
bench**. If it fails, the fallback is the spare MOSI/MISO pins, which would mean
a board change.

**Open — does the CYD parse the pattern format, or stay a pure blob store?**
Blob store is trivially correct and ships sooner; parsing is eventually required
for a web UI that can browse and edit patterns. Undecided.

**Note — build host.** v2 was built on macOS (`.vscode/arduino.json` references
`/dev/tty.usbmodem14101`). The Linux workstation needs `arduino-cli` plus the
`adafruit:samd` and `esp32` cores.
