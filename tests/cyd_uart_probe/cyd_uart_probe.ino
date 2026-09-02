/*
 * Synthseqr v3 — CYD link SERCOM probe
 * Target: Adafruit Feather M4 Express (SAMD51J19)
 *
 * Proves the D10/D11 UART to the CYD before the v3 PCBs exist.
 *
 * There is no Serial2 on this board, and Serial1 (D0/D1) is DIN MIDI on v3,
 * so the CYD link needs a hand-instantiated SERCOM. SERCOM5 is already taken
 * by Serial1 and SERCOM1/2 by SPI/Wire, which leaves SERCOM3:
 *
 *   D10 = PA20 -> SERCOM3 PAD[2] (TX)  via peripheral function D
 *   D11 = PA21 -> SERCOM3 PAD[3] (RX)  via peripheral function D
 *
 * Function D is PIO_SERCOM_ALT. Function C on these pins is SERCOM5, which is
 * unavailable — using PIO_SERCOM instead of PIO_SERCOM_ALT will silently fail.
 *
 * BENCH TEST — no CYD required:
 *   Jumper D10 to D11 and open the USB serial monitor at 115200.
 *   Loopback pass means the SERCOM is correctly muxed and clocked.
 *   Remove the jumper; the test must then fail. (A test that passes when
 *   unwired is measuring nothing.)
 */

#include <Arduino.h>
#include <wiring_private.h>

Uart SerialCYD(&sercom3, 11, 10, SERCOM_RX_PAD_3, UART_TX_PAD_2);

void SERCOM3_0_Handler() { SerialCYD.IrqHandler(); }
void SERCOM3_1_Handler() { SerialCYD.IrqHandler(); }
void SERCOM3_2_Handler() { SerialCYD.IrqHandler(); }
void SERCOM3_3_Handler() { SerialCYD.IrqHandler(); }

constexpr uint32_t CYD_BAUD = 115200;

static bool loopbackTest(uint32_t baud) {
  SerialCYD.end();
  SerialCYD.begin(baud);
  pinPeripheral(10, PIO_SERCOM_ALT);
  pinPeripheral(11, PIO_SERCOM_ALT);
  delay(5);
  while (SerialCYD.available()) SerialCYD.read();

  const char* msg = "synthseqr";
  SerialCYD.write(msg);
  SerialCYD.flush();

  char got[16] = {0};
  size_t n = 0;
  uint32_t deadline = millis() + 200;
  while (n < strlen(msg) && millis() < deadline)
    if (SerialCYD.available()) got[n++] = SerialCYD.read();

  Serial.print(F("  "));
  Serial.print(baud);
  Serial.print(F(" baud -> received \""));
  Serial.print(got);
  Serial.print(F("\"  "));
  bool ok = (n == strlen(msg)) && (strcmp(got, msg) == 0);
  Serial.println(ok ? F("PASS") : F("FAIL"));
  return ok;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000);

  Serial.println(F("\n=== CYD link SERCOM3 probe ==="));
  Serial.println(F("Jumper D10 -> D11 for loopback.\n"));

  loopbackTest(CYD_BAUD);
}

void loop() {
  // Re-run on any keypress, so baud rates can be swept by hand.
  if (!Serial.available()) return;
  while (Serial.available()) Serial.read();
  Serial.println(F("\n--- rerun ---"));
  static const uint32_t bauds[] = {115200UL, 460800UL, 921600UL, 1000000UL};
  for (uint8_t i = 0; i < 4; i++) loopbackTest(bauds[i]);
}
