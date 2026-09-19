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

### Power tree, and what USB alone does not power

From `hardware/netlist_v3.txt`:

```
J1 barrel "5V 4A in" -> +5V -+- NetTie_1 -> LED_5V -> U5 VCC, 36x SK6812, J4
                             +- D1 -> +5V_FTHR -> Feather pin 19 (USB)
                             +- D2 -> +5V_CYD  -> J5 pin 1
                             +- D4 SMBJ5.0A TVS, C1 1000uF
Feather 3V3 regulator -> 3V3 -> RN1-4, RV1-16, U1/U2/U3, U4
```

**`LED_5V` is the barrel jack and nothing else.** It reaches `+5V` through a net
tie — no diode, no switch — and it is also `U5`'s VCC, so with only USB plugged
in the level shifter has no supply and all 36 LEDs are dark. That is the
expected result on USB alone, not a fault. Buttons, encoder and faders keep
working throughout, because they hang off the Feather's own 3V3 regulator.

> Careful reading diode polarity out of the netlist: pad numbering is not
> consistent across the footprint libraries this board uses. `D3`'s net names
> (`OPTO_A` / `OPTO_K`) imply pad 1 = anode, while `D4`, a unidirectional TVS
> with pad 1 on `+5V`, implies pad 1 = cathode. Both cannot be right. Check
> `D1`/`D2` against the schematic before concluding which way the power path
> flows — and see `hardware/footprint_audit.md`, since v3.2 lost a week to
> exactly this class of error in a hand-drawn footprint.

### Running the whole thing from one USB cable (v3.3 proposal)

Nothing currently back-feeds `LED_5V` from USB, by design. To allow it, add a
**second Schottky from the Feather's USB pin to `LED_5V`**, anode at the Feather
side, alongside the existing direct barrel feed:

```
J1 barrel -> +5V -+- NetTie_1 --------> LED_5V   (5.0V when the jack is in)
                  +- D1 -> +5V_FTHR -> Feather USB pin
                                        `- D_NEW -> LED_5V  (4.65V on USB alone)
```

It self-arbitrates. Barrel in: `LED_5V` sits at 5.0 V and `D_NEW` is reverse
biased, because the Feather's USB pin is a diode drop below it — nothing pushes
back into the host. USB only: `LED_5V` gets 5 V less a Schottky drop, which the
SK6812 are happy with and which still lets the AHCT125 clear their V<sub>IH</sub>.
Both: the barrel wins. Extend the same node to `+5V_CYD` if the CYD should
follow.

Two caveats. **It depends on `D1` being the orientation the schematic intends** —
unresolved, see `hardware/footprint_audit.md`; if `D1` is reversed the board
already back-feeds and this makes it worse. And plugging USB in now charges
`C1`'s 1000 µF through `D_NEW`, which is an inrush some hubs will trip on;
a soft-start or a smaller bulk cap on the USB side is worth considering.

**Budget, because brightness alone bounds nothing.** 36 SK6812 at full white is
~2.2 A — that is what the 4 A jack is for. A USB 2 port gives 500 mA total:

| | draw |
|---|---|
| Feather M4 | ~50 mA |
| 36 LEDs, powered but dark | ~36 mA (controller quiescent) |
| 36 LEDs, all white, brightness 40 | ~375 mA |
| 36 LEDs, all white, full | ~2160 mA |
| CYD, idle, radios off | ~150 mA |
| CYD, ESP32 transmitting | peaks ~500 mA |

So: **LEDs on a 500 mA port, yes** — ~400 mA of headroom is about brightness 45
all-white, and a sequencer pattern lighting a handful of pixels never comes
close. **LEDs and the CYD on the same 500 mA port, only with the radios off**,
and even then the ESP32's TX peaks are what `C2`'s 470 µF exists to cover. On a
900 mA USB 3 port both fit comfortably. For one cable at full brightness the
honest answer is a USB-C PD sink (CH224K or similar) negotiating 5 V at 3 A,
which is a new part and a power-only connector.

The firmware half is in `tests/synthseqr_bringup/`: `LED_BUDGET_MA` and
`ledShowCapped()` scale each frame to a mA ceiling before `show()`, since a
brightness cap alone still lets 36 white pixels draw 8x what a dim pattern does.
Test `c` walks brightness with a running estimate so the estimate can be
calibrated against a meter.

### v3 pin map

Derived from the fab revision of `hardware/synthseqr_v3.kicad_pcb` (pad→net via
the symbol's pin-number table), not from the v2 source. Regenerate the
supporting dump with `tools/kicad_netlist.py`; `hardware/netlist_v3.txt` is the
committed copy, so a future revision is a re-run and a diff.

| Feather M4 | Net | Notes |
|---|---|---|
| A0 | `FADER_SIGNALS` | U1 mux COM — 16 slide pots |
| SDA / SCL | `SIG_BTNA` / `SIG_BTNB` | U2/U3 mux COM. Plain GPIO — **I2C is unavailable** |
| D4 / D6 / D9 / D12 | `S0`–`S3` | shared select lines, all three muxes |
| D5 | `LED_DATA` | R9 10k pulldown → 74AHCT125 → R37 series → 36x SK6812 |
| A2 / A3 / A4 | `ENC_A` / `ENC_B` / `ENC_SW` | EC11 encoder |
| A1 / MOSI / SCK / A5 | `SHIFT` / `STOP` / `RECORD` / `PLAY` | SW33–36 transport — **active HIGH**, see below |
| MISO | S1 soft-power rocker | to GND, no external pull-up — `INPUT_PULLUP`. PB22 / EXTINT[6], so it can wake from standby |
| D0 / D1 | `MIDI_RX` / `MIDI_TX` | `Serial1`. DIN in via H11L1 opto (J6), DIN out (J7) |
| D10 / D11 | `CYD_TX` / `CYD_RX` | J5, 4-pin to CYD. No `Serial2` — needs SERCOM3, see §7 |
| RST | S2 | reset button; not firmware-visible |
| — | free | D13 only |

J5 to the CYD is pin 1 `+5V_CYD`, 2 `CYD_RX`, 3 `CYD_TX`, 4 `GND`.

> **The two button groups have opposite polarity.** SW1–32 (the 16 step buttons
> and the 8x2 block) switch to GND against RN1–4 10k pull-ups: **active LOW**,
> read with `INPUT_PULLUP`. SW33–36 (transport) switch to **3V3** against R5–R8
> 1k pull-downs: **active HIGH**, read with plain `INPUT`. Scanning all 36 with
> one polarity would leave the transport buttons reading permanently inverted.

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
| `0x6x` | mostly F→C | soft power: shutdown, saved, sleep, wake, abort |

### Transport

`protocol/ss_link.*` sits on the codec and provides ring buffers, the HELLO
handshake, and keepalive. It has no dependency on Arduino, a UART, or a clock
source: the caller pushes received bytes in, pulls bytes to transmit out, and
supplies `now_ms`. Platform glue is roughly twenty lines per board, and the
entire state machine is verified natively.

Single-producer/single-consumer per direction, so no critical sections are
needed provided the contexts stay separated:

| Call | Context |
|---|---|
| `ss_link_rx_byte` | one only — typically the UART RX ISR |
| `ss_link_tx_pull` | one only — main loop or a TX-empty ISR |
| `ss_link_send`, `ss_link_poll` | main loop only |

Behaviour worth knowing:

- **Nothing blocks.** `ss_link_send` fails immediately if the link is down or
  the TX ring is full, and counts the drop. Callers sending superseded data
  (display refreshes) should discard; callers that care (storage) must retry.
- **Frames are all-or-nothing.** A frame that does not fit entirely is not
  written at all — a partial write would desynchronise the peer for every
  subsequent frame.
- **Application frames are refused until the handshake completes**, in both
  directions. Without an agreed protocol version the peer cannot be trusted.
- **Timers use unsigned differences**, so the 32-bit millisecond counter
  wrapping at ~49 days of uptime does not drop the link.
- Cost: 800 bytes of flash and a 2,416-byte `ss_link_t` on the Cortex-M4.

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

### Soft power

S1 is a maintained rocker on MISO. It does not break the rail — J1 feeds both
boards through D1/D2 — so "off" is a cooperative shutdown, and the reason it
exists at all is the save window: the CYD owns storage, and cutting power
mid-write is the corruption case worth engineering around. A hard cutoff was
considered and rejected; beyond losing that window, switching ~1.6 mF of bulk
capacitance with a mechanical contact pits it.

`protocol/ss_power.*` is the Feather-side authority. The CYD only ever answers.

```
rocker off ─> quiesce (notes off) ─> PWR_SHUTDOWN ─> await PWR_SAVED
           ─> save_local ─> PWR_SLEEP ─> drain TX ─> standby
```

- **Nothing the CYD does can prevent shutdown.** Every wait has a deadline and
  every deadline ends in powering down anyway. Refusing to turn off would leave
  the user pulling the barrel jack, which is the failure mode being eliminated.
  An absent peer skips the wait entirely rather than burning the full window.
- **Quiesce runs first and unconditionally**, before any timeout can apply. A
  slow save must not hold a note on, and once the transport is stopped there is
  no sequencer left to release it.
- **The rocker is a level, not an edge.** Powering up with it off settles
  straight back to sleep instead of coming up live, and the state survives reset.
- **`PWR_SHUTDOWN` is retried if the link was down when it was first sent**,
  which is precisely the power-up-with-rocker-off case. Without it the Feather
  waits the full save window for a reply to a request nobody received.
- MISO is PB22 / `EXTINT[6]`, so the pin can wake the SAMD51 out of standby.
- Cost: 406 bytes of flash and an 80-byte `ss_power_t`.

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

**Resolved — the CYD link needs a hand-instantiated SERCOM3 UART.** On v2 the
LCD lived on `Serial1`. On v3 `Serial1` (D0/D1) is DIN MIDI, and the CYD sits
on D10/D11, where the Feather M4 has no `Serial2`.

Verified against the CMSIS pinmux tables (`pio/samd51j19a.h`) and the
`feather_m4` variant:

```
PA20 (D10): function C = SERCOM5 PAD2, function D = SERCOM3 PAD2
PA21 (D11): function C = SERCOM5 PAD3, function D = SERCOM3 PAD3
```

SERCOM5 is already `Serial1`, SERCOM1 is SPI and SERCOM2 is Wire, so the link
must use **SERCOM3 via peripheral function D — `PIO_SERCOM_ALT`, not
`PIO_SERCOM`**. Selecting the wrong function points the pins back at the
in-use SERCOM5 and fails silently.

```cpp
Uart SerialCYD(&sercom3, 11, 10, SERCOM_RX_PAD_3, UART_TX_PAD_2);
void SERCOM3_0_Handler() { SerialCYD.IrqHandler(); }   // also _1, _2, _3
pinPeripheral(10, PIO_SERCOM_ALT);
pinPeripheral(11, PIO_SERCOM_ALT);
```

`tests/cyd_uart_probe/` implements this and compiles for the Feather M4. It
self-tests via a D10→D11 jumper, so the SERCOM can be confirmed on the bench
with one wire and no CYD. Still to prove on hardware: sustained throughput at
921600/1 M, and that it stays clean while the TC4 sequencer ISR is running.

Contingency if the bench test fails: the spare MOSI/MISO pins, which would
require a board change.

**Open — does the CYD parse the pattern format, or stay a pure blob store?**
Blob store is trivially correct and ships sooner; parsing is eventually required
for a web UI that can browse and edit patterns. Undecided.

**Resolved — `analogRead()` is useless on this chip until `analogReference()`
is called.** `adafruit:samd` 1.7.17 `init()` (`cores/arduino/wiring.c`)
configures the prescaler, resolution, sample time and averaging for ADC0/ADC1
and never writes `REFCTRL`, so `REFSEL` keeps its reset value `0x0` = `INTREF`
— the internal bandgap, 1.0 V by default. Full scale is then 1.0 V, and a fader
wired 3V3 - element - GND saturates at 4095 a third of the way up its travel.
It reads exactly like an exponential-taper pot.

```cpp
analogReference(AR_DEFAULT);   // REFSEL = INTVCC1; on the SAMD51 that is VDDANA
analogReadResolution(12);
```

The REFSEL encodings are **not** shared with the SAMD21: here `INTVCC0` = `0x2`
= ½ VDDANA and `INTVCC1` = `0x3` = VDDANA, while on the SAMD21 `0x2` is VDDANA
and `0x3` is AREFA. AREF (header pin 3) has no net on v3, so `AR_EXTERNAL` is
not an option — which is fine, since `INTVCC1` is the same rail the pots divide
and the reading comes out ratiometric.

See `docs/fader_triage.md`; `tests/synthseqr_bringup/` grew tests 9/a/b for it.

## 8. Build

v2 was built on macOS (`.vscode/arduino.json` references
`/dev/tty.usbmodem14101`). The Linux workstation is now set up:
`arduino-cli` 1.4.0 in `~/.local/bin`, with `adafruit:samd` 1.7.17 and
`esp32:esp32` 3.3.11.

```sh
arduino-cli compile --fqbn adafruit:samd:adafruit_feather_m4 tests/cyd_uart_probe
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/cyd
```

Both `tests/cyd_uart_probe` and `tests/synthseqr_bringup` currently compile
clean for the Feather M4.
