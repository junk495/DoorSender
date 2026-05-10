#include "Arduino.h"
#include "LoRa_E220.h"

//#define MESSAGE_TYPE "open"

// With FIXED SENDER configuration
#define DESTINATION_ADDL 3
#define TODO "open"

// With FIXED RECEIVER configuration
//#define DESTINATION_ADDL 2
//#define ROOM "Bathroo"

// If you want use RSSI uncomment //#define ENABLE_RSSI true
// and use relative configuration with RSSI enabled
// #define ENABLE_RSSI true



#define RXD1 16
#define TXD1 17


// ---------- esp32 pins --------------
LoRa_E220 e220ttl(&Serial2, 18, 21, 19); //  RX AUX M0 M1

#define BUTTON_PIN_BITMASK 0x200000000 // GPIO 33 // 0x8004 // GPIOs 2 and 15
RTC_DATA_ATTR int bootCount = 0;

void print_wakeup_reason();
void print_GPIO_wake_up();
