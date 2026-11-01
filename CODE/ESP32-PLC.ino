// Web control panel
// - Joins your WiFi and serves a control page at http://hvac-board.local (or the IP shown in the serial monitor)
// - Shows TH1/TH2 temperatures (F), DI1-DI4 states, RSSI
// - Lets you switch DO1-DO5 and set the 0-10V analog output
//
// Board: ESP32 Dev Module. Uses only libraries that ship with the ESP32 Arduino core.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// ---------- CHANGE THESE ----------
const char* WIFI_SSID  = "WIFI_NETWORK";  //CHANGE to correct SSID
const char* WIFI_PASS  = "PASSWORD";  //CHANGE to correct password
const char* HOSTNAME   = "ESP32 PLC";   // http://esp32-PLC.local
const char* PANEL_USER = "admin";
const char* PANEL_PASS = "password";    // pick your own password
// ----------------------------------

// ---------- NAMES SHOWN ON THE PAGE (edit the text between the quotes) ----------
const char* TEMP_NAMES[2]  = {"TH1", "TH2"};          // TH1, TH2
const char* INPUT_NAMES[4] = {"DI1", "DI2", "DI3", "DI4"};           // DI1 - DI4
const char* RELAY_NAMES[5] = {"DO1", "DO2", "DO3", "DO4", "DO5"}; // DO1 - DO5
// ---------------------------------------------------------------------------------

// Pins (from the schematic)
const int DI_PINS[4] = {32, 33, 34, 35};         // dry-contact inputs, LOW = closed
const int DO_PINS[5] = {4, 18, 19, 21, 22};      // ULN2003 inputs -> relays 1-5
const int TH_PINS[2] = {36, 39};                 // TH1 = SENSOR_VP, TH2 = SENSOR_VN
const int DAC_PIN    = 25;                       // DAC1 -> op-amp -> AO1

// Thermistor (10K Type 2, beta 3977) with 3.3V -> thermistor -> node -> 10k -> GND
const float R_FIXED = 10000.0;
const float VCC_MV  = 3300.0;                    // measure your 3.3V rail and adjust
const float R0      = 10000.0;
const float BETA    = 3977.0;
const float T0_K    = 298.15;
const float OFFSET_F[2] = {0.0, 0.0};            // calibration offsets in degrees F

// Analog output
const float FULL_SCALE_V = 10.56;                // 3.3 V * gain 3.2; adjust after measuring
const float MAX_VOLTS    = 10.0;                 // hard cap, never exceeded

WebServer server(80);

bool  doState[5] = {false, false, false, false, false};
float aoVolts = 0.0;
float thF[2] = {NAN, NAN};
unsigned long lastSensor = 0;
unsigned long lastWifiTry = 0;

// ---------- hardware helpers ----------
float readTempF(int ch) {
  long sum = 0;
  for (int i = 0; i < 32; i++) {
    sum += analogReadMilliVolts(TH_PINS[ch]);
    delay(2);
  }
  float mv = sum / 32.0;
  if (mv < 50 || mv > 3000) return NAN;          // open / shorted / out of range
  float rTherm = R_FIXED * (VCC_MV - mv) / mv;
  float invT = 1.0 / T0_K + log(rTherm / R0) / BETA;
  float c = 1.0 / invT - 273.15;
  return c * 9.0 / 5.0 + 32.0 + OFFSET_F[ch];
}

void setRelay(int i, bool on) {
  doState[i] = on;
  digitalWrite(DO_PINS[i], on ? HIGH : LOW);
}

void setAO(float v) {
  if (isnan(v) || v < 0) v = 0;
  if (v > MAX_VOLTS) v = MAX_VOLTS;
  int code = (int)(v / FULL_SCALE_V * 255.0 + 0.5);
  if (code > 255) code = 255;
  dacWrite(DAC_PIN, code);
  aoVolts = v;
}

// ---------- web page ----------
const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Universal PLC</title>
<style>
  body{font-family:system-ui,Arial,sans-serif;margin:0 auto;padding:16px;max-width:560px;background:#12171c;color:#e8edf2}
  h1{font-size:20px;margin:4px 0 0}
  h2{font-size:13px;text-transform:uppercase;letter-spacing:.07em;color:#8fa3b8;margin:22px 0 8px}
  .sub{color:#8fa3b8;font-size:13px}
  .card{background:#1b232b;border-radius:10px;padding:10px 14px}
  .row{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid #26313b}
  .row:last-child{border-bottom:none}
  .val{font-size:18px;font-variant-numeric:tabular-nums}
  .on{color:#5ad18a}
  .off{color:#8fa3b8}
  button{font-size:16px;padding:10px 14px;border:0;border-radius:8px;background:#2a3541;color:#e8edf2;min-width:120px}
  button.on{background:#1f7a4a;color:#fff}
  button.alloff{background:#7a2a2a;color:#fff;width:100%;margin-top:8px}
  input[type=range]{width:100%;margin:10px 0}
  #conn{color:#e07a5f;height:18px;font-size:13px}
</style>
</head>
<body>
<h1>ESP32 Universal PLC</h1>
<div class="sub">Signal <span id="rssi">--</span> &middot; <span id="conn"></span></div>

<h2>Temperatures</h2>
<div class="card" id="temps"></div>

<h2>Digital inputs</h2>
<div class="card" id="dis"></div>

<h2>Relay outputs</h2>
<div class="card" id="dos"></div>
<button class="alloff" onclick="allOff()">All relays OFF</button>

<h2>Analog output (0-10 V)</h2>
<div class="card">
  <div class="row"><span>AO1</span><span class="val" id="aoval">-- V</span></div>
  <input type="range" id="ao" min="0" max="10" step="0.1" value="0">
</div>

<script>
const $ = id => document.getElementById(id);
let aoBusy = false;

// build the rows, using the names from the sketch (fetched once from /api/config)
const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
async function buildRows() {
  let cfg = {th: ['TH1', 'TH2'], di: ['DI1', 'DI2', 'DI3', 'DI4'], do: ['DO1', 'DO2', 'DO3', 'DO4', 'DO5']};
  try {
    const r = await fetch('/api/config', {cache: 'no-store'});
    if (r.ok) cfg = await r.json();
  } catch (e) {}
  cfg.th.forEach((n, i) => $('temps').insertAdjacentHTML('beforeend',
    '<div class="row"><span>' + esc(n) + '</span><span class="val" id="th' + i + '">--</span></div>'));
  cfg.di.forEach((n, i) => $('dis').insertAdjacentHTML('beforeend',
    '<div class="row"><span>' + esc(n) + '</span><span class="val off" id="di' + i + '">--</span></div>'));
  cfg.do.forEach((n, i) => $('dos').insertAdjacentHTML('beforeend',
    '<div class="row"><span>' + esc(n) + '</span><button id="do' + i + '" onclick="toggle(' + i + ')">--</button></div>'));
}

let failCount = 0;
async function refresh() {
  try {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), 5000);
    const r = await fetch('/api/status', {signal: ctl.signal, cache: 'no-store'});
    clearTimeout(timer);
    if (!r.ok) throw new Error('bad status ' + r.status);
    const s = await r.json();
    s.th.forEach((t, i) => { $('th' + i).textContent = (t === null) ? '--' : t.toFixed(1) + ' °F'; });
    s.di.forEach((d, i) => { const e = $('di' + i); e.textContent = d ? 'CLOSED' : 'OPEN'; e.className = 'val ' + (d ? 'on' : 'off'); });
    s.do.forEach((d, i) => { const b = $('do' + i); b.textContent = d ? 'ON' : 'OFF'; b.className = d ? 'on' : ''; });
    if (!aoBusy) { $('ao').value = s.ao; $('aoval').textContent = s.ao.toFixed(1) + ' V'; }
    $('rssi').textContent = s.rssi + ' dBm';
    failCount = 0;
    $('conn').textContent = '';
  } catch (e) {
    failCount++;
    if (failCount >= 3) $('conn').textContent = 'Connection lost... (' + e.message + ')';
  }
}

async function toggle(i) {
  const cur = $('do' + i).classList.contains('on');
  await fetch('/api/do?ch=' + (i + 1) + '&state=' + (cur ? 0 : 1), {method: 'POST'});
  refresh();
}

async function allOff() {
  await fetch('/api/alloff', {method: 'POST'});
  refresh();
}

const slider = $('ao');
slider.addEventListener('input', () => { aoBusy = true; $('aoval').textContent = parseFloat(slider.value).toFixed(1) + ' V'; });
slider.addEventListener('change', async () => {
  await fetch('/api/ao?v=' + slider.value, {method: 'POST'});
  aoBusy = false;
  refresh();
});

buildRows().then(() => { refresh(); setInterval(refresh, 2000); });
</script>
</body>
</html>
)rawliteral";

// ---------- HTTP handlers ----------
bool authed() {
  if (!server.authenticate(PANEL_USER, PANEL_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

void handleRoot() {
  if (!authed()) return;
  server.send_P(200, "text/html", PAGE);
}

void handleStatus() {
  if (!authed()) return;
  String j = "{\"th\":[";
  for (int i = 0; i < 2; i++) {
    if (i) j += ",";
    if (isnan(thF[i])) j += "null"; else j += String(thF[i], 1);
  }
  j += "],\"di\":[";
  for (int i = 0; i < 4; i++) {
    if (i) j += ",";
    j += (digitalRead(DI_PINS[i]) == LOW) ? "1" : "0";
  }
  j += "],\"do\":[";
  for (int i = 0; i < 5; i++) {
    if (i) j += ",";
    j += doState[i] ? "1" : "0";
  }
  j += "],\"ao\":" + String(aoVolts, 2);
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"uptime\":" + String(millis() / 1000);
  j += "}";
  server.send(200, "application/json", j);
}

// Escape quotes and backslashes so names are safe inside JSON
String jsonStr(const char* s) {
  String out = "\"";
  for (const char* p = s; *p; p++) {
    if (*p == '"' || *p == '\\') out += "\\";
    char buf[2] = {*p, 0};
    out += buf;
  }
  out += "\"";
  return out;
}

void handleConfig() {
  if (!authed()) return;
  String j = "{\"th\":[";
  for (int i = 0; i < 2; i++) { if (i) j += ","; j += jsonStr(TEMP_NAMES[i]); }
  j += "],\"di\":[";
  for (int i = 0; i < 4; i++) { if (i) j += ","; j += jsonStr(INPUT_NAMES[i]); }
  j += "],\"do\":[";
  for (int i = 0; i < 5; i++) { if (i) j += ","; j += jsonStr(RELAY_NAMES[i]); }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleDO() {
  if (!authed()) return;
  int ch = server.arg("ch").toInt();        // 1..5
  int st = server.arg("state").toInt();     // 0 or 1
  if (ch < 1 || ch > 5) { server.send(400, "text/plain", "bad channel"); return; }
  setRelay(ch - 1, st != 0);
  server.send(200, "text/plain", "ok");
}

void handleAllOff() {
  if (!authed()) return;
  for (int i = 0; i < 5; i++) setRelay(i, false);
  server.send(200, "text/plain", "ok");
}

void handleAO() {
  if (!authed()) return;
  setAO(server.arg("v").toFloat());
  server.send(200, "text/plain", "ok");
}

void handleNotFound() {
  server.send(404, "text/plain", "not found");
}

// ---------- WiFi ----------
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("Open http://%s.local  or  http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
    }
  } else {
    Serial.println("\nWiFi connect failed, will keep retrying.");
  }
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);

  // Outputs start OFF
  for (int i = 0; i < 5; i++) {
    pinMode(DO_PINS[i], OUTPUT);
    digitalWrite(DO_PINS[i], LOW);
  }
  for (int i = 0; i < 4; i++) pinMode(DI_PINS[i], INPUT);   // external 10k pull-ups on the board
  analogSetAttenuation(ADC_11db);
  setAO(0);

  connectWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfig);
  server.on("/api/do", HTTP_POST, handleDO);
  server.on("/api/alloff", HTTP_POST, handleAllOff);
  server.on("/api/ao", HTTP_POST, handleAO);
  server.onNotFound(handleNotFound);
  server.begin();

  for (int i = 0; i < 2; i++) thF[i] = readTempF(i);
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastSensor >= 1000) {
    lastSensor = now;
    for (int i = 0; i < 2; i++) thF[i] = readTempF(i);
  }

  if (WiFi.status() != WL_CONNECTED && now - lastWifiTry >= 10000) {
    lastWifiTry = now;
    Serial.println("WiFi lost, reconnecting...");
    WiFi.reconnect();
  }
}
