// SaunaController.ino
// ls /dev/ttyUSB0 
// sudo chmod a+rw /dev/ttyUSB0
// click on the boot button while you compile

// Libraries
#include <Arduino.h>
#include <WiFi.h>

const char* ssid  = "ESP32-Network";
const char* pwd   = "ESP32-pwd";

WiFiServer server(80);
String header;

// Constants
const int TEMP_SENSOR_PIN = 34;  // Analog pin for temperature sensor
int potValue;
const int RELAY_PIN_plus = 26;        // Digital pin for relay control
const int RELAY_PIN_minus = 33;        // Digital pin for relay control
const float TEMP_HIGH_THRESHOLD = 110.0;  // Upper temperature limit (°C)
const float TEMP_LOW_THRESHOLD = 80.0;    // Lower temperature limit (°C)
const int SAFETY_TIMER_PERIOD_MS = 5000;  // Safety watchdog timer (in ms)

// Forward declarations
class SaunaController;

// Timer Variables
unsigned long previousMillis = 0;
unsigned long timerDuration = 0;

// Class Definition
class SaunaController {
  public:
    enum SaunaState {
      OFF,
      HEATING,
      REGIME
    };

    SaunaController(int relayPin, int tempPin) : relayPin(relayPin), tempPin(tempPin), state(OFF), safetyTriggered(false) {
      pinMode(relayPin, OUTPUT);
      turnOffSauna();
    }

    void update(float currentTemp, bool isTimerExpired) {
      // Safety feature to always turn off in case of failure
      if (safetyTriggered) {
        turnOffSauna();
        return;
      }

      switch (state) {
        case OFF:
          turnOffSauna();
          if (!isTimerExpired) {
            state = HEATING;
          }
          break;

        case HEATING:
          if (isTimerExpired) {
            state = OFF;
            turnOffSauna();
          } else if (currentTemp >= TEMP_HIGH_THRESHOLD) {
            turnOffSauna();
            state = REGIME;
          } else {
            turnOnSauna();
          }
          break;

        case RUNNING:
          if (isTimerExpired) {
            state = OFF;
            turnOffSauna();
          } else if (currentTemp <= TEMP_LOW_THRESHOLD) {
            state = HEATING;
            turnOnSauna();
          }
          break;
      }
    }

    void triggerSafetyShutdown() {
      safetyTriggered = true;
      turnOffSauna();
    }

  private:
    int relayPin;
    int tempPin;
    bool safetyTriggered;
    SaunaState state;

    void turnOnSauna() {
      digitalWrite(relayPin, HIGH);  // Turn on relay
    }

    void turnOffSauna() {
      digitalWrite(relayPin, LOW);   // Turn off relay
    }
};

// SaunaController instance
SaunaController sauna(RELAY_PIN_plus, TEMP_SENSOR_PIN);

// Timer management
unsigned long startMillis;
unsigned long saunaDuration = 3600000;  // Example: 1 hour sauna duration in milliseconds

// Function to read temperature (dummy analog reading, replace with actual conversion)
float readTemperature(int tempPin) {
  int rawValue = analogRead(tempPin);  // Read analog value
  float voltage = rawValue * (3.3 / 4095.0);  // Convert to voltage (assuming 3.3V reference)
//  float temperature = (voltage - 0.5) * 100;  // Convert to temperature (dummy conversion)
  float temperature = 0.035087719*rawValue - 11.929824561;
  return temperature;
}

// Safety watchdog timer
void checkSafetyTimer() {
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= SAFETY_TIMER_PERIOD_MS) {
    previousMillis = currentMillis;
    // If sauna is still on but should be off, trigger safety shutdown
    if (digitalRead(RELAY_PIN_plus) == HIGH) {
      sauna.triggerSafetyShutdown();
    }
  }
}

void setup() {
  // Start Serial for debugging
  Serial.begin(115200);

  // Initialize relay and sensor pins
  pinMode(RELAY_PIN_minus, OUTPUT);
  pinMode(RELAY_PIN_plus, OUTPUT);
  digitalWrite(RELAY_PIN_minus, LOW);  // Make sure sauna is off at startup
  digitalWrite(RELAY_PIN_plus, HIGH);  // Make sure sauna is off at startup

  // Initialize the start of sauna timer
  startMillis = millis();
}

void loop() {
//  /*
    // Timer-based condition to determine whether sauna time is up
    unsigned long currentMillis = millis();
    bool isTimerExpired = (currentMillis - startMillis >= saunaDuration);

    // Read current temperature from the sensor
//    int rawValue = analogRead(TEMP_SENSOR_PIN);  // Read analog value
    float currentTemp = readTemperature(TEMP_SENSOR_PIN);
    Serial.print("Current temperature: ");
    Serial.println(currentTemp);
//    Serial.print("Current millis: ");
//    Serial.println(currentMillis);
    if (currentTemp<10) {
      digitalWrite(RELAY_PIN_plus, HIGH); // Turn on relay
      digitalWrite(RELAY_PIN_minus, LOW); // Turn on relay
    } else {
      digitalWrite(RELAY_PIN_plus, LOW);  // Turn off relay
      digitalWrite(RELAY_PIN_minus, HIGH);  // Turn off relay
    }

    delay(1000);

    // Update sauna control based on the temperature and timer
    sauna.update(80, isTimerExpired);

    // Check safety timer
    checkSafetyTimer();
//  */

}
