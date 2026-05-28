#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <MFRC522.h>
#include <U8g2lib.h>

#define OLED_SDA 21
#define OLED_SCL 22

#define RFID_SS  5
#define RFID_RST 27

#define BUZZER_PIN 25

#define SERVO_PIN 13
#define SERVO_CLOSED_ANGLE 0
#define SERVO_OPEN_ANGLE   90

#define TRIG1 14
#define ECHO1 34
#define TRIG2 16
#define ECHO2 35
#define TRIG3 17
#define ECHO3 36

String ALLOWED_UID = "7D 4B 42 05";

#define OCCUPIED_ENTER_CM 15
#define OCCUPIED_EXIT_CM  22

const long DIST_MIN_VALID_CM = 2;
const long DIST_MAX_VALID_CM = 250;

const int SENSOR_CONFIRM_COUNT = 2;

const unsigned long SENSOR_INTER_GAP_MS = 30;

const char* AP_SSID     = "SmartParking_AP";
const char* AP_PASSWORD = "12345678";

const bool  LOGGING_ENABLED = true;
const char* LOG_SERVER_URL  = "http://192.168.4.2:5000/api/log";
const uint16_t LOG_HTTP_TIMEOUT_MS = 300;
const unsigned long LOG_RETRY_INTERVAL_MS = 4000;

U8G2_SH1106_128X64_NONAME_F_SW_I2C u8g2(
  U8G2_R0,
  22,
  21,
  U8X8_PIN_NONE
);

MFRC522 rfid(RFID_SS, RFID_RST);
WebServer server(80);

bool rfidReady = false;

struct ParkingSensor {
  int  trigPin;
  int  echoPin;
  long distance;
  long lastRawDistance;
  long lastGoodDistance;
  bool occupied;
  int  busyConfirmCount;
  int  freeConfirmCount;
};

ParkingSensor sensors[3] = {
  { TRIG1, ECHO1, -1, -1, 999, false, 0, 0 },
  { TRIG2, ECHO2, -1, -1, 999, false, 0, 0 },
  { TRIG3, ECHO3, -1, -1, 999, false, 0, 0 },
};

long   distance1 = 999;
long   distance2 = 999;
long   distance3 = 999;
bool   busy1 = false;
bool   busy2 = false;
bool   busy3 = false;
int    freeSlots = 3;

String lastUID           = "NONE";
String lastAccessStatus  = "NONE";
String lastAccessMessage = "";

unsigned long lastSensorUpdate  = 0;
unsigned long lastDisplayUpdate = 0;

unsigned long lastRFIDReadTime = 0;
const unsigned long RFID_COOLDOWN_MS = 700;

const unsigned long SENSOR_INTERVAL_MS  = 1000;
const unsigned long DISPLAY_INTERVAL_MS = 1000;

bool          pendingLogExists      = false;
String        pendingLogUID         = "";
String        pendingLogStatus      = "";
String        pendingLogMessage     = "";
int           pendingLogFreeSpots   = 0;
bool          pendingLogBusy1       = false;
bool          pendingLogBusy2       = false;
bool          pendingLogBusy3       = false;
long          pendingLogDist1       = 0;
long          pendingLogDist2       = 0;
long          pendingLogDist3       = 0;
unsigned long pendingLogNextAttempt = 0;

int lastLogHttpCode = 0;

String barrierStatus = "CLOSED";

void   setupWiFiAP();
void   setupWebServer();
void   handleRoot();
void   handleDebug();
void   handleData();
String buildHtml();

long   readDistanceRawCm(int trigPin, int echoPin);
bool   isValidDistance(long distance);
long   readDistanceFilteredCm(int trigPin, int echoPin, long &lastRawOut);
void   updateOneSensor(ParkingSensor &s);
void   updateSensors();

void   drawFrame();
void   showStatus();
void   showAccessGranted(String uid);
void   showAccessDenied(String uid);
void   showParkingFull(String uid);

void   playToneManual(int frequency, int durationMs);
void   soundAccessGranted();
void   soundAccessDenied();
void   soundParkingFull();

int    angleToPulseUs(int angle);
void   servoWriteFor(int angle, int durationMs);
void   openBarrier();
void   closeBarrier();

void   processRFIDFast();
String uidToString(MFRC522::Uid uid);

String jsonEscape(String value);
String buildLogPayload(String uid, String accessStatus, String message,
                       int freeSpotsSnap,
                       bool busy1Snap, bool busy2Snap, bool busy3Snap,
                       long dist1Snap, long dist2Snap, long dist3Snap);
void   queueAccessLog(String uid, String accessStatus, String message);
void   flushPendingLog();

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("Smart Parking system started");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);

  pinMode(TRIG1, OUTPUT); pinMode(ECHO1, INPUT);
  pinMode(TRIG2, OUTPUT); pinMode(ECHO2, INPUT);
  pinMode(TRIG3, OUTPUT); pinMode(ECHO3, INPUT);

  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "SmartParking boot");
  u8g2.sendBuffer();

  closeBarrier();

  setupWiFiAP();
  setupWebServer();

  SPI.begin(18, 19, 23, RFID_SS);
  rfid.PCD_Init();
  delay(50);

  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("RFID VersionReg: 0x");
  Serial.println(v, HEX);

  if (v == 0x00 || v == 0xFF) {
    rfidReady = false;
    Serial.println("RFID NOT DETECTED - continuing without RFID");
  } else {
    rfidReady = true;
    Serial.println("RFID OK");
  }

  Serial.println("System ready");
  Serial.print("Connect to WiFi: ");
  Serial.println(AP_SSID);
  Serial.println("Open: http://192.168.4.1");

  showStatus();
}

void loop() {
  server.handleClient();

  if (rfidReady) {
    processRFIDFast();
  }

  unsigned long now = millis();

  if (now - lastSensorUpdate > SENSOR_INTERVAL_MS) {
    lastSensorUpdate = now;
    updateSensors();
  }

  if (now - lastDisplayUpdate > DISPLAY_INTERVAL_MS) {
    lastDisplayUpdate = now;
    showStatus();
  }

  flushPendingLog();
}

void setupWiFiAP() {
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(100);
  IPAddress ip = WiFi.softAPIP();

  Serial.println("WiFi Access Point started");
  Serial.print("SSID: ");     Serial.println(AP_SSID);
  Serial.print("Password: "); Serial.println(AP_PASSWORD);
  Serial.print("IP address: "); Serial.println(ip);

  if (!ok) {
    Serial.println("softAP FAILED");
  }
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/debug", handleDebug);
  server.on("/data", handleData);
  server.onNotFound(handleRoot);
  server.begin();
  Serial.println("Web server started");
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", buildHtml());
}

String buildHtml() {
  String s;
  s.reserve(5500);
  s += F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  s += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  s += F("<title>Smart Parking</title>");
  s += F("<style>");
  s += F("*{box-sizing:border-box}");
  s += F("body{font-family:Arial,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:16px;line-height:1.4}");
  s += F(".wrap{max-width:880px;margin:0 auto}");
  s += F("h1{color:#4ade80;margin:0 0 4px;font-size:24px}");
  s += F(".sub{color:#94a3b8;font-size:13px;margin-bottom:18px}");
  s += F(".hero{background:linear-gradient(135deg,#1e293b,#0f172a);padding:24px;border-radius:12px;text-align:center;margin-bottom:18px;border:1px solid #1e293b}");
  s += F(".hero .lbl{font-size:12px;color:#94a3b8;text-transform:uppercase;letter-spacing:2px}");
  s += F(".hero .count{font-size:64px;font-weight:bold;color:#fff;line-height:1;margin:6px 0}");
  s += F(".hero .count small{font-size:24px;color:#64748b}");
  s += F(".badge{display:inline-block;padding:6px 18px;border-radius:999px;font-weight:bold;font-size:13px;letter-spacing:1px}");
  s += F(".b-a{background:#14532d;color:#86efac}.b-w{background:#78350f;color:#fcd34d}.b-f{background:#7f1d1d;color:#fca5a5}");
  s += F(".pill{background:#1e3a8a;color:#bfdbfe;padding:3px 10px;font-size:11px;border-radius:6px;margin-left:8px;font-weight:normal}");
  s += F(".pill.off{background:#7f1d1d;color:#fca5a5}");
  s += F(".slots{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-bottom:18px}");
  s += F(".slot{background:#1e293b;border:2px solid transparent;border-radius:10px;padding:16px;text-align:center}");
  s += F(".slot .pid{font-size:13px;color:#94a3b8;letter-spacing:1px}");
  s += F(".slot .stat{font-size:22px;font-weight:bold;margin:6px 0}");
  s += F(".slot .dist{font-size:13px;color:#94a3b8}");
  s += F(".sF{border-color:#16a34a}.sF .stat{color:#4ade80}");
  s += F(".sB{border-color:#dc2626}.sB .stat{color:#f87171}");
  s += F(".sN{border-color:#ca8a04}.sN .stat{color:#fcd34d}");
  s += F(".sec{background:#1e293b;border-radius:10px;padding:14px 18px;margin-bottom:14px}");
  s += F(".sec h2{margin:0 0 10px;color:#93c5fd;font-size:13px;text-transform:uppercase;letter-spacing:1px}");
  s += F(".row{display:flex;justify-content:space-between;gap:12px;padding:4px 0;border-bottom:1px solid #334155;font-size:14px}");
  s += F(".row:last-child{border:none}.row .k{color:#94a3b8}.row .v{color:#e2e8f0;font-family:monospace;text-align:right;word-break:break-all}");
  s += F(".links{text-align:center;margin-top:18px}.links a{color:#93c5fd;margin:0 10px;text-decoration:none;font-size:13px}");
  s += F(".links a:hover{text-decoration:underline}");
  s += F("@media(max-width:520px){.slots{grid-template-columns:1fr}.hero .count{font-size:48px}}");
  s += F("</style></head><body>");

  s += F("<div class='wrap'>");
  s += F("<h1>Smart Parking Dashboard<span class='pill' id='online'>System Online</span></h1>");
  s += F("<div class='sub'>Live status &middot; Uptime <span id='uptime'>-</span></div>");

  s += F("<div class='hero'>");
  s += F("<div class='lbl'>Free spots</div>");
  s += F("<div class='count'><span id='free'>-</span><small> / 3</small></div>");
  s += F("<span class='badge b-a' id='pbadge'>-</span>");
  s += F("</div>");

  s += F("<div class='slots' id='slots'></div>");

  s += F("<div class='sec'><h2>Last RFID Event</h2>");
  s += F("<div class='row'><span class='k'>UID</span><span class='v' id='r-uid'>-</span></div>");
  s += F("<div class='row'><span class='k'>Status</span><span class='v' id='r-stat'>-</span></div>");
  s += F("<div class='row'><span class='k'>Message</span><span class='v' id='r-msg'>-</span></div>");
  s += F("<div class='row'><span class='k'>RFID hardware</span><span class='v' id='r-hw'>-</span></div>");
  s += F("</div>");

  s += F("<div class='sec'><h2>Barrier</h2>");
  s += F("<div class='row'><span class='k'>Status</span><span class='v' id='b-stat'>-</span></div>");
  s += F("<div class='row'><span class='k'>Last action</span><span class='v' id='b-msg'>-</span></div>");
  s += F("</div>");

  s += F("<div class='sec'><h2>Logger</h2>");
  s += F("<div class='row'><span class='k'>Server URL</span><span class='v' id='l-url'>-</span></div>");
  s += F("<div class='row'><span class='k'>Last HTTP code</span><span class='v' id='l-code'>-</span></div>");
  s += F("<div class='row'><span class='k'>Pending log</span><span class='v' id='l-pen'>-</span></div>");
  s += F("</div>");

  s += F("<div class='links'><a href='/debug'>Debug Panel</a> &middot; <a href='/data'>Data API (JSON)</a></div>");
  s += F("</div>");

  s += F("<script>");
  s += F("function ut(ms){var s=Math.floor(ms/1000),h=Math.floor(s/3600);s-=h*3600;var m=Math.floor(s/60);s-=m*60;return (h>0?h+'h ':'')+m+'m '+s+'s';}");
  s += F("function setBadge(st){var e=document.getElementById('pbadge');var c='b-a',t=st;if(st==='ALMOST_FULL'){c='b-w';t='ALMOST FULL';}else if(st==='FULL'){c='b-f';t='FULL';}else t='AVAILABLE';e.className='badge '+c;e.textContent=t;}");
  s += F("function renderSlots(arr){var h='';for(var i=0;i<arr.length;i++){var s=arr[i];var cls=s.no_echo?'sN':(s.status==='BUSY'?'sB':'sF');var lab=s.no_echo?'NO ECHO':s.status;var d=(s.distance<0)?'- cm':(s.distance+' cm');h+='<div class=\"slot '+cls+'\"><div class=\"pid\">'+s.id+'</div><div class=\"stat\">'+lab+'</div><div class=\"dist\">'+d+'</div></div>';}document.getElementById('slots').innerHTML=h;}");
  s += F("function setOnline(ok){var e=document.getElementById('online');e.textContent=ok?'System Online':'Disconnected';e.className='pill'+(ok?'':' off');}");
  s += F("async function tick(){try{var r=await fetch('/data');var d=await r.json();document.getElementById('free').textContent=d.free_spots;setBadge(d.parking_status);renderSlots(d.slots);document.getElementById('r-uid').textContent=d.rfid.last_uid||'-';document.getElementById('r-stat').textContent=d.rfid.last_access_status||'-';document.getElementById('r-msg').textContent=d.rfid.last_access_message||'-';document.getElementById('r-hw').textContent=d.rfid.ready?'OK':'NOT DETECTED';document.getElementById('b-stat').textContent=d.barrier_status;document.getElementById('b-msg').textContent=d.rfid.last_access_message||'-';document.getElementById('l-url').textContent=d.logger.url;document.getElementById('l-code').textContent=d.logger.last_http_code;document.getElementById('l-pen').textContent=d.logger.pending_log?'YES':'no';document.getElementById('uptime').textContent=ut(d.system_uptime_ms);setOnline(true);}catch(e){setOnline(false);}}");
  s += F("tick();setInterval(tick,2000);");
  s += F("</script></body></html>");
  return s;
}

void handleData() {
  String parkingStatus;
  if (freeSlots == 0)      parkingStatus = "FULL";
  else if (freeSlots == 1) parkingStatus = "ALMOST_FULL";
  else                     parkingStatus = "AVAILABLE";

  String json;
  json.reserve(700);
  json += "{";
  json += "\"free_spots\":";     json += String(freeSlots); json += ",";
  json += "\"parking_status\":\""; json += parkingStatus; json += "\",";

  json += "\"slots\":[";
  for (int i = 0; i < 3; i++) {
    long shown = isValidDistance(sensors[i].distance) ? sensors[i].distance : sensors[i].lastGoodDistance;
    bool noecho = !isValidDistance(sensors[i].distance);
    json += "{";
    json += "\"id\":\"P"; json += String(i + 1); json += "\",";
    json += "\"status\":\""; json += (sensors[i].occupied ? "BUSY" : "FREE"); json += "\",";
    json += "\"distance\":"; json += String(shown); json += ",";
    json += "\"no_echo\":"; json += (noecho ? "true" : "false");
    json += "}";
    if (i < 2) json += ",";
  }
  json += "],";

  json += "\"rfid\":{";
  json += "\"ready\":";              json += (rfidReady ? "true" : "false"); json += ",";
  json += "\"last_uid\":\"";         json += jsonEscape(lastUID);           json += "\",";
  json += "\"last_access_status\":\""; json += jsonEscape(lastAccessStatus); json += "\",";
  json += "\"last_access_message\":\""; json += jsonEscape(lastAccessMessage); json += "\"";
  json += "},";

  json += "\"barrier_status\":\""; json += barrierStatus; json += "\",";

  json += "\"logger\":{";
  json += "\"url\":\"";           json += LOG_SERVER_URL; json += "\",";
  json += "\"last_http_code\":";  json += String(lastLogHttpCode); json += ",";
  json += "\"pending_log\":";     json += (pendingLogExists ? "true" : "false");
  json += "},";

  json += "\"last_log_http_code\":"; json += String(lastLogHttpCode); json += ",";
  json += "\"pending_log\":";        json += (pendingLogExists ? "true" : "false"); json += ",";
  json += "\"system_uptime_ms\":";   json += String(millis());
  json += "}";

  server.send(200, "application/json; charset=utf-8", json);
}

void handleDebug() {
  String s;
  s.reserve(3700);
  s += F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  s += F("<meta http-equiv='refresh' content='2'>");
  s += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  s += F("<title>Smart Parking - Debug</title>");
  s += F("<style>");
  s += F("body{font-family:monospace;background:#0f172a;color:#e2e8f0;margin:0;padding:20px;line-height:1.4}");
  s += F(".wrap{max-width:880px;margin:0 auto}");
  s += F(".bar{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px}");
  s += F("h1{color:#4ade80;margin:0;font-size:22px}");
  s += F("h2{color:#93c5fd;margin:18px 0 8px;font-size:14px;text-transform:uppercase;letter-spacing:1px}");
  s += F(".back{background:#1e3a8a;color:#bfdbfe;padding:8px 14px;border-radius:6px;text-decoration:none;font-size:13px}");
  s += F(".back:hover{background:#1d4ed8;color:#fff}");
  s += F("table{border-collapse:collapse;margin-bottom:8px;width:100%;max-width:680px;background:#1e293b;border-radius:6px;overflow:hidden}");
  s += F("th,td{padding:6px 12px;border-bottom:1px solid #334155;text-align:left;font-size:13px}");
  s += F("th{background:#111827;color:#93c5fd;font-weight:normal;text-transform:uppercase;letter-spacing:.5px;font-size:11px}");
  s += F("tr:last-child td{border-bottom:none}");
  s += F(".ok{color:#4ade80;font-weight:bold}.bad{color:#f87171;font-weight:bold}");
  s += F("</style></head><body><div class='wrap'>");

  s += F("<div class='bar'><h1>Smart Parking - Debug</h1>");
  s += F("<a class='back' href='/'>&larr; Back to Dashboard</a></div>");

  s += F("<h2>System</h2><table>");
  s += F("<tr><th>RFID</th><td>");
  s += rfidReady ? F("<span class='ok'>OK</span>") : F("<span class='bad'>ERROR</span>");
  s += F("</td></tr>");
  s += F("<tr><th>WiFi AP SSID</th><td>"); s += AP_SSID; s += F("</td></tr>");
  s += F("<tr><th>ESP32 IP</th><td>"); s += WiFi.softAPIP().toString(); s += F("</td></tr>");
  s += F("<tr><th>Uptime</th><td>"); s += String(millis() / 1000UL); s += F(" s</td></tr>");
  s += F("<tr><th>Uptime (ms)</th><td>"); s += String(millis()); s += F("</td></tr>");
  s += F("<tr><th>Free heap</th><td>"); s += String(ESP.getFreeHeap()); s += F(" bytes</td></tr>");
  s += F("</table>");

  s += F("<h2>RFID</h2><table>");
  s += F("<tr><th>Last UID</th><td>"); s += lastUID; s += F("</td></tr>");
  s += F("<tr><th>Last access</th><td>"); s += lastAccessStatus; s += F("</td></tr>");
  s += F("<tr><th>Last message</th><td>"); s += lastAccessMessage; s += F("</td></tr>");
  s += F("</table>");

  s += F("<h2>Logging</h2><table>");
  s += F("<tr><th>LOG_SERVER_URL</th><td>"); s += LOG_SERVER_URL; s += F("</td></tr>");
  s += F("<tr><th>Last log HTTP code</th><td>"); s += String(lastLogHttpCode); s += F("</td></tr>");
  s += F("<tr><th>Pending log</th><td>");
  s += pendingLogExists ? F("YES") : F("no");
  s += F("</td></tr>");
  s += F("<tr><th>Barrier status</th><td>"); s += barrierStatus; s += F("</td></tr>");
  s += F("</table>");

  s += F("<h2>Sensors</h2><table>");
  s += F("<tr><th>#</th><th>Raw</th><th>Filtered</th><th>LastGood</th><th>Status</th></tr>");
  for (int i = 0; i < 3; i++) {
    s += F("<tr><td>P"); s += String(i + 1); s += F("</td>");
    s += F("<td>"); s += String(sensors[i].lastRawDistance); s += F("</td>");
    s += F("<td>"); s += String(sensors[i].distance); s += F("</td>");
    s += F("<td>"); s += String(sensors[i].lastGoodDistance); s += F("</td>");
    s += F("<td>");
    s += sensors[i].occupied ? F("<span class='bad'>BUSY</span>") : F("<span class='ok'>FREE</span>");
    s += F("</td></tr>");
  }
  s += F("</table>");

  s += F("<p>Free spots: <b>"); s += String(freeSlots); s += F(" / 3</b></p>");

  s += F("<h2>Thresholds</h2><table>");
  s += F("<tr><th>OCCUPIED_ENTER_CM</th><td>"); s += String(OCCUPIED_ENTER_CM); s += F("</td></tr>");
  s += F("<tr><th>OCCUPIED_EXIT_CM</th><td>");  s += String(OCCUPIED_EXIT_CM);  s += F("</td></tr>");
  s += F("<tr><th>SENSOR_CONFIRM_COUNT</th><td>"); s += String(SENSOR_CONFIRM_COUNT); s += F("</td></tr>");
  s += F("<tr><th>DIST_MIN_VALID_CM</th><td>"); s += String(DIST_MIN_VALID_CM); s += F("</td></tr>");
  s += F("<tr><th>DIST_MAX_VALID_CM</th><td>"); s += String(DIST_MAX_VALID_CM); s += F("</td></tr>");
  s += F("<tr><th>SENSOR_INTER_GAP_MS</th><td>"); s += String(SENSOR_INTER_GAP_MS); s += F("</td></tr>");
  s += F("</table>");

  s += F("</div></body></html>");

  server.send(200, "text/html; charset=utf-8", s);
}

long readDistanceRawCm(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 12000UL);
  if (duration == 0) return -1;
  long cm = duration / 58;
  if (cm < DIST_MIN_VALID_CM || cm > DIST_MAX_VALID_CM) return -1;
  return cm;
}

bool isValidDistance(long distance) {
  return distance >= DIST_MIN_VALID_CM && distance <= DIST_MAX_VALID_CM;
}

long readDistanceFilteredCm(int trigPin, int echoPin, long &lastRawOut) {
  long valid[3];
  int  validCount = 0;
  long lastRaw = -1;

  for (int i = 0; i < 3; i++) {
    long r = readDistanceRawCm(trigPin, echoPin);
    lastRaw = r;
    if (isValidDistance(r)) {
      valid[validCount++] = r;
    }
    if (i < 2) delay(10);
  }
  lastRawOut = lastRaw;

  if (validCount == 0) return -1;
  if (validCount == 1) return valid[0];
  if (validCount == 2) return (valid[0] + valid[1]) / 2;

  if (valid[0] > valid[1]) { long t = valid[0]; valid[0] = valid[1]; valid[1] = t; }
  if (valid[1] > valid[2]) { long t = valid[1]; valid[1] = valid[2]; valid[2] = t; }
  if (valid[0] > valid[1]) { long t = valid[0]; valid[0] = valid[1]; valid[1] = t; }
  return valid[1];
}

void updateOneSensor(ParkingSensor &s) {
  long raw = -1;
  long d = readDistanceFilteredCm(s.trigPin, s.echoPin, raw);
  s.lastRawDistance = raw;
  s.distance = d;

  if (!isValidDistance(d)) {
    Serial.print("Sensor trig=");
    Serial.print(s.trigPin);
    Serial.println(": NO ECHO");
    return;
  }

  s.lastGoodDistance = d;

  if (!s.occupied) {
    if (d < OCCUPIED_ENTER_CM) {
      s.busyConfirmCount++;
      s.freeConfirmCount = 0;
      if (s.busyConfirmCount >= SENSOR_CONFIRM_COUNT) {
        s.occupied = true;
      }
    } else {
      s.busyConfirmCount = 0;
    }
  } else {
    if (d > OCCUPIED_EXIT_CM) {
      s.freeConfirmCount++;
      s.busyConfirmCount = 0;
      if (s.freeConfirmCount >= SENSOR_CONFIRM_COUNT) {
        s.occupied = false;
      }
    } else {
      s.freeConfirmCount = 0;
    }
  }
}

void updateSensors() {
  for (int i = 0; i < 3; i++) {
    updateOneSensor(sensors[i]);
    if (i < 2) delay(SENSOR_INTER_GAP_MS);
  }

  distance1 = isValidDistance(sensors[0].distance) ? sensors[0].distance : sensors[0].lastGoodDistance;
  distance2 = isValidDistance(sensors[1].distance) ? sensors[1].distance : sensors[1].lastGoodDistance;
  distance3 = isValidDistance(sensors[2].distance) ? sensors[2].distance : sensors[2].lastGoodDistance;

  busy1 = sensors[0].occupied;
  busy2 = sensors[1].occupied;
  busy3 = sensors[2].occupied;

  freeSlots = (busy1 ? 0 : 1) + (busy2 ? 0 : 1) + (busy3 ? 0 : 1);
}

void drawFrame() {
  u8g2.drawFrame(0, 0, 128, 64);
}

void showStatus() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  u8g2.drawStr(2, 12, "SMART PARKING");

  char buf[24];
  snprintf(buf, sizeof(buf), "Free: %d / 3", freeSlots);
  u8g2.drawStr(2, 26, buf);

  snprintf(buf, sizeof(buf), "P1:%s P2:%s P3:%s",
           busy1 ? "B" : "F",
           busy2 ? "B" : "F",
           busy3 ? "B" : "F");
  u8g2.drawStr(2, 40, buf);

  u8g2.drawStr(2, 54, rfidReady ? "RFID OK" : "RFID OFF");

  drawFrame();
  u8g2.sendBuffer();
}

void showAccessGranted(String uid) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  u8g2.drawStr(0, 10, "ACCESS GRANTED");
  u8g2.drawStr(0, 24, uid.c_str());
  u8g2.drawStr(0, 38, "Barrier opening");

  drawFrame();
  u8g2.sendBuffer();
}

void showAccessDenied(String uid) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  u8g2.drawStr(0, 10, "ACCESS DENIED");
  u8g2.drawStr(0, 24, uid.c_str());
  u8g2.drawStr(0, 38, "Barrier closed");

  drawFrame();
  u8g2.sendBuffer();
}

void showParkingFull(String uid) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  u8g2.drawStr(0, 10, "CARD OK");
  u8g2.drawStr(0, 24, "BUT PARKING FULL");
  u8g2.drawStr(0, 38, uid.c_str());
  u8g2.drawStr(0, 52, "Barrier closed");

  drawFrame();
  u8g2.sendBuffer();
}

void playToneManual(int frequency, int durationMs) {
  if (frequency <= 0) {
    delay(durationMs);
    return;
  }

  long periodUs = 1000000L / frequency;
  long halfPeriodUs = periodUs / 2;
  long cycles = ((long)durationMs * 1000L) / periodUs;

  for (long i = 0; i < cycles; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(halfPeriodUs);

    digitalWrite(BUZZER_PIN, LOW);
    delayMicroseconds(halfPeriodUs);
  }

  digitalWrite(BUZZER_PIN, LOW);
}

void soundAccessGranted() {
  playToneManual(1800, 220);
}

void soundAccessDenied() {
  playToneManual(400, 250);
  delay(150);
  playToneManual(400, 250);
}

void soundParkingFull() {
  playToneManual(700, 150);
  delay(100);
  playToneManual(500, 300);
}

int angleToPulseUs(int angle) {
  angle = constrain(angle, 0, 180);
  return map(angle, 0, 180, 500, 2400);
}

void servoWriteFor(int angle, int durationMs) {
  int pulseUs = angleToPulseUs(angle);
  unsigned long startTime = millis();

  while (millis() - startTime < (unsigned long)durationMs) {
    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(pulseUs);

    digitalWrite(SERVO_PIN, LOW);
    delayMicroseconds(20000 - pulseUs);
  }

  digitalWrite(SERVO_PIN, LOW);
}

void closeBarrier() {
  servoWriteFor(SERVO_CLOSED_ANGLE, 700);
  digitalWrite(SERVO_PIN, LOW);
}

void openBarrier() {
  Serial.println("Opening barrier");
  barrierStatus = "OPENING";

  servoWriteFor(SERVO_OPEN_ANGLE, 900);
  delay(1500);

  Serial.println("Closing barrier");
  barrierStatus = "CLOSING";

  servoWriteFor(SERVO_CLOSED_ANGLE, 900);
  digitalWrite(SERVO_PIN, LOW);
  barrierStatus = "CLOSED";
}

String uidToString(MFRC522::Uid uid) {
  String s;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
    if (i < uid.size - 1) s += " ";
  }
  s.toUpperCase();
  return s;
}

void processRFIDFast() {
  if (millis() - lastRFIDReadTime < RFID_COOLDOWN_MS) return;

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  lastRFIDReadTime = millis();

  String uid = uidToString(rfid.uid);
  lastUID = uid;
  Serial.println();
  Serial.println("Card detected!");
  Serial.print("Card UID: ");
  Serial.println(uid);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (uid == ALLOWED_UID) {
    if (freeSlots > 0) {
      lastAccessStatus  = "GRANTED";
      lastAccessMessage = "Correct card. Barrier opened.";
      Serial.println("ACCESS GRANTED");

      showAccessGranted(uid);
      soundAccessGranted();
      openBarrier();

      queueAccessLog(uid, "GRANTED", "Correct card. Barrier opened.");

      rfid.PCD_Init();
    } else {
      lastAccessStatus  = "DENIED";
      lastAccessMessage = "Correct card, but parking is full.";
      Serial.println("PARKING FULL");

      showParkingFull(uid);
      soundParkingFull();

      queueAccessLog(uid, "DENIED", "Correct card, but parking is full.");
    }
  } else {
    lastAccessStatus  = "DENIED";
    lastAccessMessage = "Wrong RFID card.";
    Serial.println("ACCESS DENIED");

    showAccessDenied(uid);
    soundAccessDenied();

    queueAccessLog(uid, "DENIED", "Wrong RFID card.");
  }
}

String jsonEscape(String value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value.charAt(i);
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

String buildLogPayload(String uid, String accessStatus, String message,
                       int freeSpotsSnap,
                       bool busy1Snap, bool busy2Snap, bool busy3Snap,
                       long dist1Snap, long dist2Snap, long dist3Snap) {
  const char* p1s = busy1Snap ? "BUSY" : "FREE";
  const char* p2s = busy2Snap ? "BUSY" : "FREE";
  const char* p3s = busy3Snap ? "BUSY" : "FREE";

  String json;
  json.reserve(320);
  json += "{";
  json += "\"uid\":\"";           json += jsonEscape(uid);           json += "\",";
  json += "\"access_status\":\""; json += jsonEscape(accessStatus); json += "\",";
  json += "\"message\":\"";       json += jsonEscape(message);       json += "\",";
  json += "\"free_spots\":";      json += String(freeSpotsSnap);     json += ",";
  json += "\"p1_status\":\"";     json += p1s;                       json += "\",";
  json += "\"p1_distance\":";     json += String(dist1Snap);         json += ",";
  json += "\"p2_status\":\"";     json += p2s;                       json += "\",";
  json += "\"p2_distance\":";     json += String(dist2Snap);         json += ",";
  json += "\"p3_status\":\"";     json += p3s;                       json += "\",";
  json += "\"p3_distance\":";     json += String(dist3Snap);
  json += "}";
  return json;
}

void queueAccessLog(String uid, String accessStatus, String message) {
  if (!LOGGING_ENABLED) return;

  if (pendingLogExists) {
    Serial.print("Warning: overwriting undelivered pending log for UID ");
    Serial.println(pendingLogUID);
  }

  pendingLogUID         = uid;
  pendingLogStatus      = accessStatus;
  pendingLogMessage     = message;
  pendingLogFreeSpots   = freeSlots;
  pendingLogBusy1       = busy1;
  pendingLogBusy2       = busy2;
  pendingLogBusy3       = busy3;
  pendingLogDist1       = distance1;
  pendingLogDist2       = distance2;
  pendingLogDist3       = distance3;
  pendingLogNextAttempt = millis();
  pendingLogExists      = true;

  Serial.print("Log queued: ");
  Serial.print(uid);
  Serial.print(" / ");
  Serial.println(accessStatus);
}

void flushPendingLog() {
  if (!pendingLogExists) return;

  unsigned long now = millis();
  if ((long)(now - pendingLogNextAttempt) < 0) return;

  String payload = buildLogPayload(
    pendingLogUID, pendingLogStatus, pendingLogMessage,
    pendingLogFreeSpots,
    pendingLogBusy1, pendingLogBusy2, pendingLogBusy3,
    pendingLogDist1, pendingLogDist2, pendingLogDist3
  );

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(LOG_HTTP_TIMEOUT_MS);
  http.setTimeout(LOG_HTTP_TIMEOUT_MS);

  if (!http.begin(client, LOG_SERVER_URL)) {
    Serial.println("Pending log: http.begin() failed, will retry");
    lastLogHttpCode       = -999;
    pendingLogNextAttempt = millis() + LOG_RETRY_INTERVAL_MS;
    return;
  }
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);
  lastLogHttpCode = code;

  if (code > 0 && code < 400) {
    Serial.print("Pending log sent OK (HTTP ");
    Serial.print(code);
    Serial.println(")");
    pendingLogExists = false;
  } else {
    Serial.print("Pending log failed (code=");
    Serial.print(code);
    Serial.print(" ");
    Serial.print(http.errorToString(code));
    Serial.print("), retry in ");
    Serial.print(LOG_RETRY_INTERVAL_MS);
    Serial.println(" ms");
    pendingLogNextAttempt = millis() + LOG_RETRY_INTERVAL_MS;
  }

  http.end();
}
