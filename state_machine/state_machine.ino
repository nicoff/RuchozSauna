#include <Arduino.h>
#include "SaunaCalibration.h"
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
float temperatureTolerance = 2.0;   // Allowable tolerance in Celsius
unsigned long maxSaunaTime = 2UL * 3600000UL;                // Default session: 2 hours
const unsigned long hardcodedMaxSaunaTime = 5UL * 3600000UL; // Maximum per session: 5 hours
const unsigned long maxSessionMinutes = hardcodedMaxSaunaTime / 60000UL;
const float maxSaunaTempC = 120.0f;

// FreeRTOS Task Handles
TaskHandle_t timeControlTaskHandle;

// WiFi credentials
const char* ssid = "VillaRuchoz HighSpeed";
const char* password = "Flocki1nTheHouse";

// Web server instance on port 80
WebServer server(80);

int16_t history[120];  // Tenths of a degree, 240 bytes total
uint32_t historySeconds[120];
uint16_t historyRaw[120];
unsigned long historyStartMillis = 0;
uint8_t historyCount = 0;
unsigned long sampleInterval = 60000;
unsigned long lastSample;

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
      historyCount = 0;
      historyStartMillis = sessionStartTime;
      sampleInterval = maxTime / 119 + 1;
      lastSample = sessionStartTime;
    }

    void update() {
      // Get the new raw sauna temperature reading and update moving average
      int saunaRawValue = analogRead(saunaTempPin);
      float newTemp = SaunaCalibration::temperature(saunaRawValue);
      updateMovingAverage(newTemp);  // Update the moving average with the new temperature

      // Use the average sauna temperature for control and display
      float saunaTemp = getAverageTemperature();
      float boxTemp = getBoxTemperature();
      
      unsigned long now = millis();
      if (historyCount < 120 && (historyCount == 0 || now - lastSample >= sampleInterval)) {
        historySeconds[historyCount] = (now - historyStartMillis) / 1000UL;
        history[historyCount] = lroundf(saunaTemp * 10);
        historyRaw[historyCount] = saunaRawValue;
        ++historyCount;
        lastSample = now;
      }

      if (state == HEATING) {

        
        if (!isfinite(newTemp) || newTemp >= maxSaunaTempC || boxTemp > 50 ||
            millis() - sessionStartTime >= safetyTimeLimit) {              
        //if (millis() - sessionStartTime > safetyTimeLimit) {
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
  html += "<p>Sauna Temperature: <span id='saunaTemp'></span> &deg;C</p>";

  if (sauna.getState() != SaunaController::IDLE) {
    html += "<p>Set Temperature: <span id='desiredTemp'></span> &plusmn; <span id='tolerance'></span> &deg;C</p>";
    html += "<p>Time Running: <span id='elapsedTime'></span></p>";
    html += "<p>Time Left: <span id='remainingTime'></span></p>";
  }

  html += "<h2>Set New Session Parameters</h2>";
  html += "<form action='/setParams' method='POST'>";
  html += "Desired Temperature: <input type='number' name='desiredTemp' value='" + String(sauna.getDesiredTemperature()) + "'> &deg;C<br>";
  html += "Tolerance: <input type='number' name='tolerance' value='" + String(sauna.getTemperatureTolerance()) + "'> &deg;C<br>";
//  html += "Total Time (minutes): <input type='number' name='sessionTime' value='" + String(maxSaunaTime / 60000) + "'><br>";
  html += "Total Time (minutes): <input type='number' name='sessionTime' min='1' max='" +
        String(maxSessionMinutes) + "' step='1' required value='" +
        String(maxSaunaTime / 60000UL) + "'><br>";
  html += "<input type='submit' value='Set &amp; Restart Session'>";
  html += "</form><br>";

  html += "<form action='/stopSession' method='POST'>";
  html += "<input type='submit' value='Stop Session'>";
  html += "</form>";

  html += R"HTML(
    <script>
    async function refreshStatus() {
      const response = await fetch('/status', {cache: 'no-store'});
      if (!response.ok) return;
      const data = await response.json();

      for (const id of ['saunaTemp', 'desiredTemp', 'tolerance', 'elapsedTime', 'remainingTime']) {
        const element = document.getElementById(id);
        if (element) element.textContent = data[id];
      }
    }
    refreshStatus();
    setInterval(refreshStatus, 1000);
    </script>

    <p>Sauna temperature history</p>
    <svg viewBox="0 0 360 172" style="width:100%;max-width:460px"
         role="img" aria-label="Sauna temperature in degrees Celsius over time">
      <g fill="none" stroke="#aaa">
        <path d="M44 24V126H342"/>
        <path d="M44 24H342 M44 75H342" stroke-dasharray="3 4"/>
      </g>
      <g font-family="sans-serif" font-size="11" fill="currentColor">
        <text x="8" y="14">&deg;C</text>
        <g text-anchor="end">
          <text id="yHi" x="39" y="28"></text>
          <text id="yMid" x="39" y="79"></text>
          <text id="yLo" x="39" y="130"></text>
        </g>
        <text x="44" y="143">0</text>
        <text id="xMid" x="193" y="143" text-anchor="middle"></text>
        <text id="xEnd" x="342" y="143" text-anchor="end"></text>
        <text x="193" y="165" text-anchor="middle">Elapsed minutes</text>
      </g>
      <polyline id="curve" fill="none" stroke="#1769aa" stroke-width="2"/>
      <circle id="latest" r="3" fill="#1769aa" visibility="hidden"/>
    </svg>

    <script>
    async function plot() {
      const response = await fetch('/history.csv', {cache: 'no-store'});
      if (!response.ok) return;

      const body = (await response.text()).trim();
      const samples = body.split('\n').slice(1).map(line => line.split(',').map(Number));
      const temps = samples.map(sample => sample[1]);
      const el = id => document.getElementById(id);

      const lo = temps.length ? 5 * Math.floor((Math.min(...temps) - 1) / 5) : 0;
      const hi = temps.length ? 5 * Math.ceil((Math.max(...temps) + 1) / 5) : 10;
      const lastSeconds = samples.length ? samples[samples.length - 1][0] : 0;
      const minutes = Math.max(1, Math.ceil(lastSeconds / 60));

      el('yLo').textContent = lo;
      el('yMid').textContent = (lo + hi) / 2;
      el('yHi').textContent = hi;
      el('xMid').textContent = minutes / 2;
      el('xEnd').textContent = minutes;

      const x = i => 44 + 298 * samples[i][0] / (60 * minutes);
      const y = t => 126 - 102 * (t - lo) / (hi - lo);
      const points = temps.map((t, i) => x(i) + ',' + y(t));
      el('curve').setAttribute('points', points.join(' '));

      const last = temps.length - 1;
      el('latest').setAttribute('visibility', last >= 0 ? 'visible' : 'hidden');
      if (last >= 0) {
        el('latest').setAttribute('cx', x(last));
        el('latest').setAttribute('cy', y(temps[last]));
      }
    }
    plot();
    setInterval(plot, 30000);
    </script>

    <p><a href="/history">Temperature history</a></p>
    <p><a href="/calibration">Calibrate sauna probe</a></p>
    </body></html>
  )HTML";

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

  // Add the raw data to the print
  json.remove(json.length() - 1);  // Remove the closing brace
  json += ",\"saunaRaw\":" + String(analogRead(saunaTempPin));
  json += ",\"boxRaw\":" + String(analogRead(boxTempPin)) + "}";                

  server.send(200, "application/json", json);
}

// Handle the form submission to set new session parameters
void handleSetParams() {
  if (server.hasArg("desiredTemp") && server.hasArg("tolerance") && server.hasArg("sessionTime")) {

    
    float newTemp, newTolerance;
    if (!SaunaCalibration::number(server.arg("desiredTemp"), newTemp) ||
        !SaunaCalibration::number(server.arg("tolerance"), newTolerance) ||
        newTolerance < 0 || newTemp + newTolerance > maxSaunaTempC) {
      server.send(400, "text/plain",
                  "Enter a valid temperature and a non-negative tolerance. "
                  "Temperature plus tolerance must be at most 120 C.");
      return;
    }    
    
    
//    float newTemp = server.arg("desiredTemp").toFloat();
//    float newTolerance = server.arg("tolerance").toFloat();


    String input = server.arg("sessionTime");
    input.trim();
    
    unsigned long minutes = 0;
    for (size_t i = 0; i < input.length(); ++i) {
      char c = input[i];
      if (c < '0' || c > '9') {
        server.send(400, "text/plain", "Enter a whole number of minutes.");
        return;
      }
    
      minutes = minutes * 10UL + (c - '0');
      if (minutes > maxSessionMinutes) {
        server.send(400, "text/plain", String("Maximum session time is ") + (maxSessionMinutes / 60UL) +
                    " hours and " + (maxSessionMinutes % 60UL) + " minutes.");
        return;
      }
    }
    
    if (minutes == 0) {
      server.send(400, "text/plain", "Minimum session time is 1 minute.");
      return;
    }
    
    unsigned long newTime = minutes * 60000UL;



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


void lockHeaterOff(const char* reason) {
  sauna.stopSession();  // Commands GPIO 26 LOW
  Serial.println(reason);
  for (;;) {
    delay(1000);        // No web requests or new heating sessions
  }
}

bool startLoopWatchdog() {
  esp_task_wdt_config_t config = {};
  config.timeout_ms = watchdogTimeout;
  config.trigger_panic = true;

  // Preserve the idle-task monitoring selected by the board configuration.
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
  config.idle_core_mask |= 1U;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
  config.idle_core_mask |= 2U;
#endif

  esp_err_t result = esp_task_wdt_reconfigure(&config);
  if (result == ESP_ERR_INVALID_STATE) {
    result = esp_task_wdt_init(&config);
  }
  if (result != ESP_OK) return false;

  // setup() and loop() run in the same Arduino task.
  if (esp_task_wdt_status(NULL) == ESP_OK) return true;
  return esp_task_wdt_add(NULL) == ESP_OK;
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
  server.on("/history.csv", HTTP_GET, []() {
    String csv;
    csv.reserve(32 + historyCount * 24);
    csv = "seconds,temp_c,gpio34_raw\n";
    for (uint8_t i = 0; i < historyCount; ++i) {
      csv += String(historySeconds[i]);
      csv += ',';
      csv += String(history[i] / 10.0f, 1);
      csv += ',';
      csv += String(historyRaw[i]);
      csv += '\n';
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/csv; charset=utf-8", csv);
  });
  server.on("/history", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", R"HTML(
<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sauna history</title>
<style>
body {font:1rem system-ui;margin:1rem} th,td {text-align:right;padding:.4rem;border-bottom:1px solid #ccc}
table {border-collapse:collapse}
</style>
<p><a href="/">Home</a> &middot; <a href="/history.csv" download>Download CSV</a></p>
<h1>Sauna history</h1>
<table><thead><tr><th>Elapsed (s)</th><th>Sauna (&deg;C)</th><th>GPIO 34 (counts)</th></tr></thead>
<tbody id="rows"></tbody></table>
<p id="message"></p>
<script>
async function refreshHistory() {
  const message = document.getElementById('message');
  try {
    const response = await fetch('/history.csv', {cache:'no-store'});
    if (!response.ok) throw new Error();
    const lines = (await response.text()).trim().split('\n').slice(1);
    const rows = document.getElementById('rows');
    rows.textContent = '';
    for (const line of lines) {
      const row = rows.insertRow();
      for (const value of line.split(',')) row.insertCell().textContent = value;
    }
    message.textContent = lines.length ? '' : 'No samples yet.';
  } catch {
    message.textContent = 'Could not refresh history.';
  }
  setTimeout(refreshHistory, 30000);
}
refreshHistory();
</script>)HTML");
  });
  SaunaCalibration::begin(server, []() {
    return sauna.getState() == SaunaController::IDLE;
  });
  if (!startLoopWatchdog()) {
    lockHeaterOff("Watchdog setup failed. Heater disabled.");
  }
  server.begin();
}


void loop() {
  sauna.update();
  server.handleClient();

  // Test Watchdog:
  // if (Serial.read() == 'W') delay(90000);  // Temporary watchdog test
  if (esp_task_wdt_reset() != ESP_OK) {
    lockHeaterOff("Watchdog feed failed. Heater disabled.");
  }

  delay(500);
}
