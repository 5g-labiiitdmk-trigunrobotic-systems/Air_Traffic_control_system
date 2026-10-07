/* IIITDM KURNOOL - DRONE ESP8266 FIRMWARE v6
 * ============================================
 * Works from ANYWHERE - no hardcoded server IP.
 * Server URL is fetched from a GitHub Gist on boot.
 *
 * HARDWARE
 *   ESP8266 (NodeMCU / Wemos D1 Mini)
 *   UART TX->RX / RX->TX wired to Pixhawk TELEM2 port
 *   Baud: 57600  (match with SERIAL2_BAUD in ArduCopter params)
 *
 * LIBRARIES (install via Arduino Library Manager)
 *   WiFiManager  by tzapu
 *   ArduinoJson  by bblanchon  (v6)
 *   MAVLink      (copy mavlink/ folder next to this .ino)
 *
 * ONE-TIME SETUP
 *   1. Set droneID below (unique per drone)
 *   2. Set GIST_RAW_URL to the permanent raw URL printed by start.py
 *      Get this from: https://gist.github.com
 *      Click "Raw" on your drone_server.txt gist and copy the URL
 *      Example: https://gist.githubusercontent.com/USERNAME/GIST_ID/raw/drone_server.txt
 *   3. Flash. On first boot the ESP opens WiFi portal "DroneSetup"
 *      Connect your phone to that AP and enter your hotspot credentials.
 *      Credentials saved to flash - never needed again.
 *
 * FLOW
 *   Boot -> WiFi -> fetch server URL from Gist-> push telemetry every 1s
 *   Pilot tries to arm -> ESP sends /pilot/request ->authority approves
 *   Authority queues commands -> ESP polls /drone/poll_commands every 1s
 */

   #include <ESP8266WiFi.h>
   #include <ESP8266HTTPClient.h>
   #include <WiFiClient.h>
   #include <WiFiManager.h>   // tzapu version 2.0.x+
#include "mavlink/common/mavlink.h"
#include <ArduinoJson.h>
#include <WiFiManager.h>

// ─────────────────────────────────────────
//  !! CONFIGURE THESE TWO !!
// ─────────────────────────────────────────
const char* droneID      = "Drone-2";
// Get this from: https://gist.github.com
// Click Raw on your drone_server.txt gist and copy the URL
// Example: https://gist.githubusercontent.com/USERNAME/GIST_ID/raw/drone_server.txt
const char* GIST_RAW_URL = "PASTE_YOUR_GIST_RAW_URL_HERE";
// ─────────────────────────────────────────

// Flight mode constants (ArduCopter)
#define MODE_STABILIZE  0
#define MODE_ALT_HOLD   2
#define MODE_AUTO       3
#define MODE_GUIDED     4
#define MODE_LOITER     5
#define MODE_RTL        6
#define MODE_LAND       9

// Safety thresholds
#define ALT_CEILING_M   30.0f
#define ALT_FLOOR_M      0.5f
#define BATT_LAND_V     14.4f
#define HB_TIMEOUT_MS   4500

// Server URL (filled at runtime from Gist)
char serverBase[200] = "";   // e.g. "https://xxxx.a.pinggy.link"
char serverIP[256]   = "";   // serverBase + "/update"
bool  serverURLFetched  = false;
unsigned long lastURLFetch = 0;
#define URL_REFRESH_MS  300000UL   // re-fetch every 5 min

// Local web server (for direct LAN commands as fallback)
ESP8266WebServer localServer(80);

// ── Telemetry state ───────────────────────────────────────────────────────────
float         lat              = 0.0f;
float         lon              = 0.0f;
float         alt              = 0.0f;
float         batt             = 0.0f;
String        flightMode       = "STABILIZE";
bool          armed            = false;
unsigned long lastHeartbeat    = 0;
bool          heartBeatOK      = false;
bool          gpsValid         = false;
bool          battFailsafeFired = false;
uint8_t       lastBaseMode     = 0;
bool          guidedRequested  = false;
unsigned long lastMoveTime     = 0;

// ── Arm/approval state ────────────────────────────────────────────────────────
bool          prevArmed         = false;
bool          armRequestSent    = false;
bool          flightApproved    = false;
bool          wasArmedInFlight  = false;
unsigned long armAttemptTime    = 0;
#define ARM_REQUEST_COOLDOWN_MS  10000UL

// ── Debug macros ──────────────────────────────────────────────────────────────
#define DBG(x)    Serial.println(x)
#define DBGf(...) Serial.printf(__VA_ARGS__)

// =============================================================================
//  MAVLink helpers
// =============================================================================

void sendSetMode(uint8_t customMode) {
  mavlink_message_t msg;
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  uint8_t baseMode = lastBaseMode | MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
  mavlink_msg_set_mode_pack(255, 200, &msg, 1, baseMode, customMode);
  uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);
  Serial.write(buf, len);
}

void sendCommandLong(uint16_t command,
                     float p1, float p2, float p3,
                     float p4, float p5, float p6, float p7) {
  mavlink_message_t msg;
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  mavlink_msg_command_long_pack(255, 200, &msg,
    1, 1, command, 0, p1, p2, p3, p4, p5, p6, p7);
  uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);
  Serial.write(buf, len);
}

void sendVelocity(float vx, float vy, float vz) {
  mavlink_message_t msg;
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  uint16_t type_mask = 0x0FC7;
  mavlink_msg_set_position_target_local_ned_pack(
    255, 200, &msg,
    millis(), 1, 1,
    MAV_FRAME_LOCAL_NED, type_mask,
    0, 0, 0,
    vx, vy, vz,
    0, 0, 0,
    0, 0
  );
  uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);
  Serial.write(buf, len);
}

void sendDisarm() {
  sendCommandLong(400, 0, 21196, 0, 0, 0, 0, 0);
  DBG("[SAFETY] Disarm sent");
}

void requestDataStreams() {
  mavlink_message_t msg;
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  uint16_t len;

  mavlink_msg_request_data_stream_pack(255, 200, &msg,
    1, 1, MAV_DATA_STREAM_EXTENDED_STATUS, 2, 1);
  len = mavlink_msg_to_send_buffer(buf, &msg);
  Serial.write(buf, len);

  mavlink_msg_request_data_stream_pack(255, 200, &msg,
    1, 1, MAV_DATA_STREAM_POSITION, 2, 1);
  len = mavlink_msg_to_send_buffer(buf, &msg);
  Serial.write(buf, len);

  mavlink_msg_request_data_stream_pack(255, 200, &msg,
    1, 1, MAV_DATA_STREAM_EXTRA1, 2, 1);
  len = mavlink_msg_to_send_buffer(buf, &msg);
  Serial.write(buf, len);
}

// =============================================================================
//  Parse incoming MAVLink from Pixhawk
// =============================================================================

void parseMAVLink() {
  mavlink_message_t msg;
  mavlink_status_t  status;

  while (Serial.available()) {
    uint8_t c = Serial.read();
    if (!mavlink_parse_char(MAVLINK_COMM_0, c, &msg, &status)) continue;

    switch (msg.msgid) {

      case MAVLINK_MSG_ID_HEARTBEAT: {
        mavlink_heartbeat_t hb;
        mavlink_msg_heartbeat_decode(&msg, &hb);
        lastHeartbeat = millis();
        heartBeatOK   = true;
        lastBaseMode  = hb.base_mode;

        bool currentArmed = (hb.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;

        // Pilot armed WITHOUT approval → force disarm
        if (currentArmed && !prevArmed && !flightApproved) {
          DBG("[SAFETY] Arm blocked — no approval. Disarming.");
          sendDisarm();
          delay(200);
          sendDisarm();   // send twice for reliability
        }

        // Pilot armed WITH approval
        if (currentArmed && !prevArmed && flightApproved) {
          DBG("[INFO] Arm approved — pilot cleared to fly");
        }

        // Track for disarm-reset
        if (currentArmed && flightApproved) wasArmedInFlight = true;
        if (!currentArmed && wasArmedInFlight) {
          DBG("[INFO] Disarmed after flight — authorization reset");
          flightApproved   = false;
          armRequestSent   = false;
          wasArmedInFlight = false;
          armAttemptTime   = 0;
        }

        // Arm attempt without request sent yet → send request
        if (currentArmed && !prevArmed && !armRequestSent && !flightApproved) {
          unsigned long now = millis();
          if (now - armAttemptTime > ARM_REQUEST_COOLDOWN_MS) {
            armAttemptTime  = now;
            armRequestSent  = true;
            sendFlightRequest();
          }
        }

        prevArmed = currentArmed;
        armed     = currentArmed;

        // Decode mode string
        switch (hb.custom_mode) {
          case 0:             flightMode = "STABILIZE"; break;
          case 1:             flightMode = "ACRO";      break;
          case MODE_ALT_HOLD: flightMode = "ALT_HOLD";  break;
          case MODE_AUTO:     flightMode = "AUTO";       break;
          case MODE_GUIDED:   flightMode = "GUIDED";     break;
          case MODE_LOITER:   flightMode = "LOITER";     break;
          case MODE_RTL:      flightMode = "RTL";        break;
          case 7:             flightMode = "CIRCLE";     break;
          case MODE_LAND:     flightMode = "LAND";       break;
          default: flightMode = "MODE_" + String(hb.custom_mode);
        }
        break;
      }

      case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t p;
        mavlink_msg_global_position_int_decode(&msg, &p);
        if (p.lat != 0 || p.lon != 0) {
          lat      = p.lat          / 10000000.0f;
          lon      = p.lon          / 10000000.0f;
          alt      = p.relative_alt / 1000.0f;
          gpsValid = true;
        }
        break;
      }

      case MAVLINK_MSG_ID_SYS_STATUS: {
        mavlink_sys_status_t s;
        mavlink_msg_sys_status_decode(&msg, &s);
        batt = s.voltage_battery / 1000.0f;
        break;
      }
    }
  }

  // Heartbeat timeout
  if (lastHeartbeat > 0 && millis() - lastHeartbeat > HB_TIMEOUT_MS) {
    if (heartBeatOK) DBG("[WARN] Heartbeat lost");
    heartBeatOK = false;
    gpsValid    = false;
  }
}

// =============================================================================
//  Server URL discovery via GitHub Gist
// =============================================================================

void fetchServerURL() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, GIST_RAW_URL);
  http.setTimeout(6000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code == 200) {
    String url = http.getString();
    url.trim();
    if (url.startsWith("http") && url.length() > 10) {
      url.toCharArray(serverBase, sizeof(serverBase));
      String full = url + "/update";
      full.toCharArray(serverIP, sizeof(serverIP));
      serverURLFetched = true;
      lastURLFetch     = millis();
      DBGf("[URL] Server: %s\n", serverBase);
    } else {
      DBG("[URL] Invalid content in Gist");
    }
  } else {
    DBGf("[URL] Gist fetch failed: HTTP %d\n", code);
  }
  http.end();
}

// =============================================================================
//  Telemetry push → Flask /update
// =============================================================================

void sendTelemetry() {
  if (WiFi.status() != WL_CONNECTED || !serverURLFetched) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, serverIP);
  http.setTimeout(800);
  http.addHeader("Content-Type", "application/json");

  String json =
    "{\"id\":\""   + String(droneID)                      + "\""
    ",\"lat\":"    + String(lat,  6)                       +
    ",\"lon\":"    + String(lon,  6)                       +
    ",\"alt\":"    + String(alt,  1)                       +
    ",\"batt\":"   + String(batt, 2)                       +
    ",\"mode\":\"" + flightMode                            + "\""
    ",\"mav_hb\":" + (heartBeatOK ? "true" : "false")      +
    ",\"armed\":"  + (armed       ? "true" : "false")      +
    ",\"gps_ok\":" + (gpsValid    ? "true" : "false")      +
    ",\"ip\":\""   + WiFi.localIP().toString()             + "\"}";

  int code = http.POST(json);
  if (code != 200) DBGf("[TEL] POST failed: %d\n", code);
  http.end();
}

// =============================================================================
//  Flight request → Flask /pilot/request
// =============================================================================

void sendFlightRequest() {
  if (WiFi.status() != WL_CONNECTED || !serverURLFetched) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String(serverBase) + "/pilot/request";
  http.begin(client, url);
  http.setTimeout(800);
  http.addHeader("Content-Type", "application/json");
  String json = "{\"drone_id\":\"" + String(droneID) + "\"}";
  int code = http.POST(json);
  DBGf("[REQ] Flight request → HTTP %d\n", code);
  http.end();
}

// =============================================================================
//  Check approval → Flask /pilot/status/<id>
// =============================================================================

void checkApproval()
{
  if (WiFi.status() != WL_CONNECTED || !serverURLFetched) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String(serverBase) + "/pilot/status/" + String(droneID);
  http.begin(client, url);
  http.setTimeout(800);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, body)) {
      bool   isApproved = doc["approved"] | false;
      String status     = doc["status"]   | "none";

      if (isApproved && !flightApproved) {
        flightApproved = true;
        DBG("[INFO] Flight APPROVED — pilot can arm via RC");
      }
      if (!isApproved) {
        flightApproved = false;
        if (armRequestSent && (status == "denied" || status == "none")) {
          armRequestSent = false;
          armAttemptTime = 0;
          DBG("[INFO] Authorization reset");
        }
      }
    }
  }
  http.end();
}

// =============================================================================
//  Poll commands → Flask /drone/poll_commands/<id>
// =============================================================================

void pollCommands() {
  if (WiFi.status() != WL_CONNECTED || !serverURLFetched) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String(serverBase) + "/drone/poll_commands/" + String(droneID);
  http.begin(client, url);
  http.setTimeout(800);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, body) && doc.is<JsonArray>()) {
      JsonArray arr = doc.as<JsonArray>();
      for (JsonObject cmd : arr) {
        String type = cmd["cmd"] | "";
        DBGf("[CMD] Received: %s\n", type.c_str());
        if      (type == "rtl")   sendSetMode(MODE_RTL);
        else if (type == "land")  sendSetMode(MODE_LAND);
        else if (type == "kill")  sendSetMode(MODE_LAND);
        else if (type == "hover") sendSetMode(MODE_LOITER);
        else if (type == "move") {
          float vx = cmd["vx"] | 0.0f;
          float vy = cmd["vy"] | 0.0f;
          float vz = cmd["vz"] | 0.0f;
          if (!guidedRequested && flightMode != "GUIDED") {
            sendSetMode(MODE_GUIDED);
            guidedRequested = true;
          }
          sendVelocity(vx, vy, vz);
          lastMoveTime = millis();
        }
      }
    }
  }
  http.end();
}

// =============================================================================
//  Failsafes
// =============================================================================

void velocityFailsafe() {
  if (lastMoveTime == 0) return;
  if (millis() - lastMoveTime > 500) {
    sendVelocity(0.0f, 0.0f, 0.0f);
    lastMoveTime = 0;
  }
}

void batteryFailsafe() {
  if (battFailsafeFired || batt < 1.0f || !armed) return;
  if (batt < BATT_LAND_V) {
    DBGf("[WARN] Low battery %.2fV — auto-landing\n", batt);
    sendVelocity(0.0f, 0.0f, 0.0f);
    sendSetMode(MODE_LAND);
    battFailsafeFired = true;
  }
}

// =============================================================================
//  Local web server handlers (LAN fallback / direct commands)
// =============================================================================

void addCORS() {
  localServer.sendHeader("Access-Control-Allow-Origin",  "*");
  localServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  localServer.sendHeader("Access-Control-Allow-Headers", "*");
}
void handleCORS()  { addCORS(); localServer.send(204); }
void handleRTL()   { sendSetMode(MODE_RTL);    addCORS(); localServer.send(200, "text/plain", "RTL_SENT"); }
void handleHover() { sendSetMode(MODE_LOITER); addCORS(); localServer.send(200, "text/plain", "HOVER_SENT"); }
void handleLand()  { sendSetMode(MODE_LAND);   addCORS(); localServer.send(200, "text/plain", "LAND_SENT"); }
void handleKill()  { sendSetMode(MODE_LAND);   addCORS(); localServer.send(200, "text/plain", "KILL_SENT"); }

void handleMove() {
  addCORS();
  if (!gpsValid)      { localServer.send(409, "text/plain", "NO_GPS");        return; }
  if (!flightApproved){ localServer.send(403, "text/plain", "NOT_APPROVED");  return; }
  if (!heartBeatOK)   { localServer.send(409, "text/plain", "NO_HEARTBEAT");  return; }
  if (!armed)         { localServer.send(409, "text/plain", "NOT_ARMED");     return; }
  if (!localServer.hasArg("plain")) { localServer.send(400, "text/plain", "NO_BODY"); return; }

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, localServer.arg("plain"))) {
    localServer.send(400, "text/plain", "JSON_ERR");
    return;
  }
  float vx = constrain((float)(doc["vx"] | 0.0f), -5.0f, 5.0f);
  float vy = constrain((float)(doc["vy"] | 0.0f), -5.0f, 5.0f);
  float vz = constrain((float)(doc["vz"] | 0.0f), -3.0f, 3.0f);
  if (alt >= ALT_CEILING_M && vz < 0.0f) vz = 0.0f;
  if (alt <= ALT_FLOOR_M   && vz > 0.0f) vz = 0.0f;
  if (!guidedRequested && flightMode != "GUIDED") {
    sendSetMode(MODE_GUIDED);
    guidedRequested = true;
  }
  sendVelocity(vx, vy, vz);
  lastMoveTime = millis();
  localServer.send(200, "text/plain", "MOVE_SENT");
}

void handleTest() {
  String json = "{\"status\":\"ok\",\"drone\":\"" + String(droneID) +
                "\",\"ip\":\""   + WiFi.localIP().toString() +
                "\",\"batt\":"   + String(batt, 2) +
                ",\"mode\":\""   + flightMode + "\""
                ",\"server\":\"" + String(serverBase) + "\"}";
  addCORS();
  localServer.send(200, "application/json", json);
}

// =============================================================================
//  SETUP
// =============================================================================

void setup() {
  Serial.begin(57600);   // MAVLink to Pixhawk — match SERIAL2_BAUD param
  DBG("\n[BOOT] IIITDM Kurnool Drone Firmware v6");
  DBGf("[BOOT] Drone ID: %s\n", droneID);

  // WiFiManager — opens portal "DroneSetup" on first boot
  WiFiManager wm;
  wm.setConfigPortalTimeout(120);
  wm.setConnectTimeout(30);

  if (!wm.autoConnect("DroneSetup")) {
    DBG("[WIFI] Failed to connect — restarting in 3s");
    delay(3000);
    ESP.restart();
  }
  DBGf("[WIFI] Connected. IP: %s\n", WiFi.localIP().toString().c_str());

  // Fetch server URL from Gist (retry up to 5 times)
  for (int i = 0; i < 5 && !serverURLFetched; i++) {
    DBGf("[URL] Fetch attempt %d/5...\n", i + 1);
    fetchServerURL();
    if (!serverURLFetched) delay(2000);
  }

  if (serverURLFetched) {
    DBGf("[URL] Ready. Server: %s\n", serverBase);
  } else {
    DBG("[URL] !! Could not reach Gist. Will retry in loop.");
  }

  // Local web server routes
  localServer.on("/rtl",   HTTP_GET,     handleRTL);
  localServer.on("/hover", HTTP_GET,     handleHover);
  localServer.on("/land",  HTTP_GET,     handleLand);
  localServer.on("/kill",  HTTP_GET,     handleKill);
  localServer.on("/move",  HTTP_POST,    handleMove);
  localServer.on("/test",  HTTP_GET,     handleTest);
  localServer.on("/rtl",   HTTP_OPTIONS, handleCORS);
  localServer.on("/hover", HTTP_OPTIONS, handleCORS);
  localServer.on("/land",  HTTP_OPTIONS, handleCORS);
  localServer.on("/kill",  HTTP_OPTIONS, handleCORS);
  localServer.on("/move",  HTTP_OPTIONS, handleCORS);
  localServer.begin();
  DBG("[HTTP] Local server on port 80");

  requestDataStreams();
  DBG("[BOOT] Setup complete\n");
}

// =============================================================================
//  LOOP
// =============================================================================

void loop() {
  localServer.handleClient();
  parseMAVLink();
  velocityFailsafe();
  batteryFailsafe();

  // Re-fetch server URL every 5 min (handles tunnel restarts)
  if (!serverURLFetched || millis() - lastURLFetch > URL_REFRESH_MS) {
    fetchServerURL();
  }

  // Poll approval while request pending and not yet approved
  static unsigned long lastApprovalCheck = 0;
  if (armRequestSent && !flightApproved &&
      millis() - lastApprovalCheck > 2000) {
    checkApproval();
    lastApprovalCheck = millis();
  }

  // Telemetry + command poll every 1s
  static unsigned long lastTel = 0;
  if (millis() - lastTel > 1000) {
    sendTelemetry();
    pollCommands();
    lastTel = millis();
  }
}
