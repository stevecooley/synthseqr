#include <Adafruit_NeoPixel.h>


/*
 * Synthseqr v3 — Hardware Bring-Up Test Suite
 * Target: Adafruit Feather M4 Express (SAMD51)
 *
 * Pin map verified against synthseqr_v3_wtf.kicad_pcb:
 *   D5           LED data  -> 74AHCT125 -> 470R -> SK6812 chain (33 LEDs)
 *   D4/D6/D9/D12 mux select S0/S1/S2/S3 (shared by all three CD74HC4067)
 *   A0           fader mux COM   (U1, 16 slide pots)
 *   SDA          button mux COM  (U2, BTN0-15)
 *   SCL          button mux COM  (U3, BTN16-31)
 *   D13          BTN33 direct (shares pin with onboard LED — see test 7)
 *   A2/A3/A4     encoder A / B / switch
 *
 * Buttons are active LOW (switch to GND, 10k pull-up via RN1-4).
 * Do NOT call Wire.begin() — SDA/SCL are plain GPIO on this board.
 *
 * Send a digit over serial at 115200 to run a test. '?' reprints the menu.
 */


// ---------------------------------------------------------------- pin map
constexpr uint8_t PIN_S0 = 4, PIN_S1 = 6, PIN_S2 = 9, PIN_S3 = 12;
constexpr uint8_t PIN_FADER = A0;
constexpr uint8_t PIN_BTN_A = SDA;   // BTN0-15
constexpr uint8_t PIN_BTN_B = SCL;   // BTN16-31
constexpr uint8_t PIN_BTN_33 = 13;   // direct; A5 is unconnected on this PCB
constexpr uint8_t PIN_LED_DATA = 5;
constexpr uint8_t PIN_ENC_A = A2, PIN_ENC_B = A3, PIN_ENC_SW = A4;

constexpr uint8_t NUM_LEDS = 33;
constexpr uint8_t NUM_FADERS = 16;
constexpr uint8_t NUM_BTNS = 33;

// Keep this low. 33 SK6812 at full white is ~2A; bring-up runs off whatever
// supply is on the bench. 40/255 is bright enough to see, gentle on the rail.
constexpr uint8_t LED_BRIGHTNESS = 40;

Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------- state
uint16_t faderRaw[NUM_FADERS];
bool     btnState[NUM_BTNS];
volatile int32_t encPos = 0;
volatile uint8_t encPrev = 0;

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
  btnState[32] = (digitalRead(PIN_BTN_33) == LOW);
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
    else if (lo[i] > 4088)  Serial.print(F("   <-- stuck at 3V3?"));
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
  Serial.println(F("--- done ---"));
}

// 1 — walk one LED at a time; confirms count, order, and every solder joint
void testLedWalk() {
  Serial.println(F("\nWalking LEDs 0..32 — watch for skips or wrong order."));
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    strip.clear();
    strip.setPixelColor(i, strip.Color(255, 255, 255));
    strip.show();
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
    strip.fill(cols[c]); strip.show();
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
    strip.show();
    delay(5);
  }
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear(); strip.show();
  while (Serial.available()) Serial.read();
}

// 7 — D13 sanity: that pin shares the onboard LED, so verify it reads cleanly
void testD13() {
  Serial.println(F("\nD13 / BTN33 check — leave it untouched for 2s, then hold it."));
  delay(300);
  uint16_t highCount = 0;
  for (int i = 0; i < 200; i++) { if (digitalRead(PIN_BTN_33) == HIGH) highCount++; delay(10); }
  Serial.print(F("  idle read HIGH on ")); Serial.print(highCount);
  Serial.println(F("/200 samples"));
  if (highCount < 190)
    Serial.println(F("  -> D13 is NOT idling high. The onboard LED is dragging it down;\n"
                     "     add an external 10k pull-up to 3V3 on BTN33."));
  else
    Serial.println(F("  -> idles high correctly."));
  Serial.println(F("  Now hold BTN33 for 3s..."));
  delay(500);
  uint16_t lowCount = 0;
  for (int i = 0; i < 300; i++) { if (digitalRead(PIN_BTN_33) == LOW) lowCount++; delay(10); }
  Serial.print(F("  read LOW on ")); Serial.print(lowCount); Serial.println(F("/300 samples"));
}

// 8 — one channel at a time, averaged, for comparing against a DMM
void testFaderFocus() {
  uint8_t ch = 1;   // not 0: RV1's wiper is unconnected on this revision
  Serial.println(F("\nSingle-channel fader probe."));
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
        ch = sel; lo = 0xFFFF; hi = 0; lastShown = -9999;
        Serial.print(F("\n-- F")); Serial.println(ch);
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
      Serial.print(F("%FS   wiper is "));
      Serial.print(raw * 3.30f / 4095.0f, 3);
      Serial.print(F("V if ref=3.30 | "));
      Serial.print(raw * 1.65f / 4095.0f, 3);
      Serial.print(F("V if ref=1.65   span "));
      Serial.print(lo); Serial.print('-'); Serial.println(hi);
    }
    delay(40);
  }
  while (Serial.available()) Serial.read();
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
  Serial.println(F("  7  D13 / BTN33 pull-up check"));
  Serial.println(F("  8  single fader probe (for DMM comparison)"));
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
  pinMode(PIN_BTN_33, INPUT_PULLUP);

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  encPrev = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  analogReadResolution(12);

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();

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
    case '7': testD13();         break;
    case '8': testFaderFocus();  break;
    case '?': printMenu();       break;
    default: return;
  }
  Serial.println(F("\n(send ? for menu)"));
}
