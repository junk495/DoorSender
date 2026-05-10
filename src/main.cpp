// =====================================================================================
// Tuersender (code4)
// -------------------------------------------------------------------------------------
// Letzte Änderung: 06. Juli 2025, 18:30 (Speicheroptimierung für ATmega168)
// Hardware:          Arduino Pro Mini (3.3V, 16MHz, ATmega168), INA219 Sensor
// ... (restlicher Header bleibt gleich) ...
// =====================================================================================

#include <Arduino.h>
#include <LowPower.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <LoRa_E220.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include "secrets.h"

// PIN-Definitionen und Konstanten bleiben unverändert
#define TOUCH_PIN 2
#define POWER_PIN 12
#define LED_PIN 13
#define LORA_RX 6
#define LORA_TX 5
#define LORA_AUX 9
#define LEARN_PIN 7

#define DESTINATION_ADDL 2
#define EEPROM_ADDR_ID 0


const bool DEBUG = false;
const bool USE_DEEP_SLEEP = true;

struct __attribute__((packed)) LoRaPayload {
  uint16_t messageID;
  float temperatureInnen, humidityInnen, absHumidityInnen;
  float temperatureAussen, humidityAussen, absHumidityAussen;
  bool  fanOn;
  uint8_t torStatus, fingerID, confidence;
  bool  fingerEventValid;
  uint16_t actionID;
  float batteryVoltage;
  uint8_t wakeupCause; // <-- WICHTIG: Das hat gefehlt!
};

SoftwareSerial fingerSerial(10, 11);
SoftwareSerial loraSerial(LORA_RX, LORA_TX);

LoRa_E220 e220ttl(&loraSerial, LORA_AUX);
Adafruit_Fingerprint finger(&fingerSerial);
Adafruit_INA219 ina219;

volatile bool wakeSignal = false;
uint8_t nextID = 1;

void touchInterrupt() {
  wakeSignal = true;
}

void blinkLED(uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(150);
    digitalWrite(LED_PIN, LOW);
    delay(150);
  }
}

// Führt den Registrierungsprozess für einen neuen Fingerabdruck durch.
bool enrollFinger(uint8_t id) {
  finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 200, 0x02, 0);
  if (DEBUG) Serial.println(F("L: P1")); // GEÄNDERT: "Platzier Finger 1"

  unsigned long startEnrollTime = millis();
  bool fingerPlaced = false;
  do {
    if (finger.getImage() == FINGERPRINT_OK) {
      fingerPlaced = true;
      break;
    }
    delay(50);
  } while (millis() - startEnrollTime < 3000);

  if (!fingerPlaced) {
    if (DEBUG) Serial.println(F("L: T/O1")); // GEÄNDERT: "Timeout 1"
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x01, 0);
    delay(1000);
    finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
    return false;
  }

  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x01, 0);
    delay(1000);
    finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
    return false;
  }
  
  finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x04, 0);
  if (DEBUG) Serial.println(F("L: REM")); // GEÄNDERT: "Entfernen"
  delay(1000);
  
  unsigned long startRemoveTime = millis();
  bool fingerRemoved = false;
  do {
    if (finger.getImage() == FINGERPRINT_NOFINGER) {
      fingerRemoved = true;
      break;
    }
    delay(50);
  } while (millis() - startRemoveTime < 3000);

  if (!fingerRemoved) {
    if (DEBUG) Serial.println(F("L: T/O-R")); // GEÄNDERT: "Timeout Entfernen"
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x01, 0);
    delay(1000);
    finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
    return false;
  }

  finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 200, 0x02, 0);
  if (DEBUG) Serial.println(F("L: P2")); // GEÄNDERT: "Platzier Finger 2"
  
  startEnrollTime = millis();
  fingerPlaced = false;
  do {
    if (finger.getImage() == FINGERPRINT_OK) {
      fingerPlaced = true;
      break;
    }
    delay(50);
  } while (millis() - startEnrollTime < 3000);

  if (!fingerPlaced) {
    if (DEBUG) Serial.println(F("L: T/O2")); // GEÄNDERT: "Timeout 2"
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x01, 0);
    delay(1000);
    finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
    return false;
  }

  if (finger.image2Tz(2) != FINGERPRINT_OK || finger.createModel() != FINGERPRINT_OK || finger.storeModel(id) != FINGERPRINT_OK) {
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x01, 0);
    delay(1000);
    finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
    return false;
  }
  
  finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x04, 0);
  delay(1000);
  finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
  return true;
}

// ======================= SETUP FUNKTION =======================
void setup() {
  pinMode(TOUCH_PIN, INPUT);
  digitalWrite(TOUCH_PIN, HIGH);
  pinMode(POWER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(LORA_AUX, INPUT);
  pinMode(LEARN_PIN, INPUT_PULLUP);

  digitalWrite(POWER_PIN, HIGH);
  digitalWrite(LED_PIN, LOW);

  if (DEBUG) {
    Serial.begin(9600);
    delay(1000);
    Serial.println(F("S: OK"));
  }

  loraSerial.begin(9600);
  e220ttl.begin();
  delay(200);

  nextID = EEPROM.read(EEPROM_ADDR_ID);
  if (nextID == 0xFF || nextID == 0) nextID = 1;

  delay(100);
  if (digitalRead(LEARN_PIN) == LOW) {
    if (DEBUG) Serial.println(F("L: MODE")); // GEÄNDERT
    digitalWrite(POWER_PIN, LOW);
    delay(50);
    fingerSerial.begin(57600);
    finger.begin(57600);
    delay(10);

    if (finger.verifyPassword()) {
      if (enrollFinger(nextID)) {
        if (DEBUG) Serial.println(F("L: OK")); // GEÄNDERT
        nextID++;
        EEPROM.write(EEPROM_ADDR_ID, nextID);
      } else {
        if (DEBUG) Serial.println(F("L: FAIL")); // GEÄNDERT
      }
    } else {
      if (DEBUG) Serial.println(F("F: VFAIL_S")); // GEÄNDERT
      for(int i = 0; i < 6; i++) {
        digitalWrite(LED_PIN, HIGH); delay(200);
        digitalWrite(LED_PIN, LOW); delay(200);
      }
      digitalWrite(POWER_PIN, HIGH);
      attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), touchInterrupt, FALLING);
      if (USE_DEEP_SLEEP) {
        LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
      }
    }
    digitalWrite(POWER_PIN, HIGH);
    if (USE_DEEP_SLEEP) { 
      LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
    }
  }

  attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), touchInterrupt, CHANGE);
  blinkLED(2);
  if (DEBUG) Serial.println(F("S: END")); 
  delay(500);
}

// ======================= LOOP FUNKTION =======================
void loop() {
  
  // 1. ARBEITS-BLOCK: Nur ausführen, wenn wir geweckt wurden
  if (wakeSignal) {
    wakeSignal = false;
    
    // Interrupt deaktivieren, damit uns Mehrfach-Berührungen nicht stören
    detachInterrupt(digitalPinToInterrupt(TOUCH_PIN));
    if (DEBUG) Serial.println(F("I: WAKE"));

    blinkLED(1);

    // Strom AN und Sensor booten lassen
    digitalWrite(POWER_PIN, LOW); 
    
    // Gib ihm ruhig 1 ganze Sekunde für den Boot. Batterie kostet das quasi nichts.
    delay(1000); 

    // I2C starten
    Wire.begin();
    Wire.setWireTimeout(25000, true); 
    ina219.begin();

    // Fingerprint Serial starten
    fingerSerial.begin(57600);
    fingerSerial.listen();
    delay(50); 

    // --- NEU: DIE PUFFER-SPÜLUNG ---
    // Wir werfen allen "Strom-Einschalt-Müll" weg, bevor wir mit dem Sensor reden!
    while (fingerSerial.available()) {
        fingerSerial.read();
    }

    finger.begin(57600);
    delay(50); 

    finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 200, 0x02, 0);
    if (finger.verifyPassword()) {
      if (DEBUG) Serial.println(F("F: VFY"));
      uint8_t result;
      uint32_t start = millis();
      const uint32_t timeout = 3000;

      do {
        result = finger.getImage();
      } while (result != FINGERPRINT_OK && millis() - start < timeout);

      if (result == FINGERPRINT_OK &&
          finger.image2Tz(1) == FINGERPRINT_OK &&
          finger.fingerSearch() == FINGERPRINT_OK) {

        finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x04, 0); 
        if (DEBUG) { Serial.print(F("F: M ")); Serial.println(finger.fingerID); }

        loraSerial.begin(9600);
        delay(200);
        e220ttl.begin();
        delay(200);

        LoRaPayload p = {};
        p.fingerEventValid = true;
        p.fingerID = finger.fingerID;
        p.confidence = finger.confidence;
        p.actionID = NUKI_TRIGGER_ID; // WICHTIG: Das Define muss oben stehen!
        p.batteryVoltage = ina219.getBusVoltage_V();
        p.wakeupCause = 0; // Unser Dummy-Byte für die 38 Bytes

        e220ttl.sendMessage(&p, sizeof(p));
        delay(500); // Dem Modul Zeit zum Senden geben
        
      } else {
        finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x01, 0); 
        delay(1000);
      }
      finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0); 
    } else {
      for(int i = 0; i < 6; i++) {
        digitalWrite(LED_PIN, HIGH); delay(200);
        digitalWrite(LED_PIN, LOW); delay(200);
      }
    }

   // --- STROM ABSCHALTEN & PHANTOM-STROM VERHINDERN ---
    digitalWrite(POWER_PIN, HIGH); 

    // WICHTIG: Alle Kommunikations-Pins komplett tot legen!
    pinMode(LORA_TX, INPUT); digitalWrite(LORA_TX, LOW);
    pinMode(11, INPUT);      digitalWrite(11, LOW); // Fingerprint Arduino TX
    pinMode(10, INPUT);      digitalWrite(10, LOW); // NEU: Fingerprint Arduino RX !!!

    // I2C Hardware hart abschalten und Pins runterziehen
    TWCR = 0; // NEU: Schaltet das TWI (I2C) Register im ATmega komplett ab
    pinMode(A4, INPUT);      digitalWrite(A4, LOW); 
    pinMode(A5, INPUT);      digitalWrite(A5, LOW);

    blinkLED(2);
  }

  // 2. SCHLAF-BLOCK: Wird immer erreicht, wenn die Arbeit erledigt ist
  if (USE_DEEP_SLEEP) {
    // RISING statt CHANGE: Wacht nur auf, wenn der Finger AUFGELEGT wird!
    attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), touchInterrupt, FALLING);
    LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
  }
}