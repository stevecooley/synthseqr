# Footprint audit — the pads the netlist cannot check

`tools/kicad_netlist.py` maps **pad numbers** to nets. It is the right tool for
"is `RV1` pin 1 on 3V3", and it is blind to "is pad 1 where the datasheet says
terminal 1 is". The v3.2 fader short was exactly that blind spot: a hand-drawn
`PS45-11PC3BR10K` with the wiper and an end terminal transposed, which put 3V3
on the wiper and turned every fader into a slider-position-controlled short.

**A clean netlist diff is not validation for these.** Neither is DRC — it checks
the drawing against itself, not against the part.

## Self-drawn and vendor-imported footprints

`PCM_synthseqr` is the local library. Everything in it was drawn or imported by
hand and carries the same risk as `RV` did.

| footprint | parts | what an error looks like |
|---|---|---|
| `PS45-11PC3BR10K` | `RV1`–`RV16` | **the v3.2 bug.** Fixed in v3.3 |
| `D_3220_8050Metric…HandSolder` | `D1` `D2` (SS14) | **highest remaining risk — see below** |
| `DIOM5436X261N` | `D4` (SMBJ5.0A TVS) | reversed: conducts at 0.7 V instead of clamping at 6.4 V, and sits across `+5V` |
| `865060253008` | `C2` (470 µF) | polarised electrolytic, backwards = vents |
| `EN11HSM1AF15` | `ENC1` | A/B swapped only reverses direction; switch pin wrong is dead |
| `5PINDINMIDI_adafruit` | `J6` `J7` | DIN numbering is not sequential around the connector |
| `CYD_UART_cable_attach` | `J5` | TX/RX/5V/GND order; 5 V into a UART pin kills the far end |
| `Marquardt_rockerswitch_1803.5102` | `S1` | soft power |
| `CAPC3225X200N` | `C7` | 1206 ceramic, unpolarised, low risk |

Also non-stock but vendor-supplied: `PCM_marbastlib-mx` (`LED1`–`LED36`,
`SW1`–`SW36`), `Samacsys` (`S2`), `PCM_4ms_*` (`D3`, `R5`–`R9`), `SSQ`,
`NetTie`, `TestPoint`.

## Open: diode pad numbering is not consistent across these libraries

The netlist implies two different conventions, and they cannot both be right:

- `D3` (`PCM_4ms_Diode:D_SOD-123`) has pad 1 on `OPTO_K` and pad 2 on `OPTO_A`.
  As the antiparallel diode across the opto's input, that means **pad 1 =
  anode**.
- `D4` (`PCM_synthseqr:DIOM5436X261N`, an SMBJ5.0A) has pad 1 on `+5V`. A
  unidirectional TVS puts its **cathode** on the positive rail, so **pad 1 =
  cathode**.

`D1`/`D2` are in the same local library as `D4` and carry the whole 5 V feed to
the Feather and the CYD. Backwards, the barrel jack cannot power the Feather and
the Feather's USB back-feeds the board's `+5V` — including `C1`'s 1000 µF and
`C2`'s 470 µF across VBUS on every plug-in. **Resolve this before v3.3 goes to
fab.**

## Checking one, before it costs $300

1. Open the datasheet's **terminal drawing**, not the schematic symbol. Note
   which physical terminal carries which function.
2. In the footprint editor, hover each pad and read its number. Write the
   mapping down in the table above.
3. Print the footprint 1:1 and lay the physical part on it. This is the step
   that catches transpositions, because the part is right there.
4. For anything polarised, confirm the silkscreen marker matches the pad you
   just proved is pin 1.
5. Record the date checked. An audited footprint stays audited; an edited one
   goes back to step 1.

| footprint | verified against datasheet | verified against part | by |
|---|---|---|---|
| `PS45-11PC3BR10K` | 2026-09-19 | 2026-09-19 | |
| `D_3220_8050Metric…` | | | |
| `DIOM5436X261N` | | | |
| `865060253008` | | | |
| `EN11HSM1AF15` | | | |
| `5PINDINMIDI_adafruit` | | | |
| `CYD_UART_cable_attach` | | | |
| `Marquardt_rockerswitch_1803.5102` | | | |
| `CAPC3225X200N` | | | |
