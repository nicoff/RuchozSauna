#include <Arduino.h>
#include <WiFi.h>           // WiFi library for ESP32
#include <WebServer.h>      // Web server library for ESP32
#include <esp_task_wdt.h>   // Include this for ESP32 watchdog functions

// Constants for pins
const int heaterPin = 26;        // GPIO pin to control heater
const int saunaTempPin = 34;     // GPIO pin for sauna temperature sensor (analog input)
const int boxTempPin = 35;       // GPIO pin for electrical box temperature sensor (analog input)
const int watchdogTimeout = 60000; // 60 seconds watchdog timeout

// User-specified parameters
float desiredTemperature = 95.0;    // Desired temperature in Celsius
float temperatureTolerance = 5.0;   // Allowable tolerance in Celsius
unsigned long maxSaunaTime = 7200000; // Maximum sauna session time (2 hours)

// FreeRTOS Task Handles
TaskHandle_t timeControlTaskHandle;

// WiFi credentials
const char* ssid = "VillaRuchoz HighSpeed";
const char* password = "Flocki1nTheHouse";

// Web server instance on port 80
WebServer server(80);

// Sauna controller class
class SaunaController {
  public:
    enum SaunaState {
      IDLE,
      HEATING,
      SAFETY_CUTOFF
    };

    SaunaController(int heaterPin, int saunaTempPin, int boxTempPin)
        : heaterPin(heaterPin), saunaTempPin(saunaTempPin), boxTempPin(boxTempPin), state(IDLE), heaterState(false), measurementIndex(0), totalTemperature(0.0) {
      pinMode(heaterPin, OUTPUT);
      digitalWrite(heaterPin, LOW);  // Ensure heater is off at the start

      // Initialize the temperature measurements array to 10
      for (int i = 0; i < 10; i++) {
        temperatureMeasurements[i] = 10.0; // Default initialization
        totalTemperature += temperatureMeasurements[i];
      }
    }

    void initializeParameters(float defaultTemp, float defaultTolerance, unsigned long defaultMaxTime) {
      // Set up initial parameters without starting heating
      desiredTemperature = defaultTemp;
      temperatureTolerance = defaultTolerance;
      safetyTimeLimit = defaultMaxTime;
      state = IDLE;  // Ensure state is IDLE
    }

    void beginSession(float desiredTemp, float tolerance, unsigned long maxTime) {
      desiredTemperature = desiredTemp;
      temperatureTolerance = tolerance;
      safetyTimeLimit = maxTime;
      sessionStartTime = millis();
      state = HEATING;
    }

    void update() {
      // Get the new raw sauna temperature reading and update moving average
      float newTemp = getSaunaTemperature();
      updateMovingAverage(newTemp);  // Update the moving average with the new temperature

      // Use the average sauna temperature for control and display
      float saunaTemp = getAverageTemperature();
      float boxTemp = getBoxTemperature();

      if (state == HEATING) {
        if (millis() - sessionStartTime > safetyTimeLimit) {
          state = SAFETY_CUTOFF;
        } else if (saunaTemp >= desiredTemperature + temperatureTolerance) {
          turnHeaterOff();
        } else if (saunaTemp <= desiredTemperature - temperatureTolerance) {
          turnHeaterOn();
        }

        if (boxTemp > 50) { // Safety cutoff for high box temperature
          state = SAFETY_CUTOFF;
        }
      }

      if (state == SAFETY_CUTOFF || state == IDLE) {
        turnHeaterOff();
      }

      // Print temperature, raw value, and state information
      int saunaRawValue = analogRead(saunaTempPin);
      int boxRawValue = analogRead(boxTempPin);
      Serial.print("[");
      Serial.print(millis());
      Serial.print(" ms] Sauna Temp (Avg): ");
      Serial.print(saunaTemp);
      Serial.print(" C (Raw: ");
      Serial.print(saunaRawValue);
      Serial.print("), Box Temp: ");
      Serial.print(boxTemp);
      Serial.print(" C (Raw: ");
      Serial.print(boxRawValue);
      Serial.print("), State: ");
      Serial.println(getStateString());
    }

    void stopSession() {
      state = IDLE;
      turnHeaterOff();
    }

    String getStateString() const {
      switch (state) {
        case IDLE: return "IDLE";
        case HEATING: return "HEATING";
        case SAFETY_CUTOFF: return "SAFETY_CUTOFF";
        default: return "UNKNOWN";
      }
    }

    // Accessor methods to expose private members to other functions
    SaunaState getState() const { return state; }
    float getDesiredTemperature() const { return desiredTemperature; }
    float getTemperatureTolerance() const { return temperatureTolerance; }
    bool getHeaterState() const { return heaterState; }
    unsigned long getSessionStartTime() const { return sessionStartTime; }
    unsigned long getRemainingTime() const { return safetyTimeLimit - (millis() - sessionStartTime); }
    unsigned long getElapsedTime() const { return millis() - sessionStartTime; }
    String formatTime(unsigned long ms) const {
      unsigned long seconds = ms / 1000;
      unsigned long minutes = seconds / 60;
      unsigned long hours = minutes / 60;
      seconds = seconds % 60;
      minutes = minutes % 60;
      return String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s";
    }

    float getSaunaTemperature() {
      int rawValue = analogRead(saunaTempPin);  // Read analog value
      return -0.0556 * rawValue + 145.03;  // Updated conversion based on recalibration
    }

    float getBoxTemperature() {
      int analogValue = analogRead(boxTempPin);
      return map(analogValue, 0, 4095, 20, 80);  // Convert to Celsius
    }

    float getAverageTemperature() const {
      return totalTemperature / 10.0;  // Return the moving average
    }

    void updateMovingAverage(float newTemp) {
      // Subtract the oldest temperature from the total
      totalTemperature -= temperatureMeasurements[measurementIndex];
      
      // Replace it with the new temperature and add it to the total
      temperatureMeasurements[measurementIndex] = newTemp;
      totalTemperature += newTemp;

      // Move to the next position, wrapping around to 0 if we reach the end
      measurementIndex = (measurementIndex + 1) % 10;
    }

  private:
    int heaterPin;
    int saunaTempPin;
    int boxTempPin;
    SaunaState state;
    unsigned long sessionStartTime;
    unsigned long safetyTimeLimit;
    float desiredTemperature;
    float temperatureTolerance;
    bool heaterState;

    // Moving average variables
    float temperatureMeasurements[10];  // Array to hold the last 10 measurements
    int measurementIndex;               // Current index for the moving average
    float totalTemperature;             // Sum of temperatures in the array for quick average calculation

    void turnHeaterOn() {
      digitalWrite(heaterPin, HIGH);
      heaterState = true;
      Serial.println("Heater ON");
    }

    void turnHeaterOff() {
      digitalWrite(heaterPin, LOW);
      heaterState = false;
      Serial.println("Heater OFF");
    }
};

// Instantiate sauna controller
SaunaController sauna(heaterPin, saunaTempPin, boxTempPin);

// Task to monitor time and stop sauna after max time
void timeControlTask(void *parameter) {
  for (;;) {
    if (millis() - sauna.getSessionStartTime() > maxSaunaTime) {
      sauna.stopSession();
      Serial.println("Max sauna time reached, stopping session.");
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Check every second
  }
}

// Handle the root URL of the web server (with user inputs and controls)
void handleRoot() {
  String html = "<html><body>";
  html += "<h1>Sauna Ruchoz... What can possibly go wrong?!</h1>";
  html += "<p>Sauna Temperature: <span id='saunaTemp'></span> C</p>";
  html += "<p>Box Temperature: <span id='boxTemp'></span> C</p>";
  html += "<p>State: <span id='state'></span></p>";
  html += "<p>Oven: <span id='heaterStatus'></span></p>";
  
  if (sauna.getState() != SaunaController::IDLE) {
    html += "<p>Set Temperature: <span id='desiredTemp'></span> + <span id='tolerance'></span> C</p>";
    html += "<p>Time Running: <span id='elapsedTime'></span></p>";
    html += "<p>Time Left: <span id='remainingTime'></span></p>";
  }

  // Form to set temperature, tolerance, and time
  html += "<h2>Set New Session Parameters</h2>";
  html += "<form action='/setParams' method='POST'>";
  html += "Desired Temperature: <input type='number' name='desiredTemp' value='" + String(sauna.getDesiredTemperature()) + "'> C<br>";
  html += "Tolerance: <input type='number' name='tolerance' value='" + String(sauna.getTemperatureTolerance()) + "'> C<br>";
  html += "Total Time (minutes): <input type='number' name='sessionTime' value='" + String(maxSaunaTime / 60000) + "'><br>";
  html += "<input type='submit' value='Set & Restart Session'>";
  html += "</form><br>";

  // Button to stop the session
  html += "<form action='/stopSession' method='POST'>";
  html += "<input type='submit' value='Stop Session'>";
  html += "</form>";

  // JavaScript for updating the page every second
  html += "<script>";
  html += "setInterval(function() {";
  html += "  fetch('/status').then(response => response.json()).then(data => {";
  html += "    document.getElementById('saunaTemp').innerText = data.saunaTemp;";
  html += "    document.getElementById('boxTemp').innerText = data.boxTemp;";
  html += "    document.getElementById('state').innerText = data.state;";
  html += "    document.getElementById('heaterStatus').innerText = data.heaterStatus;";
  if (sauna.getState() != SaunaController::IDLE) {
    html += "    document.getElementById('desiredTemp').innerText = data.desiredTemp;";
    html += "    document.getElementById('tolerance').innerText = data.tolerance;";
    html += "    document.getElementById('elapsedTime').innerText = data.elapsedTime;";
    html += "    document.getElementById('remainingTime').innerText = data.remainingTime;";
  }
  html += "  });";
  html += "}, 1000);"; // Update every second
  html += "</script>";
  html += "</body></html>";

  // Send the response
  server.send(200, "text/html", html);
}

// Handle AJAX request for JSON data
void handleStatus() {
  float saunaTemp = sauna.getAverageTemperature(); // Use moving average for the displayed temp
  float boxTemp = sauna.getBoxTemperature();
  String stateString = sauna.getStateString();
  String heaterStatus = sauna.getHeaterState() ? "On" : "Off";
  String elapsedTime = sauna.getState() == SaunaController::IDLE ? "" : sauna.formatTime(sauna.getElapsedTime());
  String remainingTime = sauna.getState() == SaunaController::IDLE ? "" : sauna.formatTime(sauna.getRemainingTime());
  float desiredTemp = sauna.getDesiredTemperature();
  float tolerance = sauna.getTemperatureTolerance();

  // Send JSON response
  String json = "{\"saunaTemp\":" + String(saunaTemp) +
                ",\"boxTemp\":" + String(boxTemp) +
                ",\"state\":\"" + stateString + "\"," +
                "\"heaterStatus\":\"" + heaterStatus + "\"," +
                "\"desiredTemp\":" + String(desiredTemp) + "," +
                "\"tolerance\":" + String(tolerance) + "," +
                "\"elapsedTime\":\"" + elapsedTime + "\"," +
                "\"remainingTime\":\"" + remainingTime + "\"}";
  server.send(200, "application/json", json);
}

// Handle the form submission to set new session parameters
void handleSetParams() {
  if (server.hasArg("desiredTemp") && server.hasArg("tolerance") && server.hasArg("sessionTime")) {
    float newTemp = server.arg("desiredTemp").toFloat();
    float newTolerance = server.arg("tolerance").toFloat();
    unsigned long newTime = server.arg("sessionTime").toInt() * 60000;

    // Restart session with new parameters
    sauna.beginSession(newTemp, newTolerance, newTime);
    maxSaunaTime = newTime;
    handleRoot();  // Stay on the same page after setting new params
  }
}

// Handle stopping the session
void handleStopSession() {
  sauna.stopSession();
  handleRoot();  // Stay on the same page after stopping the session
}

void setup() {
  Serial.begin(115200);

  // Initialize parameters without starting heating
  sauna.initializeParameters(desiredTemperature, temperatureTolerance, maxSaunaTime);

  // WiFi setup
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }

  Serial.println("\nConnected!");
  Serial.print("Web server started, visit http://");
  Serial.println(WiFi.localIP());

  // Start the web server
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/setParams", handleSetParams);
  server.on("/stopSession", handleStopSession);
  server.begin();
}

void loop() {
  sauna.update();

  // Feed the watchdog to ensure loop is running
  esp_task_wdt_reset();

  // Handle web server requests
  server.handleClient();

  // Simulate delay for other processes
  delay(500);
}
