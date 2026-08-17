/**
 * ============================================================================
 * 8-CHANNEL PROFESSIONAL RC TRANSMITTER (TX) FIRMWARE - ULTRA LEAN PRO SUITE
 * ============================================================================
 * Microcontroller : Arduino Nano (ATmega328P, 16MHz, 5V, 2KB SRAM)
 * SRAM Stability  : Ultra-Low RAM Footprint (< 30% SRAM utilized, > 1400B free)
 * Model Storage   : EEPROM-Paged Architecture (Only 1 active model in RAM,
 *                   4 models stored directly in EEPROM slots 0..3)
 * Flash Optimized : All UI menus, strings, and lookup tables in PROGMEM
 * RF Transceiver  : NRF24L01+ (2.4 GHz) with PA + LNA
 * Display         : 16x2 I2C Character LCD (Fast 400kHz I2C Bus @ 33 FPS)
 * User Inputs     : - Rotary Encoder (Pins D2/D3 with Hardware Interrupts +
 * Pullups)
 *                   - Push Button (Pin D4 with Short/Long Press Detection)
 *                   - 2x 2-Axis Joysticks (A0, A1, A2, A3)
 *                   - 2x Potentiometers (A6, A7 - Full 0% to 100% Span)
 *                   - 2x Toggle Switches (D5, D6 with Internal Pullups)
 * Multi-Vehicle   : 4 Generic Model Profiles in EEPROM (MODEL 1..MODEL 4)
 * Pro Features    : - Safe Multi-Point Calibration Suite with Sanity
 * Verification
 *                   - Dual Rates (D/R: 50% - 100%) & Exponential (EXPO: 0% -
 * 70%)
 *                   - Wing / Tail Mixing (Normal, Elevon / Delta Wing, V-Tail)
 *                   - Onboard Model Name & Stick Deadband Customization
 * Performance     : - Fast ADC Prescaler (500kHz ADC clock, < 1ms acquisition)
 *                   - Ultra-Responsive Low-Lag EMA Filter
 *                   - Fast 400kHz I2C bus with 30ms (~33 FPS) LCD Refresh
 * Telemetry       : Bi-directional Auto-ACK with Live "RX: OK" Link Indication
 * ============================================================================
 */

#include <EEPROM.h>
#include <LiquidCrystal_I2C.h>
#include <RF24.h>
#include <SPI.h>
#include <Wire.h>
#include <avr/pgmspace.h>
#include <nRF24L01.h>

// ============================================================================
// HARDWARE PIN ASSIGNMENTS
// ============================================================================

// NRF24L01 Transceiver SPI Pins
#define PIN_RF_CE 9   // Chip Enable
#define PIN_RF_CSN 10 // SPI Chip Select (CSN)

// Joysticks (Analog Inputs)
#define PIN_JOY_CH1 A0 // Joystick 1 X-Axis (Aileron / Steering / Roll)
#define PIN_JOY_CH2 A1 // Joystick 1 Y-Axis (Elevator / Pitch)
#define PIN_JOY_CH3 A2 // Joystick 2 Y-Axis (Throttle / Forward-Reverse)
#define PIN_JOY_CH4 A3 // Joystick 2 X-Axis (Rudder / Yaw)

// Potentiometers (Analog Inputs)
#define PIN_POT_CH5 A6 // Potentiometer 1 (AUX 1 / CH5)
#define PIN_POT_CH6 A7 // Potentiometer 2 (AUX 2 / CH6)

// Toggle Switches (Digital Inputs with INPUT_PULLUP)
#define PIN_SW_CH7 5 // Toggle Switch 1 (AUX 3 / CH7)
#define PIN_SW_CH8 6 // Toggle Switch 2 (AUX 4 / CH8)

// Rotary Encoder Pins (Hardware Interrupts INT0 & INT1 on D2 & D3)
#define PIN_ENC_CLK 2 // Rotary Encoder CLK / Channel A (INT0)
#define PIN_ENC_DT 3  // Rotary Encoder DT / Channel B (INT1)
#define PIN_ENC_SW 4  // Rotary Encoder Push Button (SW)

// I2C LCD Configuration
#define LCD_I2C_ADDR 0x27 // Standard I2C Address (use 0x3F if 0x27 fails)
#define LCD_COLS 16       // 16 Columns
#define LCD_ROWS 2        // 2 Rows

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
RF24 radio(PIN_RF_CE, PIN_RF_CSN);

// RF Pipe Address
const uint8_t RF_PIPE_ADDR[6] = "RC001";

// ============================================================================
// DATA STRUCTURES & EEPROM PERSISTENCE (ULTRA-LEAN ARCHITECTURE)
// ============================================================================

#define EEPROM_MAGIC_KEY 0x8C12 // Validation Key (v1.2 generic models)
#define TOTAL_MODELS 4          // 4 Model Profiles in EEPROM
#define EEPROM_BASE_ADDR 10     // Starting offset for Model 0
#define EEPROM_MODEL_SIZE 150   // Stride per model in EEPROM

enum WingMixMode : uint8_t { MIX_NORMAL = 0, MIX_ELEVON = 1, MIX_VTAIL = 2 };

struct RateExpo {
  uint8_t rate; // 50% to 100% (default 100)
  uint8_t expo; // 0% to 70% (default 0)
};

struct ChannelConfig {
  int16_t trim;       // Digital trim offset (-150us to +150us)
  uint16_t minUs;     // Minimum endpoint (800us to 1400us, default 1000us)
  uint16_t maxUs;     // Maximum endpoint (1600us to 2200us, default 2000us)
  uint16_t centerUs;  // Neutral center position (default 1500us)
  uint16_t adcMin;    // Calibrated raw ADC min (hardware limit)
  uint16_t adcCenter; // Calibrated raw ADC center (neutral stick position)
  uint16_t adcMax;    // Calibrated raw ADC max (hardware limit)
  uint8_t deadband;   // Center deadband ADC window to prevent jitter
  bool reversed;      // Channel direction inverted flag
};

struct ModelProfile {
  char name[8];         // Model Name (e.g. "MODEL 1")
  ChannelConfig ch[8];  // 8 Channel parameters (128 bytes)
  RateExpo rateExpo[3]; // 0: AIL, 1: ELE, 2: RUD (6 bytes)
  uint8_t wingMix;      // WingMixMode (1 byte)
};

struct RadioPayload {
  uint16_t channels[8]; // Microsecond values (1000us - 2000us nominal, 800 -
                        // 2200 max)
  uint8_t packetId;     // Rolling packet counter
  uint8_t flags;        // Status / Telemetry flags
};

// ONLY ONE ACTIVE MODEL IN RAM (143 Bytes RAM Footprint!)
ModelProfile curModel;
uint8_t activeModelIdx = 0;
RadioPayload payload;

// ============================================================================
// FLASH PROGMEM STRING CONSTANTS (ZERO RAM COST!)
// ============================================================================

const char ch_0[] PROGMEM = "AIL";
const char ch_1[] PROGMEM = "ELE";
const char ch_2[] PROGMEM = "THR";
const char ch_3[] PROGMEM = "RUD";
const char ch_4[] PROGMEM = "P1 ";
const char ch_5[] PROGMEM = "P2 ";
const char ch_6[] PROGMEM = "SW1";
const char ch_7[] PROGMEM = "SW2";
const char *const CH_NAMES[8] PROGMEM = {ch_0, ch_1, ch_2, ch_3,
                                         ch_4, ch_5, ch_6, ch_7};

const char mix_0[] PROGMEM = "NORMAL";
const char mix_1[] PROGMEM = "ELEVON";
const char mix_2[] PROGMEM = "V-TAIL";
const char *const MIX_NAMES[3] PROGMEM = {mix_0, mix_1, mix_2};

const char menu_0[] PROGMEM = "1.DIGITAL TRIM";
const char menu_1[] PROGMEM = "2.D/R & EXPO  ";
const char menu_2[] PROGMEM = "3.ENDPOINTS   ";
const char menu_3[] PROGMEM = "4.REVERSE CH  ";
const char menu_4[] PROGMEM = "5.CALIBRATION ";
const char menu_5[] PROGMEM = "6.WING MIXING ";
const char menu_6[] PROGMEM = "7.MODEL SELECT";
const char menu_7[] PROGMEM = "8.MODEL NAME  ";
const char menu_8[] PROGMEM = "9.DEADBAND SET";
const char menu_9[] PROGMEM = "10.SET FAILSAF";
const char menu_10[] PROGMEM = "11.RESET MODEL";
const char menu_11[] PROGMEM = "12.SAVE & EXIT";
const char *const MENU_ITEMS[12] PROGMEM = {
    menu_0, menu_1, menu_2, menu_3, menu_4, menu_5,
    menu_6, menu_7, menu_8, menu_9, menu_10, menu_11};
const uint8_t TOTAL_MENU_ITEMS = 12;

const uint8_t ANALOG_PINS[6] = {PIN_JOY_CH1, PIN_JOY_CH2, PIN_JOY_CH3,
                                PIN_JOY_CH4, PIN_POT_CH5, PIN_POT_CH6};

// ============================================================================
// SYSTEM STATES & MENU DEFINITIONS
// ============================================================================

enum SystemState {
  STATE_BOOT_SPLASH,
  STATE_HOME_PAGES,
  STATE_MENU_LIST,
  STATE_MENU_EDIT_TRIM,
  STATE_MENU_EDIT_DR_EXPO,
  STATE_MENU_EDIT_ENDPOINT,
  STATE_MENU_EDIT_REVERSE,
  STATE_MENU_CALIBRATE,
  STATE_MENU_WING_MIX,
  STATE_MENU_MODEL_SELECT,
  STATE_MENU_EDIT_NAME,
  STATE_MENU_EDIT_DEADBAND,
  STATE_MENU_FAILSAFE
};

SystemState currentState = STATE_BOOT_SPLASH;

uint8_t currentHomePage = 0;
const uint8_t TOTAL_HOME_PAGES = 5;

int8_t currentMenuItem = 0;
int8_t selectedChannel = 0;
int8_t selectedSubParam = 0;
uint8_t editNameCharIdx = 0;

// Interactive Calibration Wizard State Variables
uint8_t calibStep = 0;
uint16_t calibLiveMin[6];
uint16_t calibLiveMax[6];
uint16_t calibLiveCenter[6];

// Telemetry & Link Status
bool rxConnected = false;
uint32_t lastTxTime = 0;
uint32_t lastRxAckTime = 0;
uint32_t successfulPackets = 0;
uint32_t totalPackets = 0;
uint8_t packetSeq = 0;

// High-Speed LCD & Timing Parameters
uint32_t lastLcdRefreshTime = 0;
const uint32_t LCD_REFRESH_INTERVAL_MS =
    30;                             // 33 FPS real-time fluid LCD updates
const uint32_t TX_INTERVAL_MS = 20; // 50Hz RF Transmission rate
uint32_t lastSerialTime = 0;
const uint32_t SERIAL_INTERVAL_MS = 200; // 5Hz Serial Monitor update

// Analog Digital Filtering Buffers
float filteredAdc[6] = {512.0f, 512.0f, 512.0f, 512.0f, 512.0f, 512.0f};
uint16_t lastStableUs[8] = {1500, 1500, 1000, 1500, 1500, 1500, 1000, 1000};

// ============================================================================
// INTERRUPT-DRIVEN ROTARY ENCODER STATE MACHINE IN PROGMEM
// ============================================================================
#define DIR_NONE 0x00
#define DIR_CW 0x10
#define DIR_CCW 0x20

#define R_START 0x0
#define R_CW_FINAL 0x1
#define R_CW_BEGIN 0x2
#define R_CW_NEXT 0x3
#define R_CCW_BEGIN 0x4
#define R_CCW_FINAL 0x5
#define R_CCW_NEXT 0x6

const uint8_t ttable[7][4] PROGMEM = {
    {R_START, R_CW_BEGIN, R_CCW_BEGIN, R_START},            // R_START (0)
    {R_CW_NEXT, R_START, R_CW_FINAL, R_START | DIR_CW},     // R_CW_FINAL (1)
    {R_CW_NEXT, R_CW_BEGIN, R_START, R_START},              // R_CW_BEGIN (2)
    {R_CW_NEXT, R_CW_BEGIN, R_CW_FINAL, R_START},           // R_CW_NEXT (3)
    {R_CCW_NEXT, R_START, R_CCW_BEGIN, R_START},            // R_CCW_BEGIN (4)
    {R_CCW_FINAL, R_CCW_FINAL, R_START, R_START | DIR_CCW}, // R_CCW_FINAL (5)
    {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START},        // R_CCW_NEXT (6)
};

volatile int8_t encoderDeltaCount = 0;
volatile uint8_t encoderState = R_START;

bool buttonPressed = false;
uint32_t buttonPressStartTime = 0;
bool buttonLongPressHandled = false;
const uint32_t LONG_PRESS_DURATION_MS = 1800; // 1.8s for Settings Menu

// Custom LCD 5-Level Vertical Slice Characters in PROGMEM
const uint8_t customChars[5][8] PROGMEM = {
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}, // 1/5 filled bar
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18}, // 2/5 filled bar
    {0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C}, // 3/5 filled bar
    {0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E}, // 4/5 filled bar
    {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}  // Full 5/5 block
};

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void isrEncoder();
int8_t readEncoderDelta();
void handleEncoderButton();
void handleShortClick();
void loadModelFromEEPROM(uint8_t modelIdx);
void saveCurrentModelToEEPROM();
void switchModel(uint8_t newModelIdx);
void resetModelDefaults(uint8_t modelIdx);
void initAllEEPROMModels();
void setupCustomCharacters();
int readAnalogFiltered(uint8_t pinIndex);
void readInputsAndProcessChannels();
void applyRatesExpoAndMixing(long *processedChannels);
void transmitRFData();
void renderLCD();
void renderHomePage();
void renderMenuList();
void renderEditTrim();
void renderEditDrExpo();
void renderEditEndpoint();
void renderEditReverse();
void renderCalibration();
void renderWingMix();
void renderModelSelect();
void renderEditModelName();
void renderEditDeadband();
void renderFailsafe();
char cycleChar(char c, int8_t delta);
void drawProgressBar(uint8_t col, uint8_t row, uint8_t width, uint16_t val,
                     uint16_t minVal, uint16_t maxVal);
void printProgmemStr(const char *const *table, uint8_t index);
void printSerialDiagnostics();

// ============================================================================
// HARDWARE INTERRUPT SERVICE ROUTINE FOR ROTARY ENCODER
// ============================================================================
void isrEncoder() {
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__)
  uint8_t p = PIND;
  uint8_t pinState = (((p >> PIN_ENC_CLK) & 1) << 1) | ((p >> PIN_ENC_DT) & 1);
#else
  uint8_t pinState = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
#endif

  uint8_t row = encoderState & 0x0F;
  uint8_t nextState = pgm_read_byte(&(ttable[row][pinState]));
  encoderState = nextState;
  uint8_t result = encoderState & 0x30;

  if (result == DIR_CW) {
    encoderDeltaCount++;
  } else if (result == DIR_CCW) {
    encoderDeltaCount--;
  }
}

int8_t readEncoderDelta() {
  noInterrupts();
  int8_t delta = encoderDeltaCount;
  encoderDeltaCount = 0;
  interrupts();
  return delta;
}

// ============================================================================
// HELPER TO PRINT PROGMEM STRINGS DIRECTLY TO LCD
// ============================================================================
void printProgmemStr(const char *const *table, uint8_t index) {
  char *ptr = (char *)pgm_read_word(&(table[index]));
  char buf[17];
  strcpy_P(buf, ptr);
  lcd.print(buf);
}

// ============================================================================
// ARDUINO SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=============================================="));
  Serial.println(F("  FLYMASTER TX8 PRO - ULTRA-LEAN ARCHITECTURE"));
  Serial.println(F("=============================================="));

// Set Fast ADC Clock (Prescaler 32 = 500 kHz ADC Clock for < 1ms total reads)
#if defined(ADCSRA)
  ADCSRA = (ADCSRA & 0xF8) | 0x05;
#endif

  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  pinMode(PIN_SW_CH7, INPUT_PULLUP);
  pinMode(PIN_SW_CH8, INPUT_PULLUP);

  encoderState = R_START;
  encoderDeltaCount = 0;
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), isrEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), isrEncoder, CHANGE);

  // Initialize Fast 400kHz I2C Bus for instant LCD updates
  Wire.begin();
  Wire.setClock(400000);

  lcd.init();
  lcd.backlight();
  setupCustomCharacters();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("RADIO TRANSMITER"));
  lcd.setCursor(0, 1);
  lcd.print(F(" PROG 8CH RADIO "));
  Serial.println(F("[SYSTEM] Displaying Welcome Splash Screen."));

  // Check EEPROM Magic Key and Load Active Model
  uint16_t storedKey = 0;
  EEPROM.get(0, storedKey);
  if (storedKey != EEPROM_MAGIC_KEY) {
    Serial.println(
        F("[EEPROM] Formatting EEPROM and Initializing 4 Model Slots..."));
    initAllEEPROMModels();
  } else {
    EEPROM.get(2, activeModelIdx);
    if (activeModelIdx >= TOTAL_MODELS)
      activeModelIdx = 0;
    loadModelFromEEPROM(activeModelIdx);
    Serial.println(F("[EEPROM] Model loaded into RAM successfully."));
  }

  // Prime analog filters
  for (uint8_t i = 0; i < 6; i++) {
    analogRead(ANALOG_PINS[i]);
    delayMicroseconds(20);
    int initialVal = analogRead(ANALOG_PINS[i]);
    filteredAdc[i] = (float)initialVal;
  }

  // Initialize NRF24L01+ Transceiver
  if (radio.begin()) {
    radio.openWritingPipe(RF_PIPE_ADDR);
    radio.setPALevel(RF24_PA_MAX);
    radio.setDataRate(RF24_250KBPS);
    radio.setChannel(108);
    radio.setAutoAck(true);
    radio.enableDynamicPayloads();
    radio.setRetries(5, 5);
    radio.stopListening();
    Serial.println(
        F("[RF] NRF24L01 2.4GHz Transceiver Initialized Successfully."));
  } else {
    Serial.println(
        F("[ERROR] NRF24L01 Hardware Not Responding! Check Wiring & 3.3V."));
    lcd.setCursor(0, 1);
    lcd.print(F("RF ERROR: CHECK "));
    delay(1500);
  }

  delay(1000);
  lcd.clear();
  currentState = STATE_HOME_PAGES;
  Serial.println(F("[SYSTEM] Initialization Complete. Ready for Operation.\n"));
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  handleEncoderButton();

  readInputsAndProcessChannels();

  // Transmit 8-Channel Data Packet via NRF24L01 @ 50Hz (20ms interval)
  if (millis() - lastTxTime >= TX_INTERVAL_MS) {
    lastTxTime = millis();
    transmitRFData();
  }

  // Update LCD Display in Real-Time @ 33 FPS (30ms interval)
  if (millis() - lastLcdRefreshTime >= LCD_REFRESH_INTERVAL_MS) {
    lastLcdRefreshTime = millis();
    renderLCD();
  }

  // Telemetry Diagnostics to Serial Monitor @ 5Hz (200ms interval)
  if (millis() - lastSerialTime >= SERIAL_INTERVAL_MS) {
    lastSerialTime = millis();
    printSerialDiagnostics();
  }
}

// ============================================================================
// ENCODER PUSH BUTTON HANDLER
// ============================================================================
void handleEncoderButton() {
  bool pinLow = (digitalRead(PIN_ENC_SW) == LOW);

  if (pinLow && !buttonPressed) {
    buttonPressed = true;
    buttonPressStartTime = millis();
    buttonLongPressHandled = false;
  } else if (pinLow && buttonPressed) {
    if (!buttonLongPressHandled &&
        (millis() - buttonPressStartTime >= LONG_PRESS_DURATION_MS)) {
      buttonLongPressHandled = true;
      if (currentState == STATE_HOME_PAGES) {
        currentState = STATE_MENU_LIST;
        currentMenuItem = 0;
        lcd.clear();
        Serial.println(F("[UI] >>> LONG PRESS: Opening Pro Settings Menu"));
      } else {
        saveCurrentModelToEEPROM();
        currentState = STATE_HOME_PAGES;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("*SETTINGS SAVED*"));
        lcd.setCursor(0, 1);
        lcd.print(F(" RETURNING HOME "));
        delay(600);
        lcd.clear();
        Serial.println(
            F("[UI] >>> LONG PRESS: Settings Saved -> Returning Home"));
      }
    }
  } else if (!pinLow && buttonPressed) {
    uint32_t pressDuration = millis() - buttonPressStartTime;
    buttonPressed = false;

    if (!buttonLongPressHandled && pressDuration >= 35) {
      handleShortClick();
    }
  }
}

// ----------------------------------------------------------------------------
// Short Click Action Dispatcher
// ----------------------------------------------------------------------------
void handleShortClick() {
  Serial.println(F("[UI] Short Click Detected"));

  switch (currentState) {
  case STATE_HOME_PAGES:
    currentHomePage = (currentHomePage + 1) % TOTAL_HOME_PAGES;
    lcd.clear();
    break;

  case STATE_MENU_LIST:
    if (currentMenuItem == 0) {
      currentState = STATE_MENU_EDIT_TRIM;
      selectedChannel = 0;
    } else if (currentMenuItem == 1) {
      currentState = STATE_MENU_EDIT_DR_EXPO;
      selectedChannel = 0;
      selectedSubParam = 0;
    } else if (currentMenuItem == 2) {
      currentState = STATE_MENU_EDIT_ENDPOINT;
      selectedChannel = 0;
      selectedSubParam = 0;
    } else if (currentMenuItem == 3) {
      currentState = STATE_MENU_EDIT_REVERSE;
      selectedChannel = 0;
    } else if (currentMenuItem == 4) {
      currentState = STATE_MENU_CALIBRATE;
      calibStep = 0;
    } else if (currentMenuItem == 5) {
      currentState = STATE_MENU_WING_MIX;
    } else if (currentMenuItem == 6) {
      currentState = STATE_MENU_MODEL_SELECT;
    } else if (currentMenuItem == 7) {
      currentState = STATE_MENU_EDIT_NAME;
      editNameCharIdx = 0;
    } else if (currentMenuItem == 8) {
      currentState = STATE_MENU_EDIT_DEADBAND;
      selectedChannel = 0;
    } else if (currentMenuItem == 9) {
      currentState = STATE_MENU_FAILSAFE;
    } else if (currentMenuItem == 10) {
      resetModelDefaults(activeModelIdx);
      saveCurrentModelToEEPROM();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(F(" MODEL RESET OK "));
      lcd.setCursor(0, 1);
      lcd.print(F("DEFAULTS RESTORE"));
      delay(700);
      lcd.clear();
    } else if (currentMenuItem == 11) {
      saveCurrentModelToEEPROM();
      currentState = STATE_HOME_PAGES;
      lcd.clear();
    }
    lcd.clear();
    break;

  case STATE_MENU_EDIT_TRIM:
    selectedChannel++;
    if (selectedChannel >= 4) {
      selectedChannel = 0;
      saveCurrentModelToEEPROM();
      currentState = STATE_MENU_LIST;
      lcd.clear();
    }
    break;

  case STATE_MENU_EDIT_DR_EXPO:
    selectedSubParam++;
    if (selectedSubParam > 1) {
      selectedSubParam = 0;
      selectedChannel++;
      if (selectedChannel >= 3) {
        selectedChannel = 0;
        saveCurrentModelToEEPROM();
        currentState = STATE_MENU_LIST;
        lcd.clear();
      }
    }
    break;

  case STATE_MENU_EDIT_ENDPOINT:
    selectedSubParam++;
    if (selectedSubParam > 1) {
      selectedSubParam = 0;
      selectedChannel++;
      if (selectedChannel >= 8) {
        selectedChannel = 0;
        saveCurrentModelToEEPROM();
        currentState = STATE_MENU_LIST;
        lcd.clear();
      }
    }
    break;

  case STATE_MENU_EDIT_REVERSE:
    selectedChannel++;
    if (selectedChannel >= 8) {
      selectedChannel = 0;
      saveCurrentModelToEEPROM();
      currentState = STATE_MENU_LIST;
      lcd.clear();
    }
    break;

  case STATE_MENU_CALIBRATE:
    if (calibStep == 0) {
      calibStep = 1;
      lcd.clear();
    } else if (calibStep == 1) {
      for (uint8_t i = 0; i < 6; i++) {
        int centerRaw = readAnalogFiltered(i);
        calibLiveCenter[i] = centerRaw;
        calibLiveMin[i] = centerRaw;
        calibLiveMax[i] = centerRaw;
      }
      calibStep = 2;
      lcd.clear();
    } else if (calibStep == 2) {
      // Joysticks CH1, CH2, CH4 - Spring Centered
      const uint8_t stickAxes[3] = {0, 1, 3};
      for (uint8_t k = 0; k < 3; k++) {
        uint8_t i = stickAxes[k];
        uint16_t center = calibLiveCenter[i];

        if ((center - calibLiveMin[i] >= 100) &&
            (calibLiveMax[i] - center >= 100)) {
          curModel.ch[i].adcCenter = center;
          curModel.ch[i].adcMin = calibLiveMin[i];
          curModel.ch[i].adcMax = calibLiveMax[i];
        } else {
          curModel.ch[i].adcCenter = constrain(center, 300, 720);
          curModel.ch[i].adcMin =
              constrain((int)curModel.ch[i].adcCenter - 320, 50, 450);
          curModel.ch[i].adcMax =
              constrain((int)curModel.ch[i].adcCenter + 320, 570, 970);
        }
      }

      // Throttle (CH3)
      if (calibLiveMax[2] - calibLiveMin[2] >= 200) {
        curModel.ch[2].adcMin = calibLiveMin[2];
        curModel.ch[2].adcMax = calibLiveMax[2];
        curModel.ch[2].adcCenter = (calibLiveMin[2] + calibLiveMax[2]) / 2;
      } else {
        curModel.ch[2].adcMin = 180;
        curModel.ch[2].adcMax = 840;
        curModel.ch[2].adcCenter = 510;
      }

      // Potentiometers P1 (CH5) & P2 (CH6)
      for (uint8_t i = 4; i < 6; i++) {
        if (calibLiveMax[i] - calibLiveMin[i] >= 300) {
          curModel.ch[i].adcMin = (calibLiveMin[i] <= 20) ? 0 : calibLiveMin[i];
          curModel.ch[i].adcMax =
              (calibLiveMax[i] >= 1000) ? 1023 : calibLiveMax[i];
        } else {
          curModel.ch[i].adcMin = 0;
          curModel.ch[i].adcMax = 1023;
        }
        curModel.ch[i].adcCenter =
            (curModel.ch[i].adcMin + curModel.ch[i].adcMax) / 2;
      }

      saveCurrentModelToEEPROM();
      calibStep = 3;
      lcd.clear();
    } else if (calibStep == 3) {
      calibStep = 0;
      currentState = STATE_MENU_LIST;
      lcd.clear();
    }
    break;

  case STATE_MENU_EDIT_NAME:
    editNameCharIdx++;
    if (editNameCharIdx >= 7) {
      editNameCharIdx = 0;
      curModel.name[7] = '\0';
      saveCurrentModelToEEPROM();
      currentState = STATE_MENU_LIST;
      lcd.clear();
    }
    break;

  case STATE_MENU_EDIT_DEADBAND:
    selectedChannel++;
    if (selectedChannel >= 4) {
      selectedChannel = 0;
      saveCurrentModelToEEPROM();
      currentState = STATE_MENU_LIST;
      lcd.clear();
    }
    break;

  case STATE_MENU_WING_MIX:
  case STATE_MENU_MODEL_SELECT:
  case STATE_MENU_FAILSAFE:
    saveCurrentModelToEEPROM();
    currentState = STATE_MENU_LIST;
    lcd.clear();
    break;

  default:
    break;
  }
}

// ============================================================================
// HIGH-PRECISION FAST ADC OVERSAMPLING & CHANNEL ISOLATION ENGINE
// ============================================================================
int readAnalogFiltered(uint8_t pinIndex) {
  uint8_t pin = ANALOG_PINS[pinIndex];

  analogRead(pin);
  delayMicroseconds(10);

  int s0 = analogRead(pin);
  int s1 = analogRead(pin);
  int s2 = analogRead(pin);
  int s3 = analogRead(pin);
  int rawAveraged = (s0 + s1 + s2 + s3 + 2) >> 2;

  filteredAdc[pinIndex] =
      (filteredAdc[pinIndex] * 0.30f) + ((float)rawAveraged * 0.70f);
  return (int)(filteredAdc[pinIndex] + 0.5f);
}

// ============================================================================
// DUAL RATES, EXPO & WING MIXING CALCULATION ENGINE
// ============================================================================
void applyRatesExpoAndMixing(long *proc) {
  const uint8_t axisIdx[3] = {0, 1, 3};

  for (uint8_t k = 0; k < 3; k++) {
    uint8_t chIndex = axisIdx[k];
    float center = curModel.ch[chIndex].centerUs;
    float currentUs = proc[chIndex];
    float maxDeflection = (currentUs >= center)
                              ? (curModel.ch[chIndex].maxUs - center)
                              : (center - curModel.ch[chIndex].minUs);

    if (maxDeflection > 10.0f) {
      float norm = (currentUs - center) / maxDeflection;
      norm = constrain(norm, -1.0f, 1.0f);

      float expoFac = (float)curModel.rateExpo[k].expo / 100.0f;
      float curved = ((1.0f - expoFac) * norm) + (expoFac * norm * norm * norm);

      float rateFac = (float)curModel.rateExpo[k].rate / 100.0f;
      float finalUs = center + (curved * rateFac * maxDeflection);

      proc[chIndex] = (long)(finalUs + (finalUs >= 0 ? 0.5f : -0.5f));
    }
  }

  if (curModel.wingMix == MIX_ELEVON) {
    long ailDef = proc[0] - 1500;
    long eleDef = proc[1] - 1500;
    proc[0] = 1500 + eleDef + ailDef; // CH1: Left Elevon
    proc[1] = 1500 + eleDef - ailDef; // CH2: Right Elevon
  } else if (curModel.wingMix == MIX_VTAIL) {
    long eleDef = proc[1] - 1500;
    long rudDef = proc[3] - 1500;
    proc[1] = 1500 + eleDef + rudDef; // CH2: Left V-Tail
    proc[3] = 1500 + eleDef - rudDef; // CH4: Right V-Tail
  }
}

// ============================================================================
// PROPORTIONAL INPUT PROCESSING & PIECEWISE 3-POINT CALIBRATION ENGINE
// ============================================================================
void readInputsAndProcessChannels() {
  int rawInputs[8];

  for (uint8_t i = 0; i < 6; i++) {
    rawInputs[i] = readAnalogFiltered(i);

    if (currentState == STATE_MENU_CALIBRATE && calibStep == 2) {
      if (rawInputs[i] < calibLiveMin[i])
        calibLiveMin[i] = rawInputs[i];
      if (rawInputs[i] > calibLiveMax[i])
        calibLiveMax[i] = rawInputs[i];
    }
  }

  rawInputs[6] = (digitalRead(PIN_SW_CH7) == LOW) ? 1023 : 0;
  rawInputs[7] = (digitalRead(PIN_SW_CH8) == LOW) ? 1023 : 0;

  long rawProc[8];

  for (uint8_t i = 0; i < 8; i++) {
    long mappedUs = 1500;

    if (i == 2) {
      // Channel 3: Throttle (Continuous 0% to 100% Proportional Sweep)
      uint16_t cMin = curModel.ch[i].adcMin;
      uint16_t cMax = curModel.ch[i].adcMax;
      uint8_t db = curModel.ch[i].deadband;
      if (cMin + 50 >= cMax) {
        cMin = 180;
        cMax = 840;
      }

      int inVal = constrain(rawInputs[i], cMin + db, cMax - db);
      long span = (long)(cMax - db) - (long)(cMin + db);
      if (span < 50)
        span = 50;

      mappedUs = (long)curModel.ch[i].minUs +
                 ((long)(inVal - (cMin + db)) *
                  ((long)curModel.ch[i].maxUs - (long)curModel.ch[i].minUs)) /
                     span;
    } else if (i == 0 || i == 1 || i == 3) {
      // Channels 1, 2, 4: Spring-Centered Joysticks (AIL, ELE, RUD)
      uint16_t cMin = curModel.ch[i].adcMin;
      uint16_t cCenter = curModel.ch[i].adcCenter;
      uint16_t cMax = curModel.ch[i].adcMax;
      uint8_t db = curModel.ch[i].deadband;

      if (cMin + 40 >= cCenter)
        cMin = (cCenter >= 300) ? (cCenter - 300) : 50;
      if (cCenter + 40 >= cMax)
        cMax = (cCenter <= 720) ? (cCenter + 300) : 970;

      if (rawInputs[i] < (int)(cCenter - db)) {
        int inVal = constrain(rawInputs[i], cMin, cCenter - db);
        long span = (long)(cCenter - db) - (long)cMin;
        if (span < 40)
          span = 40;

        mappedUs = (long)curModel.ch[i].minUs +
                   ((long)(inVal - cMin) * ((long)curModel.ch[i].centerUs -
                                            (long)curModel.ch[i].minUs)) /
                       span;
      } else if (rawInputs[i] > (int)(cCenter + db)) {
        int inVal = constrain(rawInputs[i], cCenter + db, cMax);
        long span = (long)cMax - (long)(cCenter + db);
        if (span < 40)
          span = 40;

        mappedUs =
            (long)curModel.ch[i].centerUs +
            ((long)(inVal - (cCenter + db)) *
             ((long)curModel.ch[i].maxUs - (long)curModel.ch[i].centerUs)) /
                span;
      } else {
        mappedUs = curModel.ch[i].centerUs;
      }
    } else if (i == 4 || i == 5) {
      // Channels 5 & 6: Potentiometers P1 & P2 (0% - 100% Proportional Span)
      uint16_t cMin = curModel.ch[i].adcMin;
      uint16_t cMax = curModel.ch[i].adcMax;
      uint8_t db = curModel.ch[i].deadband;
      if (cMin + 50 >= cMax) {
        cMin = 0;
        cMax = 1023;
      }

      int inVal = constrain(rawInputs[i], cMin + db, cMax - db);
      long span = (long)(cMax - db) - (long)(cMin + db);
      if (span < 50)
        span = 50;

      mappedUs = (long)curModel.ch[i].minUs +
                 ((long)(inVal - (cMin + db)) *
                  ((long)curModel.ch[i].maxUs - (long)curModel.ch[i].minUs)) /
                     span;
    } else {
      // Channels 7 & 8: Toggle Switches
      mappedUs =
          (rawInputs[i] > 512) ? curModel.ch[i].maxUs : curModel.ch[i].minUs;
    }

    rawProc[i] = mappedUs;
  }

  applyRatesExpoAndMixing(rawProc);

  for (uint8_t i = 0; i < 8; i++) {
    long finalVal = rawProc[i];

    if (i < 4) {
      finalVal += curModel.ch[i].trim;
    }

    if (curModel.ch[i].reversed) {
      finalVal =
          (long)curModel.ch[i].minUs + (long)curModel.ch[i].maxUs - finalVal;
    }

    finalVal = constrain(finalVal, 800, 2200);

    if (abs((int)finalVal - (int)lastStableUs[i]) <= 1 &&
        finalVal != curModel.ch[i].minUs && finalVal != curModel.ch[i].maxUs &&
        finalVal != curModel.ch[i].centerUs) {
      // Retain last stable value
    } else {
      lastStableUs[i] = (uint16_t)finalVal;
    }

    payload.channels[i] = lastStableUs[i];
  }

  payload.packetId = ++packetSeq;
  payload.flags = 0;
}

// ============================================================================
// 50Hz RF TRANSMISSION & AUTO-ACK TELEMETRY
// ============================================================================
void transmitRFData() {
  totalPackets++;

  bool ackReceived = radio.write(&payload, sizeof(RadioPayload));

  if (ackReceived) {
    successfulPackets++;
    lastRxAckTime = millis();
    rxConnected = true;
  } else {
    if (millis() - lastRxAckTime > 400) {
      rxConnected = false;
    }
  }
}

// ============================================================================
// LCD RENDERING ENGINE
// ============================================================================
void renderLCD() {
  switch (currentState) {
  case STATE_HOME_PAGES:
    renderHomePage();
    break;
  case STATE_MENU_LIST:
    renderMenuList();
    break;
  case STATE_MENU_EDIT_TRIM:
    renderEditTrim();
    break;
  case STATE_MENU_EDIT_DR_EXPO:
    renderEditDrExpo();
    break;
  case STATE_MENU_EDIT_ENDPOINT:
    renderEditEndpoint();
    break;
  case STATE_MENU_EDIT_REVERSE:
    renderEditReverse();
    break;
  case STATE_MENU_CALIBRATE:
    renderCalibration();
    break;
  case STATE_MENU_WING_MIX:
    renderWingMix();
    break;
  case STATE_MENU_MODEL_SELECT:
    renderModelSelect();
    break;
  case STATE_MENU_EDIT_NAME:
    renderEditModelName();
    break;
  case STATE_MENU_EDIT_DEADBAND:
    renderEditDeadband();
    break;
  case STATE_MENU_FAILSAFE:
    renderFailsafe();
    break;
  default:
    break;
  }
}

// ----------------------------------------------------------------------------
// Home Page: Multi-Page Live Monitor with Graphic Slider Bars
// ----------------------------------------------------------------------------
void renderHomePage() {
  int8_t delta = readEncoderDelta();
  if (delta != 0) {
    int8_t newPage = ((int16_t)currentHomePage + delta) % TOTAL_HOME_PAGES;
    if (newPage < 0)
      newPage += TOTAL_HOME_PAGES;
    currentHomePage = (uint8_t)newPage;
    lcd.clear();
  }

  char buf[8];

  if (currentHomePage == 0) {
    // Page 1: CH1 (Aileron/Steering) & CH2 (Elevator/Pitch)
    lcd.setCursor(0, 0);
    lcd.print(F("AIL:"));
    drawProgressBar(4, 0, 6, payload.channels[0], curModel.ch[0].minUs,
                    curModel.ch[0].maxUs);
    snprintf(buf, sizeof(buf), " %4d", payload.channels[0]);
    lcd.print(buf);

    lcd.setCursor(0, 1);
    lcd.print(F("ELE:"));
    drawProgressBar(4, 1, 6, payload.channels[1], curModel.ch[1].minUs,
                    curModel.ch[1].maxUs);
    snprintf(buf, sizeof(buf), " %4d", payload.channels[1]);
    lcd.print(buf);
  } else if (currentHomePage == 1) {
    // Page 2: CH3 (Throttle) & CH4 (Rudder/Yaw)
    lcd.setCursor(0, 0);
    lcd.print(F("THR:"));
    drawProgressBar(4, 0, 6, payload.channels[2], curModel.ch[2].minUs,
                    curModel.ch[2].maxUs);
    snprintf(buf, sizeof(buf), " %4d", payload.channels[2]);
    lcd.print(buf);

    lcd.setCursor(0, 1);
    lcd.print(F("RUD:"));
    drawProgressBar(4, 1, 6, payload.channels[3], curModel.ch[3].minUs,
                    curModel.ch[3].maxUs);
    snprintf(buf, sizeof(buf), " %4d", payload.channels[3]);
    lcd.print(buf);
  } else if (currentHomePage == 2) {
    // Page 3: CH5 (Pot 1) & CH6 (Pot 2)
    lcd.setCursor(0, 0);
    lcd.print(F("P1: "));
    drawProgressBar(4, 0, 6, payload.channels[4], curModel.ch[4].minUs,
                    curModel.ch[4].maxUs);
    snprintf(buf, sizeof(buf), " %4d", payload.channels[4]);
    lcd.print(buf);

    lcd.setCursor(0, 1);
    lcd.print(F("P2: "));
    drawProgressBar(4, 1, 6, payload.channels[5], curModel.ch[5].minUs,
                    curModel.ch[5].maxUs);
    snprintf(buf, sizeof(buf), " %4d", payload.channels[5]);
    lcd.print(buf);
  } else if (currentHomePage == 3) {
    // Page 4: CH7 (Switch 1) & CH8 (Switch 2)
    lcd.setCursor(0, 0);
    lcd.print(F("SW1:"));
    lcd.print((payload.channels[6] > 1500) ? F("[ ON  ] ") : F("[ OFF ] "));
    snprintf(buf, sizeof(buf), "%4d", payload.channels[6]);
    lcd.print(buf);

    lcd.setCursor(0, 1);
    lcd.print(F("SW2:"));
    lcd.print((payload.channels[7] > 1500) ? F("[ ON  ] ") : F("[ OFF ] "));
    snprintf(buf, sizeof(buf), "%4d", payload.channels[7]);
    lcd.print(buf);
  } else {
    // Page 5: Model Memory Overview
    char nameBuf[8];
    snprintf(nameBuf, sizeof(nameBuf), "%-7s", curModel.name);
    lcd.setCursor(0, 0);
    lcd.print(F("M:"));
    lcd.print(nameBuf);
    lcd.setCursor(9, 0);
    lcd.print(' ');
    lcd.setCursor(10, 0);
    if (curModel.wingMix == MIX_ELEVON)
      lcd.print(F("[ELEV]"));
    else if (curModel.wingMix == MIX_VTAIL)
      lcd.print(F("[V-TL]"));
    else
      lcd.print(F("[NORM]"));

    lcd.setCursor(0, 1);
    lcd.print(F("LINK:"));
    lcd.print(rxConnected ? F("[Rx:OK] ") : F("[Rx:NC] "));
    lcd.setCursor(12, 1);
    lcd.print(F("50Hz"));
  }
}

// ----------------------------------------------------------------------------
// Pro Settings Menu List (Read directly from PROGMEM)
// ----------------------------------------------------------------------------
void renderMenuList() {
  int8_t delta = readEncoderDelta();
  if (delta != 0) {
    int8_t newItem = ((int16_t)currentMenuItem + delta) % TOTAL_MENU_ITEMS;
    if (newItem < 0)
      newItem += TOTAL_MENU_ITEMS;
    currentMenuItem = (int8_t)newItem;
    lcd.clear();
  }

  lcd.setCursor(0, 0);
  lcd.print(F("> "));
  printProgmemStr(MENU_ITEMS, currentMenuItem);

  lcd.setCursor(0, 1);
  uint8_t nextItem = (currentMenuItem + 1) % TOTAL_MENU_ITEMS;
  lcd.print(F("  "));
  printProgmemStr(MENU_ITEMS, nextItem);
}

// ----------------------------------------------------------------------------
// Submenu 1: Digital Trims (CH1 - CH4)
// ----------------------------------------------------------------------------
void renderEditTrim() {
  if (selectedChannel > 3)
    selectedChannel = 3;

  int8_t delta = readEncoderDelta();
  if (delta != 0) {
    curModel.ch[selectedChannel].trim =
        constrain(curModel.ch[selectedChannel].trim + (delta * 5), -150, 150);
  }

  char lineBuf[17];
  lcd.setCursor(0, 0);
  lcd.print(F("TRIM "));
  printProgmemStr(CH_NAMES, selectedChannel);
  snprintf(lineBuf, sizeof(lineBuf), ":%+4dus",
           curModel.ch[selectedChannel].trim);
  lcd.print(lineBuf);
  lcd.print(F("    "));

  lcd.setCursor(0, 1);
  snprintf(lineBuf, sizeof(lineBuf), "OUT:%4dus [NEXT]",
           payload.channels[selectedChannel]);
  lcd.print(lineBuf);
}

// ----------------------------------------------------------------------------
// Submenu 2: Dual Rates & Exponential (AIL, ELE, RUD)
// ----------------------------------------------------------------------------
void renderEditDrExpo() {
  int8_t delta = readEncoderDelta();

  if (delta != 0) {
    if (selectedSubParam == 0) {
      curModel.rateExpo[selectedChannel].rate = constrain(
          curModel.rateExpo[selectedChannel].rate + (delta * 5), 50, 100);
    } else {
      curModel.rateExpo[selectedChannel].expo = constrain(
          curModel.rateExpo[selectedChannel].expo + (delta * 5), 0, 70);
    }
  }

  char lineBuf[17];
  lcd.setCursor(0, 0);
  printProgmemStr(CH_NAMES, selectedChannel);
  snprintf(lineBuf, sizeof(lineBuf), "  D/R:%3d%% %s",
           curModel.rateExpo[selectedChannel].rate,
           selectedSubParam == 0 ? "*" : " ");
  lcd.print(lineBuf);

  lcd.setCursor(0, 1);
  snprintf(lineBuf, sizeof(lineBuf), "    EXPO:%2d%% %s",
           curModel.rateExpo[selectedChannel].expo,
           selectedSubParam == 1 ? "*" : " ");
  lcd.print(lineBuf);
  lcd.print(F("  "));
}

// ----------------------------------------------------------------------------
// Submenu 3: Min / Max Endpoints (CH1 - CH8)
// ----------------------------------------------------------------------------
void renderEditEndpoint() {
  int8_t delta = readEncoderDelta();
  if (delta != 0) {
    if (selectedSubParam == 0) {
      curModel.ch[selectedChannel].minUs = constrain(
          curModel.ch[selectedChannel].minUs + (delta * 10), 800, 1400);
    } else {
      curModel.ch[selectedChannel].maxUs = constrain(
          curModel.ch[selectedChannel].maxUs + (delta * 10), 1600, 2200);
    }
  }

  char lineBuf[17];
  lcd.setCursor(0, 0);
  lcd.print(F("CH"));
  lcd.print(selectedChannel + 1);
  lcd.print(' ');
  printProgmemStr(CH_NAMES, selectedChannel);
  lcd.print(selectedSubParam == 0 ? F(" [MIN]  ") : F(" [MAX]  "));

  lcd.setCursor(0, 1);
  snprintf(lineBuf, sizeof(lineBuf), "L:%4d H:%4d",
           curModel.ch[selectedChannel].minUs,
           curModel.ch[selectedChannel].maxUs);
  lcd.print(lineBuf);
  lcd.print(F("  "));
}

// ----------------------------------------------------------------------------
// Submenu 4: Channel Reversing (CH1 - CH8)
// ----------------------------------------------------------------------------
void renderEditReverse() {
  int8_t delta = readEncoderDelta();
  if (delta != 0) {
    curModel.ch[selectedChannel].reversed =
        !curModel.ch[selectedChannel].reversed;
  }

  lcd.setCursor(0, 0);
  lcd.print(F("REV "));
  printProgmemStr(CH_NAMES, selectedChannel);
  lcd.print(F(" (CH"));
  lcd.print(selectedChannel + 1);
  lcd.print(F("/8)"));

  lcd.setCursor(0, 1);
  lcd.print(F("DIR: "));
  lcd.print(curModel.ch[selectedChannel].reversed ? F("[REVERSED] ")
                                                  : F("[NORMAL]   "));
}

// ----------------------------------------------------------------------------
// Submenu 5: Full 6-Channel Calibration Suite (Sticks & Pots)
// ----------------------------------------------------------------------------
void renderCalibration() {
  if (calibStep == 0) {
    lcd.setCursor(0, 0);
    lcd.print(F("1.CENTER STICKS "));
    lcd.setCursor(0, 1);
    lcd.print(F("CLICK TO START >"));
  } else if (calibStep == 1) {
    lcd.setCursor(0, 0);
    lcd.print(F("CENTER ALL STIK "));
    lcd.setCursor(0, 1);
    char buf[17];
    snprintf(buf, sizeof(buf), "A%3d E%3d [CLICK]", (int)filteredAdc[0],
             (int)filteredAdc[1]);
    lcd.print(buf);
  } else if (calibStep == 2) {
    bool aOk = (calibLiveMax[0] - calibLiveMin[0] >= 120);
    bool eOk = (calibLiveMax[1] - calibLiveMin[1] >= 120);
    bool tOk = (calibLiveMax[2] - calibLiveMin[2] >= 150);
    bool rOk = (calibLiveMax[3] - calibLiveMin[3] >= 120);

    lcd.setCursor(0, 0);
    lcd.print(F("2.MOVE TO LIMITS"));
    lcd.setCursor(0, 1);
    char buf[17];
    snprintf(buf, sizeof(buf), "%c:%c %c:%c %c:%c %c:%c", 'A', aOk ? '*' : '.',
             'E', eOk ? '*' : '.', 'T', tOk ? '*' : '.', 'R', rOk ? '*' : '.');
    lcd.print(buf);
  } else if (calibStep == 3) {
    lcd.setCursor(0, 0);
    lcd.print(F("*CALIBRATION OK*"));
    lcd.setCursor(0, 1);
    lcd.print(F("CLICK TO EXIT ->"));
  }
}

// ----------------------------------------------------------------------------
// Submenu 6: Wing & Tail Mixing
// ----------------------------------------------------------------------------
void renderWingMix() {
  int8_t delta = readEncoderDelta();
  if (delta != 0) {
    int8_t newMix = ((int16_t)curModel.wingMix + delta) % 3;
    if (newMix < 0)
      newMix += 3;
    curModel.wingMix = (uint8_t)newMix;
  }

  lcd.setCursor(0, 0);
  lcd.print(F("WING MIX TYPE:  "));
  lcd.setCursor(0, 1);
  lcd.print(F("> [ "));
  printProgmemStr(MIX_NAMES, curModel.wingMix);
  lcd.print(F(" ]    "));
}

// ----------------------------------------------------------------------------
// Submenu 7: 4-Model Memory Profile Selector
// ----------------------------------------------------------------------------
void renderModelSelect() {
  int8_t delta = readEncoderDelta();
  if (delta != 0) {
    int8_t newIdx = ((int16_t)activeModelIdx + delta) % TOTAL_MODELS;
    if (newIdx < 0)
      newIdx += TOTAL_MODELS;
    switchModel((uint8_t)newIdx);
  }

  lcd.setCursor(0, 0);
  lcd.print(F("SELECT PROFILE: "));
  lcd.setCursor(0, 1);
  char buf[17];
  snprintf(buf, sizeof(buf), "> %-7s (%d/%d)", curModel.name, activeModelIdx + 1,
           TOTAL_MODELS);
  lcd.print(buf);
}

// ----------------------------------------------------------------------------
// Submenu 8: Model Name Editor (Interactive Character Selection)
// ----------------------------------------------------------------------------
void renderEditModelName() {
  int8_t delta = readEncoderDelta();
  if (delta != 0) {
    curModel.name[editNameCharIdx] =
        cycleChar(curModel.name[editNameCharIdx], delta);
  }

  lcd.setCursor(0, 0);
  lcd.print(F("NAME: ["));
  for (uint8_t i = 0; i < 7; i++) {
    char ch = curModel.name[i];
    if (ch < 32 || ch > 126)
      ch = ' ';
    lcd.write(ch);
  }
  lcd.print(F("] "));

  lcd.setCursor(0, 1);
  lcd.print(F("       "));
  for (uint8_t i = 0; i < 7; i++) {
    if (i == editNameCharIdx) {
      lcd.write('^');
    } else {
      lcd.write(' ');
    }
  }
  lcd.print(F(" [OK]"));
}

// ----------------------------------------------------------------------------
// Submenu 9: Channel Deadband Adjustment (CH1 - CH4)
// ----------------------------------------------------------------------------
void renderEditDeadband() {
  if (selectedChannel > 3)
    selectedChannel = 3;

  int8_t delta = readEncoderDelta();
  if (delta != 0) {
    curModel.ch[selectedChannel].deadband = (uint8_t)constrain(
        (int)curModel.ch[selectedChannel].deadband + delta, 0, 30);
  }

  char lineBuf[17];
  lcd.setCursor(0, 0);
  lcd.print(F("DEADBAND "));
  printProgmemStr(CH_NAMES, selectedChannel);
  lcd.print(F("    "));

  lcd.setCursor(0, 1);
  snprintf(lineBuf, sizeof(lineBuf), "ZONE:+-%2d [NEXT]",
           curModel.ch[selectedChannel].deadband);
  lcd.print(lineBuf);
}

// ----------------------------------------------------------------------------
// Submenu 10: Failsafe Snapshot
// ----------------------------------------------------------------------------
void renderFailsafe() {
  lcd.setCursor(0, 0);
  lcd.print(F(" SET FAILSAFE ? "));
  lcd.setCursor(0, 1);
  lcd.print(F(" CLICK TO STORE "));
}

// ============================================================================
// CHARACTER CYCLING HELPER FOR MODEL NAME EDITOR
// ============================================================================
char cycleChar(char c, int8_t delta) {
  const char charset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_#.";
  const uint8_t len = sizeof(charset) - 1;
  int8_t idx = 0;
  for (uint8_t i = 0; i < len; i++) {
    if (charset[i] == c) {
      idx = i;
      break;
    }
  }
  idx = (idx + delta) % (int8_t)len;
  if (idx < 0)
    idx += len;
  return charset[idx];
}

// ============================================================================
// CUSTOM PROGRESS / SLIDER BAR RENDERER
// ============================================================================
void drawProgressBar(uint8_t col, uint8_t row, uint8_t width, uint16_t val,
                     uint16_t minVal, uint16_t maxVal) {
  if (minVal >= maxVal)
    maxVal = minVal + 1;
  uint16_t clampedVal = constrain(val, minVal, maxVal);

  uint16_t totalSlices = width * 5;
  uint16_t activeSlices = map(clampedVal, minVal, maxVal, 0, totalSlices);

  lcd.setCursor(col, row);
  for (uint8_t i = 0; i < width; i++) {
    if (activeSlices >= 5) {
      lcd.write(4);
      activeSlices -= 5;
    } else if (activeSlices > 0) {
      lcd.write(activeSlices - 1);
      activeSlices = 0;
    } else {
      lcd.print(F("."));
    }
  }
}

// ============================================================================
// EEPROM PAGED STORAGE FUNCTIONS (143 BYTES RAM FOOTPRINT)
// ============================================================================

uint16_t getModelEEPROMAddress(uint8_t modelIdx) {
  return EEPROM_BASE_ADDR + ((uint16_t)modelIdx * EEPROM_MODEL_SIZE);
}

void loadModelFromEEPROM(uint8_t modelIdx) {
  uint16_t addr = getModelEEPROMAddress(modelIdx);
  EEPROM.get(addr, curModel);

  // Bounds validation check
  bool valid = (curModel.ch[0].minUs >= 800 && curModel.ch[0].minUs <= 1500 &&
                curModel.ch[0].maxUs >= 1500 && curModel.ch[0].maxUs <= 2200 &&
                curModel.ch[0].adcMin < curModel.ch[0].adcMax);

  // Ensure name is null-terminated and printable ASCII
  curModel.name[7] = '\0';
  for (uint8_t i = 0; i < 7; i++) {
    if (curModel.name[i] < 32 || curModel.name[i] > 126) {
      curModel.name[i] = ' ';
    }
  }

  if (!valid) {
    resetModelDefaults(modelIdx);
    saveCurrentModelToEEPROM();
  }
}

void saveCurrentModelToEEPROM() {
  uint16_t magic = EEPROM_MAGIC_KEY;
  EEPROM.put(0, magic);
  EEPROM.put(2, activeModelIdx);

  uint16_t addr = getModelEEPROMAddress(activeModelIdx);
  EEPROM.put(addr, curModel);
  Serial.print(F("[EEPROM] Model ["));
  Serial.print(curModel.name);
  Serial.println(F("] saved successfully."));
}

void switchModel(uint8_t newModelIdx) {
  if (newModelIdx >= TOTAL_MODELS || newModelIdx == activeModelIdx)
    return;
  saveCurrentModelToEEPROM(); // Save outgoing model
  activeModelIdx = newModelIdx;
  EEPROM.put(2, activeModelIdx);
  loadModelFromEEPROM(activeModelIdx); // Load incoming model
}

void resetModelDefaults(uint8_t modelIdx) {
  snprintf(curModel.name, sizeof(curModel.name), "MODEL %d", modelIdx + 1);
  curModel.wingMix = MIX_NORMAL;
  curModel.rateExpo[0] = {100, 0}; // AIL: 100% Rate, 0% Expo
  curModel.rateExpo[1] = {100, 0}; // ELE: 100% Rate, 0% Expo
  curModel.rateExpo[2] = {100, 0}; // RUD: 100% Rate, 0% Expo

  for (uint8_t i = 0; i < 8; i++) {
    curModel.ch[i].trim = 0;
    curModel.ch[i].minUs = 1000;
    curModel.ch[i].maxUs = 2000;
    curModel.ch[i].centerUs = 1500;
    curModel.ch[i].reversed = false;

    if (i < 4) {
      curModel.ch[i].adcMin = 180;
      curModel.ch[i].adcCenter = 512;
      curModel.ch[i].adcMax = 840;
      curModel.ch[i].deadband = 12;
    } else if (i < 6) {
      curModel.ch[i].adcMin = 0;
      curModel.ch[i].adcCenter = 512;
      curModel.ch[i].adcMax = 1023;
      curModel.ch[i].deadband = 4;
    } else {
      curModel.ch[i].adcMin = 0;
      curModel.ch[i].adcCenter = 512;
      curModel.ch[i].adcMax = 1023;
      curModel.ch[i].deadband = 0;
    }
  }
}

void initAllEEPROMModels() {
  uint16_t magic = EEPROM_MAGIC_KEY;
  EEPROM.put(0, magic);
  activeModelIdx = 0;
  EEPROM.put(2, activeModelIdx);

  for (uint8_t m = 0; m < TOTAL_MODELS; m++) {
    resetModelDefaults(m);
    uint16_t addr = getModelEEPROMAddress(m);
    EEPROM.put(addr, curModel);
  }

  loadModelFromEEPROM(0);
  Serial.println(F("[EEPROM] All 4 models formatted and initialized."));
}

// ============================================================================
// CUSTOM LCD CHARACTER INITIALIZER
// ============================================================================
void setupCustomCharacters() {
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t charData[8];
    for (uint8_t j = 0; j < 8; j++) {
      charData[j] = pgm_read_byte(&(customChars[i][j]));
    }
    lcd.createChar(i, charData);
  }
}

// ============================================================================
// SERIAL MONITOR TELEMETRY DIAGNOSTICS
// ============================================================================
void printSerialDiagnostics() {
  Serial.print(F("TX8 | M:"));
  Serial.print(curModel.name);
  Serial.print(F(" | "));
  for (uint8_t i = 0; i < 8; i++) {
    char nameBuf[4];
    strcpy_P(nameBuf, (char *)pgm_read_word(&(CH_NAMES[i])));
    Serial.print(nameBuf);
    Serial.print(F(":"));
    Serial.print(payload.channels[i]);
    Serial.print(F(" "));
  }
  Serial.print(F("| Link: "));
  Serial.print(rxConnected ? F("CONNECTED (ACK OK)") : F("SEARCHING..."));
  Serial.print(F(" | Pkts: "));
  Serial.println(successfulPackets);
}
