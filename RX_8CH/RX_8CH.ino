/**
 * ============================================================================
 * 8-CHANNEL PROFESSIONAL RC RECEIVER (RX) FIRMWARE
 * ============================================================================
 * Microcontroller : Arduino Nano (ATmega328P, 16MHz, 5V)
 * RF Transceiver  : NRF24L01+ (2.4 GHz) with PA + LNA
 * Outputs         : 8 Standard 50Hz Servo PWM Channels (1000us - 2000us)
 * Failsafe        : Auto-Throttle CUT (1000us) & Center Neutral (1500us) on Lost Link
 * Jitter Filter   : Pulse Stabilization & Hysteresis to Prevent Servo Buzzing
 * Telemetry       : Bi-directional Auto-ACK Transmit Confirmation to TX
 * Link Status     : Dedicated Connection Status LED (Pin A1)
 *                   - SOLID ON  = Connected to Transmitter (Live Link OK)
 *                   - BLINKING  = Disconnected / Signal Lost / Failsafe Active
 * ============================================================================
 */

#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <Servo.h>

// ============================================================================
// HARDWARE PIN DEFINITIONS
// ============================================================================

// NRF24L01 RF Module Pins
#define PIN_RF_CE          9    // Chip Enable
#define PIN_RF_CSN        10    // SPI Chip Select (CSN)

// Receiver Pin Assignments (CH1: D0/RX0, CH2..CH8: D2..D8, Status LED: A1)
// CH1 uses D0/RX0; CH2..CH8 use D2..D8; Status LED uses A1.
#define PIN_OUT_CH1        0    // RX0 : CH1 (Aileron / Roll Servo)
#define PIN_OUT_CH2        2    // D2 : CH2 (Elevator / Pitch Servo)
#define PIN_OUT_CH3        3    // D3 : CH3 (Throttle / ESC Motor Controller)
#define PIN_OUT_CH4        4    // D4 : CH4 (Rudder / Yaw Servo)
#define PIN_OUT_CH5        5    // D5 : CH5 (AUX 1 / Potentiometer 1 / Gimbal)
#define PIN_OUT_CH6        6    // D6 : CH6 (AUX 2 / Potentiometer 2 / Flaps)
#define PIN_OUT_CH7        7    // D7 : CH7 (AUX 3 / Switch 1 / Gear / Relay)
#define PIN_OUT_CH8        8    // D8 : CH8 (AUX 4 / Switch 2 / Buzzer / Arm)

// Status LED Indicator Pin (Pin A1)
// Note: On Arduino Nano, the onboard 'L' LED is wired to Pin 13 (SCK clock for NRF24).
// Pin A1 provides a clean, dedicated connection status LED with zero RF interference.
#define PIN_STATUS_LED    A1    // A1 : Solid = Connected, Blinking = Failsafe / Signal Lost

RF24 radio(PIN_RF_CE, PIN_RF_CSN);
const uint8_t RF_PIPE_ADDR[6] = "RC001";

Servo servoOutputs[8];
const uint8_t SERVO_PINS[8] = {
  PIN_OUT_CH1, PIN_OUT_CH2, PIN_OUT_CH3, PIN_OUT_CH4,
  PIN_OUT_CH5, PIN_OUT_CH6, PIN_OUT_CH7, PIN_OUT_CH8
};

// ============================================================================
// DATA STRUCTURES & FAILSAFE PRESETS
// ============================================================================

struct RadioPayload {
  uint16_t channels[8];   // Microsecond values (800us - 2200us)
  uint8_t  packetId;      // Sequence counter
  uint8_t  flags;         // Telemetry / status flags
};

RadioPayload receivedPayload;

// Safe Default Failsafe Values (Throttle CUT @ 1000us, others Neutral @ 1500us)
const uint16_t FAILSAFE_VALUES[8] = {
  1500, // CH1: Aileron Neutral
  1500, // CH2: Elevator Neutral
  1000, // CH3: Throttle CUT (Motor OFF - Critical Safety)
  1500, // CH4: Rudder Neutral
  1500, // CH5: AUX 1 Neutral
  1500, // CH6: AUX 2 Neutral
  1000, // CH7: Switch 1 OFF / Low
  1000  // CH8: Switch 2 OFF / Low
};

// Active & Filtered Output Pulse Widths
uint16_t currentOutputs[8];
uint16_t lastAppliedOutputs[8];

// Link & Failsafe Watchdog Variables
uint32_t lastPacketTime = 0;
const uint32_t FAILSAFE_TIMEOUT_MS = 500;  // Trigger failsafe if no RF packet for 500ms
bool isFailsafeActive = true;
uint32_t totalPacketsReceived = 0;

// LED Blink Timer
uint32_t lastLedBlinkTime = 0;
bool ledState = false;

// Serial Diagnostics Timing
uint32_t lastSerialTime = 0;
const uint32_t SERIAL_INTERVAL_MS = 200;   // 5Hz diagnostics printout

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void applyOutputs(bool force);
void activateFailsafe();
void updateStatusLED();
void printSerialDiagnostics();

// ============================================================================
// ARDUINO SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=============================================="));
  Serial.println(F("  FLYMASTER RX8 - 8CH RECEIVER INITIALIZING"));
  Serial.println(F("=============================================="));

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  // Attach all 8 Servo Output Pins with standard 544us - 2400us timing
  for (uint8_t i = 0; i < 8; i++) {
    servoOutputs[i].attach(SERVO_PINS[i], 544, 2400);
    currentOutputs[i] = FAILSAFE_VALUES[i];
    lastAppliedOutputs[i] = FAILSAFE_VALUES[i];
    servoOutputs[i].writeMicroseconds(currentOutputs[i]);
  }
  Serial.println(F("[SYSTEM] 8 Servo PWM Outputs Attached (CH1:D0, CH2:D2, CH3:D3, CH4:D4, CH5:D5, CH6:D6, CH7:D7, CH8:D8)."));

  // Initialize NRF24L01+ Transceiver in Receiver Mode
  if (radio.begin()) {
    radio.openReadingPipe(1, RF_PIPE_ADDR);
    radio.setPALevel(RF24_PA_MAX);
    radio.setDataRate(RF24_250KBPS);      // 250kbps for maximum range & noise immunity
    radio.setChannel(108);                // 2.508 GHz channel
    radio.setAutoAck(true);               // Auto-ACK confirmation back to TX
    radio.enableDynamicPayloads();
    radio.startListening();
    Serial.println(F("[RF] NRF24L01 Receiver Listening on Pipe. Ready!"));
  } else {
    Serial.println(F("[ERROR] NRF24L01 Hardware Not Responding! Check Wiring & 3.3V."));
  }

  Serial.println(F("[SYSTEM] Ready. Waiting for Transmitter link...\n"));
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  bool newPacketReceived = false;

  // Drain incoming RF FIFO
  while (radio.available()) {
    radio.read(&receivedPayload, sizeof(RadioPayload));
    lastPacketTime = millis();
    totalPacketsReceived++;
    isFailsafeActive = false;
    newPacketReceived = true;

    // Constrain and copy channel pulse widths
    for (uint8_t i = 0; i < 8; i++) {
      currentOutputs[i] = constrain(receivedPayload.channels[i], 800, 2200);
    }
  }

  if (newPacketReceived) {
    applyOutputs(false);
  }

  // Failsafe Watchdog (Triggers after 500ms of lost packets)
  if (millis() - lastPacketTime > FAILSAFE_TIMEOUT_MS) {
    if (!isFailsafeActive) {
      isFailsafeActive = true;
      activateFailsafe();
      Serial.println(F("[WARNING] RF Signal Lost! FAILSAFE ACTIVATED (Motor CUT @ 1000us)."));
    }
  }

  // Update Connection Status LED
  updateStatusLED();

  if (millis() - lastSerialTime >= SERIAL_INTERVAL_MS) {
    lastSerialTime = millis();
    printSerialDiagnostics();
  }
}

// ============================================================================
// SERVO OUTPUT HANDLERS WITH JITTER SUPPRESSION
// ============================================================================
void applyOutputs(bool force) {
  for (uint8_t i = 0; i < 8; i++) {
    // Suppress tiny 1us noise to eliminate servo motor buzzing & cross-channel power spikes
    if (force || abs((int)currentOutputs[i] - (int)lastAppliedOutputs[i]) >= 2) {
      servoOutputs[i].writeMicroseconds(currentOutputs[i]);
      lastAppliedOutputs[i] = currentOutputs[i];
    }
  }
}

void activateFailsafe() {
  for (uint8_t i = 0; i < 8; i++) {
    currentOutputs[i] = FAILSAFE_VALUES[i];
  }
  applyOutputs(true);
}

// ============================================================================
// STATUS LED BEHAVIOR
// ============================================================================
void updateStatusLED() {
  if (!isFailsafeActive) {
    // Solid ON: Connected to Transmitter (Active RF Link)
    digitalWrite(PIN_STATUS_LED, HIGH);
  } else {
    // Blinking (500ms): Disconnected / Signal Lost / Waiting for Transmitter
    if (millis() - lastLedBlinkTime >= 500) {
      lastLedBlinkTime = millis();
      ledState = !ledState;
      digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);
    }
  }
}

// ============================================================================
// SERIAL MONITOR TELEMETRY DIAGNOSTICS
// ============================================================================
void printSerialDiagnostics() {
  Serial.print(F("RX8 | "));
  for (uint8_t i = 0; i < 8; i++) {
    Serial.print(F("CH"));
    Serial.print(i + 1);
    Serial.print(F(":"));
    Serial.print(currentOutputs[i]);
    Serial.print(F("us "));
  }
  Serial.print(F("| Status: "));
  if (isFailsafeActive) {
    Serial.print(F("[FAILSAFE - NO SIGNAL]"));
  } else {
    Serial.print(F("[LINK OK - "));
    Serial.print(totalPacketsReceived);
    Serial.print(F(" pkts]"));
  }
  Serial.println();
}
