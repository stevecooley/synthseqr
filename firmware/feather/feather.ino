/*
 * Synthseqr v3 — Feather M4 platform glue (phase 0).
 *
 * Binds the portable protocol code to this board: the SERCOM3 UART to the CYD,
 * the soft-power rocker on MISO, and DIN MIDI on Serial1. There is no sequencer
 * here yet — this is the scaffold the v2 core gets ported onto.
 *
 * src/ holds symlinks to protocol/, so both boards compile the same files.
 *
 * Build:
 *   arduino-cli compile --fqbn adafruit:samd:adafruit_feather_m4 firmware/feather
 */

#include <Arduino.h>
#include <wiring_private.h>       // pinPeripheral

#include "src/ss_link.h"
#include "src/ss_power.h"

// ------------------------------------------------------------------ pins
// Verified against the fab revision of hardware/synthseqr_v3.kicad_pcb.
static const uint8_t PIN_CYD_TX = 10;   // PA20, SERCOM3 PAD2
static const uint8_t PIN_CYD_RX = 11;   // PA21, SERCOM3 PAD3
static const uint8_t PIN_ROCKER = MISO; // PB22 / EXTINT[6], S1 soft power

// S1 closes to GND against the internal pull-up. Flip this one constant if the
// rocker turns out to be fitted the other way round.
static const bool ROCKER_CLOSED_MEANS_ON = true;

// ------------------------------------------------------------------ uart
// Serial1 is DIN MIDI and already owns SERCOM5; D10/D11 reach SERCOM3 only via
// peripheral function D. Selecting PIO_SERCOM instead of PIO_SERCOM_ALT points
// them back at SERCOM5 and fails silently. See docs/architecture.md §7.
Uart SerialCYD(&sercom3, PIN_CYD_RX, PIN_CYD_TX, SERCOM_RX_PAD_3, UART_TX_PAD_2);

void SERCOM3_0_Handler() { SerialCYD.IrqHandler(); }
void SERCOM3_1_Handler() { SerialCYD.IrqHandler(); }
void SERCOM3_2_Handler() { SerialCYD.IrqHandler(); }
void SERCOM3_3_Handler() { SerialCYD.IrqHandler(); }

static const uint32_t CYD_BAUD = 115200;   // bring-up rate; see §3

// ------------------------------------------------------------------ state
static ss_link_t  s_link;
static ss_power_t power;

static void onFrame(const ss_frame_t *f, void *user)
{
  (void)user;
  if (ss_power_on_frame(&power, f)) return;

  switch (f->type) {
    // Phases 1-5 land here: display, input events, storage, MIDI relay.
    default:
      break;
  }
}

// ------------------------------------------------------------------ power hooks

// Runs before any timeout can apply, so it must be fast and unconditional.
// Once the transport is stopped there is no sequencer left to release a note.
static void quiesce(void *user)
{
  (void)user;
  for (uint8_t ch = 0; ch < 16; ch++) {
    Serial1.write((uint8_t)(0xB0 | ch));
    Serial1.write((uint8_t)123);   // All Notes Off
    Serial1.write((uint8_t)0);
  }
  Serial1.flush();
}

static void saveLocal(void *user)
{
  (void)user;
  // TODO phase 3: FlashAsEEPROM_SAMD fallback write, so a Feather that boots
  // with no CYD attached still comes back with its patterns.
}

static void resumeRun(void *user) { (void)user; }

static const ss_power_hooks_t POWER_HOOKS = { quiesce, saveLocal, resumeRun };

static bool rockerOn()
{
  const bool closed = (digitalRead(PIN_ROCKER) == LOW);
  return ROCKER_CLOSED_MEANS_ON ? closed : !closed;
}

// ------------------------------------------------------------------ sleep

static void enterStandby()
{
  SerialCYD.flush();

  // Wake on the rocker returning. MISO is PB22 = EXTINT[6].
  attachInterrupt(digitalPinToInterrupt(PIN_ROCKER), [](){}, CHANGE);

  SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
  __DSB();
  __WFI();
  SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;

  detachInterrupt(digitalPinToInterrupt(PIN_ROCKER));
}

// ------------------------------------------------------------------ setup

void setup()
{
  Serial.begin(115200);

  Serial1.begin(31250);            // DIN MIDI

  SerialCYD.begin(CYD_BAUD);
  pinPeripheral(PIN_CYD_TX, PIO_SERCOM_ALT);
  pinPeripheral(PIN_CYD_RX, PIO_SERCOM_ALT);

  pinMode(PIN_ROCKER, INPUT_PULLUP);

  ss_link_init(&s_link, onFrame, NULL);
  ss_power_init(&power, &s_link, &POWER_HOOKS, NULL, rockerOn());
}

// ------------------------------------------------------------------ loop

void loop()
{
  const uint32_t now = millis();

  // RX. The Uart class buffers into its own ring first; at 115200 that is ample.
  // Above ~921600 this becomes the limiting buffer rather than SS_RX_RING, and
  // the ISR should feed ss_link_rx_byte directly instead.
  while (SerialCYD.available() > 0) {
    ss_link_rx_byte(&s_link, (uint8_t)SerialCYD.read());
  }

  // TX. Bounded per iteration so a large queued transfer cannot stall the loop.
  uint8_t out[64];
  const size_t n = ss_link_tx_pull(&s_link, out, sizeof(out));
  if (n > 0) SerialCYD.write(out, n);

  ss_link_poll(&s_link, now);

  ss_power_switch(&power, rockerOn(), now);
  ss_power_poll(&power, now);

  if (ss_power_should_sleep(&power)) {
    enterStandby();
    // Falls through on wake; ss_power_poll sends PWR_WAKE once the rocker
    // reads on again.
  }
}
