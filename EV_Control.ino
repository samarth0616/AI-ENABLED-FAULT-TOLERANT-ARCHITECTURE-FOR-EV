/*
  ============================================================
  EV Chassis — Web-Controlled Drive + Induced Fault Mode
  ============================================================
  Hardware (matches the Phase 1 electrical diagram):
    L298N #1 = LEFT side  (FL + RL motors wired in parallel on OUT1/OUT2)
      ENA -> GPIO 32
      IN1 -> GPIO 33
      IN2 -> GPIO 25

    L298N #2 = RIGHT side (FR + RR motors wired in parallel on OUT1/OUT2)
      ENA -> GPIO 26
      IN1 -> GPIO 27
      IN2 -> GPIO 14

    Both L298N boards get 12V + GND straight from the Main Switch.
    ESP32 is powered from the buck converter's 5V rail.
    ALL GNDs (battery, both L298N boards, buck converter, ESP32) are common.

  What this sketch does:
    - ESP32 starts its own WiFi Access Point (no router needed).
    - Serves a mobile-friendly web page with Forward / Backward /
      Left / Right / Stop buttons.
    - A "Induce Fault Mode" button simulates a right-side drivetrain
      fault: the right L298N is forced off regardless of command,
      so the car keeps moving in a degraded (left-side-only) mode
      instead of stopping completely — the same fault-tolerant
      philosophy as your BMS project, applied to the drive system.

  How to use:
    1. Upload this sketch to the ESP32 (Board: "ESP32 Dev Module").
    2. Open Serial Monitor at 115200 baud, wait for "HTTP server started".
    3. On your phone/laptop, connect to WiFi network "EV_RC_CAR"
       (password below).
    4. Open a browser to  http://192.168.4.1
    5. Use the on-screen buttons to drive. Tap "Induce Fault Mode"
       to test degraded-mode behavior; tap again to clear it.
  ============================================================
*/

#include <WiFi.h>
#include <WebServer.h>

// ---------------- WiFi Access Point credentials ----------------
const char* AP_SSID = "EV_RC_CAR";
const char* AP_PASS = "12345678";   // must be 8+ characters

// ---------------- Motor driver pins ----------------
// LEFT side  (L298N #1)
const int L_ENA = 32;
const int L_IN1 = 33;
const int L_IN2 = 25;

// RIGHT side (L298N #2)
const int R_ENA = 26;
const int R_IN1 = 27;
const int R_IN2 = 14;

WebServer server(80);

bool   faultMode     = false;   // true = simulated right-side drivetrain fault
String currentAction = "STOP";

// ================= Low-level motor helpers =================
void leftForward()  { digitalWrite(L_IN1, HIGH); digitalWrite(L_IN2, LOW);  digitalWrite(L_ENA, HIGH); }
void leftBackward() { digitalWrite(L_IN1, LOW);  digitalWrite(L_IN2, HIGH); digitalWrite(L_ENA, HIGH); }
void leftStop()      { digitalWrite(L_IN1, LOW);  digitalWrite(L_IN2, LOW);  digitalWrite(L_ENA, LOW);  }

void rightForward()  { digitalWrite(R_IN1, HIGH); digitalWrite(R_IN2, LOW);  digitalWrite(R_ENA, HIGH); }
void rightBackward() { digitalWrite(R_IN1, LOW);  digitalWrite(R_IN2, HIGH); digitalWrite(R_ENA, HIGH); }
void rightStop()      { digitalWrite(R_IN1, LOW);  digitalWrite(R_IN2, LOW);  digitalWrite(R_ENA, LOW);  }

void allStop() {
  leftStop();
  rightStop();
  currentAction = "STOP";
}

// ============= High-level driving commands (fault-aware) =============
// When faultMode is true, the RIGHT side is always forced off,
// simulating a lost drivetrain — the car degrades instead of dying.

void doForward() {
  currentAction = "FORWARD";
  leftForward();
  if (faultMode) rightStop(); else rightForward();
}

void doBackward() {
  currentAction = "BACKWARD";
  leftBackward();
  if (faultMode) rightStop(); else rightBackward();
}

void doLeft() {
  // Normal: pivot turn (left side backward, right side forward)
  // Fault:  only left side available -> curves left instead of pivoting
  currentAction = "LEFT";
  if (faultMode) { leftStop(); rightForward(); }
  else            { leftBackward(); rightForward(); }
}

void doRight() {
  // Normal: pivot turn (left side forward, right side backward)
  // Fault:  right side is dead -> left side alone can't pivot right,
  //         so we just drive the working side forward (best-effort)
  currentAction = "RIGHT";
  leftForward();
  if (faultMode) rightStop(); else rightBackward();
}

// ========================= Web page =========================
void handleRoot() {
  String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>EV RC Control</title>
<style>
  body { font-family: Arial, sans-serif; text-align: center; background:#1e1e1e; color:#eee; margin:0; padding:20px; }
  h1 { margin-top: 10px; font-size: 22px; }
  .pad { display: grid; grid-template-columns: 80px 80px 80px; grid-template-rows: 80px 80px 80px;
         gap: 10px; justify-content: center; margin: 30px auto; }
  button { font-size: 22px; border-radius: 10px; border: none; background:#3a7d44; color:white; }
  button:active { background:#2c5e33; }
  #stopBtn { background:#c0392b; font-size:14px; font-weight:bold; }
  #faultBtn { background:#e67e22; padding:14px 26px; font-size:16px; margin-top:25px;
              border-radius:8px; border:none; color:white; }
  #faultBtn.active { background:#c0392b; }
  #status { margin-top: 15px; font-size: 17px; }
  .badge { padding:4px 12px; border-radius:20px; font-weight:bold; }
  .ok { background:#27ae60; }
  .fault { background:#c0392b; }
</style>
</head>
<body>
<h1>EV Chassis Control</h1>
<div id="status">
  Action: <span id="action">STOP</span> &nbsp;|&nbsp;
  Mode: <span id="mode" class="badge ok">NORMAL</span>
</div>

<div class="pad">
  <div></div><button onclick="cmd('forward')">&#8593;</button><div></div>
  <button onclick="cmd('left')">&#8592;</button>
  <button id="stopBtn" onclick="cmd('stop')">STOP</button>
  <button onclick="cmd('right')">&#8594;</button>
  <div></div><button onclick="cmd('backward')">&#8595;</button><div></div>
</div>

<button id="faultBtn" onclick="toggleFault()">Induce Fault Mode</button>

<script>
function cmd(action) {
  fetch('/' + action).then(refreshStatus);
}
function toggleFault() {
  fetch('/togglefault').then(refreshStatus);
}
function refreshStatus() {
  fetch('/status').then(r => r.json()).then(d => {
    document.getElementById('action').innerText = d.action;
    const modeEl = document.getElementById('mode');
    const faultBtn = document.getElementById('faultBtn');
    if (d.fault) {
      modeEl.innerText = 'FAULT (right side down)';
      modeEl.className = 'badge fault';
      faultBtn.innerText = 'Clear Fault Mode';
      faultBtn.classList.add('active');
    } else {
      modeEl.innerText = 'NORMAL';
      modeEl.className = 'badge ok';
      faultBtn.innerText = 'Induce Fault Mode';
      faultBtn.classList.remove('active');
    }
  });
}
setInterval(refreshStatus, 1000);
window.onload = refreshStatus;
</script>
</body>
</html>
)HTML";
  server.send(200, "text/html", html);
}

// ========================= HTTP handlers =========================
void handleForward()  { doForward();  server.send(200, "text/plain", "OK"); }
void handleBackward() { doBackward(); server.send(200, "text/plain", "OK"); }
void handleLeft()     { doLeft();     server.send(200, "text/plain", "OK"); }
void handleRight()    { doRight();    server.send(200, "text/plain", "OK"); }
void handleStopReq()  { allStop();    server.send(200, "text/plain", "OK"); }

void handleToggleFault() {
  faultMode = !faultMode;
  allStop();  // safety: stop motors whenever the mode changes
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  String json = "{\"action\":\"" + currentAction + "\",\"fault\":" + (faultMode ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// ========================= Setup / Loop =========================
void setup() {
  Serial.begin(115200);

  pinMode(L_ENA, OUTPUT); pinMode(L_IN1, OUTPUT); pinMode(L_IN2, OUTPUT);
  pinMode(R_ENA, OUTPUT); pinMode(R_IN1, OUTPUT); pinMode(R_IN2, OUTPUT);
  allStop();  // ensure motors are off at boot

  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("Access Point started. IP address: ");
  Serial.println(WiFi.softAPIP());   // normally 192.168.4.1

  server.on("/", handleRoot);
  server.on("/forward", handleForward);
  server.on("/backward", handleBackward);
  server.on("/left", handleLeft);
  server.on("/right", handleRight);
  server.on("/stop", handleStopReq);
  server.on("/togglefault", handleToggleFault);
  server.on("/status", handleStatus);
  server.begin();

  Serial.println("HTTP server started. Connect to WiFi 'EV_RC_CAR' and browse to 192.168.4.1");
}

void loop() {
  server.handleClient();
}
