// =====================================================================================
// Tuersender (code4)
// -------------------------------------------------------------------------------------
// Letzte Änderung: 10. Mai 2026 (Agiler UART-Boot, Retry-Logik, AUX-Check & Multitasking)
// Hardware:          Arduino Pro Mini (3.3V, 16MHz, ATmega168), INA219 Sensor
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

// PIN-Definitionen und Konstanten
#define TOUCH_PIN 2
#define POWER_PIN 12
#define LED_PIN 13
#define LORA_RX 6
#define LORA_TX 5
#define LORA_AUX 9
#define LEARN_PIN 7

#define DESTINATION_ADDL 2
#define EEPROM_ADDR_ID 0

const bool DEBUG = true;
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
  uint8_t wakeupCause; // <-- WICHTIG: Das hat gefehlt! Füllt das Paket auf exakt 38 Bytes.
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
  if (DEBUG) Serial.println(F("L: P1")); 

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
    if (DEBUG) Serial.println(F("L: T/O1")); 
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
  if (DEBUG) Serial.println(F("L: REM")); 
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
    if (DEBUG) Serial.println(F("L: T/O-R")); 
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x01, 0);
    delay(1000);
    finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
    return false;
  }

  finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 200, 0x02, 0);
  if (DEBUG) Serial.println(F("L: P2")); 
  
  startEnrollTime = millis();
  bool fingerPlaced2 = false;
  do {
    if (finger.getImage() == FINGERPRINT_OK) {
      fingerPlaced2 = true;
      break;
    }
    delay(50);
  } while (millis() - startEnrollTime < 3000);

  if (!fingerPlaced2) {
    if (DEBUG) Serial.println(F("L: T/O2")); 
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
    if (DEBUG) Serial.println(F("L: MODE")); 
    digitalWrite(POWER_PIN, LOW);
    delay(50);
    fingerSerial.begin(57600);
    finger.begin(57600);
    delay(10);

    if (finger.verifyPassword()) {
      if (enrollFinger(nextID)) {
        if (DEBUG) Serial.println(F("L: OK")); 
        nextID++;
        EEPROM.write(EEPROM_ADDR_ID, nextID);
      } else {
        if (DEBUG) Serial.println(F("L: FAIL")); 
      }
    } else {
      if (DEBUG) Serial.println(F("F: VFAIL_S")); 
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

  // WICHTIG: Einheitlich auf FALLING geändert
  attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), touchInterrupt, FALLING);
  blinkLED(2);
  if (DEBUG) Serial.println(F("S: END")); 
  delay(500);
}

// ======================= LOOP FUNKTION =======================
void loop() {
  
  if (wakeSignal) {
    wakeSignal = false;
    
    // Interrupt deaktivieren, damit uns Mehrfach-Berührungen nicht stören
    detachInterrupt(digitalPinToInterrupt(TOUCH_PIN));
    if (DEBUG) Serial.println(F("I: WAKE"));

    // --- 1. STROM SOFORT AN (jede Millisekunde zählt!) ---
    // Bevor der Sensor Strom bekommt, MÜSSEN die Datenleitungen auf HIGH liegen.
    pinMode(11, OUTPUT); 
    digitalWrite(11, HIGH); 
    pinMode(10, INPUT_PULLUP); 
    
    digitalWrite(POWER_PIN, LOW); // Sensor bootet ab exakt JETZT!
    unsigned long bootStartTime = millis();

    // --- 2. MULTITASKING WÄHREND DEM BOOTEN ---
    // Wir nutzen die Hochfahr-Zeit des Sensors, um das Arduino-Blinken 
    // und den I2C-Start abzuarbeiten. Das spart uns 300ms reine Wartezeit!
    Wire.begin();
    Wire.setWireTimeout(25000, true); 
    ina219.begin();

    // Arduino Status-LED blinken (versteckt sich in der Sensor-Boot-Zeit)
    digitalWrite(LED_PIN, HIGH);
    delay(150);
    digitalWrite(LED_PIN, LOW);

    // --- 3. RESTLICHE BOOT-ZEIT ABWARTEN ---
    // Der Sensor braucht ca. 800ms. Wir ziehen die Zeit ab, die das Blinken gekostet hat.
    unsigned long elapsed = millis() - bootStartTime;
    if (elapsed < 800) {
        delay(800 - elapsed);
    }

    finger.begin(57600); // Ruft intern fingerSerial.begin() auf
    fingerSerial.listen();
    delay(50); 

    // Puffer spülen (Einschalt-Müll sicher entfernen)
    while (fingerSerial.available()) {
        fingerSerial.read();
    }

    // --- 4. AGILERER HANDSHAKE & LED SOFORT AN ---
    // 4 schnelle Versuche (a 150ms), um den allerersten wachen Moment zu erwischen!
    bool sensorReady = false;
    for (int i = 0; i < 4; i++) {
      if (finger.verifyPassword()) {
        // JAAAA! Sensor ist wach -> SOFORT DIE LED EINSCHALTEN!
        finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 200, 0x02, 0);
        sensorReady = true;
        break; 
      }
      delay(150); 
    }

    if (sensorReady) {
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
        p.actionID = NUKI_TRIGGER_ID; 
        p.batteryVoltage = ina219.getBusVoltage_V();
        p.wakeupCause = 0; // Unser Dummy-Byte für die exakten 38 Bytes

        e220ttl.sendMessage(&p, sizeof(p));
        
        // --- PROFI-LÖSUNG ZUM SENDEN ---
        // Anstatt fix 500ms zu warten, warten wir genau so lange, bis das LoRa Modul 
        // über den AUX-Pin meldet: "Ich habe fertig gesendet!" (Max. 1,5 Sekunden Timeout)
        unsigned long waitStart = millis();
        while (digitalRead(LORA_AUX) == LOW && (millis() - waitStart < 1500)) {
          delay(10);
        }
        
      } else {
        finger.LEDcontrol(FINGERPRINT_LED_ON, 0, 0x01, 0); 
        delay(1000);
      }
      finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0); 
      
    } else {
      // Sensor hat trotz 4 Versuchen nicht geantwortet
      if (DEBUG) Serial.println(F("F: FAIL BOOT"));
      for(int i = 0; i < 6; i++) {
        digitalWrite(LED_PIN, HIGH); delay(200);
        digitalWrite(LED_PIN, LOW); delay(200);
      }
    }

    // --- 5. STROM ABSCHALTEN & PHANTOM-STROM VERHINDERN ---
    digitalWrite(POWER_PIN, HIGH); 

    // WICHTIG: Nur Fingerprint-Pins komplett tot legen (da dieser stromlos ist)
    // LORA_TX wird NICHT angefasst, da das LoRa-Modul unter Strom bleibt!
    pinMode(11, INPUT);      digitalWrite(11, LOW); 
    pinMode(10, INPUT);      digitalWrite(10, LOW); 

    // I2C Hardware hart abschalten und Pins runterziehen
    TWCR = 0; 
    pinMode(A4, INPUT);      digitalWrite(A4, LOW); 
    pinMode(A5, INPUT);      digitalWrite(A5, LOW);

    blinkLED(2);
  }

  // SCHLAF-BLOCK
  if (USE_DEEP_SLEEP) {
    attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), touchInterrupt, FALLING);
    LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
  }
}