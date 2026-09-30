/*
  ============================================================
  RFID VOTING MACHINE v3 — NodeMCU ESP8266 + Google Sheets
  Hardware: NodeMCU ESP8266, RC522, SSD1306 OLED (128x64),
            Buzzer, LED, Push Button
  ============================================================

  INTERACTION FLOW:
  ┌─────────────────────────────────────────────────────────┐
  │ IDLE         → Shows live tally. Waiting for card.      │
  │ CARD SCANNED → Enters SELECT mode. Candidate highlighted.│
  │ BTN SHORT    → Cycles A → B → C → A (in SELECT mode)    │
  │ BTN HOLD 2s  → Confirms vote + sends to Google Sheets   │
  │ BTN HOLD 5s  → Admin reset (IDLE only) + logs to Sheet  │
  │ DUPLICATE    → Rejected instantly, long low beep        │
  │ TIMEOUT 10s  → Returns to IDLE without voting           │
  └─────────────────────────────────────────────────────────┘

  PIN ASSIGNMENTS (NodeMCU ESP8266):
  ┌─────────────────────────────────────────────────────────┐
  │ RC522 SDA   → D8  (GPIO15)                              │
  │ RC522 SCK   → D5  (GPIO14)  [SPI CLK]                  │
  │ RC522 MOSI  → D7  (GPIO13)  [SPI MOSI]                 │
  │ RC522 MISO  → D6  (GPIO12)  [SPI MISO]                 │
  │ RC522 RST   → D0  (GPIO16)                              │
  │ RC522 3.3V  → 3V3                                       │
  │ RC522 GND   → GND                                       │
  │ OLED SDA    → D2  (GPIO4)   [I2C SDA]                  │
  │ OLED SCL    → D1  (GPIO5)   [I2C SCL]                  │
  │ OLED VCC    → 3V3 or VIN                                │
  │ OLED GND    → GND                                       │
  │ BUZZER (+)  → D3  (GPIO0)                               │
  │ LED (+)     → D4  (GPIO2) → 220Ω → GND                 │
  │ BUTTON      → D9  (GPIO3/RX) → GND  (INPUT_PULLUP)     │
  └─────────────────────────────────────────────────────────┘

  ⚠️  IMPORTANT NOTES FOR ESP8266:
  - D8 (GPIO15) is the RC522 SDA/SS pin. On NodeMCU this pin
    has a pull-down resistor on the board — that is fine for
    SPI CS, leave it as-is.
  - D0 (GPIO16) cannot use interrupts; used for RC522 RST
    which only needs digitalWrite — this is fine.
  - D3 (GPIO0) is the buzzer. GPIO0 is a boot-mode pin.
    The buzzer is passive (no current at boot), so this is
    safe. If your buzzer clicks at boot, move it to D9/RX.
  - D4 (GPIO2) has a built-in LED on NodeMCU (active LOW).
    Your external LED will also blink with it — that's fine.
  - D9 is the RX pin (GPIO3). Using it as a button means
    Serial monitor won't work while button is connected.
    If you need Serial debug, move button to D9 only after
    removing from RX, OR swap button to a free analog pin
    and use analogRead threshold.

  REQUIRED LIBRARIES (install via Arduino Library Manager):
  - MFRC522         by GithubCommunity
  - Adafruit SSD1306
  - Adafruit GFX Library
  - ESP8266WiFi     (comes with ESP8266 board package)
  - ESP8266HTTPClient (comes with ESP8266 board package)
  - WiFiClientSecure  (comes with ESP8266 board package)

  BOARD SETUP in Arduino IDE:
  1. File → Preferences → Additional Boards Manager URLs:
     https://arduino.esp8266.com/stable/package_esp8266com_index.json
  2. Tools → Board → Boards Manager → search "esp8266" → Install
  3. Tools → Board → "NodeMCU 1.0 (ESP-12E Module)"
  4. Tools → Upload Speed → 115200
  ============================================================
*/

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

// ═══════════════════════════════════════════════════════════════
//  ★ EDIT ZONE 1 — WiFi Credentials
// ═══════════════════════════════════════════════════════════════
const char* WIFI_SSID     = "YOUR_WIFI_NAME";       // ← change
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";   // ← change

// ═══════════════════════════════════════════════════════════════
//  ★ EDIT ZONE 2 — Google Apps Script Web App URL
//  After deploying your script: Extensions → Apps Script →
//  Deploy → New Deployment → Web App → copy the URL here.
// ═══════════════════════════════════════════════════════════════
const char* SCRIPT_URL = "YOUR_APP_SCRIPT_URL";   // ← change

// ═══════════════════════════════════════════════════════════════
//  ★ EDIT ZONE 3 — Candidate Names (must match Apps Script)
// ═══════════════════════════════════════════════════════════════
#define NUM_CANDIDATES 3
const char* CANDIDATES[NUM_CANDIDATES] = {
  "Araf",    // ← change
  "Alif",    // ← change
  "Raj"      // ← change
};

// ═══════════════════════════════════════════════════════════════
//  Pin Definitions — NodeMCU ESP8266
// ═══════════════════════════════════════════════════════════════
#define RST_PIN     D0   // GPIO16 — RC522 RST
#define SS_PIN      D8   // GPIO15 — RC522 SDA/SS
#define BUZZER_PIN  D3   // GPIO0  — Buzzer
#define LED_PIN     D4   // GPIO2  — LED (also onboard LED, active LOW)
#define BTN_PIN     D9   // GPIO3/RX — Push Button (GND when pressed)

// ── OLED ─────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
// I2C: SDA=D2(GPIO4), SCL=D1(GPIO5) — ESP8266 default I2C pins
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ── RFID ─────────────────────────────────────────────────────
MFRC522 rfid(SS_PIN, RST_PIN);

// ═══════════════════════════════════════════════════════════════
//  Voting State
// ═══════════════════════════════════════════════════════════════
int votes[NUM_CANDIDATES] = {0, 0, 0};

// Voter Registry — stores UIDs of cards that already voted
#define MAX_VOTERS 30
byte voterUIDs[MAX_VOTERS][4];
int  voterCount = 0;

// State Machine
enum State {
  STATE_IDLE,
  STATE_SELECT,
  STATE_SENDING,    // ← NEW: WiFi HTTP request in progress
  STATE_CONFIRM,
  STATE_REJECTED
};
State currentState = STATE_IDLE;

// Selection
int  selectedCandidate = 0;
byte activeUID[4];
unsigned long selectEnteredAt = 0;
#define SELECT_TIMEOUT_MS 10000

// Button
bool          btnPrevState  = HIGH;
unsigned long btnDownAt     = 0;
bool          btnLongFired  = false;

#define BTN_SHORT_MAX_MS  500
#define BTN_CONFIRM_MS   2000
#define BTN_RESET_MS     5000

// Display timing
unsigned long stateEnteredAt = 0;
#define CONFIRM_DISPLAY_MS  2000
#define REJECTED_DISPLAY_MS 1800

// ── Forward declarations ──────────────────────────────────────
void enterIdle();
void enterSelect();
void enterRejected();
void castVote();
void adminReset();
void drawTallyScreen();
void drawSelectScreen(unsigned long holdMs);
void drawSendingScreen();
void drawConfirmScreen(int candidateIdx, bool sheetOk);
void drawRejectedScreen();
void buzzCardScanned();
void buzzCycle();
void buzzVoteSuccess();
void buzzDoubleVote();
void buzzTimeout();
void buzzReset();
void flashLED(int times);
bool sendVoteToSheet(const char* candidate);
bool sendResetToSheet();

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(9600);

  // ── I2C for OLED on D2/D1 ───────────────────────────────────
  Wire.begin(D2, D1);   // SDA, SCL

  // ── SPI for RC522 ───────────────────────────────────────────
  SPI.begin();
  rfid.PCD_Init();

  // ── Pins ────────────────────────────────────────────────────
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BTN_PIN,    INPUT_PULLUP);

  // LED off (D4/GPIO2 is active LOW on NodeMCU)
  digitalWrite(LED_PIN, HIGH);

  // ── OLED ────────────────────────────────────────────────────
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED not found. Check wiring."));
    while (true);
  }

  showSplash();

  // ── Connect to WiFi ─────────────────────────────────────────
  connectWiFi();

  delay(600);
  enterIdle();
}

// ═══════════════════════════════════════════════════════════════
//  WIFI CONNECTION
// ═══════════════════════════════════════════════════════════════
void connectWiFi() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(22, 10);
  display.println(F("Connecting WiFi"));
  display.setCursor(10, 26);
  display.println(WIFI_SSID);
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    display.setCursor(50 + dots * 6, 44);
    display.print(F("."));
    display.display();
    dots = (dots + 1) % 6;
  }

  display.clearDisplay();
  display.setCursor(18, 16);
  display.println(F("WiFi Connected!"));
  display.setCursor(0, 32);
  display.print(F("IP: "));
  display.println(WiFi.localIP());
  display.display();

  Serial.print(F("WiFi connected. IP: "));
  Serial.println(WiFi.localIP());
  delay(1200);
}

// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  handleButton();
  handleStateTimeout();

  // Only scan RFID when idle
  if (currentState == STATE_IDLE) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      byte* uid = rfid.uid.uidByte;

      if (hasVoted(uid)) {
        enterRejected();
      } else {
        memcpy(activeUID, uid, 4);
        selectedCandidate = 0;
        enterSelect();
      }

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  BUTTON HANDLER
// ═══════════════════════════════════════════════════════════════
void handleButton() {
  bool btnNow = (digitalRead(BTN_PIN) == LOW);
  unsigned long now = millis();

  // Button just pressed
  if (btnNow && !btnPrevState) {
    btnDownAt    = now;
    btnLongFired = false;
  }

  // Button held — check thresholds
  if (btnNow && btnPrevState) {
    unsigned long held = now - btnDownAt;

    if (currentState == STATE_SELECT && !btnLongFired && held >= BTN_CONFIRM_MS) {
      btnLongFired = true;
      castVote();   // vote + send to sheet
    }

    if (currentState == STATE_IDLE && !btnLongFired && held >= BTN_RESET_MS) {
      btnLongFired = true;
      adminReset();
    }

    // Live hold-progress bar in SELECT mode
    if (currentState == STATE_SELECT && !btnLongFired) {
      drawSelectScreen(held);
    }
  }

  // Button released
  if (!btnNow && btnPrevState) {
    unsigned long held = now - btnDownAt;

    if (!btnLongFired && held < BTN_SHORT_MAX_MS) {
      if (currentState == STATE_SELECT) {
        selectedCandidate = (selectedCandidate + 1) % NUM_CANDIDATES;
        buzzCycle();
        drawSelectScreen(0);
      }
    }
    btnLongFired = false;
  }

  btnPrevState = btnNow;
}

// ═══════════════════════════════════════════════════════════════
//  STATE TIMEOUTS
// ═══════════════════════════════════════════════════════════════
void handleStateTimeout() {
  unsigned long now = millis();

  if (currentState == STATE_SELECT) {
    if (now - selectEnteredAt > SELECT_TIMEOUT_MS) {
      buzzTimeout();
      enterIdle();
    }
  }

  if (currentState == STATE_CONFIRM || currentState == STATE_REJECTED) {
    unsigned long dur = (currentState == STATE_CONFIRM)
                        ? CONFIRM_DISPLAY_MS : REJECTED_DISPLAY_MS;
    if (now - stateEnteredAt > dur) {
      enterIdle();
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  STATE TRANSITIONS
// ═══════════════════════════════════════════════════════════════
void enterIdle() {
  currentState = STATE_IDLE;
  drawTallyScreen();
}

void enterSelect() {
  currentState    = STATE_SELECT;
  selectEnteredAt = millis();
  drawSelectScreen(0);
  buzzCardScanned();

  Serial.print(F("Card scanned. UID: "));
  for (int i = 0; i < 4; i++) { Serial.print(activeUID[i], HEX); Serial.print(" "); }
  Serial.println();
}
void enterRejected() {
  currentState   = STATE_REJECTED;
  stateEnteredAt = millis();
  drawRejectedScreen();
  buzzDoubleVote();

  Serial.println(F("Vote rejected: card already voted."));
}
// ─────────────────────────────────────────────────────────────
//  castVote — records locally + sends to Google Sheets
// ─────────────────────────────────────────────────────────────
void castVote() {
  // ── 1. Update local tally ───────────────────────────────────
  votes[selectedCandidate]++;
  registerVoter(activeUID);

  // ── 2. Show "Sending…" screen ───────────────────────────────
  currentState = STATE_SENDING;
  drawSendingScreen();
  buzzVoteSuccess();
  flashLED(2);

  // ── 3. Send to Google Sheet ─────────────────────────────────
  bool ok = sendVoteToSheet(CANDIDATES[selectedCandidate]);

  // ── 4. Show result ──────────────────────────────────────────
  currentState   = STATE_CONFIRM;
  stateEnteredAt = millis();
  drawConfirmScreen(selectedCandidate, ok);

  if (!ok) flashLED(5);   // extra flashes if send failed

  Serial.print(F("Vote cast for: "));
  Serial.print(CANDIDATES[selectedCandidate]);
  Serial.println(ok ? F(" — Sheet OK") : F(" — Sheet FAILED (vote still counted locally)"));
}

// ─────────────────────────────────────────────────────────────
//  adminReset — clears local tally + logs reset to Sheet
// ─────────────────────────────────────────────────────────────
void adminReset() {
  for (int i = 0; i < NUM_CANDIDATES; i++) votes[i] = 0;
  memset(voterUIDs, 0, sizeof(voterUIDs));
  voterCount = 0;

  // Show reset screen
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(14, 14);
  display.println(F("** ADMIN RESET **"));
  display.setCursor(14, 30);
  display.println(F("Sending to sheet..."));
  display.display();

  // Send reset event to sheet
  bool ok = sendResetToSheet();

  display.setCursor(14, 46);
  display.println(ok ? F("Sheet reset logged.") : F("Sheet log FAILED."));
  display.display();

  buzzReset();
  delay(2000);
  enterIdle();

  Serial.print(F("Admin reset. Sheet log: "));
  Serial.println(ok ? F("OK") : F("FAILED"));
}

// ═══════════════════════════════════════════════════════════════
//  HTTP HELPERS — send data to Google Apps Script
//
//  Google Script URLs redirect (302) to a different domain.
//  ESP8266 HTTPClient follows redirects automatically when
//  using WiFiClientSecure with setInsecure().
//  We use setInsecure() because Google's cert chain changes
//  frequently and hard-coding a fingerprint breaks silently.
//  For a classroom/local project this is acceptable.
// ═══════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────
//  sendVoteToSheet
//  Calls: SCRIPT_URL?action=vote&candidate=NAME
// ─────────────────────────────────────────────────────────────
bool sendVoteToSheet(const char* candidate) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi lost. Attempting reconnect..."));
    connectWiFi();
  }

  // URL-encode spaces in candidate name (replace ' ' with '+')
  String candidateEncoded = String(candidate);
  candidateEncoded.replace(" ", "+");

  String url = String(SCRIPT_URL)
               + "?action=vote&candidate=" + candidateEncoded;

  Serial.print(F("GET "));
  Serial.println(url);

  WiFiClientSecure client;
  client.setInsecure();   // skip SSL cert verification (see note above)

  HTTPClient http;
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);   // 10s timeout

  int code = http.GET();
  String body = http.getString();
  http.end();

  Serial.print(F("HTTP code: ")); Serial.println(code);
  Serial.print(F("Response:  ")); Serial.println(body);

  // HTTP 200 + JSON contains "ok"
  return (code == 200 && body.indexOf("\"ok\"") >= 0);
}

// ─────────────────────────────────────────────────────────────
//  sendResetToSheet
//  Calls: SCRIPT_URL?action=reset_log
// ─────────────────────────────────────────────────────────────
bool sendResetToSheet() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  String url = String(SCRIPT_URL) + "?action=reset_log";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);

  int code = http.GET();
  String body = http.getString();
  http.end();

  return (code == 200 && body.indexOf("\"ok\"") >= 0);
}

// ═══════════════════════════════════════════════════════════════
//  VOTER REGISTRY
// ═══════════════════════════════════════════════════════════════
bool hasVoted(byte* uid) {
  for (int i = 0; i < voterCount; i++) {
    if (memcmp(voterUIDs[i], uid, 4) == 0) return true;
  }
  return false;
}

void registerVoter(byte* uid) {
  if (voterCount < MAX_VOTERS) {
    memcpy(voterUIDs[voterCount], uid, 4);
    voterCount++;
  }
}

// ═══════════════════════════════════════════════════════════════
//  DISPLAY SCREENS
// ═══════════════════════════════════════════════════════════════

void showSplash() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(14, 4);
  display.println(F("VOTING"));
  display.setCursor(10, 24);
  display.println(F("MACHINE"));
  display.setTextSize(1);
  display.setCursor(24, 50);
  display.println(F("NodeMCU v3"));
  display.display();
}

// ─────────────────────────────────────────────────────────────
void drawTallyScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(22, 0);
  display.println(F("LIVE TALLY"));
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  int maxV = 1;
  for (int i = 0; i < NUM_CANDIDATES; i++) if (votes[i] > maxV) maxV = votes[i];

  int totalVotes = 0;
  for (int i = 0; i < NUM_CANDIDATES; i++) totalVotes += votes[i];

  for (int i = 0; i < NUM_CANDIDATES; i++) {
    int y = 12 + i * 16;

    char name[6];
    strncpy(name, CANDIDATES[i], 5);
    name[5] = '\0';
    display.setCursor(0, y);
    display.print(name);

    int barW = map(votes[i], 0, maxV, 0, 76);
    display.drawRect(36, y, 76, 7, SSD1306_WHITE);
    if (barW > 0) display.fillRect(36, y, barW, 7, SSD1306_WHITE);

    display.setCursor(115, y);
    display.print(votes[i]);
  }

  display.drawLine(0, 54, 127, 54, SSD1306_WHITE);
  display.setCursor(0, 56);
  display.print(F("Total:"));
  display.print(totalVotes);
  display.setCursor(64, 56);
  display.print(F("Voters:"));
  display.print(voterCount);

  // WiFi indicator (top-right corner)
  if (WiFi.status() == WL_CONNECTED) {
    display.setCursor(116, 0);
    display.print(F("W"));   // "W" = WiFi OK
  }

  display.display();
}

// ─────────────────────────────────────────────────────────────
void drawSelectScreen(unsigned long holdMs) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(8, 0);
  display.println(F("SELECT CANDIDATE"));
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  for (int i = 0; i < NUM_CANDIDATES; i++) {
    int y = 13 + i * 14;

    if (i == selectedCandidate) {
      display.fillRoundRect(0, y - 1, 118, 12, 2, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(6, y);
    if (i == selectedCandidate) display.print(F("> "));
    else                         display.print(F("  "));
    display.print(CANDIDATES[i]);

    display.setTextColor(SSD1306_WHITE);
  }

  display.drawLine(0, 54, 127, 54, SSD1306_WHITE);
  display.setCursor(0, 56);

  if (holdMs < 300) {
    display.print(F("[short]cycle [hold]vote"));
  } else {
    int barW = constrain(map(holdMs, 0, BTN_CONFIRM_MS, 0, 96), 0, 96);
    display.print(F("Hold:"));
    display.drawRect(30, 57, 96, 6, SSD1306_WHITE);
    display.fillRect(30, 57, barW, 6, SSD1306_WHITE);
  }

  display.display();
}

// ─────────────────────────────────────────────────────────────
void drawSendingScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(20, 14);
  display.println(F("Sending vote..."));
  display.setCursor(6, 30);
  display.println(F("Please wait a moment"));
  // Animated dots drawn once — loop not needed since this
  // function returns immediately and HTTP call blocks briefly
  display.setCursor(54, 46);
  display.println(F("[ ... ]"));
  display.display();
}

// ─────────────────────────────────────────────────────────────
//  ok = whether the HTTP send succeeded
void drawConfirmScreen(int candidateIdx, bool sheetOk) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.drawRoundRect(48, 2, 32, 22, 4, SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(54, 6);
  display.print(F("OK"));

  display.setTextSize(1);
  display.setCursor(8, 30);
  display.print(F("Voted: "));
  display.println(CANDIDATES[candidateIdx]);

  display.setCursor(4, 44);
  if (sheetOk) {
    display.println(F("Sheet: saved ✓"));
  } else {
    display.println(F("Sheet: FAILED (local ok)"));
  }

  display.display();
}

// ─────────────────────────────────────────────────────────────
void drawRejectedScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.drawRoundRect(48, 2, 32, 22, 4, SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(54, 6);
  display.print(F("X!"));

  display.setTextSize(1);
  display.setCursor(14, 30);
  display.print(F("Already voted!"));
  display.setCursor(4, 44);
  display.print(F("Each card: 1 vote only"));
  display.display();
}

// ═══════════════════════════════════════════════════════════════
//  BUZZER PATTERNS
// ═══════════════════════════════════════════════════════════════
void buzzCardScanned() {
  tone(BUZZER_PIN, 900, 80);  delay(80);  noTone(BUZZER_PIN);
}
void buzzCycle() {
  tone(BUZZER_PIN, 1100, 60); delay(60);  noTone(BUZZER_PIN);
}
void buzzVoteSuccess() {
  tone(BUZZER_PIN, 880,  80);  delay(100);
  tone(BUZZER_PIN, 1100, 80);  delay(100);
  tone(BUZZER_PIN, 1320, 150); delay(150);
  noTone(BUZZER_PIN);
}
void buzzDoubleVote() {
  tone(BUZZER_PIN, 300, 700); delay(700); noTone(BUZZER_PIN);
}
void buzzTimeout() {
  tone(BUZZER_PIN, 600, 200); delay(250);
  tone(BUZZER_PIN, 500, 200); delay(200);
  noTone(BUZZER_PIN);
}
void buzzReset() {
  tone(BUZZER_PIN, 880, 100); delay(130);
  tone(BUZZER_PIN, 660, 100); delay(130);
  tone(BUZZER_PIN, 440, 200); delay(200);
  noTone(BUZZER_PIN);
}

// ═══════════════════════════════════════════════════════════════
//  LED FLASH
//  Note: D4/GPIO2 is active LOW on NodeMCU.
//  HIGH = LED off, LOW = LED on.
// ═══════════════════════════════════════════════════════════════
void flashLED(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW);  delay(80);
    digitalWrite(LED_PIN, HIGH); delay(80);
  }
}
