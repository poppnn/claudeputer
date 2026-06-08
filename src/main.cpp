// =============================================================================
//  Claudeputer -- talk to Claude from an M5Stack Cardputer over WiFi
//
//  Works on BOTH the Cardputer 1.1 and the Cardputer ADV: the M5Cardputer
//  library (>= 1.2.0) auto-detects the board via M5.getBoard() and selects the
//  right keyboard driver (IO-matrix on 1.1, TCA8418 on ADV). A single binary
//  runs on either device.
//
//  Configuration (WiFi + Anthropic API key) can come from:
//    1. On-device setup screen  -> stored in NVS (Preferences)   [web-flash]
//    2. Compile-time src/config.h (optional)                     [local build]
//  NVS values take priority. If nothing is configured, the setup screen opens.
//
//  Controls:
//    INPUT  : type, ENTER = send, DEL = backspace
//             "/setup" + ENTER  -> reconfigure WiFi / API key
//             "/reset" + ENTER  -> clear conversation history
//    REPLY  : ; / .  scroll up/down, SPACE page down, `  back to input
// =============================================================================

#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>

// config.h is optional (git-ignored). Without it we fall back to empty values
// and the on-device setup screen handles configuration.
#if __has_include("config.h")
  #include "config.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
  #define WIFI_PASSWORD ""
#endif
#ifndef ANTHROPIC_API_KEY
  #define ANTHROPIC_API_KEY ""
#endif
#ifndef CLAUDE_MODEL
  #define CLAUDE_MODEL "claude-haiku-4-5"
#endif
#ifndef MAX_TOKENS
  #define MAX_TOKENS 1024
#endif
#ifndef SYSTEM_PROMPT
  #define SYSTEM_PROMPT "You are Claude running on a tiny M5Stack Cardputer with a 240x135 screen. Keep answers concise and plain-text. Avoid markdown tables and long code blocks."
#endif
#ifndef MAX_HISTORY_TURNS
  #define MAX_HISTORY_TURNS 6
#endif

// ---- Layout constants (text size 1: ~6px wide, ~8px tall glyphs) ------------
static const int   SCREEN_W   = 240;
static const int   SCREEN_H   = 135;
static const int   CHAR_W     = 6;
static const int   LINE_H     = 9;
static const int   COLS       = SCREEN_W / CHAR_W;        // ~40 chars/line
static const int   HEADER_H   = 12;
static const int   BODY_ROWS  = (SCREEN_H - HEADER_H) / LINE_H;

// ---- Colours ----------------------------------------------------------------
#define COL_BG      TFT_BLACK
#define COL_HEADER  0x5AEB
#define COL_USER    TFT_GREENYELLOW
#define COL_CLAUDE  TFT_WHITE
#define COL_ACCENT  0xFD20
#define COL_ERR     TFT_RED

// ---- Runtime configuration --------------------------------------------------
Preferences g_prefs;
struct AppConfig {
  String ssid;
  String pass;
  String apiKey;
  String model;
};
AppConfig g_cfg;

void loadConfig() {
  g_prefs.begin("claudeputer", true);                 // read-only
  g_cfg.ssid   = g_prefs.getString("ssid",   WIFI_SSID);
  g_cfg.pass   = g_prefs.getString("pass",   WIFI_PASSWORD);
  g_cfg.apiKey = g_prefs.getString("apikey", ANTHROPIC_API_KEY);
  g_cfg.model  = g_prefs.getString("model",  CLAUDE_MODEL);
  g_prefs.end();
}

void saveConfig() {
  g_prefs.begin("claudeputer", false);                // read-write
  g_prefs.putString("ssid",   g_cfg.ssid);
  g_prefs.putString("pass",   g_cfg.pass);
  g_prefs.putString("apikey", g_cfg.apiKey);
  g_prefs.putString("model",  g_cfg.model);
  g_prefs.end();
}

bool configComplete() {
  return g_cfg.ssid.length() > 0 && g_cfg.apiKey.length() > 0;
}

// ---- Conversation state -----------------------------------------------------
struct Turn { String role; String content; };
std::vector<Turn> g_history;

String g_input;
String g_lastReply;
int    g_scroll = 0;

// ---- Screen state -----------------------------------------------------------
// NOTE: enum values are PascalCase on purpose -- INPUT/OUTPUT/ERROR are Arduino
// macros and would break the enum at the preprocessor level.
enum class Screen { Setup, Input, Thinking, View, Error };
Screen g_screen = Screen::Input;

int    g_setupField = 0;           // 0 = SSID, 1 = password, 2 = API key
String g_setupBuf;
static const char* SETUP_LABELS[3] = {"WiFi SSID", "WiFi password", "Anthropic API key"};

const char* boardName() {
  switch (M5.getBoard()) {
    case m5::board_t::board_M5Cardputer:    return "Cardputer";
    case m5::board_t::board_M5CardputerADV: return "Cardputer ADV";
    default:                                return "ESP32-S3";
  }
}

// =============================================================================
//  UI helpers
// =============================================================================
void drawHeader(const String& title, uint16_t color) {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 0, SCREEN_W, HEADER_H, COL_BG);
  d.setTextSize(1);
  d.setTextColor(color, COL_BG);
  d.setCursor(2, 2);
  d.print(title);
  d.drawFastHLine(0, HEADER_H - 1, SCREEN_W, color);
}

std::vector<String> wrapText(const String& text, int maxChars) {
  std::vector<String> lines;
  String line, word;

  auto pushWord = [&]() {
    if (word.length() == 0) return;
    if (line.length() == 0) {
      while ((int)word.length() > maxChars) {
        lines.push_back(word.substring(0, maxChars));
        word = word.substring(maxChars);
      }
      line = word;
    } else if ((int)(line.length() + 1 + word.length()) <= maxChars) {
      line += " " + word;
    } else {
      lines.push_back(line);
      while ((int)word.length() > maxChars) {
        lines.push_back(word.substring(0, maxChars));
        word = word.substring(maxChars);
      }
      line = word;
    }
    word = "";
  };

  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '\n')      { pushWord(); lines.push_back(line); line = ""; }
    else if (c == ' ')  { pushWord(); }
    else if (c == '\r') { /* ignore */ }
    else                { word += c; }
  }
  pushWord();
  lines.push_back(line);
  return lines;
}

// =============================================================================
//  Screen renderers
// =============================================================================
void renderSetup() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  String hdr = "Setup " + String(g_setupField + 1) + "/3  [ENTER] next";
  drawHeader(hdr, COL_ACCENT);

  d.setTextColor(COL_HEADER, COL_BG);
  d.setCursor(2, HEADER_H + 4);
  d.print(SETUP_LABELS[g_setupField]);
  d.print(":");

  d.setTextColor(COL_USER, COL_BG);
  std::vector<String> lines = wrapText(g_setupBuf + "_", COLS);
  int y = HEADER_H + 4 + LINE_H;
  int start = max(0, (int)lines.size() - (BODY_ROWS - 2));
  for (int i = start; i < (int)lines.size(); i++) {
    d.setCursor(2, y);
    d.print(lines[i]);
    y += LINE_H;
  }
}

void renderInput() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader(String(boardName()) + "  [ENTER] send", COL_ACCENT);

  d.setTextColor(COL_USER, COL_BG);
  std::vector<String> lines = wrapText("> " + g_input, COLS);
  int start = max(0, (int)lines.size() - BODY_ROWS);
  int y = HEADER_H + 2;
  for (int i = start; i < (int)lines.size(); i++) {
    d.setCursor(2, y);
    d.print(lines[i]);
    y += LINE_H;
  }
}

void renderThinking() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("Claudeputer", COL_ACCENT);
  d.setTextColor(COL_CLAUDE, COL_BG);
  d.setCursor(2, SCREEN_H / 2 - 4);
  d.print("Thinking...");
}

void renderView() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(COL_BG);

  std::vector<String> lines = wrapText(g_lastReply, COLS);
  int total = lines.size();
  int maxScroll = max(0, total - BODY_ROWS);
  if (g_scroll > maxScroll) g_scroll = maxScroll;
  if (g_scroll < 0) g_scroll = 0;

  drawHeader("Claude  [;/.] scroll  [`] new", COL_HEADER);

  d.setTextColor(COL_CLAUDE, COL_BG);
  int y = HEADER_H + 2;
  for (int i = g_scroll; i < total && i < g_scroll + BODY_ROWS; i++) {
    d.setCursor(2, y);
    d.print(lines[i]);
    y += LINE_H;
  }

  if (maxScroll > 0) {
    int barH = max(6, (BODY_ROWS * (SCREEN_H - HEADER_H)) / total);
    int barY = HEADER_H + (g_scroll * (SCREEN_H - HEADER_H - barH)) / maxScroll;
    d.fillRect(SCREEN_W - 2, barY, 2, barH, COL_ACCENT);
  }
}

void renderError(const String& msg) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("Error  [`] back", COL_ERR);
  d.setTextColor(COL_ERR, COL_BG);
  std::vector<String> lines = wrapText(msg, COLS);
  int y = HEADER_H + 2;
  for (int i = 0; i < (int)lines.size() && i < BODY_ROWS; i++) {
    d.setCursor(2, y);
    d.print(lines[i]);
    y += LINE_H;
  }
}

// =============================================================================
//  Setup wizard
// =============================================================================
void startSetup() {
  g_screen     = Screen::Setup;
  g_setupField = 0;
  g_setupBuf   = g_cfg.ssid;          // prefill with current value
  renderSetup();
}

void setupAdvance() {
  // Store the current field, move to the next or finish.
  switch (g_setupField) {
    case 0: g_cfg.ssid   = g_setupBuf; break;
    case 1: g_cfg.pass   = g_setupBuf; break;
    case 2: g_cfg.apiKey = g_setupBuf; break;
  }
  g_setupField++;

  if (g_setupField <= 2) {
    g_setupBuf = (g_setupField == 1) ? g_cfg.pass : g_cfg.apiKey;
    renderSetup();
    return;
  }

  // Finished: persist and clear stale context.
  saveConfig();
  g_history.clear();
}

// =============================================================================
//  WiFi
// =============================================================================
bool connectWiFi() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader(String(boardName()), COL_ACCENT);
  d.setTextColor(COL_CLAUDE, COL_BG);
  d.setCursor(2, HEADER_H + 4);
  d.print("WiFi: ");
  d.print(g_cfg.ssid);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.begin(g_cfg.ssid.c_str(), g_cfg.pass.c_str());

  int dots = 0;
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    M5Cardputer.update();
    if (millis() - start > 20000) return false;
    d.setCursor(2, HEADER_H + 4 + LINE_H);
    d.print("Connecting");
    for (int i = 0; i < (dots % 4); i++) d.print(".");
    d.print("   ");
    dots++;
    delay(300);
  }

  d.setCursor(2, HEADER_H + 4 + 2 * LINE_H);
  d.setTextColor(COL_USER, COL_BG);
  d.print("OK  ");
  d.print(WiFi.localIP().toString());
  delay(800);
  return true;
}

// =============================================================================
//  Anthropic API call
// =============================================================================
bool askClaude(const String& prompt, String& out) {
  JsonDocument doc;
  doc["model"]      = g_cfg.model;
  doc["max_tokens"] = MAX_TOKENS;
  doc["system"]     = SYSTEM_PROMPT;

  JsonArray messages = doc["messages"].to<JsonArray>();
  for (const auto& t : g_history) {
    JsonObject m = messages.add<JsonObject>();
    m["role"]    = t.role;
    m["content"] = t.content;
  }
  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"]    = "user";
  userMsg["content"] = prompt;

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();               // v1: skip cert validation
  client.setTimeout(20000);

  HTTPClient https;
  https.setReuse(false);
  if (!https.begin(client, "https://api.anthropic.com/v1/messages")) {
    out = "https.begin() failed";
    return false;
  }
  https.addHeader("content-type", "application/json");
  https.addHeader("x-api-key", g_cfg.apiKey);
  https.addHeader("anthropic-version", "2023-06-01");

  int code = https.POST(body);
  String resp = https.getString();
  https.end();

  if (code != 200) {
    JsonDocument err;
    if (deserializeJson(err, resp) == DeserializationError::Ok &&
        err["error"]["message"].is<const char*>()) {
      out = "HTTP " + String(code) + ": " + err["error"]["message"].as<String>();
    } else {
      out = "HTTP " + String(code) + " (" + resp.substring(0, 120) + ")";
    }
    return false;
  }

  JsonDocument res;
  DeserializationError e = deserializeJson(res, resp);
  if (e) { out = String("JSON parse: ") + e.c_str(); return false; }

  String text;
  for (JsonObject block : res["content"].as<JsonArray>()) {
    if (block["type"] == "text") text += block["text"].as<String>();
  }
  if (text.length() == 0) text = "(empty response)";
  out = text;
  return true;
}

void trimHistory() {
  int maxMsgs = MAX_HISTORY_TURNS * 2;
  while ((int)g_history.size() > maxMsgs) g_history.erase(g_history.begin());
}

// =============================================================================
//  Main flow
// =============================================================================
void goInput() {
  g_screen = Screen::Input;
  renderInput();
}

void submitPrompt() {
  String prompt = g_input;
  prompt.trim();
  if (prompt.length() == 0) return;

  // Slash commands
  if (prompt == "/setup") { g_input = ""; startSetup(); return; }
  if (prompt == "/reset") {
    g_history.clear();
    g_input = "";
    g_lastReply = "Conversation history cleared.";
    g_scroll = 0;
    g_screen = Screen::View;
    renderView();
    return;
  }

  g_screen = Screen::Thinking;
  renderThinking();

  String reply;
  if (askClaude(prompt, reply)) {
    g_history.push_back({"user", prompt});
    g_history.push_back({"assistant", reply});
    trimHistory();
    g_input     = "";
    g_lastReply = reply;
    g_scroll    = 0;
    g_screen    = Screen::View;
    renderView();
  } else {
    g_screen = Screen::Error;
    renderError(reply);
  }
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextFont(&fonts::Font0);

  loadConfig();

  // Hold the top (G0) button while powering on to force reconfiguration.
  M5Cardputer.update();
  bool forceSetup = M5Cardputer.BtnA.isPressed();

  if (forceSetup || !configComplete()) {
    startSetup();
    return;          // the loop() handles connecting after the wizard
  }

  if (!connectWiFi()) {
    g_screen = Screen::Error;
    renderError("WiFi connection failed. Type /setup after this, or check credentials. [`] to continue.");
    return;
  }
  goInput();
}

void loop() {
  M5Cardputer.update();

  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
    delay(5);
    return;
  }

  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

  switch (g_screen) {
    case Screen::Setup: {
      if (status.enter) {
        setupAdvance();
        if (g_setupField > 2) {            // wizard finished
          if (connectWiFi()) goInput();
          else { g_screen = Screen::Error; renderError("WiFi failed. Type /setup to retry. [`] continue."); }
        }
        break;
      }
      if (status.del && g_setupBuf.length() > 0) g_setupBuf.remove(g_setupBuf.length() - 1);
      for (auto c : status.word) g_setupBuf += c;
      renderSetup();
      break;
    }

    case Screen::Input: {
      if (status.enter) { submitPrompt(); break; }
      if (status.del && g_input.length() > 0) g_input.remove(g_input.length() - 1);
      for (auto c : status.word) g_input += c;
      renderInput();
      break;
    }

    case Screen::View: {
      bool changed = false;
      for (auto c : status.word) {
        if (c == '`') { goInput(); return; }
        if (c == ';') { g_scroll -= 1; changed = true; }
        if (c == '.') { g_scroll += 1; changed = true; }
        if (c == ' ') { g_scroll += BODY_ROWS - 1; changed = true; }
      }
      if (changed) renderView();
      break;
    }

    case Screen::Error: {
      for (auto c : status.word) if (c == '`') { goInput(); return; }
      break;
    }

    case Screen::Thinking:
    default:
      break;
  }
}
