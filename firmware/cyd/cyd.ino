/*
 * Synthseqr v3 — CYD (ESP32) platform glue (phase 0).
 *
 * The client end of the link. The Feather is authoritative: this side renders,
 * stores, and proposes, and for soft power it only ever answers.
 *
 * src/ holds symlinks to protocol/, so both boards compile the same files.
 *
 * Build:
 *   arduino-cli compile --fqbn esp32:esp32:esp32 firmware/cyd
 */

#include <Arduino.h>
#include <esp_sleep.h>
#include <driver/uart.h>

#include "src/ss_link.h"
#include "src/ss_power.h"

// ------------------------------------------------------------------ pins
//
// J5 on the Synthseqr board is 1=+5V, 2=CYD_RX, 3=CYD_TX, 4=GND (netlist_v3.txt).
// The CYD connector's power pin is VIN, so +5V from D2 feeds its regulator the
// same way USB does — correct as spec'd.
//
// The GPIO numbers below come from board illustrations, not a datasheet, and the
// connector's pin order has not been checked against J5's. Confirm both with a
// meter before wiring. GPIO35 is input-only in ESP32 silicon, so whichever pin it
// turns out to be can only ever be RX.
static const int PIN_FEATHER_RX = 35;   // CYD receives  <- Feather CYD_TX
static const int PIN_FEATHER_TX = 22;   // CYD transmits -> Feather CYD_RX

static const uart_port_t UART_NUM = UART_NUM_1;
static const uint32_t FEATHER_BAUD = 115200;

HardwareSerial SerialFeather(1);

// ------------------------------------------------------------------ state
static ss_link_t s_link;
static bool sleeping = false;

static bool persistEverything()
{
  // TODO phase 3: flush pattern/song/config state to SPIFFS or the SD slot.
  // Until storage exists there is nothing dirty to lose.
  return true;
}

static void enterLowPower()
{
  // TODO phase 4: backlight and panel off — that is where the actual power goes.
  sleeping = true;

  // Wake when the Feather starts talking again. Light sleep keeps RAM, so the
  // mirror of sequencer state survives and no reload is needed on wake.
  uart_set_wakeup_threshold(UART_NUM, 3);
  esp_sleep_enable_uart_wakeup(UART_NUM);
  esp_light_sleep_start();

  sleeping = false;
}

static void onFrame(const ss_frame_t *f, void *user)
{
  (void)user;

  switch (f->type) {
    case SS_MSG_PWR_SHUTDOWN: {
      const uint8_t status = persistEverything() ? SS_PWR_SAVE_OK
                                                 : SS_PWR_SAVE_FAILED;
      // Best effort. If this does not reach the Feather it powers down anyway
      // after its save window, which is the intended behaviour.
      ss_link_send(&s_link, SS_MSG_PWR_SAVED, &status, 1);
      break;
    }

    case SS_MSG_PWR_SLEEP:
      // Deferred to the main loop: the reply above still has to drain out of
      // the TX ring, and sleeping here would strand it.
      sleeping = true;
      break;

    case SS_MSG_PWR_WAKE:
    case SS_MSG_PWR_ABORT:
      sleeping = false;
      break;

    // Phases 1-5 land here: display, state mirror, storage, MIDI relay.
    default:
      break;
  }
}

void setup()
{
  Serial.begin(115200);
  SerialFeather.begin(FEATHER_BAUD, SERIAL_8N1, PIN_FEATHER_RX, PIN_FEATHER_TX);
  ss_link_init(&s_link, onFrame, NULL);
}

void loop()
{
  const uint32_t now = millis();

  while (SerialFeather.available() > 0) {
    ss_link_rx_byte(&s_link, (uint8_t)SerialFeather.read());
  }

  uint8_t out[64];
  const size_t n = ss_link_tx_pull(&s_link, out, sizeof(out));
  if (n > 0) SerialFeather.write(out, n);

  ss_link_poll(&s_link, now);

  if (sleeping && ss_link_tx_pending(&s_link) == 0) {
    SerialFeather.flush();
    enterLowPower();
  }
}
