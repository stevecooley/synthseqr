# Fader triage — "exponential" readings and one very hot slide pot

Bring-up on the fab revision of `synthseqr_v3` turned up two fader problems at
once:

1. the 16 slide pots read as if they had an exponential taper — roughly the
   first 10 mm of a 45 mm sweep covers ~85% of the numbers, and the last 35 mm
   sits near the top value;
2. the single fitted pot got **very hot**.

The components are confirmed linear, and the element measures linear on a
multimeter. Those two facts are consistent with everything below: **these are
two separate faults, and neither is in the schematic.** One is firmware, one is
this board's assembly. The firmware one masks the diagnostic signature of the
other, so fix it first.

## 1. The taper: the SAMD51 ADC reference is never set up

Not a taper at all. The reading is linear and then **hard-clipped at 4095**,
because full scale is the SAMD51's 1.0 V bandgap instead of the 3.3 V rail.

`adafruit:samd` 1.7.17, `cores/arduino/wiring.c`, `init()`: the SAMD51 branch
sets `PRESCALER`, `CTRLB.RESSEL`, `SAMPCTRL`, `INPUTCTRL` and `AVGCTRL` on both
ADCs — and never touches `REFCTRL`. So `REFCTRL.REFSEL` keeps its reset value
`0x0` = `INTREF`, the internal bandgap, which `SUPC->VREF.SEL` defaults to
**1.0 V**. The only place the core writes `REFSEL` is `analogReference()` in
`wiring_analog.c`, which the bring-up sketch never called.

With a 10 k pot across 3V3 and a 1.0 V full scale:

| travel | wiper | raw, ref = 1.0 V | raw, ref = 3.3 V |
|---|---|---|---|
| 0 mm | 0.00 V | 0 | 0 |
| 5 mm | 0.37 V | 1502 | 455 |
| 10 mm | 0.73 V | 3003 | 910 |
| **13.6 mm** | **1.00 V** | **4095 — saturated** | 1241 |
| 20 mm | 1.47 V | 4095 | 1820 |
| 30 mm | 2.20 V | 4095 | 2730 |
| 45 mm | 3.30 V | 4095 | 4095 |

Predicted: everything happens in the first ~13.6 mm, the remaining ~31 mm reads
4095. Reported: everything in the first ~10 mm, the remaining ~35 mm near the
top value. Same effect.

### Fix

```cpp
analogReference(AR_DEFAULT);    // REFSEL = INTVCC1, and on the SAMD51 that is VDDANA
analogReadResolution(12);
(void)analogRead(PIN_FADER);    // the first conversion after a reference change is junk
```

Already applied in `tests/synthseqr_bringup/`. Two traps worth writing down:

- **The REFSEL values differ across the family.** On the SAMD51 `INTVCC1` =
  `0x3` = VDDANA and `INTVCC0` = `0x2` = ½ VDDANA; on the SAMD21 `0x2` is
  VDDANA and `0x3` is AREFA. The macro names are shared, the values are not, so
  SAMD21 example code silently picks the wrong reference here.
- **AREF has no net on v3** (Feather header pin 3 is unconnected in
  `hardware/netlist_v3.txt`), so `AR_EXTERNAL` is not available to us. We do not
  want it: `INTVCC1` is the same rail the pots divide, which makes the reading
  ratiometric — rail sag cancels instead of scaling every fader.

### Confirm it on the bench

- **Test 9, ADC reference sweep** — walks every `REFSEL`, and for each one
  prints the *measured* full scale (from the ADC's internal VDDIO/4 channel,
  which needs no external anything) next to what the fader reads. 1024 counts
  on that internal channel means a 3.3 V reference, 2048 means 1.65 V, 3378
  means 1.0 V.
- **Test a, travel profile** — capture a slow end-to-end sweep and print it as a
  bar chart. This is the test that separates the two faults: it reports where
  the reading stopped changing, and how far the unclipped part deviates from a
  straight line.

After the fix, expect 1241 counts per volt and 4095 only at the very top of the
travel.

## 2. The heat: the designed circuit cannot do it

`RV1–16` are PS45-11PC3BR10K — 45 mm travel, 10 kΩ, **0.25 W**. Wired as the
netlist has them (3V3 – element – GND):

- I = 3.3 V / 10 kΩ = **0.33 mA**
- P = 3.3² / 10 kΩ = **1.1 mW**, i.e. 0.4% of the element's rating
- all 16 fitted = 5.3 mA off 3V3

1.1 mW cannot be felt. For the element to get *hot* it needs on the order of
100 mW, which across 3.3 V means an effective **≤ ~110 Ω** — about 1% of a 10 k
element, roughly half a millimetre of track. At the full 250 mW rating it is
44 Ω. So there are only two shapes of fault:

- **A rail is reaching the wiper, or two rails are landing within a few percent
  of each other on the element.** Then the current crowds into a short length of
  track next to the slider: a local hot spot that can burn the element, and a
  current that rises as 1/position. A solder bridge across the pot's pads (1–2
  and 2–3 are adjacent), a footprint whose pad numbering does not match the
  part's terminals, or the part fitted one pad over all do this.
- **The element is not 10 kΩ.** A ~100 Ω element dissipates 109 mW and still
  reads perfectly linear, so nothing about the *ratio* would look wrong.

A third possibility that leaves the readings alone entirely: the metal frame or
a mounting lug bridging two nets, so the body heats and the divider does not.

> While this is unresolved, do not leave the board powered with the pot hot. A
> 0.25 W element run at hundreds of mW degrades. When you re-measure, check the
> element is still *monotonic* across the whole travel — a burnt spot shows up as
> a jump or a dead zone, not as a changed end-to-end value.

### The mounting holes

The pots' mounting holes are unplated, and the suspicion is that a pour comes
right up to the hole with no clearance, leaving 5 V copper where the frame lug
can touch it.

**This repo cannot answer that.** `hardware/*.kicad_pcb` is gitignored (5 MB),
`netlist_v3.txt` carries only footprint *origins* — no pad positions, no zones,
no traces, no layers — and NPTH pads have no net, so `kicad_netlist.py` drops
them by construction. RV1–16 showing only pads 1/2/3 is therefore not evidence
that the footprint has no mounting-hole pads. Use
`tools/kicad_hole_clearance.py` against your local board file:

```sh
tools/kicad_hole_clearance.py hardware/synthseqr_v3.kicad_pcb RV
```

It reports, for every drilled hole, the nearest copper on each layer and which
net it belongs to — including holes milled as Edge.Cuts circles, which DRC does
not see as holes at all. Worth knowing: a board carried forward from KiCad 5
often has no hole-clearance constraint set, so DRC never checks copper against
an NPTH hole even when it is defined as a pad.

What the netlist *does* say about where 5 V lives:

- `+5V` proper is confined to the power corner — its nearest pad to the fader
  row (y = 129.98) is `NetTie_1` at y = 72.19, **58 mm away**. Not a candidate.
- `/LED_5V` is the 76-pad net, and it has to get from `U5`/`J4` at y ≈ 189 to
  the LED blocks at y ≈ 70–91. That run crosses the fader row. Nearest LED_5V
  pads are at y ≈ 91.3 (38 mm above) and y ≈ 179.4 (49 mm below). A 2 A rail is
  usually poured, so **plausible, unverified**.
- A PS45 is ~60 mm long for 45 mm of travel, so a pot centred at y = 130 spans
  roughly y = 100–160 and its mounting holes land near y ≈ 103 and y ≈ 157.

### What the theory does and does not explain

**One contact is not a fault.** A lug touching one net and nothing else carries
no current and makes no heat. You need two potentials: two lugs on two nets, or
one lug on a rail plus a frame internally tied to a terminal (measure frame-to-
terminal on a spare pot — do not assume it is isolated).

**A hard 5 V-to-GND short would take the board down,** not warm one pot: `D1`
and `D2` feed the Feather and the CYD from the same `+5V`. A *resistive* contact
fits much better — 5 V through a marginal few-ohm touch is 0.5 A and 2.5 W right
at the contact, very hot, while a 4 A supply keeps everything running.

**The dangerous variant is 5 V reaching the element**, via a frame that is tied
to a terminal. 5 V on a mux input whose VCC is 3.3 V forward-biases the
CD74HC4067's input protection diode into the 3V3 rail: it lifts 3V3 and can take
out `U1` and the Feather with it. If the pot is hot, check 3V3 reads 3.30 V and
not 3.6–4 V before doing anything else.

### Telling it apart, in about a minute each

| question | mounting-lug short | rail on the wiper / low element |
|---|---|---|
| what is hot? | the metal frame and body | the track under the slider |
| supply current, pot fitted vs lifted | up by hundreds of mA | up by tens of mA |
| fader readings (reference fixed) | normal and linear | pinned or compressed |
| 5 V rail / LEDs | sags, LEDs may flicker | unaffected |

Then, power off, DMM from each mounting lug and from the frame to `GND`, `3V3`
and `+5V`: anything under ~100 kΩ is a contact. Lift the pot and check whether
the exposed copper is at the hole wall on either surface.

### Should the mounting lugs come off?

Tempting, and it would remove the failure mode — but not yet, and probably not
as the permanent answer.

**First find out whether it is even possible** (30 seconds, on a spare pot):

1. Are the lugs metal, or plastic locating pegs? Plastic kills the theory
   outright.
2. Continuity lug ↔ lug. If the two lugs are *not* common, a single contact
   carries no current and you need copper at **both** holes, ~50 mm apart, at
   two different potentials — a much narrower coincidence.
3. Continuity lug ↔ each of the three terminals. If the frame is tied to a
   terminal, then one contact *is* enough, and that is the case to rule out
   first (see the 5 V-on-the-element warning above).

**Do not cut the lugs off the pot that is fitted.** It is the only instance of
the fault you have. Cutting makes the symptom vanish without saying which
candidate it was, and if the real cause is a bridge on the pot pads the heat
will simply continue — with a part now butchered for nothing.

**The same experiment, reversibly:** a strip of Kapton or fish paper between the
pot body and the board, or nylon shoulder washers in the holes, lugs left
unsoldered. Heat gone with the insulator and back without it proves it.

**Why not permanently:** a 45 mm fader takes side and downward load every time
it is played. Without the lugs the three signal pins are the only anchor, and
the joints crack and lift pads — on 16 faders that is when, not if. It is
defensible only if a front panel carries the faders and the lugs are just
alignment.

**The board fix is needed either way,** because the board's own mounting holes
have the same exposure and those get metal standoffs and screws:

| hole | nearest 5 V-net pad |
|---|---|
| `H1` | `D4` (`+5V`), 11.7 mm |
| `H3` / `H4` | `LED1` / `LED16` (`/LED_5V`), 11.4 mm |
| `H6` (160.70, 190.85) | `C28` (`/LED_5V`), 11.8 mm |
| `H7` | `J5` (`/+5V_CYD`), 9.1 mm |

Close enough that a pour plausibly reaches them. So: clearance around every NPTH
hole, and **set the hole-clearance DRC constraint** so it cannot come back. For
a metal-frame fader, pick a side deliberately — either full clearance so the
frame floats, or plated holes tied to GND on purpose. Floating-by-accident with
copper nearby is the one option that bites.

Two traps while you are in there. Deleting the mounting-hole *pads* from the
footprint does not help: the physical hole still has to exist, and a hole drawn
as a graphic instead of a pad is invisible to DRC — which is the case
`tools/kicad_hole_clearance.py` goes looking for. And `H6` is used twice, at
(160.70, 190.85) and (147.06, 46.18); one of them wants renaming before the next
netlist diff.

### Why fault 1 hides fault 2

"Reads close to the upper value over the top of the travel" is exactly what a
wiper shorted to 3V3 looks like — and exactly what a 1.0 V reference looks like.
While the reference is wrong you cannot tell them apart, which is why the order
below matters.

### Decision tree

**Step 0 — DMM at the pot, board unpowered.** The decisive measurement, and it
needs nothing else:

| measurement | healthy | if not |
|---|---|---|
| pad 1 ↔ pad 3 (3V3 ↔ GND ends) | ~10 kΩ, **unchanged as you move the slider** | changes with position → a rail is on the wiper: pad mapping, footprint, or a bridge. **This is your heat.** |
| pad 1 ↔ pad 3 | ~10 kΩ | ≪ 1 kΩ → wrong element value. **Also your heat.** |
| pad 2 ↔ pad 1 and pad 2 ↔ pad 3 | vary with travel, sum ≈ pad 1 ↔ pad 3 | a few hundred Ω at *every* position → bridge from the wiper to that rail |

**Step 0b — DMM from the mounting lugs and the frame to each rail**, per *The
mounting holes* above, if the frame is what feels hot.

**Step 1 — DMM on the wiper, powered, at 0 / 25 / 50 / 75 / 100% of travel.** A
healthy 10 k divider gives 0 / 0.83 / 1.65 / 2.48 / 3.30 V. If the DMM says
that, the divider is fine and every remaining oddity is in the ADC. If the DMM
itself is compressed (say 0 / 2.1 / 2.8 / 3.1 / 3.3 V) there is a resistive path
from the wiper node to 3V3 — hunt the bridge.

**Step 2 — flash the updated sketch, run 9 then a.** Reference measured at
3.3 V, and the sweep linear to the very top. If the top is *still* pinned with
the reference correct, go back to step 0: that is the wiper being pulled up.

**Step 3 — run b, the wiper impedance probe.** `C4–C21` put 100 nF on every
wiper, so the node has a time constant. The test drives A0 hard for 400 µs,
releases it to high-Z, and watches the 100 nF relax back through the pot:
τ = R_th·C with R_th = R_upper ∥ R_lower, which peaks at R_element/4 in mid
travel. It prints τ, R_th, the implied element resistance and the dissipation
that implies. A healthy 10 k PS45 at mid travel: **τ ≈ 250 µs, R_th ≈ 2.5 kΩ,
1.1 mW**. An element low enough to cook itself cannot produce those numbers.

Accuracy is ±30% or so — 100 nF ±10%, a ~45 µs sample period, and a two-point
exponential fit. It is a magnitude check, not a bridge.

## 3. What the netlist already rules out

From `hardware/netlist_v3.txt`, so none of this needs re-checking:

- `RVn.1` → `3V3`, `RVn.2` → wiper → one `U1` channel **and** a 100 nF to GND
  (`C4`–`C21`), `RVn.3` → `GND`. A textbook ratiometric divider on every one of
  the 16.
- `U1` (CD74HC4067): pin 24 → 3V3, pin 12 → GND, **pin 15 (E) → GND** so it is
  permanently enabled, and S0–S3 on pins 10/11/14/13 ← D4/D6/D9/D12, matching
  the sketch's bit order.
- `U1` pin 1 (COM) → `FADER_SIGNALS` → Feather header pin 5 = **A0**.
- Channel order is identity: the CD74HC4067's Y0 is pin 9, which carries
  `SlidePotentiometer_01`, running in order to Y15 on pin 16 →
  **F*n* is RV*n+1*.** Worth confirming empirically while only one pot is
  fitted — the new tests print the RV designator next to the channel.
- Header pin 3 (AREF) has no net.

Nothing in the schematic produces either symptom.

## 4. Notes

- The 100 nF on each wiper and the pot's ≤ 2.5 kΩ source impedance make a
  ~600 Hz single pole — fine for a fader, and it is what makes the test-b
  impedance probe possible.
- Optional hardening for the next revision: a resistor in each pot's 3V3 leg
  bounds the current when a wiper does end up on a rail. 100 Ω costs ~1% of the
  span and caps that fault at 33 mA. It protects the pot, not the rail.
- `tools/kicad_hole_clearance.py --selftest` checks its geometry against a
  synthetic board; it has never been run against a real 5 MB `.kicad_pcb`, so
  treat a flagged hole as "go look at it in pcbnew", not as a verdict. Pad
  copper is approximated by its bounding circle and arcs by their start-mid-end
  polyline, both of which over-report proximity.
- The sketch changes here have **not** been compiled — the toolchain is not
  available in the session they were written in. Run
  `arduino-cli compile --fqbn adafruit:samd:adafruit_feather_m4 tests/synthseqr_bringup`
  before flashing.
