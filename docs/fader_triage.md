# Fader triage — "exponential" readings and one very hot slide pot

> ## Resolved, 2026-09-19: the `RV` footprint had pads 1 and 2 transposed.
>
> `PS45-11PC3BR10K` has no ready-made footprint on DigiKey, so it was drawn by
> hand, and the drawing swapped the wiper with an end terminal. On the board
> that meant:
>
> ```
> 3V3      -> the WIPER          (should have been an end)
> mux/100nF -> an element END    (should have been the wiper)
> GND      -> the other end      (correct)
> ```
>
> So every fitted pot connected 3V3 to its wiper and let the current run down
> the element to GND. **The resistance of that path is the slider position** —
> 10 kΩ at one end of travel, a dead short at the other. Everything follows
> from that one line:
>
> - **The short that moved.** 8.9 Ω, then 53.3 Ω, then 0.375 Ω were not three
>   faults and not a degrading part. They were one fault measured at three
>   slider positions. Every component pulled chasing it — `U1`, `U2`, `U3`,
>   `C58`, `C59`, `C60`, `C63` — was innocent.
> - **The heat.** 3.3 V across a few millimetres of a 45 mm element: 371 mA at
>   8.9 Ω, concentrated next to the slider. A 0.25 W part asked for over a watt.
>   Two pots cooked because the *site* did it to them, which is what the
>   RV1-vs-RV2 comparison said and why a replacement part changed nothing.
> - **The "exponential" curve.** The mux channel sat on a floating element end,
>   so it read the wiper's potential — which is the 3V3 rail. Near one end of
>   travel there is no fault current and the rail is fine, so it reads full
>   scale; approach the other end and the rail sags under its own short and the
>   reading falls away fast. "The first 10 mm covers 85% of the values, the
>   other 35 mm sits near the top" was the ADC watching its own supply collapse
>   as a function of slider position. There was never a taper.
> - **The dead USB.** 371 mA through the Feather's LDO at (5 − 3.3) V is 0.63 W
>   in a SOT-23-5. Thermal shutdown, no VDD, nothing to enumerate.
>
> Fixed in v3.3 along with NPTH mounting holes and an increased clearance
> override.
>
> **What this section got wrong.** It led with the ADC reference as the cause of
> the curve. That bug is real and verified from the core source, and it did mask
> the signature — F0 pinned at 4095 over most of its travel either way — but it
> was not what produced the reported shape. A hand-drawn footprint was, and the
> committed netlist could not have shown it: it maps pads to nets **by pin
> number**, so a footprint whose numbering does not match the part is invisible
> to it. See `hardware/footprint_audit.md`.

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

After the fix, expect 4095 only at the very top of the travel, and
`4095 / rail` counts per volt — 1264/V on a 3.24 V rail.

Run test 9 with the fader about **a quarter** of the way up, not at mid travel:
half rail is full scale for both of the low references, so two of the four rows
would read 4095 and say nothing. At a quarter of a 3.24 V rail the wiper is
0.81 V and the rows separate cleanly:

| REFSEL | reference | measured FS | raw at 0.81 V |
|---|---|---|---|
| `0x0` | INTREF bandgap | 1.00 V | 3317 |
| `0x2` | INTVCC0, ½ VDDANA | 1.62 V | 2048 |
| `0x3` | INTVCC1, VDDANA | 3.24 V | **1024** |
| `0x4` | AREFA | — | garbage; the pin is unconnected |

The sketch measures the rail itself against the 1.0 V bandgap rather than
trusting `V_RAIL_NOM`, so those numbers follow whatever your regulator actually
puts out.

## 2. The heat: the designed circuit cannot do it

> **2026-09-14:** the mounting holes do have copper clearance, so the lug theory
> was wrong. Replacing `RV1` appeared to end the heat.
>
> **2026-09-15, reopened:** after a couple of hours on USB power the
> *replacement* `RV1` is hot and `RV2`, fitted to the same board from the same
> batch, is cold. A new part cooking in the same slot while its neighbour stays
> cold makes this the **site**, not the part — and it means the first pot was
> probably a casualty rather than the cause. `RV2` is now the control: every
> measurement below is worth taking at both positions, because the two are
> identical by design and anything that differs is the fault.

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

**Insulating to keep testing** is a reasonable call on boards that cost $300 a
turn, as long as it is barrier first and coating second — anything painted on a
pin gets scraped off going into a tight hole:

| | |
|---|---|
| nylon shoulder washer or PTFE sleeve in the hole | best; mechanical, survives insertion, reversible |
| Kapton donut around the hole, or a strip under the body | excellent dielectric, 260°C, peels off, no cure |
| UV solder-mask pen or conformal coat on the exposed copper | fine if the pin goes in once; verify coverage |
| clear enamel / nail polish | works cold, but not near a soldering iron |
| epoxy | avoid — it wins the isolation and loses the rework |

Coat the **board** rather than the pin where you can, and then **measure**: lug
to `GND`, `3V3` and `+5V` should all read open before power goes on. The
measurement is the safeguard; the material is just how you got there.

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

### Where the heat has to come from, given RV2 is fine

Only current through the element heats the element, and the wiper is the only
tap on it. So there are two shapes, and they are told apart by *where* the part
is hot and by one resistance measurement:

**Uniformly warm across the body → the element is not 10 kΩ.** The whole strip
dissipates V²/R. 100 Ω is 109 mW; 10 kΩ is 1.1 mW. In-circuit `pin 1 ↔ pin 3`
says so immediately, and `RV2` gives you the number it should be.

**Hot at one end → a rail is on the wiper.** With the wiper tied to the top rail
through some resistance Rb, and the slider a fraction x above the bottom end:

```
I      = V / (Rb + x*R)
P_elem = I^2 * x * R      maximal at x*R = Rb, where P_elem = V^2 / (4*Rb)
```

The consequence is the useful part: **the heat tracks the slider position.** It
peaks when the remaining leg of the element matches Rb and falls away at both
extremes. Note where the slider was when you found it hot, then run it to the
far end — if it cools, that is the mechanism, and the hot end tells you which
rail is on the wiper. It also means a pot that was cold right after a swap can
turn hot later simply because the slider moved.

That same position dependence is why this could not be seen earlier: with the
ADC reference at 1.0 V, F0 read 4095 over most of its travel whether the wiper
was healthy or held at 3V3.

### The measurement that settles it

**Take the pot out and measure the empty pads,** power off, against `RV2`'s:

| across | expected with nothing fitted |
|---|---|
| wiper pad ↔ `3V3` pad | open — only mux off-channel leakage |
| wiper pad ↔ `GND` pad | open at DC — `C4` blocks |
| `3V3` pad ↔ `GND` pad | whatever the rest of the board is, same as at `RV2` |

Anything finite from the wiper pad to `3V3` at `RV1` and not at `RV2` is the
fault, and the pot has been a victim twice. Then power up with the pot still
out: the empty wiper pad should float, and if it sits hard at 3V3 something on
the board is driving it. `U1` is a candidate — the first pot's abuse may have
damaged the channel 0 input — so put a finger on `U1` as well.

Two more free readings while it is hot: the Feather's own 3V3 regulator (warm
means the fault current is hundreds of mA, cool means tens), and the pot's metal
frame to `3V3`/`GND`/`+5V`, since clearance at the mounting holes says nothing
about exposed copper under the body, where the frame lies flat across ~60x10 mm.

### 2026-09-15: the Feather stopped enumerating

After hours of the `RV1` fault running, USB no longer recognises the board, and
re-plugging the cable or the hub does not bring it back. The coherent reading is
that this is the *same* fault, grown: **the SAMD51 is powered from the Feather's
own 3V3 regulator, so anything that collapses 3V3 leaves the MCU without VDD and
there is nothing left to enumerate.** Re-plugging cannot help while the short is
still fitted.

Measure before applying power again, board unpowered:

| across | expected | means |
|---|---|---|
| `3V3` ↔ `GND` | ~10 kΩ ÷ pots fitted, so ~5 kΩ with two | each pot's element is a fixed 10 k across the rail whatever the slider is doing; RN1-4 and R2/R3 have no DC path to GND with the switches open |
| `3V3` ↔ `GND`, `RV1` lifted | should rise to ~10 kΩ | if lifting `RV1` is what restores it, the story closes |
| `+5V` ↔ `GND` | high, after `C1`/`C2` finish charging the meter | low means a shorted `D4` TVS or bulk cap |
| Feather USB pin ↔ `GND` | high | this is what the host sees; low explains a port shutting down |

Take them with the Feather off the board if it is on headers — that also splits
the question in one move. A Feather that enumerates on its own is fine and the
board is the problem; one that does not gets a **double-tap reset into the UF2
bootloader**, which runs before any sketch and enumerates as a `FEATHERBOOT`
drive. Bootloader present means the chip and its USB are alive.

Note that the host end can latch independently: a hub port in overcurrent
shutdown usually needs unplugging from both mains and host for ~30 s, or a
reboot. Prove it with a known-good device in that same port before concluding
anything about the board.

**Measured, Feather removed, 200 Ω range: 8.9 Ω.** Against ~667 Ω expected from
15 pots, so a hard short — 371 mA at 3.3 V, 1.2 W. That is the collapsed rail,
and the reason USB stopped enumerating.

Localise it before replacing anything. 1.2 W lands in one part, so **make the
short find itself**: inject current and feel for the hot spot.

- A current-limited bench supply on 3V3 at 3.3 V / 400 mA is the clean way.
- Without one, the 5 V barrel supply through a **10 Ω, ≥2 W** series resistor
  into the rail gives 265 mA with the rail sitting at 2.35 V — safe for
  everything on 3V3, and still 0.6 W in the short.
- Wipe isopropyl across the area: the hot spot dries first. A fingertip works
  too.

The candidates, cheapest first, and note the fault does not care which:

| | rework cost |
|---|---|
| a bridge or solder ball across `RV1`'s empty pads — pin 1 is 3V3, pin 3 is GND, and it has been reworked twice | free, just look |
| one of the five 100 nF on 3V3 (`C58` `C59` `C60` `C61` `C63`) — a cracked MLCC is the classic single-digit-ohm rail short | 20 s each to lift |
| one of the 15 fitted pots shorted end to end — same batch as the two that already failed, and in parallel they are invisible to a rail measurement | one at a time |
| `U1`/`U2`/`U3` VCC, or `U4` | see below |

**Do not replace a SOIC-24W on suspicion.** To test a mux, lift only its **pin
24** (VCC, a corner pin) with a fine tip and a pick, and re-measure. Short gone
means that part is guilty and has earned the swap; short still there means you
have not damaged a 24-pin footprint for nothing.

An inline USB power meter is worth having for the re-test afterwards — it shows
the draw before the port decides to protect itself.

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
