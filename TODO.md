# Synthseqr v3 — TODO

Concrete next actions for Synthseqr v3 — the firmware port plus the design, tooling,
and product work around it. Strategy, phase definitions, and the reasoning behind
firmware design decisions live in [docs/architecture.md](docs/architecture.md); this
file tracks what to actually do next.

Phase 0 (link bring-up) is code-complete and building for both boards.

## Blocked on hardware

Nothing here can be settled by reading files — each needs the board or a meter.

- [ ] **Confirm the CYD connector's GPIO numbers.** `PIN_FEATHER_RX = 35` /
      `PIN_FEATHER_TX = 22` in [firmware/cyd/cyd.ino](firmware/cyd/cyd.ino) come from
      board illustrations, not a datasheet. GPIO35 is input-only in ESP32 silicon, so
      whichever pin it is can only ever be RX.
- [ ] **Check the connector's pin order against J5.** J5 is `1=+5V, 2=CYD_RX, 3=CYD_TX,
      4=GND` (per `hardware/netlist_v3.txt`). If the CYD's connector runs the other way,
      a straight 1:1 cable is reversed. The power pin itself is VIN, so +5V through D2
      is correct as spec'd — it feeds the CYD's regulator the same path USB does.
- [ ] **Determine which S1 rocker position means "on."** `ROCKER_CLOSED_MEANS_ON` in
      [firmware/feather/feather.ino](firmware/feather/feather.ino) is currently a guess.
      One constant to flip.
- [ ] **Run the D10→D11 jumper loopback.** `tests/cyd_uart_probe/` self-tests the
      SERCOM3 / `PIO_SERCOM_ALT` muxing with one wire and no CYD attached.
- [ ] **Prove sustained throughput at 921600 / 1 M with the TC4 sequencer ISR running.**
      This is the contingency trigger: if it fails, the fallback is the spare MOSI/MISO
      pins, which needs a board change. Note that above ~921600 the `Uart` class ring
      becomes the limiting buffer rather than `SS_RX_RING`, and the ISR should feed
      `ss_link_rx_byte` directly instead.
- [ ] **Verify the button polarity split on real hardware.** SW1–32 are active LOW
      (pull-ups, read with `INPUT_PULLUP`); SW33–36 transport are active HIGH (R5–R8
      pull-downs, `INPUT`). `testTransport()` in the bring-up sketch has an idle-low
      check that catches a wrongly-configured pull-up.

## Stubs to fill in

Each is deliberately empty and tagged in-source with its phase.

- [ ] `saveLocal()` — [firmware/feather/feather.ino:77](firmware/feather/feather.ino#L77).
      FlashAsEEPROM_SAMD fallback write, so a Feather that boots with no CYD attached
      still comes back with its patterns. **Phase 3.**
- [ ] `persistEverything()` — [firmware/cyd/cyd.ino:44](firmware/cyd/cyd.ino#L44).
      Flush pattern/song/config to SPIFFS or the SD slot. Currently returns `true`
      unconditionally, which is honest only while there is nothing dirty to lose.
      **Phase 3.**
- [ ] `enterLowPower()` — [firmware/cyd/cyd.ino:51](firmware/cyd/cyd.ino#L51).
      Backlight and panel off. That is where the actual power goes; the light-sleep
      call alone saves comparatively little. **Phase 4.**

## Open decisions

- [ ] **Does the CYD parse the pattern format, or stay a pure blob store?** Blob store
      is trivially correct and ships sooner; parsing is eventually required for a web UI
      that can browse and edit patterns. Blocks the phase 3 storage design. See
      architecture.md §7.

      Weightier than it first looked: multiple co-resident sequencers (see UI section)
      means the saved state is heterogeneous — beatseqr and synthseqr patterns are not
      the same shape. Worth knowing which way this goes before the storage format is
      fixed, since a blob store cares much less about that than a parser does.

## UI and interaction design

None of this is blocked on hardware or on the port — it can be settled on paper, and
phases 1–4 will go faster if it is.

- [ ] **Decide what the 8x2 mode buttons do.** SW17–32, the upper-right block. They are
      in the muxed scan alongside the 16 step buttons and each has an LED, so they can
      show state as well as select it. No assigned function yet.
- [ ] **Decide how the transport buttons work.** SW33–36 are wired as SHIFT / STOP /
      RECORD / PLAY. Open questions: is SHIFT a modifier that reshapes the other three
      (and the step and mode buttons), or a button with its own function? What do
      press-and-hold and double-press do? What do the four LEDs indicate — armed state,
      playing state, or a blink pattern carrying tempo?
- [ ] **Design navigation across multiple sequencers within one unit.** *Parked — the
      goal is clear, the UI is not. Revisit once the mode and transport buttons are
      settled.* The intent is several sequencer instances co-resident on one Feather:
      a 16-voice beatseqr always available as a second sequencer, alongside the
      single-voice synthseqr workflow built so far, and eventually several single-voice
      instances each on its own MIDI channel.

      Two things make this hard. The instances are *heterogeneous* — a 16-voice drum
      grid and a single-voice line want different controls from the same 36 buttons, so
      "switch sequencer" also means "remap the panel." And it has to work **without the
      CYD**: in standalone mode there is no touchscreen, so selecting and monitoring
      instances has to be expressible through buttons, LEDs, and the 16x2 emulation
      alone. Designing the no-CYD case first keeps the CYD an enrichment rather than the
      only path that works.

      Already pointing the right way: `quiesce()` in feather.ino sends All Notes Off on
      all 16 channels, which is what a multi-channel future needs.
- [ ] **Establish an LVGL style.** Colors, type scale, spacing, focus and touch-target
      sizes, and reusable widget patterns. Belongs to phase 4 (native UI), but settling
      it earlier is cheap and stops the per-screen retirement of the 16x2 emulation from
      each inventing its own look.

## Enclosure and cost

- [ ] **Design the physical case.** Needs the fab-revision board outline and connector
      positions as its starting constraints — barrel jack, the CYD's display cutout and
      mounting, DIN MIDI, and the S1 rocker.
- [ ] **Work out cost of goods per unit.** No data collected yet. The case and the CYD
      are likely the two largest line items.

## Tooling and infrastructure

- [ ] **Restore the web-flasher release path.** Needs a GitHub Actions workflow that
      builds both sketches and publishes artifacts the web flasher can consume. Two
      binaries now rather than v2's one, and they are built by different toolchains
      (`adafruit:samd` and `esp32:esp32`), so the workflow has to produce and version
      them as a matched pair — a Feather and a CYD on mismatched protocol versions is
      exactly what `SS_PROTOCOL_VERSION` exists to catch.
- [ ] **Add CLAUDE.md and instructions.md.** Deferred until the conventions are stable
      enough to be worth writing down.

## Repo hygiene

- [ ] **Untrack `tests/protocol/test_link`.** A build product committed by accident;
      `.gitignore` now covers `tests/protocol/test_*` but the file is already in the
      index. Needs `git rm --cached tests/protocol/test_link`.
