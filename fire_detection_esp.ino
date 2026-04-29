#include <Wire.h>
#include "Seeed_Arduino_SSCMA.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

#define SERIAL_PRINT_EVERY_BOX 0

const char* WIFI_SSID = "yunus";
const char* WIFI_PASSWORD = "sifresifre";

WebServer httpServer(80);
WebSocketsServer webSocket(81);

SSCMA AI;
static const uint8_t AI_ADDR = 0x62;

enum ClassId : int {
  CLASS_FIRE = 0,
  CLASS_SMOKE = 1
};

static const uint16_t LOOP_DELAY_MS = 80;
static const int WINDOW_SIZE = 15;
static const int FIRE_HITS_ON = 4;
static const int SMOKE_HITS_ON = 5;
static const int FIRE_HITS_OFF = 2;
static const int SMOKE_HITS_OFF = 2;

struct GeoCfg {
  int minW, minH, minArea;
  float minAspect, maxAspect;
};

GeoCfg FIRE_GEO = {20, 20, 700, 0.35f, 3.5f};
GeoCfg SMOKE_GEO = {28, 20, 900, 0.45f, 4.8f};

static const int SMOKE_MAX_AREA = 30000;
static const int SMOKE_MAX_CENTER_JUMP = 48;

enum EventState : uint8_t {
  ST_CLEAR = 0,
  ST_FIRE,
  ST_SMOKE,
  ST_FIRE_SMOKE
};

const char* stateToStr(EventState s) {
  switch (s) {
    case ST_CLEAR: return "CLEAR";
    case ST_FIRE: return "FIRE";
    case ST_SMOKE: return "SMOKE";
    case ST_FIRE_SMOKE: return "FIRE+SMOKE";
    default: return "CLEAR";
  }
}

int frameId = 0;

bool fireWin[WINDOW_SIZE] = {false};
bool smokeWin[WINDOW_SIZE] = {false};
int winPos = 0;
int fireHits = 0;
int smokeHits = 0;

EventState curState = ST_CLEAR;

bool prevSmokeCenterValid = false;
int prevSmokeCx = 0;
int prevSmokeCy = 0;

unsigned long lastHeartbeat = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastIpLog = 0;
bool wifiWasConnected = false;

static inline float safeAspect(int w, int h) {
  if (h <= 0) return 999.0f;
  return (float)w / (float)h;
}

bool geoValid(const GeoCfg& cfg, int w, int h) {
  int area = w * h;
  float ar = safeAspect(w, h);
  if (w < cfg.minW) return false;
  if (h < cfg.minH) return false;
  if (area < cfg.minArea) return false;
  if (ar < cfg.minAspect || ar > cfg.maxAspect) return false;
  return true;
}

void handleRoot() {
  String html;
  html.reserve(900);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>ESP</title></head><body style='font-family:sans-serif;padding:16px'>");
  html += F("<h2>Online</h2>");
  html += F("<p>iOS: <code>/api/state</code> · WS port <code>81</code></p>");
  html += F("<p><b>IP:</b> ");
  html += WiFi.localIP().toString();
  html += F("</p><p><b>State:</b> ");
  html += stateToStr(curState);
  html += F(" · hits fire/smoke: ");
  html += String(fireHits);
  html += F(" / ");
  html += String(smokeHits);
  html += F(" · frame: ");
  html += String(frameId);
  html += F("</p></body></html>");
  httpServer.send(200, "text/html; charset=utf-8", html);
}

void handleApiState() {
  StaticJsonDocument<160> doc;
  doc["event"] = "heartbeat";
  doc["state"] = stateToStr(curState);
  doc["fire_hits"] = fireHits;
  doc["smoke_hits"] = smokeHits;
  doc["frame"] = frameId;
  String out;
  serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

void onWsEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(clientNum);
      Serial.printf("[WS] #%u from %u.%u.%u.%u\n", clientNum, ip[0], ip[1], ip[2], ip[3]);
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("[WS] #%u off\n", clientNum);
      break;
    case WStype_TEXT:
      Serial.printf("[WS] #%u: %s\n", clientNum, (const char*)payload);
      break;
    default:
      break;
  }
}

void pushWindow(bool fireNow, bool smokeNow) {
  if (fireWin[winPos]) fireHits--;
  if (smokeWin[winPos]) smokeHits--;
  fireWin[winPos] = fireNow;
  smokeWin[winPos] = smokeNow;
  if (fireNow) fireHits++;
  if (smokeNow) smokeHits++;
  winPos = (winPos + 1) % WINDOW_SIZE;
}

EventState decideState(EventState prev) {
  bool fireOn = (fireHits >= FIRE_HITS_ON);
  bool smokeOn = (smokeHits >= SMOKE_HITS_ON);
  bool fireStillOn = (fireHits >= FIRE_HITS_OFF);
  bool smokeStillOn = (smokeHits >= SMOKE_HITS_OFF);

  switch (prev) {
    case ST_CLEAR:
      if (fireOn && smokeOn) return ST_FIRE_SMOKE;
      if (fireOn) return ST_FIRE;
      if (smokeOn) return ST_SMOKE;
      return ST_CLEAR;
    case ST_FIRE:
      if (!fireStillOn) return ST_CLEAR;
      if (smokeOn) return ST_FIRE_SMOKE;
      return ST_FIRE;
    case ST_SMOKE:
      if (!smokeStillOn) return ST_CLEAR;
      if (fireOn) return ST_FIRE_SMOKE;
      return ST_SMOKE;
    case ST_FIRE_SMOKE:
      if (!fireStillOn && !smokeStillOn) return ST_CLEAR;
      if (!fireStillOn) return ST_SMOKE;
      if (!smokeStillOn) return ST_FIRE;
      return ST_FIRE_SMOKE;
  }
  return ST_CLEAR;
}

void sendDetectionEvent(const char* from, const char* to) {
  StaticJsonDocument<160> doc;
  doc["event"] = "state_change";
  doc["from"] = from;
  doc["to"] = to;
  doc["fire_hits"] = fireHits;
  doc["smoke_hits"] = smokeHits;
  char buf[160];
  serializeJson(doc, buf);
  webSocket.broadcastTXT(buf);
  Serial.printf("[EVT] %s -> %s ws=%u\n", from, to, webSocket.connectedClients());
}

void sendHeartbeat() {
  StaticJsonDocument<160> doc;
  doc["event"] = "heartbeat";
  doc["state"] = stateToStr(curState);
  doc["fire_hits"] = fireHits;
  doc["smoke_hits"] = smokeHits;
  doc["frame"] = frameId;
  char buf[160];
  serializeJson(doc, buf);
  webSocket.broadcastTXT(buf);
  Serial.printf("[HB] %s f=%d s=%d fr=%d ws=%u\n",
                stateToStr(curState), fireHits, smokeHits, frameId, webSocket.connectedClients());
}

void emitStateIfChanged(EventState nextState) {
  if (nextState == curState) return;
  sendDetectionEvent(stateToStr(curState), stateToStr(nextState));
  curState = nextState;
}

static void printBoxLine(size_t i, int cls, float conf, int x, int y, int w, int h, bool fireValid, bool smokeValid) {
  int area = w * h;
  float ar = safeAspect(w, h);
  Serial.printf("  [%u] cls=%d cf=%.3f xy=%d,%d wh=%d,%d a=%d ar=%.2f F%c S%c\n",
                (unsigned)i, cls, conf, x, y, w, h, area, ar,
                fireValid ? '+' : '-', smokeValid ? '+' : '-');
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Wire.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  Serial.print("WiFi ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++attempts > 60) {
      Serial.println("\nWiFi failed");
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println(WiFi.localIP());
    wifiWasConnected = true;
  }

  httpServer.on("/", handleRoot);
  httpServer.on("/api/state", handleApiState);
  httpServer.begin();
  Serial.println("HTTP :80 / /api/state");

  webSocket.begin();
  webSocket.onEvent(onWsEvent);
  Serial.println("WS :81");

  if (!AI.begin(&Wire, -1, AI_ADDR)) {
    Serial.println("AI.begin failed");
    while (1) delay(1000);
  }
  Serial.println("AI OK");
}

void loop() {
  if (millis() - lastWifiCheck > 3000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiWasConnected) {
        Serial.println("[WiFi] lost");
        wifiWasConnected = false;
      }
      WiFi.disconnect(true);
      delay(100);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    } else if (!wifiWasConnected) {
      Serial.println(WiFi.localIP());
      wifiWasConnected = true;
    }
  }

  httpServer.handleClient();
  webSocket.loop();

  if (millis() - lastIpLog > 10000) {
    lastIpLog = millis();
    Serial.printf("[NET] %s ip=%s rssi=%d ws=%u\n",
                  WiFi.status() == WL_CONNECTED ? "ok" : "down",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  webSocket.connectedClients());
  }

  if (millis() - lastHeartbeat > 500) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }

  frameId++;
  int ret = AI.invoke(1, false, false);

  bool fireNow = false;
  bool smokeNow = false;
  int boxCount = 0;

  if (ret == 0) {
    auto boxes = AI.boxes();
    boxCount = (int)boxes.size();

    bool smokeCandidateFound = false;
    int bestSmokeCx = 0, bestSmokeCy = 0, bestSmokeArea = 0;

    for (size_t i = 0; i < boxes.size(); ++i) {
      auto b = boxes[i];
      int cls = (int)b.target;
      float conf = b.score;
      int x = (int)b.x, y = (int)b.y;
      int w = (int)b.w, h = (int)b.h;

      bool fireValid = false;
      bool smokeValid = false;

      if (cls == CLASS_FIRE) {
        fireValid = geoValid(FIRE_GEO, w, h);
        if (fireValid) fireNow = true;
      } else if (cls == CLASS_SMOKE) {
        smokeValid = geoValid(SMOKE_GEO, w, h);
        int area = w * h;
        if (smokeValid && area > SMOKE_MAX_AREA) smokeValid = false;
        if (smokeValid) {
          int cx = x + w / 2, cy = y + h / 2;
          if (prevSmokeCenterValid) {
            int dx = cx - prevSmokeCx, dy = cy - prevSmokeCy;
            if (dx * dx + dy * dy > SMOKE_MAX_CENTER_JUMP * SMOKE_MAX_CENTER_JUMP)
              smokeValid = false;
          }
          if (smokeValid && area > bestSmokeArea) {
            bestSmokeArea = area;
            bestSmokeCx = cx;
            bestSmokeCy = cy;
            smokeCandidateFound = true;
          }
        }
        if (smokeValid) smokeNow = true;
      }

#if SERIAL_PRINT_EVERY_BOX
      printBoxLine(i, cls, conf, x, y, w, h, fireValid, smokeValid);
#endif
    }

    if (smokeCandidateFound) {
      prevSmokeCx = bestSmokeCx;
      prevSmokeCy = bestSmokeCy;
      prevSmokeCenterValid = true;
    } else {
      prevSmokeCenterValid = false;
    }

#if !SERIAL_PRINT_EVERY_BOX
    if (boxes.size() > 0) {
      Serial.printf("[F%05d] boxes=%zu\n", frameId, boxes.size());
      for (size_t i = 0; i < boxes.size(); ++i) {
        auto b = boxes[i];
        int cls = (int)b.target;
        int x = (int)b.x, y = (int)b.y;
        int w = (int)b.w, h = (int)b.h;
        int area = w * h;
        bool fv = (cls == CLASS_FIRE) && geoValid(FIRE_GEO, w, h);
        bool sv = (cls == CLASS_SMOKE) && geoValid(SMOKE_GEO, w, h);
        if (sv && area > SMOKE_MAX_AREA) sv = false;
        printBoxLine(i, cls, b.score, x, y, w, h, fv, sv);
      }
    }
#endif

  } else if (ret == 3) {
    Serial.printf("[F%05d] busy\n", frameId);
  } else {
    Serial.printf("[F%05d] ret=%d\n", frameId, ret);
  }

  pushWindow(fireNow, smokeNow);
  emitStateIfChanged(decideState(curState));

  Serial.printf("csv,%d,%d,%d,%d,%d,%d,%d,%s\n",
                frameId, ret, boxCount,
                fireNow ? 1 : 0, smokeNow ? 1 : 0,
                fireHits, smokeHits, stateToStr(curState));

  delay(LOOP_DELAY_MS);
}