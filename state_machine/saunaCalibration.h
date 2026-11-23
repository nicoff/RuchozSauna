#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>
#include <stdlib.h>

namespace SaunaCalibration {
struct Point { float raw, temp; };
constexpr int count = 5;
static Point points[count] = {{2429.5, 8}, {1675, 65}, {1420, 75}, {1318.5, 84}, {1135, 100}};
static float slope = -0.069189f, offset = 176.798f;

inline bool fit(const Point (&p)[count], float &m, float &b) {
  double x = 0, y = 0, xx = 0, xy = 0;
  for (int i = 0; i < count; ++i) {
    if (!isfinite(p[i].raw) || !isfinite(p[i].temp) ||
        p[i].raw < 0 || p[i].raw > 4095 ||
        (i && (p[i].raw >= p[i-1].raw || p[i].temp <= p[i-1].temp))) return false;
    x += p[i].raw;
    y += p[i].temp;
  }
  x /= count;
  y /= count;
  for (const Point &v : p) {
    double dx = v.raw - x;
    xx += dx * dx;
    xy += dx * (v.temp - y);
  }
  if (xx <= 0) return false;
  m = xy / xx;
  b = y - m * x;
  return isfinite(m) && isfinite(b) && m < 0 && isfinite(b + 4095.0f * m);
}

inline float temperature(int raw) { return offset + slope * raw; }

inline bool number(const String &s, float &value) {
  const char *start = s.c_str();
  char *end;
  value = strtof(start, &end);
  return end != start && *end == '\0' && isfinite(value);
}

inline void begin(WebServer &web, bool (*isIdle)()) {
  Preferences prefs;
  Point stored[count];
  float m, b;
  if (prefs.begin("sauna-cal", true)) {
    if (prefs.getBytesLength("points-v1") == sizeof stored &&
        prefs.getBytes("points-v1", stored, sizeof stored) == sizeof stored &&
        fit(stored, m, b)) {
      memcpy(points, stored, sizeof points);
      slope = m;
      offset = b;
    }
    prefs.end();
  }
  web.on("/calibration", HTTP_GET, [&web]() {
    String html = F("<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><a href='/'>Sauna</a><h1>Sauna probe calibration</h1>");
    if (web.hasArg("saved")) html += F("<p>Calibration saved.</p>");
    html += F(R"HTML(
<p>Box temperature: <span id="boxTemp">--</span> &deg;C</p>
<p>Sauna sensor (GPIO 34): <span id="saunaRaw">--</span> ADC counts</p>
<p>Box sensor (GPIO 35): <span id="boxRaw">--</span> ADC counts</p>
<p>State: <span id="state">--</span></p>
<p>Oven: <span id="heaterStatus">--</span></p>
<script>
async function refreshReadings() {
  const ids = ['boxTemp','saunaRaw','boxRaw','state','heaterStatus'];
  try {
    const response = await fetch('/status', {cache:'no-store'});
    if (!response.ok) throw new Error();
    const data = await response.json();
    for (const id of ids) document.getElementById(id).textContent = data[id];
  } catch {
    for (const id of ids) document.getElementById(id).textContent = '--';
  }
  setTimeout(refreshReadings, 1000);
}
refreshReadings();
</script>)HTML");
    html += F("<p>Enter the midpoint of each GPIO 34 range and the thermometer reading, coldest first.</p><form method=post action='/calibration'><table><tr><th>ADC counts</th><th>Thermometer (&deg;C)</th></tr>");
    for (int i = 0; i < count; ++i) {
      String row(i);
      html += "<tr><td><input required type=number step=any min=0 max=4095 name=r" + row + " value='" + String(points[i].raw, 3) + "'></td>";
      html += "<td><input required type=number step=any name=t" + row + " value='" + String(points[i].temp, 3) + "'></td></tr>";
    }
    html += F("</table><p><button>Save calibration</button></p></form><p>Current fit (&deg;C) = ");
    html += String(offset, 4) + " + (" + String(slope, 7) + " &times; ADC counts)</p>";
    web.send(200, "text/html; charset=utf-8", html);
  });
  web.on("/calibration", HTTP_POST, [&web, isIdle]() {
    if (!isIdle()) {
      web.send(409, "text/plain", "Stop the sauna session before changing calibration.");
      return;
    }
    Point updated[count];
    for (int i = 0; i < count; ++i) {
      String row(i);
      if (!number(web.arg("r" + row), updated[i].raw) ||
          !number(web.arg("t" + row), updated[i].temp)) {
        web.send(400, "text/plain", "Enter a number in every field.");
        return;
      }
    }
    float newSlope, newOffset;
    if (!fit(updated, newSlope, newOffset)) {
      web.send(400, "text/plain", "Use rising temperatures and decreasing ADC counts from 0 to 4095.");
      return;
    }
    Preferences storage;
    bool saved = false;
    if (storage.begin("sauna-cal", false)) {
      saved = storage.putBytes("points-v1", updated, sizeof updated) == sizeof updated;
      storage.end();
    }
    if (!saved) {
      web.send(500, "text/plain", "Save failed. The previous calibration is still active.");
      return;
    }
    memcpy(points, updated, sizeof points);
    slope = newSlope;
    offset = newOffset;
    web.sendHeader("Location", "/calibration?saved=1");
    web.send(303, "text/plain", "");
  });
}
}
