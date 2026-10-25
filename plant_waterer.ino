// Automatic plant waterer. Each plant has its own moisture sensor and pump.
// When a plant's soil gets too dry, its pump runs in short bursts, with a
// soak after each, until the soil is moist again. Safety checks pause or
// stop a plant's watering if something looks wrong. A small OLED screen
// shows what every plant is doing. It sleeps after a minute and wakes
// with a button press, or by itself when a plant has a problem. A watchdog
// restarts the board if it ever freezes.

#include <U8x8lib.h>  // OLED screen library: install "U8g2" from the Library Manager
#include <Wire.h>     // I2C, the 2-wire connection the screen uses
#include <avr/wdt.h>  // watchdog timer: restarts the board if it freezes

// ---- Settings: edit these before uploading ----

// Wiring: which Arduino pins each plant's sensor and pump are connected to.
const uint8_t NUM_PLANTS = 3;
const uint8_t SENSOR_PINS[NUM_PLANTS] = {A0, A1, A2};
const uint8_t PUMP_PINS[NUM_PLANTS]   = {2, 3, 4};

// Push button that wakes the screen: one leg to this pin, the other to
// GND. No resistor needed, because the pin uses the board's built-in
// pull-up resistor (it reads HIGH normally and LOW while pressed).
const uint8_t BUTTON_PIN = 7;

// The OLED screen: 128x64 pixels with the SSD1306 chip, wired over I2C
// (SDA to A4, SCL to A5). U8X8_PIN_NONE means its reset pin isn't used.
// For a different screen, such as a 1.3" one with the SH1106 chip, swap
// this line for the matching one from the U8g2 library's list of displays.
U8X8_SSD1306_128X64_NONAME_HW_I2C screen(U8X8_PIN_NONE);

// Each sensor's raw reading when completely dry and completely wet, from
// calibrating it. Higher readings mean drier soil.
const int DRY_RAW[NUM_PLANTS] = {640, 639, 637};
const int WET_RAW[NUM_PLANTS] = {284, 287, 291};

// A reading more than SENSOR_MARGIN past either calibration value can't
// come from real soil, so it counts as a sensor fault (usually a loose or
// broken wire). That plant's watering pauses until it reads normally again.
const int SENSOR_MARGIN = 50;

// Start watering when moisture drops below START_PCT, and keep going
// until it reaches STOP_PCT. The gap stops a reading that hovers near
// one number from resetting the burst count and dodging the lockout.
const int START_PCT[NUM_PLANTS] = {35, 35, 35};
const int STOP_PCT[NUM_PLANTS]  = {45, 45, 45};

// How long each pump runs per burst, and about how much water that gives
// (shown on the screen). If you change PUMP_MS, measure the water again
// and update ML_PER_BURST to match.
const unsigned long PUMP_MS[NUM_PLANTS] = {45000UL, 45000UL, 45000UL};
const int ML_PER_BURST[NUM_PLANTS]      = {50, 50, 50};

// Timing. All times are in milliseconds.
const unsigned long SOAK_MS      = 10UL * 60000UL;    // 10 minutes for water to soak in
const unsigned long PRINT_MS     = 5000UL;            // status line every 5 seconds
const unsigned long DISPLAY_MS   = 1000UL;            // screen refresh every second
const unsigned long SCREEN_ON_MS = 60000UL;           // screen sleeps 1 minute after a press
const unsigned long DAY_MS       = 24UL * 3600000UL;  // 24 hours, for DAILY_CAP

// Safety lockouts. A locked-out plant gets no more water until the board
// is reset (reset button or unplugging it).
const uint8_t MAX_BURSTS = 4;  // bursts in a row that don't reach STOP_PCT
const uint8_t DAILY_CAP  = 8;  // bursts per plant in any 24 hours

// No watering for STARTUP_MS after power-on or any restart. Lockouts and
// the daily cap are forgotten when the board restarts, so if something
// keeps restarting it (like a power dip each time a pump starts), this
// stops the pumps from running again straight away every time.
const unsigned long STARTUP_MS = 5UL * 60000UL;  // 5 minutes

// ---- Runtime state: changes while the program runs ----

// What each plant is doing right now. A plant goes IDLE -> PUMPING ->
// SOAKING, then back to IDLE, into another burst, or to LOCKED_OUT.
enum State { IDLE, PUMPING, SOAKING, LOCKED_OUT };
State state[NUM_PLANTS];               // globals start at 0, so every plant starts IDLE
unsigned long stateStart[NUM_PLANTS];  // millis() when the current state began
uint8_t burstCount[NUM_PLANTS];        // bursts so far in this round of watering
bool sensorFault[NUM_PLANTS];          // true while a sensor reads out of range
unsigned long lastPrint = 0;           // millis() of the last status line
unsigned long lastDisplay = 0;         // millis() of the last screen refresh
unsigned long lastWake = 0;            // millis() of the last button press or problem
bool screenOn = true;                  // screen.begin() turns the screen on
bool startupDone = false;              // true once STARTUP_MS has passed

// Short reason shown on the screen while a plant is locked out. Each
// entry points to a piece of text stored in flash (see lockOut).
const __FlashStringHelper* lockHint[NUM_PLANTS];

// Start times of each plant's bursts in the last 24 hours, for DAILY_CAP.
// It's a ring buffer: each plant's DAILY_CAP slots are reused in a circle.
unsigned long recentBursts[NUM_PLANTS][DAILY_CAP];
uint8_t recentHead[NUM_PLANTS];   // slot holding the oldest time
uint8_t recentCount[NUM_PLANTS];  // how many slots are in use

// ---- Helper functions ----

// Turn plant i's pump on or off.
void setPump(uint8_t i, bool on) {
  digitalWrite(PUMP_PINS[i], on ? HIGH : LOW);
}

// Print "Plant N: " to start a message. Plants are numbered from 1.
void printPlant(uint8_t i) {
  Serial.print(F("Plant ")); Serial.print(i + 1); Serial.print(F(": "));
}

// Stop watering plant i until the board is reset. `hint` is a short
// reason for the screen; `why` is the full message for the serial log.
void lockOut(uint8_t i, const __FlashStringHelper* hint, const __FlashStringHelper* why) {
  state[i] = LOCKED_OUT;
  lockHint[i] = hint;
  printPlant(i);
  Serial.print(F("LOCKED OUT - ")); Serial.println(why);
}

// Read plant i's sensor, averaging several readings to smooth out noise.
int readRaw(uint8_t i) {
  const uint8_t SAMPLES = 10;
  long sum = 0;
  for (uint8_t s = 0; s < SAMPLES; s++) {
    sum += analogRead(SENSOR_PINS[i]);
    delay(2);  // short gap between readings
  }
  return sum / SAMPLES;
}

// Convert a raw reading to 0-100% moisture using that sensor's calibration.
int toPercent(uint8_t i, int raw) {
  int pct = map(raw, DRY_RAW[i], WET_RAW[i], 0, 100);
  return constrain(pct, 0, 100);
}

// Decide whether plant i's reading can be trusted, and print a message
// when its sensor stops or starts reading normally.
void checkSensor(uint8_t i, int raw) {
  bool bad = raw > DRY_RAW[i] + SENSOR_MARGIN || raw < WET_RAW[i] - SENSOR_MARGIN;
  if (bad == sensorFault[i]) return;  // no change since last pass
  sensorFault[i] = bad;
  printPlant(i);
  if (bad) Serial.println(F("SENSOR FAULT - reading out of range, check its wiring"));
  else     Serial.println(F("sensor reading back to normal"));
}

// Forget bursts more than a day old. Runs every pass, so no saved time
// gets old enough to be confused when millis() rolls over (~49 days).
void forgetOldBursts(uint8_t i, unsigned long now) {
  while (recentCount[i] > 0 && now - recentBursts[i][recentHead[i]] >= DAY_MS) {
    recentHead[i] = (recentHead[i] + 1) % DAILY_CAP;
    recentCount[i]--;
  }
}

// Start one burst of watering, unless plant i has hit DAILY_CAP.
void startBurst(uint8_t i, unsigned long now) {
  if (recentCount[i] >= DAILY_CAP) {
    // Wants more water than any normal day needs: water running
    // through the pot or a bad sensor reading. Stop until reset.
    lockOut(i, F("leak?"), F("daily cap hit - check for leaks/sensor, then reset"));
    return;
  }
  recentBursts[i][(recentHead[i] + recentCount[i]) % DAILY_CAP] = now;
  recentCount[i]++;

  setPump(i, true);
  state[i] = PUMPING;
  stateStart[i] = now;
  burstCount[i]++;
  printPlant(i);
  Serial.print(F("watering, burst ")); Serial.println(burstCount[i]);
}

// ---- Screen ----
// In text mode the screen fits 16 characters x 8 rows. Each plant gets
// two rows, so there's room for 4 plants:
//   PLANT 1      42%
//    waiting  150mL     <- water given in the last 24 hours
// To save the screen from burn-in, it sleeps SCREEN_ON_MS after the last
// button press, but stays on while any plant has a problem.

// True if any plant is locked out or has a faulty sensor.
bool anyProblem() {
  for (uint8_t i = 0; i < NUM_PLANTS; i++) {
    if (state[i] == LOCKED_OUT || sensorFault[i]) return true;
  }
  return false;
}

// How much of a wait of `total` ms is left, counting from when plant i
// entered its current state.
unsigned long timeLeft(uint8_t i, unsigned long total, unsigned long now) {
  unsigned long elapsed = now - stateStart[i];
  return elapsed < total ? total - elapsed : 0;
}

// Print a time as minutes:seconds, such as 7:05. Returns how many
// characters it printed.
uint8_t printMinSec(unsigned long ms) {
  unsigned long secs = ms / 1000;
  uint8_t n = screen.print(secs / 60);
  n += screen.print(':');
  if (secs % 60 < 10) n += screen.print('0');
  n += screen.print(secs % 60);
  return n;
}

// Draw plant i's two rows. Every row is filled out to all 16 columns, so
// nothing is left over from a longer message drawn there before.
void drawPlant(uint8_t i, int pct, unsigned long now) {
  uint8_t row = i * 2;

  // Row 1: the plant number, then moisture in the last 4 columns.
  screen.setCursor(0, row);
  screen.print(F("PLANT ")); screen.print(i + 1); screen.print(F("     "));
  if (sensorFault[i]) {
    screen.print(F("  --"));
  } else {
    if (pct < 100) screen.print(' ');  // line the numbers up on the right
    if (pct < 10)  screen.print(' ');
    screen.print(pct); screen.print('%');
  }

  // Row 2: what the plant is doing. Problems are shown first.
  screen.setCursor(0, row + 1);
  uint8_t n;  // characters printed so far on this row
  if (state[i] == LOCKED_OUT) {
    n = screen.print(F(" LOCKED: "));
    n += screen.print(lockHint[i]);
  } else if (sensorFault[i]) {
    n = screen.print(F(" SENSOR FAULT!"));
  } else if (state[i] == PUMPING) {
    n = screen.print(F(" PUMPING  "));
    n += printMinSec(timeLeft(i, PUMP_MS[i], now));
  } else if (state[i] == SOAKING) {
    n = screen.print(F(" soaking  "));
    n += printMinSec(timeLeft(i, SOAK_MS, now));
  } else if (!startupDone) {
    n = screen.print(F(" starting "));
    n += printMinSec(STARTUP_MS - now);
  } else {
    n = screen.print(F(" waiting  "));
    n += screen.print(recentCount[i] * ML_PER_BURST[i]);
    n += screen.print(F("mL"));
  }
  while (n < 16) n += screen.print(' ');  // blank the rest of the row
}

// ---- Main program ----

// Runs once, at power-on or reset.
void setup() {
  Serial.begin(9600);
  for (uint8_t i = 0; i < NUM_PLANTS; i++) {
    setPump(i, false);  // write "off" first so the pump can't blip on
    pinMode(PUMP_PINS[i], OUTPUT);
  }
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  screen.begin();
  screen.setFont(u8x8_font_chroma48medium8_r);
  screen.setCursor(0, 0);
  screen.print(F("Starting"));

  Serial.print(F("Starting - no watering for the first "));
  Serial.print(STARTUP_MS / 60000); Serial.println(F(" minutes"));

  // Turn on the watchdog. If loop() ever stops running for 4 seconds
  // (the board froze), the watchdog restarts the board, and the restart
  // switches every pump off. loop() calls wdt_reset() to show it's alive.
  wdt_enable(WDTO_4S);
}

// Runs over and over, forever. Each pass is quick, so every plant gets
// checked many times a second and no plant has to wait for another.
void loop() {
  wdt_reset();  // tell the watchdog the program is still running
  unsigned long now = millis();

  // Watering stays off until STARTUP_MS after power-on. (Once this flips
  // to true it stays true, so millis() rolling over can't restart it.)
  if (!startupDone && now >= STARTUP_MS) {
    startupDone = true;
    Serial.println(F("Startup delay over - watering enabled"));
  }

  // 1. Read each sensor once and check it. The state machine, status
  //    line and screen below all use these readings.
  int raw[NUM_PLANTS], pct[NUM_PLANTS];
  for (uint8_t i = 0; i < NUM_PLANTS; i++) {
    raw[i] = readRaw(i);
    pct[i] = toPercent(i, raw[i]);
    checkSensor(i, raw[i]);
  }

  // 2. Move each plant along through its states.
  for (uint8_t i = 0; i < NUM_PLANTS; i++) {
    forgetOldBursts(i, now);

    switch (state[i]) {
      case IDLE:
        // Waiting for the soil to dry out (and for the startup delay
        // to end and the sensor to be working).
        if (startupDone && !sensorFault[i] && pct[i] < START_PCT[i]) startBurst(i, now);
        break;

      case PUMPING:
        // Pump runs for PUMP_MS, then the water soaks in.
        if (now - stateStart[i] >= PUMP_MS[i]) {
          setPump(i, false);
          state[i] = SOAKING;
          stateStart[i] = now;
          printPlant(i);
          Serial.println(F("pump off, soaking"));
        }
        break;

      case SOAKING:
        // After SOAK_MS: wet enough, give up, or water again. With a
        // faulty sensor it waits here, so a bad reading decides nothing.
        if (now - stateStart[i] >= SOAK_MS && !sensorFault[i]) {
          if (pct[i] >= STOP_PCT[i]) {
            burstCount[i] = 0;
            state[i] = IDLE;
          } else if (burstCount[i] >= MAX_BURSTS) {
            // Still dry after several bursts: reservoir empty,
            // tube off, or sensor out of the soil. Stop until reset.
            lockOut(i, F("refill?"), F("still dry - check reservoir/sensor, then reset"));
          } else {
            startBurst(i, now);
          }
        }
        break;

      case LOCKED_OUT:
        // Nothing to do until the board is reset.
        break;
    }
  }

  // 3. Print a status line every PRINT_MS.
  if (now - lastPrint >= PRINT_MS) {
    lastPrint = now;
    for (uint8_t i = 0; i < NUM_PLANTS; i++) {
      Serial.print(F("P")); Serial.print(i + 1);
      Serial.print(F(" raw=")); Serial.print(raw[i]);
      if (sensorFault[i]) {
        Serial.print(F(" FAULT  "));
      } else {
        Serial.print(F(" ")); Serial.print(pct[i]); Serial.print(F("%  "));
      }
    }
    Serial.println();
  }

  // 4. Wake the screen on a button press or a problem, and let it sleep
  //    once SCREEN_ON_MS passes with neither. Button bounce doesn't
  //    matter here: extra presses just restart the same timer.
  bool woke = false;
  if (digitalRead(BUTTON_PIN) == LOW || anyProblem()) {
    lastWake = now;
    if (!screenOn) {
      screenOn = true;
      woke = true;
      screen.setPowerSave(0);  // 0 = screen on
    }
  } else if (screenOn && now - lastWake >= SCREEN_ON_MS) {
    screenOn = false;
    screen.setPowerSave(1);    // 1 = screen off (what it shows is kept)
  }

  // 5. While the screen is on, redraw it every DISPLAY_MS, and right
  //    away when it wakes so it never shows old information.
  if (screenOn && (woke || now - lastDisplay >= DISPLAY_MS)) {
    lastDisplay = now;
    for (uint8_t i = 0; i < NUM_PLANTS; i++) drawPlant(i, pct[i], now);
  }
}
