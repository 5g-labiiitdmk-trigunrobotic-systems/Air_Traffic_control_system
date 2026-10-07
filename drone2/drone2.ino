#include <ESP32Servo.h>

// ─────────────────────────────────────────────────────────────────────────
// ARM-REQUEST/APPROVAL BLOCK — TEMPLATE, NOT VERIFIED AGAINST REAL HARDWARE
// This file had no MAVLink/WiFi code at all before this addition (it was a
// standalone PWM-triggered servo sketch). Everything below is a best-guess
// scaffold using the standard MAVLink C library + ESP32 WiFi/HTTPClient
// conventions. Before this will compile/run you must:
//   1. Drop the generated `common/mavlink.h` (from the mavlink/mavlink
//      generator) into the empty drone2/mavlink/ folder and #include it.
//   2. Confirm which UART the flight controller's telemetry port is wired
//      to (assumed Serial2, RX2=16/TX2=17, 57600 baud below) and fix pins.
//   3. Fill in WIFI_SSID/WIFI_PASS/GIST_RAW_URL/DRONE_ID for your network.
//   4. Confirm mavlink_msg_command_long_pack's target_system/target_component
//      match your FC (assumed 1/1 below).
// ─────────────────────────────────────────────────────────────────────────
#include <WiFi.h>
#include <HTTPClient.h>
// #include "mavlink/common/mavlink.h"   // <-- place generated headers here, then uncomment

const char* WIFI_SSID   = "YOUR_WIFI_SSID";
const char* WIFI_PASS   = "YOUR_WIFI_PASS";
const int   SERVER_PORT = 5000;
const char* DRONE_ID    = "Drone-1";

// ── Server discovery via GitHub Gist ────────────────────────────────────
// 1. Get this from: https://gist.github.com
//    Create a Gist with a file named "drone_server.txt" (start.py on the
//    server laptop keeps its content updated with the server's current IP).
// 2. Click "Raw" on that drone_server.txt file and copy the URL it gives
//    you — paste it below as GIST_RAW_URL.
// 3. This is the ONLY thing that needs changing per drone — every drone
//    points at the SAME gist, so if the server's IP changes (new network,
//    new hotspot), every drone picks up the new address automatically on
//    its next boot. No reflashing needed for IP changes.
//    Example: https://gist.githubusercontent.com/trigunrobotics/71775a92c0a67d79d6974ad1e7fd83e8/raw/drone_server.txt
const char* GIST_RAW_URL = "PASTE_YOUR_GIST_RAW_URL_HERE";

bool   serverURLFetched = false;  // true once fetchServerURL() succeeds
String serverBase       = "";     // e.g. "http://192.168.1.42:5000" — filled at runtime
String serverIP         = "";     // e.g. "192.168.1.42" — filled at runtime

// Fetches GIST_RAW_URL, expects plain text like "http://192.168.1.42:5000".
// Sets serverBase/serverIP/serverURLFetched on success. Non-fatal on failure
// (caller retries) so a flaky Gist read never bricks the drone.
bool fetchServerURL() {
  HTTPClient http;
  http.begin(GIST_RAW_URL);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    body.trim();
    if (body.startsWith("http://") || body.startsWith("https://")) {
      serverBase = body;
      int hostStart = body.indexOf("://") + 3;
      int colon = body.indexOf(':', hostStart);
      int slash = body.indexOf('/', hostStart);
      int hostEnd = (colon > 0) ? colon : (slash > 0 ? slash : body.length());
      serverIP = body.substring(hostStart, hostEnd);
      serverURLFetched = true;
      Serial.println("[GIST] Server URL fetched: " + serverBase);
      http.end();
      return true;
    }
  }
  Serial.println("[GIST] Fetch failed (HTTP " + String(code) + "), will retry");
  http.end();
  return false;
}

#define MAVLINK_SERIAL   Serial2
#define MAVLINK_BAUD     57600
#define FC_SYSID         1
#define FC_COMPID        1

bool armPending = false;
unsigned long armPollTimer   = 0;
unsigned long armWaitStarted = 0;
const unsigned long ARM_POLL_INTERVAL_MS = 500;
const unsigned long ARM_WAIT_TIMEOUT_MS  = 30000;

String armServerUrl(const char* path) {
  return serverBase + path;
}

void sendArmRequest() {
  HTTPClient http;
  http.begin(armServerUrl("/drone/arm_request"));
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"drone_id\":\"") + DRONE_ID + "\"}";
  http.POST(body);
  http.end();
}

String pollArmPermission() {
  HTTPClient http;
  http.begin(armServerUrl((String("/drone/poll_arm_permission/") + DRONE_ID).c_str()));
  int code = http.GET();
  String status = "none";
  if (code == 200) {
    String payload = http.getString();
    if (payload.indexOf("\"approved\"") >= 0) status = "approved";
    else if (payload.indexOf("\"denied\"")  >= 0) status = "denied";
    else if (payload.indexOf("\"pending\"") >= 0) status = "pending";
  }
  http.end();
  return status;
}

// Forwards MAV_CMD_COMPONENT_ARM_DISARM to the FC. arm=true to arm, false to disarm.
void forwardArmCommandToFC(bool arm) {
  mavlink_message_t msg;
  mavlink_msg_command_long_pack(255, 0, &msg, FC_SYSID, FC_COMPID,
                                 MAV_CMD_COMPONENT_ARM_DISARM, 0,
                                 arm ? 1.0f : 0.0f, 0, 0, 0, 0, 0, 0);
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);
  MAVLINK_SERIAL.write(buf, len);
}

// Call once per loop() iteration. Non-blocking: reads whatever MAVLink
// bytes are available, watches for an arm attempt, and while armPending is
// true, polls Flask every 500ms (respecting a 30s safety timeout).
void pollAndHandleArming() {
  while (MAVLINK_SERIAL.available()) {
    uint8_t c = MAVLINK_SERIAL.read();
    mavlink_message_t msg;
    mavlink_status_t status;
    if (mavlink_parse_char(MAVLINK_COMM_0, c, &msg, &status)) {
      if (!armPending) {
        if (msg.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
          mavlink_heartbeat_t hb;
          mavlink_msg_heartbeat_decode(&msg, &hb);
          bool fcArmed = hb.base_mode & MAV_MODE_FLAG_SAFETY_ARMED;
          if (fcArmed) {
            armPending = true;
            armWaitStarted = millis();
            armPollTimer = millis();
            sendArmRequest();
            Serial.println("[ARM] Intercepted HEARTBEAT armed=true -> holding, requesting authority");
          }
        } else if (msg.msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
          mavlink_command_long_t cmd;
          mavlink_msg_command_long_decode(&msg, &cmd);
          if (cmd.command == MAV_CMD_COMPONENT_ARM_DISARM && cmd.param1 == 1) {
            armPending = true;
            armWaitStarted = millis();
            armPollTimer = millis();
            sendArmRequest();
            Serial.println("[ARM] Intercepted COMMAND_LONG arm request -> holding, requesting authority");
          }
        }
      }
    }
  }

  if (armPending) {
    unsigned long now = millis();
    if (now - armWaitStarted > ARM_WAIT_TIMEOUT_MS) {
      Serial.println("[ARM] Timed out waiting for authority -> auto-deny, disarming");
      forwardArmCommandToFC(false);
      armPending = false;
      return;
    }
    if (now - armPollTimer >= ARM_POLL_INTERVAL_MS) {
      armPollTimer = now;
      String status = pollArmPermission();
      if (status == "approved") {
        Serial.println("[ARM] Authority approved -> forwarding arm to FC");
        forwardArmCommandToFC(true);
        armPending = false;
      } else if (status == "denied") {
        Serial.println("[ARM] Authority denied -> disarming FC");
        forwardArmCommandToFC(false);
        armPending = false;
      }
      // "pending"/"none" -> keep waiting, non-blocking
    }
  }
}
// ─────────────────────────────────────────────────────────────────────────
// END ARM-REQUEST/APPROVAL BLOCK
// ─────────────────────────────────────────────────────────────────────────

const int triggerPin = 34;
const int servoPin   = 18;

Servo myServo;

const int restAngle    = 0;
const int triggerAngle = 90;
const int triggerThreshold = 1500;

volatile unsigned long pulseStart = 0;
volatile unsigned long pulseWidth = 0;
volatile bool newPulse = false;

void IRAM_ATTR handleInterrupt() {
  if (digitalRead(triggerPin) == HIGH) {
    pulseStart = micros();
  } else {
    unsigned long width = micros() - pulseStart;
    if (width >= 400 && width <= 2600) {
      pulseWidth = width;
      newPulse = true;
    }
  }
}

bool stableState = false;
int consistentCount = 0;
const int requiredConsistent = 5; // require 5 matching readings before acting

void setup() {
  Serial.begin(115200);

  // ── ARM-REQUEST/APPROVAL BLOCK setup — template, see notes above ──
  MAVLINK_SERIAL.begin(MAVLINK_BAUD);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[ARM] Connecting to WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " connected" : " FAILED (continuing offline)");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[GIST] Fetching server URL...");
    for (int attempt = 0; attempt < 10 && !serverURLFetched; attempt++) {
      if (fetchServerURL()) break;
      delay(1000);
    }
  }
  // ── end ARM-REQUEST/APPROVAL BLOCK setup ──

  pinMode(triggerPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(triggerPin), handleInterrupt, CHANGE);

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  myServo.setPeriodHertz(50);
  myServo.attach(servoPin, 500, 2500);
  delay(500); // let servo settle before first command
  myServo.write(restAngle);

  Serial.println("Ready.");
}

void loop() {
  if (!serverURLFetched) {
    static unsigned long lastFetchAttempt = 0;
    if (millis() - lastFetchAttempt >= 2000) { // retry every 2s, non-blocking
      lastFetchAttempt = millis();
      fetchServerURL();
    }
  } else {
    pollAndHandleArming(); // ARM-REQUEST/APPROVAL BLOCK — template, see notes above
  }

  if (newPulse) {
    newPulse = false;
    bool reading = pulseWidth > (unsigned long)triggerThreshold;

    if (reading == stableState) {
      consistentCount = 0; // no change, reset counter
    } else {
      consistentCount++;
      if (consistentCount >= requiredConsistent) {
        stableState = reading;
        consistentCount = 0;

        if (stableState) {
          Serial.println("CONFIRMED trigger -> 90");
          myServo.write(triggerAngle);
        } else {
          Serial.println("CONFIRMED release -> rest");
          myServo.write(restAngle);
        }
      }
    }
  }
}