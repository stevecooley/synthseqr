#include <Adafruit_NeoPixel.h>


/*
 * Synthseqr v3 — Hardware Bring-Up Test Suite
 * Target: Adafruit Feather M4 Express (SAMD51)
 *
 * Pin map verified against the fab revision of synthseqr_v3.kicad_pcb:
 *   D5           LED data  -> 74AHCT125 -> R37 -> SK6812 chain (36 LEDs)
 *   D4/D6/D9/D12 mux select S0/S1/S2/S3 (shared by all three CD74HC4067)
 *   A0           fader mux COM   (U1, 16 slide pots)
 *   SDA          button mux COM  (U2, BTN0-15)
 *   SCL          button mux COM  (U3, BTN16-31)
 *   A1/MOSI/SCK/A5  SHIFT / STOP / RECORD / PLAY  (SW33-36, direct)
 *   MISO         S1 soft-power rocker (to GND, internal pull-up)
 *   A2/A3/A4     encoder A / B / switch
 *
 * TWO BUTTON POLARITIES. SW1-32 switch to GND against RN1-4 10k pull-ups, so
 * they are active LOW. The four transport buttons switch to 3V3 against R5-R8
 * 1k pull-downs, so they are active HIGH and must NOT use INPUT_PULLUP.
 *
 * Do NOT call Wire.begin() — SDA/SCL are plain GPIO on this board.
 *
 * THE ADC REFERENCE IS NOT SET UP FOR YOU. The Arduino core's init() leaves
 * ADC0/ADC1 REFCTRL.REFSEL at its reset value 0 = INTREF = the 1.0 V bandgap,
 * so analogRead() saturates a third of the way up a fader's travel unless
 * setup() calls analogReference(). See §adc below.
 *
 * Send a digit over serial at 115200 to run a test. '?' reprints the menu.
 */


// ---------------------------------------------------------------- pin map
constexpr uint8_t PIN_S0 = 4, PIN_S1 = 6, PIN_S2 = 9, PIN_S3 = 12;
constexpr uint8_t PIN_FADER = A0;
constexpr uint8_t PIN_BTN_A = SDA;   // BTN0-15
constexpr uint8_t PIN_BTN_B = SCL;   // BTN16-31
constexpr uint8_t PIN_LED_DATA = 5;
constexpr uint8_t PIN_ENC_A = A2, PIN_ENC_B = A3, PIN_ENC_SW = A4;
constexpr uint8_t PIN_SLIDE = MISO;  // S1

// Transport, in btnState order after the 32 muxed buttons.
constexpr uint8_t PIN_TRANSPORT[4] = { A1, MOSI, SCK, A5 };
const char *const TRANSPORT_NAME[4] = { "SHIFT", "STOP", "RECORD", "PLAY" };

constexpr uint8_t NUM_LEDS = 36;
constexpr uint8_t NUM_FADERS = 16;
constexpr uint8_t NUM_MUXED_BTNS = 32;
constexpr uint8_t NUM_BTNS = 36;

// Keep this low. 36 SK6812 at full white is ~2.2A; bring-up runs off whatever
// supply is on the bench. 40/255 is bright enough to see, gentle on the rail.
constexpr uint8_t LED_BRIGHTNESS = 40;

// Ceiling for the LED chain, enforced per frame by ledShowCapped(). A brightness
// setting alone does not bound anything: 36 pixels at white draw 8x what 36 at
// a dim single colour do. The budget has to be on the sum.
//
//   barrel jack, 5V 4A     2000   (the design point, room for full white)
//   USB 3 port, 900mA       700   (less the Feather's own ~50mA)
//   USB 2 port, 500mA       400
//   USB 2 sharing with CYD  200   (ESP32 idles ~150mA, TX peaks to 500mA)
constexpr uint16_t LED_BUDGET_MA = 400;

// Per channel at full scale, and the per-LED draw of the controller itself with
// every channel dark. Measure yours with test c and a USB power meter.
constexpr uint8_t LED_MA_PER_CHANNEL = 20;
constexpr uint8_t LED_MA_QUIESCENT   = 1;

Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------- leds
// What the frame sitting in the strip buffer will draw, in mA. getPixels() is
// the post-brightness byte array actually clocked out, so this sees what the
// LEDs will see, whatever setBrightness() is doing.
uint16_t ledEstimateMa() {
  uint8_t *px = strip.getPixels();
  uint16_t n  = strip.numPixels();
  uint32_t sum = 0;
  for (uint16_t i = 0; i < n * 3; i++) sum += px[i];
  return (sum * LED_MA_PER_CHANNEL) / 255 + n * LED_MA_QUIESCENT;
}

// show(), but scale the frame down first if it would exceed the budget. Scaling
// the buffer in place keeps the colour ratios and costs one pass; the quiescent
// term does not scale, so it comes out of the budget before the division.
uint16_t ledShowCapped(uint16_t budgetMa) {
  uint16_t est  = ledEstimateMa();
  uint16_t idle = strip.numPixels() * LED_MA_QUIESCENT;
  if (est > budgetMa && est > idle && budgetMa > idle) {
    uint32_t scale = ((uint32_t)(budgetMa - idle) * 256) / (est - idle);
    uint8_t *px = strip.getPixels();
    for (uint16_t i = 0; i < strip.numPixels() * 3; i++)
      px[i] = (uint8_t)((px[i] * scale) >> 8);
    est = ledEstimateMa();
  }
  strip.show();
  return est;
}

// ---------------------------------------------------------------- state
uint16_t faderRaw[NUM_FADERS];
bool     btnState[NUM_BTNS];
uint8_t  faderCh = 0;                  // channel tests 8/9/a/b work on

volatile int32_t encPos = 0;
volatile uint8_t encPrev = 0;

// ---------------------------------------------------------------- adc
// §adc — why analogReference() is not optional on the SAMD51.
//
// adafruit:samd 1.7.17, cores/arduino/wiring.c init(): the SAMD51 branch sets
// PRESCALER, RESSEL, SAMPCTRL, INPUTCTRL and AVGCTRL on both ADCs and never
// writes REFCTRL. REFSEL therefore keeps its reset value 0x0 = INTREF, the
// internal bandgap, which SUPC->VREF.SEL defaults to 1.0 V. Full scale is
// ~1.0 V, not 3.3 V, so a fader wired 3V3 - element - GND runs out of ADC at
// 1.0/3.3 = 30% of its travel: 0..~13 mm of a 45 mm PS45 sweeps 0..4095 and
// the remaining ~32 mm all reads 4095. It looks exactly like a wildly
// nonlinear pot, which is what sent us here.
//
// analogReference(AR_DEFAULT) selects REFSEL = INTVCC1 (wiring_analog.c), and
// on the SAMD51 INTVCC1 = 0x3 = VDDANA — the full 3V3. Careful reading any
// SAMD21 example: there 0x2 is VDDANA and 0x3 is AREFA, and the two families
// share the macro names but not the values.
//
// AREF (Feather header pin 3) has no net on synthseqr v3, so AR_EXTERNAL is
// not available to us. We do not need it: INTVCC1 is the whole rail, and it is
// the same rail the faders divide, so the reading is ratiometric and rail sag
// cancels out.
constexpr float ADC_FS       = 4095.0f;   // 12-bit
constexpr float V_RAIL_NOM   = 3.30f;     // fallback only; the rail is measured

// REFSEL values from the SAMD51 CMSIS header (component/adc.h). 0x1 is reserved.
struct AdcRef { uint8_t refsel; const char *name; };
const AdcRef ADC_REFS[] = {
  { 0x0, "INTREF  bandgap 1.0V  (reset default: the bug)" },
  { 0x2, "INTVCC0 1/2 VDDANA                            " },
  { 0x3, "INTVCC1 VDDANA        (what we want)          " },
  { 0x4, "AREFA   external AREF (unconnected on v3)     " },
};
constexpr uint8_t NUM_ADC_REFS = sizeof(ADC_REFS) / sizeof(ADC_REFS[0]);

// The 3V3 rail, and full scale under the reference currently selected. Both
// measured at boot and refreshed by adcReportRef(); the constant above is only
// what they fall back to if the internal channels read implausibly.
float adcRailVolts = V_RAIL_NOM;
float adcRefVolts  = V_RAIL_NOM;

uint8_t adcGetRefSel() {
#if defined(__SAMD51__)
  return ADC0->REFCTRL.bit.REFSEL;
#else
  return 0xFF;
#endif
}

const char *adcRefName(uint8_t refsel) {
  for (uint8_t i = 0; i < NUM_ADC_REFS; i++)
    if (ADC_REFS[i].refsel == refsel) return ADC_REFS[i].name;
  return "unknown";
}

// What a REFSEL should come out at, for when a measurement is unavailable.
// An external AREF has no nominal at all — on v3 the pin is not even connected.
float adcRefNominal(uint8_t refsel) {
  switch (refsel) {
    case 0x0: return 1.00f;                  // INTREF, SUPC VREF.SEL resets to 1V0
    case 0x2: return adcRailVolts / 2.0f;    // INTVCC0
    case 0x3: return adcRailVolts;           // INTVCC1
    default:  return 0.0f;                   // AREFA/B/C
  }
}

// Point REFCTRL straight at a REFSEL value. The datasheet requires throwing
// away the first conversion after a reference change; analogRead() already
// discards one internally, so two calls is belt and braces.
void adcSetRefSel(uint8_t refsel) {
#if defined(__SAMD51__)
  while (ADC0->SYNCBUSY.reg & ADC_SYNCBUSY_REFCTRL);
  ADC0->REFCTRL.bit.REFSEL = refsel;
  while (ADC0->SYNCBUSY.reg & ADC_SYNCBUSY_REFCTRL);
  delay(2);                                  // reference settling
  (void)analogRead(PIN_FADER);
  (void)analogRead(PIN_FADER);
#else
  (void)refsel;
#endif
}

#if defined(__SAMD51__)
// One of the ADC's internal MUXPOS channels, averaged. Not reachable through
// analogRead(), which only takes pins.
uint16_t adcReadInternal(uint8_t muxpos) {
  while (ADC0->SYNCBUSY.reg & ADC_SYNCBUSY_INPUTCTRL);
  ADC0->INPUTCTRL.bit.MUXPOS = muxpos;
  while (ADC0->SYNCBUSY.reg & ADC_SYNCBUSY_INPUTCTRL);

  while (ADC0->SYNCBUSY.reg & ADC_SYNCBUSY_ENABLE);
  ADC0->CTRLA.bit.ENABLE = 1;
  while (ADC0->SYNCBUSY.reg & ADC_SYNCBUSY_ENABLE);

  uint32_t acc = 0;
  for (uint8_t i = 0; i < 9; i++) {           // first conversion discarded
    ADC0->INTFLAG.reg = ADC_INTFLAG_RESRDY;
    ADC0->SWTRIG.bit.START = 1;
    while (ADC0->INTFLAG.bit.RESRDY == 0);
    uint16_t r = ADC0->RESULT.reg;
    if (i) acc += r;
  }

  while (ADC0->SYNCBUSY.reg & ADC_SYNCBUSY_ENABLE);
  ADC0->CTRLA.bit.ENABLE = 0;
  while (ADC0->SYNCBUSY.reg & ADC_SYNCBUSY_ENABLE);
  return acc / 8;
}
#endif

// Measure the reference instead of trusting it: SCALEDIOVCC is VDDIO/4 ~ 0.825V
// generated inside the chip, so the count it comes back as says what full scale
// is. 1024 counts => 3.3V ref, 2048 => 1.65V, 3378 => 1.0V. Returns 0 if the
// number is implausible (or on a non-SAMD51 build) so callers can fall back.
float adcMeasureRefVolts() {
#if defined(__SAMD51__)
  uint16_t raw = adcReadInternal(ADC_INPUTCTRL_MUXPOS_SCALEDIOVCC_Val);
  if (raw < 200 || raw > 4090) return 0.0f;        // clipped or dead
  float v = (adcRailVolts / 4.0f) * ADC_FS / raw;
  if (v < 0.5f || v > 4.0f) return 0.0f;
  return v;
#else
  return 0.0f;
#endif
}

// The 3V3 rail itself, measured rather than assumed. SCALEDIOVCC is VDDIO/4, so
// against the 1.0V bandgap the count is (VDD/4)/1.0 * 4095 and VDD falls out.
// It has to be the bandgap: against INTVCC1 the reference IS VDDANA, the ratio
// cancels, and the answer is 1024 counts no matter what the rail is doing.
float adcMeasureRailVolts() {
#if defined(__SAMD51__)
  uint8_t saved = adcGetRefSel();
  SUPC->VREF.bit.SEL = SUPC_VREF_SEL_1V0_Val;
  SUPC->VREF.bit.VREFOE = 1;                       // as the core does for INTREF
  adcSetRefSel(ADC_REFCTRL_REFSEL_INTREF_Val);
  uint16_t raw = adcReadInternal(ADC_INPUTCTRL_MUXPOS_SCALEDIOVCC_Val);
  adcSetRefSel(saved);
  if (raw < 200 || raw > 4090) return 0.0f;
  float v = 4.0f * raw / ADC_FS;                   // bandgap tolerance, so ~2%
  return (v > 2.5f && v < 4.0f) ? v : 0.0f;
#else
  return 0.0f;
#endif
}

void adcReportRef() {
  float rail = adcMeasureRailVolts();
  if (rail > 0.0f) adcRailVolts = rail;
  Serial.print(F("3V3 rail: "));
  Serial.print(adcRailVolts, 3);
  Serial.println(rail > 0.0f ? F("V measured (+/-2%, internal bandgap)")
                             : F("V assumed — rail measurement failed"));

  uint8_t sel = adcGetRefSel();
  Serial.print(F("ADC reference: REFSEL=0x"));
  Serial.print(sel, HEX);
  Serial.print(F("  ")); Serial.print(adcRefName(sel));
  float m = adcMeasureRefVolts();
  if (m > 0.0f) {
    adcRefVolts = m;
    Serial.print(F("  measured "));
    Serial.print(m, 3); Serial.println(F("V full scale"));
  } else {
    adcRefVolts = adcRefNominal(sel);
    if (adcRefVolts <= 0.0f) adcRefVolts = adcRailVolts;
    Serial.print(F("  nominal "));
    Serial.print(adcRefVolts, 3);
    Serial.println(F("V full scale (SCALEDIOVCC read implausible)"));
  }
  if (adcRefVolts < adcRailVolts * 0.9f) {
    Serial.print(F("  *** full scale is only "));
    Serial.print(adcRefVolts, 2);
    Serial.println(F("V — faders pin at 4095 partway up their travel."));
    Serial.println(F("  *** setup() needs analogReference(AR_DEFAULT). See test 9."));
  }
}

// ---------------------------------------------------------------- mux
inline void muxSelect(uint8_t ch) {
  digitalWrite(PIN_S0, ch & 0x01);
  digitalWrite(PIN_S1, (ch >> 1) & 0x01);
  digitalWrite(PIN_S2, (ch >> 2) & 0x01);
  digitalWrite(PIN_S3, (ch >> 3) & 0x01);
}

// One sweep reads every fader and every muxed button.
void scanAll() {
  for (uint8_t ch = 0; ch < 16; ch++) {
    muxSelect(ch);
    delayMicroseconds(50);          // RC + mux settling before the ADC
    (void)analogRead(PIN_FADER);    // throw away first conversion
    faderRaw[ch] = analogRead(PIN_FADER);
    btnState[ch]      = (digitalRead(PIN_BTN_A) == LOW);
    btnState[ch + 16] = (digitalRead(PIN_BTN_B) == LOW);
  }
  // Transport buttons are pulled down and switch to 3V3 — inverted vs the muxed
  // ones, so they read HIGH when pressed.
  for (uint8_t i = 0; i < 4; i++) {
    btnState[NUM_MUXED_BTNS + i] = (digitalRead(PIN_TRANSPORT[i]) == HIGH);
  }
}

// ---------------------------------------------------------------- encoder
void encISR() {
  uint8_t s = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  if (s == encPrev) return;
  // gray-code step table
  static const int8_t tbl[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
  encPos += tbl[(encPrev << 2) | s];
  encPrev = s;
}

// ---------------------------------------------------------------- tests

// 0 — quick diagnostic sweep, flags anything obviously dead
void testDiagnostic() {
  Serial.println(F("\n--- Diagnostic sweep ---"));
  adcReportRef();
  uint16_t lo[NUM_FADERS], hi[NUM_FADERS];
  for (uint8_t i = 0; i < NUM_FADERS; i++) { lo[i] = 0xFFFF; hi[i] = 0; }

  for (int pass = 0; pass < 200; pass++) {
    scanAll();
    for (uint8_t i = 0; i < NUM_FADERS; i++) {
      if (faderRaw[i] < lo[i]) lo[i] = faderRaw[i];
      if (faderRaw[i] > hi[i]) hi[i] = faderRaw[i];
    }
    delay(2);
  }

  Serial.println(F("Faders (noise band over 200 samples, fader at rest):"));
  for (uint8_t i = 0; i < NUM_FADERS; i++) {
    Serial.print(F("  F")); Serial.print(i); Serial.print(F(": "));
    Serial.print(lo[i]); Serial.print(F(".."));  Serial.print(hi[i]);
    uint16_t band = hi[i] - lo[i];
    if (hi[i] < 8)          Serial.print(F("   <-- stuck at GND?"));
    else if (lo[i] > 4088)  Serial.print(F("   <-- pinned at full scale"));
    else if (band > 40)     Serial.print(F("   <-- noisy"));
    Serial.println();
  }

  uint8_t stuck = 0;
  for (uint8_t i = 0; i < NUM_BTNS; i++) if (btnState[i]) stuck++;
  Serial.print(F("Buttons reading pressed with nothing touched: "));
  Serial.println(stuck);
  if (stuck) {
    Serial.print(F("  -> "));
    for (uint8_t i = 0; i < NUM_BTNS; i++) if (btnState[i]) { Serial.print(i); Serial.print(' '); }
    Serial.println(F("\n  (a stuck-low input usually means a missing pull-up or a solder bridge)"));
  }
  Serial.println(F("A fader pinned at full scale below the top of its travel is either"));
  Serial.println(F("the ADC reference (test 9) or its wiper leaking to 3V3 (test b)."));
  Serial.println(F("--- done ---"));
}

// 1 — walk one LED at a time; confirms count, order, and every solder joint
void testLedWalk() {
  Serial.println(F("\nWalking LEDs 0..35 — watch for skips or wrong order."));
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    strip.clear();
    strip.setPixelColor(i, strip.Color(255, 255, 255));
    ledShowCapped(LED_BUDGET_MA);
    Serial.print(F("  LED ")); Serial.println(i);
    delay(250);
  }
  strip.clear(); strip.show();
  Serial.println(F("If a LED never lit, the chain stops at the one before it."));
}

// 2 — RGB channel check across the whole chain
void testLedColors() {
  const uint32_t cols[] = { strip.Color(255,0,0), strip.Color(0,255,0),
                            strip.Color(0,0,255), strip.Color(255,255,255) };
  const char* names[] = { "RED", "GREEN", "BLUE", "WHITE" };
  for (uint8_t c = 0; c < 4; c++) {
    Serial.print(F("  all ")); Serial.println(names[c]);
    strip.fill(cols[c]); ledShowCapped(LED_BUDGET_MA);
    delay(1200);
  }
  strip.clear(); strip.show();
  Serial.println(F("Any LED showing the wrong color = bad data timing or a cold joint."));
}

// 3 — press any key, see its index
void testButtons() {
  Serial.println(F("\nPress keys — index prints on press/release. Send any char to stop."));
  bool prev[NUM_BTNS] = {false};
  while (!Serial.available()) {
    scanAll();
    for (uint8_t i = 0; i < NUM_BTNS; i++) {
      if (btnState[i] != prev[i]) {
        Serial.print(btnState[i] ? F("  DOWN ") : F("  up   "));
        Serial.println(i);
        prev[i] = btnState[i];
      }
    }
    delay(5);   // crude debounce for bring-up
  }
  while (Serial.available()) Serial.read();
}

// 4 — live fader bars
void testFaders() {
  Serial.println(F("\nMove the faders. Send any char to stop."));
  while (!Serial.available()) {
    scanAll();
    for (uint8_t i = 0; i < NUM_FADERS; i++) {
      Serial.print(F("F")); if (i < 10) Serial.print('0'); Serial.print(i);
      Serial.print(' ');
      uint8_t bars = faderRaw[i] / 205;            // 0..19
      for (uint8_t b = 0; b < 20; b++) Serial.print(b < bars ? '#' : '.');
      Serial.print(' '); Serial.println(faderRaw[i]);
    }
    Serial.println();
    delay(200);
  }
  while (Serial.available()) Serial.read();
}

// 5 — encoder
void testEncoder() {
  Serial.println(F("\nTurn the encoder / press it. Send any char to stop."));
  int32_t last = encPos - 1;
  bool lastSw = true;
  while (!Serial.available()) {
    bool sw = (digitalRead(PIN_ENC_SW) == LOW);
    if (encPos != last) { Serial.print(F("  pos ")); Serial.println(encPos); last = encPos; }
    if (sw != lastSw)   { Serial.println(sw ? F("  CLICK down") : F("  click up")); lastSw = sw; }
    delay(5);
  }
  while (Serial.available()) Serial.read();
}

// 6 — the fun one: keys light their own LED, fader 0 sets brightness
void testInteractive() {
  Serial.println(F("\nPress keys to light them. Fader 0 = brightness. Send any char to stop."));
  while (!Serial.available()) {
    scanAll();
    uint8_t b = map(faderRaw[0], 0, 4095, 2, 120);
    strip.setBrightness(b);
    strip.clear();
    for (uint8_t i = 0; i < NUM_LEDS; i++)
      if (btnState[i]) strip.setPixelColor(i, strip.Color(0, 255, 120));
    ledShowCapped(LED_BUDGET_MA);
    delay(5);
  }
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear(); strip.show();
  while (Serial.available()) Serial.read();
}

// 7 — transport buttons. These are the inverted ones: pulled down by R5-R8 and
// switched to 3V3, so idle is LOW and pressed is HIGH. The idle check catches a
// pin left with INPUT_PULLUP set, which would pin it high and mask every press.
void testTransport() {
  Serial.println(F("\nTransport check (SHIFT / STOP / RECORD / PLAY)."));
  Serial.println(F("  Hands off for 2s..."));
  delay(300);
  uint16_t idleHigh[4] = {0, 0, 0, 0};
  for (int i = 0; i < 200; i++) {
    for (uint8_t b = 0; b < 4; b++)
      if (digitalRead(PIN_TRANSPORT[b]) == HIGH) idleHigh[b]++;
    delay(10);
  }
  for (uint8_t b = 0; b < 4; b++) {
    Serial.print(F("  ")); Serial.print(TRANSPORT_NAME[b]);
    Serial.print(F(": idle HIGH on ")); Serial.print(idleHigh[b]);
    Serial.println(idleHigh[b] > 10 ? F("/200  <- NOT idling low, check pull-down")
                                    : F("/200  ok"));
  }
  Serial.println(F("\n  Now press each one in turn. 15s, q to quit early."));
  while (Serial.available()) Serial.read();
  bool seen[4] = {false, false, false, false};
  for (uint32_t t0 = millis(); millis() - t0 < 15000; ) {
    for (uint8_t b = 0; b < 4; b++) {
      if (digitalRead(PIN_TRANSPORT[b]) == HIGH && !seen[b]) {
        seen[b] = true;
        Serial.print(F("    ")); Serial.println(TRANSPORT_NAME[b]);
      }
    }
    if (Serial.available() && Serial.read() == 'q') break;
    delay(5);
  }
  for (uint8_t b = 0; b < 4; b++)
    if (!seen[b]) { Serial.print(F("  never saw ")); Serial.println(TRANSPORT_NAME[b]); }
}

// 8 — one channel at a time, averaged, for comparing against a DMM
void testFaderFocus() {
  uint8_t ch = faderCh;   // RV1's wiper was unconnected pre-fab; fixed in this revision
  Serial.println(F("\nSingle-channel fader probe."));
  adcReportRef();
  Serial.println(F("  0-9,a-f = channel    r = reset span    q = quit"));

  while (Serial.available()) Serial.read();

  uint16_t lo = 0xFFFF, hi = 0;
  int32_t  lastShown = -9999;
  uint32_t lastPrint = 0;

  for (;;) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'q' || c == 'Q') break;
      int8_t sel = -1;
      if      (c >= '0' && c <= '9') sel = c - '0';
      else if (c >= 'a' && c <= 'f') sel = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') sel = c - 'A' + 10;
      if (sel >= 0) {
        ch = sel; faderCh = sel; lo = 0xFFFF; hi = 0; lastShown = -9999;
        Serial.print(F("\n-- F")); Serial.print(ch);
        Serial.print(F("  (RV")); Serial.print(ch + 1); Serial.println(F(")"));
      } else if (c == 'r' || c == 'R') {
        lo = 0xFFFF; hi = 0;
        Serial.println(F("-- span reset"));
      }
    }

    // A DMM shows a DC average, so average here too or the numbers won't agree.
    muxSelect(ch);
    delayMicroseconds(200);
    (void)analogRead(PIN_FADER);
    uint32_t acc = 0;
    for (uint8_t i = 0; i < 32; i++) acc += analogRead(PIN_FADER);
    uint16_t raw = acc / 32;

    if (raw < lo) lo = raw;
    if (raw > hi) hi = raw;

    int32_t diff = (int32_t)raw - lastShown;
    if (diff < 0) diff = -diff;
    uint32_t now = millis();
    if (diff > 6 || now - lastPrint > 1000) {
      lastShown = raw;
      lastPrint = now;
      Serial.print(F("F"));   if (ch < 10)   Serial.print('0');
      Serial.print(ch);
      Serial.print(F("  raw "));
      if (raw < 1000) Serial.print(' ');
      if (raw < 100)  Serial.print(' ');
      if (raw < 10)   Serial.print(' ');
      Serial.print(raw);
      Serial.print(F("  "));
      Serial.print((raw * 100.0f) / 4095.0f, 1);
      Serial.print(F("%FS   wiper "));
      Serial.print(raw * adcRefVolts / ADC_FS, 3);
      Serial.print(F("V (ref "));
      Serial.print(adcRefVolts, 2);
      Serial.print(F("V)   span "));
      Serial.print(lo); Serial.print('-'); Serial.println(hi);
    }
    delay(40);
  }
  while (Serial.available()) Serial.read();
}

// c — LED current calibration. All 36 white, stepping up, printing what the
// estimator thinks each frame costs next to what it allowed through. Put a USB
// power meter inline: if the meter and the estimate diverge, edit
// LED_MA_PER_CHANNEL until they agree, and the budget becomes trustworthy.
void testLedCurrent() {
  Serial.println(F("\nAll 36 white, stepping brightness up. 2s per step."));
  Serial.print(F("  budget ")); Serial.print(LED_BUDGET_MA);
  Serial.println(F("mA — raise it in the sketch to see the uncapped curve."));
  Serial.println(F("  add ~50mA for the Feather itself when comparing to a meter.\n"));
  Serial.println(F("  bright   want    allowed"));

  for (uint16_t b = 15; b <= 255; b += 30) {
    strip.setBrightness(b);
    strip.fill(strip.Color(255, 255, 255));
    uint16_t want = ledEstimateMa();
    uint16_t got  = ledShowCapped(LED_BUDGET_MA);

    Serial.print(F("    "));
    if (b < 100) Serial.print(' ');
    Serial.print(b);
    Serial.print(F("   "));
    if (want < 1000) Serial.print(' ');
    if (want < 100)  Serial.print(' ');
    Serial.print(want); Serial.print(F("mA"));
    Serial.print(F("   "));
    if (got < 1000) Serial.print(' ');
    if (got < 100)  Serial.print(' ');
    Serial.print(got); Serial.print(F("mA"));
    if (got < want) Serial.print(F("   <-- capped"));
    Serial.println();

    delay(2000);
    if (Serial.available()) break;
  }

  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear(); strip.show();
  while (Serial.available()) Serial.read();
  Serial.println(F("\n  36 white at full is ~2.2A. A 500mA USB port, less the"));
  Serial.println(F("  Feather, leaves ~400mA — about brightness 45 all-white, and"));
  Serial.println(F("  far more than that for the handful of pixels a pattern lights."));
}

// Blocks until a mux channel is picked. 0xFF means the user bailed out.
// Mux channel n is RV(n+1): the CD74HC4067's Y0 is pin 9, which the netlist has
// on SlidePotentiometer_01, and the run continues in order up to Y15 on pin 16.
uint8_t promptChannel() {
  Serial.print(F("  channel? 0-9,a-f  (q cancels, Enter takes F"));
  Serial.print(faderCh); Serial.println(F(")"));
  while (Serial.available()) Serial.read();
  for (;;) {
    while (!Serial.available()) delay(2);
    char c = Serial.read();
    if (c == 'q' || c == 'Q')                 return 0xFF;
    if (c == '\r' || c == '\n')               return faderCh;
    if (c >= '0' && c <= '9')                 { faderCh = c - '0';      break; }
    if (c >= 'a' && c <= 'f')                 { faderCh = c - 'a' + 10; break; }
    if (c >= 'A' && c <= 'F')                 { faderCh = c - 'A' + 10; break; }
  }
  return faderCh;
}

// Averaged read of one fader channel, with the mux given time to settle.
uint16_t readFader(uint8_t ch, uint8_t n) {
  muxSelect(ch);
  delayMicroseconds(200);
  (void)analogRead(PIN_FADER);
  uint32_t acc = 0;
  for (uint8_t i = 0; i < n; i++) acc += analogRead(PIN_FADER);
  return acc / n;
}

// 9 — what is full scale, really? Walks every REFSEL and reports both the
// measured reference (from the internal VDDIO/4 channel) and what one fader
// reads under it. The row where the printed wiper volts matches your DMM is the
// reference you are actually running.
void testAdcRef() {
  uint8_t ch = promptChannel();
  if (ch == 0xFF) return;

  Serial.println(F("\nADC reference sweep."));
  Serial.print(F("  now: ")); adcReportRef();
  Serial.print(F("  fader F")); Serial.print(ch);
  Serial.print(F(" (RV")); Serial.print(ch + 1);
  Serial.println(F("), DMM on the wiper. Park it about a QUARTER of the way up,"));
  Serial.println(F("  not mid travel: half rail is full scale for both of the low"));
  Serial.println(F("  references, so two rows would read 4095 and tell you nothing."));
  Serial.print(F("  At a quarter of a "));
  Serial.print(adcRailVolts, 2);
  Serial.print(F("V rail the wiper is "));
  Serial.print(adcRailVolts / 4.0f, 2);
  Serial.println(F("V, and the rows should read roughly"));
  Serial.println(F("  3317 / 2048 / 1024 for INTREF / INTVCC0 / INTVCC1.\n"));

  Serial.println(F("  REFSEL  reference                                       meas FS   raw   wiper"));
  uint8_t saved = adcGetRefSel();
  for (uint8_t i = 0; i < NUM_ADC_REFS; i++) {
    adcSetRefSel(ADC_REFS[i].refsel);
    float    fs   = adcMeasureRefVolts();
    bool     meas = (fs > 0.0f);
    if (!meas) fs = adcRefNominal(ADC_REFS[i].refsel);   // 0 for external AREF
    uint16_t raw  = readFader(ch, 32);

    Serial.print(F("  0x")); Serial.print(ADC_REFS[i].refsel, HEX);
    Serial.print(F("     ")); Serial.print(ADC_REFS[i].name);
    Serial.print(F("  "));
    if (fs <= 0.0f)     { Serial.print(F("   ?   ")); }
    else if (meas)      { Serial.print(' '); Serial.print(fs, 3); Serial.print(F("V")); }
    else                { Serial.print('~'); Serial.print(fs, 3); Serial.print(F("V")); }
    Serial.print(F("  "));
    if (raw < 1000) Serial.print(' ');
    if (raw < 100)  Serial.print(' ');
    if (raw < 10)   Serial.print(' ');
    Serial.print(raw);
    Serial.print(F("  "));
    if (fs > 0.0f) { Serial.print(raw * fs / ADC_FS, 3); Serial.println(F("V")); }
    else           { Serial.println(F("  ?")); }
  }

  // Back to the one we want, whatever the sketch was built with.
  analogReference(AR_DEFAULT);
  delay(2);
  (void)analogRead(PIN_FADER);
  Serial.print(F("\n  restored: ")); adcReportRef();
  if (adcGetRefSel() != saved) Serial.println(F("  (was NOT AR_DEFAULT on entry)"));
  Serial.println(F("  A row reading 4095 means that reference is too low for a 0-3V3 fader."));
}

// a — capture the whole travel and print the shape. Clipping and curvature look
// identical on a live readout; on a plot they do not.
constexpr uint16_t SWEEP_N  = 200;
constexpr uint16_t SWEEP_MS = 25;      // 200 * 25ms = 5s of sweep

void testFaderSweep() {
  static uint16_t buf[SWEEP_N];
  uint8_t ch = promptChannel();
  if (ch == 0xFF) return;

  adcReportRef();
  Serial.print(F("\n  Put F")); Serial.print(ch);
  Serial.println(F(" at ONE end of its travel, then send any char."));
  Serial.println(F("  Then sweep it smoothly to the other end over about 5 seconds."));
  while (Serial.available()) Serial.read();
  while (!Serial.available()) delay(5);
  while (Serial.available()) Serial.read();

  Serial.println(F("  GO..."));
  for (uint16_t i = 0; i < SWEEP_N; i++) {
    uint32_t t0 = millis();
    buf[i] = readFader(ch, 8);
    while (millis() - t0 < SWEEP_MS);
  }
  Serial.println(F("  done.\n"));

  bool reversed = buf[0] > buf[SWEEP_N - 1];
  if (reversed)                                 // analyse bottom-to-top always
    for (uint16_t i = 0; i < SWEEP_N / 2; i++) {
      uint16_t t = buf[i]; buf[i] = buf[SWEEP_N - 1 - i]; buf[SWEEP_N - 1 - i] = t;
    }

  uint16_t mn = 0xFFFF, mx = 0;
  uint16_t pinned = 0;
  for (uint16_t i = 0; i < SWEEP_N; i++) {
    if (buf[i] < mn) mn = buf[i];
    if (buf[i] > mx) mx = buf[i];
    if (buf[i] >= 4085) pinned++;
  }

  Serial.print(F("  travel%  raw   "));
  Serial.println(reversed ? F("(swept top->bottom, shown reversed)") : F(""));
  for (uint16_t i = 0; i < SWEEP_N; i += 5) {
    uint16_t pct = (uint32_t)i * 100 / (SWEEP_N - 1);
    if (pct < 10) Serial.print(' ');
    Serial.print(F("   ")); Serial.print(pct); Serial.print(F("%  "));
    if (buf[i] < 1000) Serial.print(' ');
    if (buf[i] < 100)  Serial.print(' ');
    if (buf[i] < 10)   Serial.print(' ');
    Serial.print(buf[i]); Serial.print(' ');
    uint8_t bars = (uint32_t)buf[i] * 48 / 4095;
    for (uint8_t b = 0; b < 48; b++) Serial.print(b < bars ? '#' : '.');
    Serial.println();
  }

  // Where did it stop changing?
  uint16_t knee = SWEEP_N - 1;
  uint16_t topBand = (mx > 40) ? mx - mx / 50 : mx;   // within 2% of the top
  for (uint16_t i = 0; i < SWEEP_N; i++) if (buf[i] >= topBand) { knee = i; break; }
  uint16_t kneePct = (uint32_t)knee * 100 / (SWEEP_N - 1);

  // Deviation from a straight line between the endpoints. Only meaningful if
  // the sweep was even, so it is advisory — the clipping verdict below is not.
  int32_t worst = 0;
  for (uint16_t i = 0; i < SWEEP_N; i++) {
    int32_t line = (int32_t)buf[0] +
                   ((int32_t)buf[SWEEP_N - 1] - (int32_t)buf[0]) * i / (SWEEP_N - 1);
    int32_t d = (int32_t)buf[i] - line;
    if (d < 0) d = -d;
    if (d > worst) worst = d;
  }

  Serial.print(F("\n  span "));       Serial.print(mn); Serial.print(F(".."));  Serial.print(mx);
  Serial.print(F("   at/above 4085 for ")); Serial.print((uint32_t)pinned * 100 / SWEEP_N);
  Serial.println(F("% of the sweep"));
  Serial.print(F("  reached 98% of its maximum at ")); Serial.print(kneePct);
  Serial.println(F("% of the sweep"));
  Serial.print(F("  worst deviation from a straight line: ")); Serial.print(worst);
  Serial.print(F(" counts (")); Serial.print(worst * 100.0f / ADC_FS, 1);
  Serial.println(F("% FS, even sweep assumed)"));

  Serial.println();
  if (mx >= 4085 && kneePct < 85) {
    Serial.println(F("  CLIPPED: full scale arrives before the end of travel."));
    Serial.println(F("  The travel below the knee is the real signal; above it the ADC is"));
    Serial.println(F("  saturated, so it cannot be 'an exponential pot'. Two causes:"));
    Serial.println(F("    - reference below 3V3               -> test 9"));
    Serial.println(F("    - wiper being pulled up toward 3V3  -> test b, and DMM the wiper"));
  } else if (worst > 250) {
    Serial.println(F("  CURVED, not clipped: the divider itself is nonlinear."));
    Serial.println(F("  A linear element cannot do this on its own. Look for a resistive"));
    Serial.println(F("  path from the wiper node to a rail (solder bridge across the pot"));
    Serial.println(F("  pads, or C4-C21 leaking), then run test b. Confirm with the DMM:"));
    Serial.println(F("  a healthy 10k divider gives 0 / 0.83 / 1.65 / 2.48 / 3.30 V at"));
    Serial.println(F("  0 / 25 / 50 / 75 / 100% of travel."));
  } else {
    Serial.println(F("  Looks linear and unclipped."));
  }
}

// b — in-circuit impedance probe. C4-C21 put 100nF on every wiper, so the node
// has a time constant: drive A0 hard for a moment, let go, and watch the 100nF
// relax back through the pot. tau = R_th * C with R_th = Rupper || Rlower, which
// peaks at Relement/4 in mid travel. Solving for the element resistance says
// whether the part is the 10k the BOM asked for — and a pot drawing enough
// current to get hot is a pot whose R_th is nowhere near 2.5k.
//
// The drive is 400us into at most ~13mA (4067 Ron against a wiper sitting on a
// rail), and only the pot's own resistance sets the decay, since A0 is released
// to high-Z before the measurement starts.
constexpr uint16_t IMP_N = 64;

void testFaderImpedance() {
  static uint32_t ts[IMP_N];
  static uint16_t vs[IMP_N];

  uint8_t ch = promptChannel();
  if (ch == 0xFF) return;

  adcReportRef();
  Serial.print(F("\n  Probing F")); Serial.print(ch);
  Serial.print(F(" (RV")); Serial.print(ch + 1);
  Serial.println(F("). Park it near MID travel — R_th goes to zero at the ends."));
  Serial.println(F("  Any char stops.\n"));
  while (Serial.available()) Serial.read();

  while (!Serial.available()) {
    uint16_t v0 = readFader(ch, 32);
    bool driveHigh = (v0 < 2048);

    muxSelect(ch);
    pinMode(PIN_FADER, OUTPUT);
    digitalWrite(PIN_FADER, driveHigh ? HIGH : LOW);
    delayMicroseconds(400);
    pinMode(PIN_FADER, INPUT);            // release: the 100nF starts relaxing

    for (uint16_t i = 0; i < IMP_N; i++) {
      ts[i] = micros();
      vs[i] = analogRead(PIN_FADER);
    }

    int32_t s1 = (int32_t)vs[0] - (int32_t)v0;
    if (s1 < 0) s1 = -s1;

    Serial.print(F("  raw ")); Serial.print(v0);
    Serial.print(F("  ")); Serial.print(v0 * adcRefVolts / ADC_FS, 3);
    Serial.print(F("V  step ")); Serial.print(s1);
    Serial.print(F("  dt ")); Serial.print((ts[IMP_N - 1] - ts[0]) / (IMP_N - 1));
    Serial.print(F("us/sample   "));

    if (v0 >= 4085) {
      Serial.println(F("PINNED at full scale — fix the reference first (test 9)."));
    } else if (s1 < 40) {
      Serial.println(F("node barely moved: R_th below ~100R."));
      Serial.println(F("    Expected at the very ends of travel. Anywhere else the wiper is"));
      Serial.println(F("    clamped to a rail, or the element is a fraction of 10k — either"));
      Serial.println(F("    one is a current path big enough to cook the pot."));
    } else {
      // s(t) = A*exp(-t/tau). Two samples remove both A and the unknown t=0.
      // Stop at the first sample under 37% of the step: above the noise floor it
      // is the fit point, below it the decay outran the sampler.
      uint16_t k = 0;
      bool collapsed = false;
      for (uint16_t i = 1; i < IMP_N; i++) {
        int32_t si = (int32_t)vs[i] - (int32_t)v0;
        if (si < 0) si = -si;
        if (si <= (s1 * 37) / 100) {
          if (si >= 8) k = i;
          else         collapsed = true;
          break;
        }
      }
      uint32_t dt  = (ts[IMP_N - 1] - ts[0]) / (IMP_N - 1);
      uint32_t win = ts[IMP_N - 1] - ts[0];
      if (collapsed) {
        Serial.print(F("decayed inside one sample: R_th below ~"));
        Serial.print(dt * 10); Serial.println(F("R — shorted, or a very low element."));
      } else if (k == 0) {
        Serial.print(F("still decaying after ")); Serial.print(win);
        Serial.print(F("us: R_th above ~")); Serial.print(win / 100.0f, 0);
        Serial.println(F("k. Nothing fitted on this channel?"));
      } else {
        int32_t sk = (int32_t)vs[k] - (int32_t)v0;
        if (sk < 0) sk = -sk;
        float tau = (float)(ts[k] - ts[0]) / logf((float)s1 / (float)sk);
        float rth = tau * 10.0f;                     // ohms, for C = 100nF
        // Fraction of the rail the wiper sits at — via volts, so a mis-set
        // reference does not quietly turn into a wrong element resistance.
        float x   = (v0 * adcRefVolts / ADC_FS) / adcRailVolts;
        Serial.print(F("tau ")); Serial.print(tau, 0);
        Serial.print(F("us  R_th ")); Serial.print(rth, 0); Serial.print(F("R"));
        if (x > 0.08f && x < 0.92f) {
          float rtot = rth / (x * (1.0f - x));       // R_th = x(1-x)*Rtotal
          Serial.print(F("  element ~")); Serial.print(rtot / 1000.0f, 2);
          Serial.print(F("k  ->  "));
          Serial.print(adcRailVolts * adcRailVolts / rtot * 1000.0f, 1);
          Serial.print(F("mW across 3V3"));
          if (rtot < 6000.0f)       Serial.print(F("   <-- well under 10k"));
          else if (rtot > 16000.0f) Serial.print(F("   <-- well over 10k"));
        } else {
          Serial.print(F("  (too near a travel end to infer the element)"));
        }
        Serial.println();
      }
    }
    delay(700);
  }
  while (Serial.available()) Serial.read();
  Serial.println(F("\n  A healthy 10k PS45 at mid travel: tau ~250us, R_th ~2.5k, 1.1mW."));
  Serial.println(F("  The element is rated 250mW, so the designed circuit cannot get warm."));
}

// ---------------------------------------------------------------- menu
void printMenu() {
  Serial.println(F("\n=============================="));
  Serial.println(F(" Synthseqr v3 bring-up"));
  Serial.println(F("=============================="));
  Serial.println(F("  0  diagnostic sweep"));
  Serial.println(F("  1  LED walk (one at a time)"));
  Serial.println(F("  2  LED color check"));
  Serial.println(F("  3  button press readout"));
  Serial.println(F("  4  fader live bars"));
  Serial.println(F("  5  encoder"));
  Serial.println(F("  6  interactive (keys light LEDs)"));
  Serial.println(F("  7  transport buttons (active HIGH)"));
  Serial.println(F("  8  single fader probe (for DMM comparison)"));
  Serial.println(F("  9  ADC reference sweep (what is full scale?)"));
  Serial.println(F("  a  fader travel profile (clipped or curved?)"));
  Serial.println(F("  b  wiper impedance probe (element value, short hunt)"));
  Serial.println(F("  c  LED current budget (meter calibration)"));
  Serial.println(F("  ?  this menu"));
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000);

  pinMode(PIN_S0, OUTPUT); pinMode(PIN_S1, OUTPUT);
  pinMode(PIN_S2, OUTPUT); pinMode(PIN_S3, OUTPUT);
  muxSelect(0);

  pinMode(PIN_BTN_A, INPUT);        // external 10k pull-ups on RN1-4
  pinMode(PIN_BTN_B, INPUT);
  // R5-R8 pull these down; INPUT_PULLUP here would mask every press.
  for (uint8_t i = 0; i < 4; i++) pinMode(PIN_TRANSPORT[i], INPUT);
  pinMode(PIN_SLIDE, INPUT_PULLUP);   // S1 switches to GND, no external pull-up

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  encPrev = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  // Order matters only in that both must happen before the first analogRead().
  // Without the analogReference() call full scale is the 1.0V bandgap; see §adc.
  analogReference(AR_DEFAULT);       // REFSEL = INTVCC1 = VDDANA = 3V3
  analogReadResolution(12);
  (void)analogRead(PIN_FADER);       // first conversion after a ref change

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();

  adcReportRef();
  printMenu();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case '0': testDiagnostic();  break;
    case '1': testLedWalk();     break;
    case '2': testLedColors();   break;
    case '3': testButtons();     break;
    case '4': testFaders();      break;
    case '5': testEncoder();     break;
    case '6': testInteractive(); break;
    case '7': testTransport();   break;
    case '8': testFaderFocus();  break;
    case '9': testAdcRef();      break;
    case 'a':
    case 'A': testFaderSweep();  break;
    case 'b':
    case 'B': testFaderImpedance(); break;
    case 'c':
    case 'C': testLedCurrent();  break;
    case '?': printMenu();       break;
    default: return;
  }
  Serial.println(F("\n(send ? for menu)"));
}
