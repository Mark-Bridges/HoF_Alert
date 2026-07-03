/*
 * EA HOF Monitor — ESP32
 * ----------------------
 * Polls the Environment Agency real-time flood-monitoring API for one
 * (optionally two) gauging-station measures named in an abstraction
 * licence, and pushes phone notifications via ntfy.sh when flow/level
 * approaches or crosses a Hands-Off Flow (HOF) / Hands-Off Level (HOL)
 * trigger. Recovery and stale-gauge alerts included.
 *
 * Config is done in a browser: the ESP32 serves a setup page and the
 * *browser* talks to the EA API for station search (the API allows
 * cross-origin requests), so the device itself only ever fetches one
 * tiny "?latest" reading per site.
 *
 * Libraries (Library Manager):
 *   - ArduinoJson        (Benoit Blanchon)  v7.x
 *   - WiFiManager        (tzapu)            v2.x
 * Board: any ESP32 dev module (Arduino-ESP32 core 3.x)
 *
 * First boot: join Wi-Fi network "EA-HOF-Setup", configure your Wi-Fi,
 * then browse to http://ea-hof.local (or the IP shown on Serial).
 *
 * NOTE ON TLS: fetches use setInsecure() (no certificate validation).
 * This is pragmatic for public read-only data; for a hardened build,
 * embed the ISRG / relevant root CA and use setCACert() instead.
 *
 * NOTE ON DATA: EA gauge readings are provisional and unvalidated.
 * This device is an early-warning aid, not the legal determination of
 * when abstraction must cease under your licence.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <time.h>
#include "config_page.h"

// ---------------------------------------------------------------- constants
static const uint32_t POLL_INTERVAL_MS   = 15UL * 60UL * 1000UL; // EA updates ~15 min
// Stale threshold: readings are taken every 15 min but some stations only
// TELEMETER data back to the EA a few times a day outside flood conditions.
// Watch your station for a week and set this just above its normal gap.
static const uint32_t STALE_AFTER_SECS   = 3UL * 60UL * 60UL;    // default: 3 hours
static const uint8_t  MAX_SITES          = 2;                    // hook for a second licence point
static const uint8_t  MAX_CONSEC_FAILS   = 8;                    // reboot after ~2 h of failed polls
static const char*    EA_HOST            = "environment.data.gov.uk";
static const char*    NTFY_HOST          = "https://ntfy.sh/";
static const char*    HOSTNAME           = "ea-hof";

// Alert state machine
enum SiteState : uint8_t { ARMED = 0, WARNED = 1, TRIGGERED = 2 };

struct Site {
  bool    enabled      = false;
  String  label;                 // e.g. "Gt Somerford"
  String  measureId;             // EA measure notation, e.g. "531160-flow--i-15_min-m3_s"
  String  param;                 // "flow" | "level"
  String  nativeUnit;            // unit the EA reports in: "m3/s" or "m" etc.
  String  licenceUnit;           // unit the licence states: "l/s", "m3/s", "m"
  bool    below        = true;   // true => alert when value falls BELOW trigger (HOF/HOL)
  float   trigger      = 0;      // stored in NATIVE units
  float   warnAt       = 0;      // stored in NATIVE units (warning threshold, also re-arm level)
  // runtime
  SiteState state      = ARMED;
  bool    staleNotified = false;
  float   lastValue    = NAN;
  time_t  lastReading  = 0;
  bool    everPolled   = false;
};

Site        sites[MAX_SITES];
String      ntfyTopic;
Preferences prefs;
WebServer   server(80);
uint32_t    lastPoll      = 0;
bool        haveConfig    = false;
uint8_t     consecFails   = 0;

// ---------------------------------------------------------------- utilities

// Parse EA ISO-8601 UTC timestamp "2026-07-02T09:15:00Z" -> epoch seconds.
time_t parseIso8601Z(const char* s) {
  struct tm t = {};
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday,
                   &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) return 0;
  t.tm_year -= 1900;
  t.tm_mon  -= 1;
  // days since epoch (civil algorithm, valid 1970+)
  int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;
  y -= m <= 2;
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + doe - 719468;
  return (time_t)days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

// Convert a native-unit value into licence units for human-readable messages.
float toLicenceUnits(const Site& s, float nativeVal) {
  if (s.licenceUnit == "l/s" && (s.nativeUnit == "m3/s" || s.param == "flow"))
    return nativeVal * 1000.0f;
  return nativeVal;
}

String fmtVal(const Site& s, float nativeVal) {
  float v = toLicenceUnits(s, nativeVal);
  char buf[32];
  // l/s values are big-ish; levels want 3 dp
  if (s.licenceUnit == "m") snprintf(buf, sizeof(buf), "%.3f %s", v, s.licenceUnit.c_str());
  else                      snprintf(buf, sizeof(buf), "%.1f %s", v, s.licenceUnit.c_str());
  return String(buf);
}

// ---------------------------------------------------------------- ntfy push
bool sendNtfy(const String& title, const String& body, uint8_t priority, const char* tags) {
  if (ntfyTopic.length() == 0) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String(NTFY_HOST) + ntfyTopic)) return false;
  http.addHeader("Title", title);
  http.addHeader("Priority", String(priority)); // 5=urgent 4=high 3=default
  http.addHeader("Tags", tags);
  int code = http.POST(body);
  http.end();
  Serial.printf("[ntfy] %s -> HTTP %d\n", title.c_str(), code);
  return code >= 200 && code < 300;
}

// ---------------------------------------------------------------- EA fetch
// Fetch latest reading for one measure. Returns true on success.
bool fetchLatest(Site& s, float& valueOut, time_t& whenOut) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  String url = "https://" + String(EA_HOST) + "/flood-monitoring/id/measures/"
             + s.measureId + "/readings?latest&_limit=1";
  if (!http.begin(client, url)) return false;
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("[EA] HTTP %d for %s\n", code, s.label.c_str()); return false; }

  // Only deserialize the two fields we need — keeps RAM use tiny & fixed.
  JsonDocument filter;
  filter["items"][0]["dateTime"] = true;
  filter["items"][0]["value"]    = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("[EA] JSON error: %s\n", err.c_str()); return false; }

  JsonVariant item = doc["items"][0];
  if (item.isNull()) return false;
  valueOut = item["value"].as<float>();
  whenOut  = parseIso8601Z(item["dateTime"].as<const char*>());
  return whenOut != 0;
}

// ---------------------------------------------------------------- state machine
void evaluateSite(Site& s, float v, time_t when) {
  s.lastValue   = v;
  s.lastReading = when;
  s.everPolled  = true;

  // --- staleness -----------------------------------------------------------
  time_t now = time(nullptr);
  bool stale = (now > 1600000000) && (now - when > (time_t)STALE_AFTER_SECS);
  if (stale && !s.staleNotified) {
    s.staleNotified = true;
    sendNtfy("Gauge data stale: " + s.label,
             "Latest EA reading is over " + String(STALE_AFTER_SECS / 60) +
             " minutes old. The gauge or feed may be offline — do not rely on "
             "this device for HOF compliance until data resumes.",
             4, "hourglass,warning");
  } else if (!stale && s.staleNotified) {
    s.staleNotified = false;
    sendNtfy("Gauge data resumed: " + s.label,
             "Fresh readings are arriving again. Current: " + fmtVal(s, v) + ".",
             3, "white_check_mark");
  }
  if (stale) return; // don't act on old numbers

  // --- threshold comparison (direction-aware) ------------------------------
  bool pastTrigger, pastWarn;
  if (s.below) { pastTrigger = v <= s.trigger; pastWarn = v <= s.warnAt; }
  else         { pastTrigger = v >= s.trigger; pastWarn = v >= s.warnAt; }

  SiteState newState;
  if      (pastTrigger)                          newState = TRIGGERED;
  else if (pastWarn)                             newState = (s.state == TRIGGERED) ? TRIGGERED : WARNED;
  else                                           newState = ARMED;
  // (Staying TRIGGERED until the value recovers past warnAt gives hysteresis:
  //  the warning margin doubles as the deadband, so a river hovering at the
  //  trigger doesn't generate an alert every 15 minutes.)

  if (newState == s.state) { saveRuntimeState(); return; }

  String dir = s.below ? "below" : "above";
  if (newState == TRIGGERED) {
    sendNtfy("HOF TRIGGERED: " + s.label,
             (s.param == "flow" ? "Flow" : "Level") + String(" is ") + fmtVal(s, v) +
             " — " + dir + " your licence trigger of " + fmtVal(s, s.trigger) +
             ". Abstraction must stop until conditions recover.",
             5, "rotating_light,no_entry");
  } else if (newState == WARNED && s.state == ARMED) {
    sendNtfy("Approaching HOF: " + s.label,
             (s.param == "flow" ? "Flow" : "Level") + String(" is ") + fmtVal(s, v) +
             " — inside your warning margin (trigger " + fmtVal(s, s.trigger) +
             "). Plan for a possible shutdown.",
             4, "warning");
  } else if (newState == ARMED) {
    bool wasTriggered = (s.state == TRIGGERED);
    sendNtfy(wasTriggered ? ("HOF recovered: " + s.label) : ("Back to normal: " + s.label),
             (s.param == "flow" ? "Flow" : "Level") + String(" has recovered to ") + fmtVal(s, v) +
             (wasTriggered ? ". Check licence conditions before resuming abstraction." : "."),
             wasTriggered ? 4 : 3, "white_check_mark");
  }
  s.state = newState;
  saveRuntimeState();
}

// ---------------------------------------------------------------- persistence
void saveRuntimeState() {
  prefs.begin("eahof", false);
  for (uint8_t i = 0; i < MAX_SITES; i++) {
    prefs.putUChar(("st" + String(i)).c_str(), (uint8_t)sites[i].state);
    prefs.putBool (("sn" + String(i)).c_str(), sites[i].staleNotified);
  }
  prefs.end();
}

void loadRuntimeState() {
  prefs.begin("eahof", true);
  for (uint8_t i = 0; i < MAX_SITES; i++) {
    sites[i].state         = (SiteState)prefs.getUChar(("st" + String(i)).c_str(), ARMED);
    sites[i].staleNotified = prefs.getBool(("sn" + String(i)).c_str(), false);
  }
  prefs.end();
}

bool applyConfigJson(const String& json, bool persist) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  ntfyTopic = doc["ntfyTopic"].as<String>();
  JsonArray arr = doc["sites"];
  for (uint8_t i = 0; i < MAX_SITES; i++) {
    Site& s = sites[i];
    if (i < arr.size() && !arr[i].isNull() && arr[i]["measureId"].as<String>().length() > 0) {
      JsonVariant j   = arr[i];
      s.enabled       = true;
      s.label         = j["label"].as<String>();
      s.measureId     = j["measureId"].as<String>();
      s.param         = j["param"] | "flow";
      s.nativeUnit    = j["nativeUnit"] | "m3/s";
      s.licenceUnit   = j["licenceUnit"] | "l/s";
      s.below         = j["below"] | true;
      s.trigger       = j["trigger"] | 0.0f;   // native units
      s.warnAt        = j["warnAt"]  | 0.0f;   // native units
    } else {
      s = Site(); // reset slot
    }
  }
  haveConfig = sites[0].enabled && ntfyTopic.length() > 0;
  if (persist) {
    prefs.begin("eahof", false);
    prefs.putString("cfg", json);
    prefs.end();
  }
  return true;
}

void loadConfig() {
  prefs.begin("eahof", true);
  String json = prefs.getString("cfg", "");
  prefs.end();
  if (json.length()) applyConfigJson(json, false);
  loadRuntimeState();
}

// ---------------------------------------------------------------- web server
void handleRoot()   { server.send_P(200, "text/html", CONFIG_PAGE); }

void handleGetConfig() {
  prefs.begin("eahof", true);
  String json = prefs.getString("cfg", "{}");
  prefs.end();
  server.send(200, "application/json", json);
}

void handleSave() {
  String body = server.arg("plain");
  if (!applyConfigJson(body, true)) { server.send(400, "text/plain", "Bad JSON"); return; }
  // Reset alert states for a fresh config and poll immediately.
  for (auto& s : sites) { s.state = ARMED; s.staleNotified = false; s.everPolled = false; }
  saveRuntimeState();
  server.send(200, "application/json", "{\"ok\":true}");
  lastPoll = 0; // force poll on next loop
}

void handleTest() {
  bool ok = sendNtfy("Test alert",
                     "Your EA HOF monitor can reach ntfy. Notifications are working.",
                     3, "bell");
  server.send(ok ? 200 : 502, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void handleStatus() {
  JsonDocument doc;
  doc["haveConfig"] = haveConfig;
  doc["uptimeMin"]  = millis() / 60000UL;
  doc["nextPollS"]  = haveConfig ? (int)((POLL_INTERVAL_MS - (millis() - lastPoll)) / 1000) : -1;
  JsonArray arr = doc["sites"].to<JsonArray>();
  static const char* names[] = {"armed", "warned", "TRIGGERED"};
  for (auto& s : sites) {
    if (!s.enabled) continue;
    JsonObject o = arr.add<JsonObject>();
    o["label"] = s.label;
    o["state"] = names[s.state];
    o["stale"] = s.staleNotified;
    if (s.everPolled) {
      o["value"]       = s.lastValue;                 // native units
      o["licenceVal"]  = toLicenceUnits(s, s.lastValue);
      o["licenceUnit"] = s.licenceUnit;
      o["readingAge"]  = (long)(time(nullptr) - s.lastReading);
    }
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------- poll cycle
void pollAll() {
  bool anyFail = false;
  for (auto& s : sites) {
    if (!s.enabled) continue;
    float v; time_t when;
    if (fetchLatest(s, v, when)) {
      Serial.printf("[poll] %s = %.4f %s (state %d)\n", s.label.c_str(), v, s.nativeUnit.c_str(), s.state);
      evaluateSite(s, v, when);
    } else {
      anyFail = true;
    }
  }
  consecFails = anyFail ? consecFails + 1 : 0;
  if (consecFails >= MAX_CONSEC_FAILS) {
    Serial.println("[poll] too many consecutive failures — restarting");
    ESP.restart();
  }
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  Serial.begin(115200);
  delay(200);

  WiFiManager wm;
  wm.setHostname(HOSTNAME);
  wm.setConfigPortalTimeout(300);
  if (!wm.autoConnect("EA-HOF-Setup")) ESP.restart();
  Serial.printf("\nWi-Fi OK, IP: %s\n", WiFi.localIP().toString().c_str());

  if (MDNS.begin(HOSTNAME)) Serial.println("mDNS: http://ea-hof.local");
  configTime(0, 0, "pool.ntp.org", "time.google.com"); // UTC — EA timestamps are UTC

  loadConfig();

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/config",  HTTP_GET,  handleGetConfig);
  server.on("/save",    HTTP_POST, handleSave);
  server.on("/test",    HTTP_POST, handleTest);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.begin();

  lastPoll = 0; // poll straight away once configured
}

void loop() {
  server.handleClient();
  if (haveConfig && (lastPoll == 0 || millis() - lastPoll >= POLL_INTERVAL_MS)) {
    lastPoll = millis();
    pollAll();
  }
  delay(10);
}
