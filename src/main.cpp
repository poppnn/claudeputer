// =============================================================================
//  Claudeputer -- talk to Claude from an M5Stack Cardputer over WiFi
//
//  Works on BOTH the Cardputer 1.1 and the Cardputer ADV: the M5Cardputer
//  library (>= 1.2.0) auto-detects the board via M5.getBoard() and selects the
//  right keyboard driver (IO-matrix on 1.1, TCA8418 on ADV). A single binary
//  runs on either device.
//
//  UI: a chat view with message bubbles (you on the right, Claude on the left),
//  a status bar (WiFi + battery), a thin token-usage bar, and a rounded input
//  bar with a blinking cursor. Everything is composed off-screen on a PSRAM
//  sprite for flicker-free rendering. Replies stream in token-by-token (SSE).
//
//  Configuration (WiFi + Anthropic API key) can come from:
//    1. On-device setup wizard -> stored in NVS (Preferences)   [web-flash]
//    2. Compile-time src/config.h (optional)                    [local build]
//  NVS values take priority. If nothing is configured, setup opens.
//
//  Controls (chat):
//    type ........... write a prompt        ENTER ... send
//    DEL ............ backspace             Fn + ; / . scroll transcript
//    "/setup" ....... reconfigure WiFi/key  "/reset" . clear conversation
//    "/tokens" ...... show token usage + estimated cost
// =============================================================================

#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include <cstring>

// config.h is optional (git-ignored). Without it we fall back to empty values
// and the on-device setup wizard handles configuration.
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
// Token-usage bar: how many session tokens fill the bar.
#ifndef TOKEN_BUDGET
  #define TOKEN_BUDGET 100000
#endif
// Approximate USD pricing per 1M tokens (default = Claude Haiku 4.5). Adjust to
// match your model in config.h. Used only for the /tokens cost estimate.
#ifndef PRICE_IN_PER_MTOK
  #define PRICE_IN_PER_MTOK  1.0
#endif
#ifndef PRICE_OUT_PER_MTOK
  #define PRICE_OUT_PER_MTOK 5.0
#endif

// ---- Geometry (Font0 glyphs are 6x8 px) -------------------------------------
static const int SCREEN_W  = 240;
static const int SCREEN_H  = 135;
static const int CHAR_W    = 6;
static const int LINE_H    = 9;
static const int STATUS_H  = 14;
static const int TOKBAR_Y  = STATUS_H + 1;
static const int TOKBAR_H  = 2;
static const int INPUT_H   = 18;
static const int CHAT_TOP  = TOKBAR_Y + TOKBAR_H + 1;          // 18
static const int CHAT_H    = SCREEN_H - CHAT_TOP - INPUT_H - 1;
static const int BUB_PAD   = 4;
static const int BUB_GAP   = 5;
static const int BUB_RAD   = 5;
static const int MAX_BUB_CHARS = 28;   // 28 * 6 = 168 px of text per bubble

// ---- Theme (RGB565) ---------------------------------------------------------
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t C_BG        = rgb565( 13,  13,  18);
constexpr uint16_t C_STATUS    = rgb565( 24,  24,  30);
constexpr uint16_t C_MUTED     = rgb565(140, 140, 150);
constexpr uint16_t C_DIM       = rgb565( 60,  60,  68);
constexpr uint16_t C_ACCENT    = rgb565(217, 119,   6);   // Claude orange
constexpr uint16_t C_USER_BUB  = rgb565(217, 119,   6);
constexpr uint16_t C_USER_TX   = rgb565( 26,  16,   2);
constexpr uint16_t C_CLAUDE_BUB= rgb565( 38,  38,  46);
constexpr uint16_t C_CLAUDE_TX = rgb565(242, 242, 247);
constexpr uint16_t C_INFO_TX   = rgb565(150, 200, 255);
constexpr uint16_t C_ERR_BUB   = rgb565( 70,  22,  22);
constexpr uint16_t C_ERR_TX    = rgb565(255, 120, 120);
constexpr uint16_t C_INPUT_BG  = rgb565( 30,  30,  38);
constexpr uint16_t C_INPUT_TX  = rgb565(236, 236, 241);
constexpr uint16_t C_LOWBAT    = rgb565(220,  60,  60);

// ---- Off-screen canvas ------------------------------------------------------
M5Canvas canvas(&M5Cardputer.Display);

// ---- Runtime configuration --------------------------------------------------
Preferences g_prefs;
struct AppConfig { String ssid, pass, apiKey, model; };
AppConfig g_cfg;

void loadConfig() {
  g_prefs.begin("claudeputer", true);
  g_cfg.ssid   = g_prefs.getString("ssid",   WIFI_SSID);
  g_cfg.pass   = g_prefs.getString("pass",   WIFI_PASSWORD);
  g_cfg.apiKey = g_prefs.getString("apikey", ANTHROPIC_API_KEY);
  g_cfg.model  = g_prefs.getString("model",  CLAUDE_MODEL);
  g_prefs.end();
}
void saveConfig() {
  g_prefs.begin("claudeputer", false);
  g_prefs.putString("ssid",   g_cfg.ssid);
  g_prefs.putString("pass",   g_cfg.pass);
  g_prefs.putString("apikey", g_cfg.apiKey);
  g_prefs.putString("model",  g_cfg.model);
  g_prefs.end();
}
bool configComplete() { return g_cfg.ssid.length() > 0 && g_cfg.apiKey.length() > 0; }

// ---- Conversation -----------------------------------------------------------
struct Msg { String role; String content; };   // role: user | assistant | error | info
std::vector<Msg> g_msgs;

String g_input;
int    g_scrollPx   = 1 << 20;     // large => clamp to bottom
bool   g_cursorOn   = true;
unsigned long g_lastBlink = 0;

// ---- Token accounting -------------------------------------------------------
uint32_t g_sessIn = 0, g_sessOut = 0;     // session totals
uint32_t g_lastIn = 0, g_lastOut = 0;     // last exchange

// ---- Setup wizard -----------------------------------------------------------
enum class Screen { Setup, Chat };
Screen g_screen = Screen::Chat;
int    g_setupField = 0;
String g_setupBuf;
static const char* SETUP_LABELS[3] = {"WiFi SSID", "WiFi password", "Anthropic API key"};

const char* boardName() {
  switch (M5.getBoard()) {
    case m5::board_t::board_M5Cardputer:    return "Cardputer";
    case m5::board_t::board_M5CardputerADV: return "Cardputer ADV";
    default:                                return "Claudeputer";
  }
}

// =============================================================================
//  Text wrapping
// =============================================================================
std::vector<String> wrapText(const String& text, int maxChars) {
  std::vector<String> lines;
  String line, word;
  auto pushWord = [&]() {
    if (word.length() == 0) return;
    if (line.length() == 0) {
      while ((int)word.length() > maxChars) { lines.push_back(word.substring(0, maxChars)); word = word.substring(maxChars); }
      line = word;
    } else if ((int)(line.length() + 1 + word.length()) <= maxChars) {
      line += " " + word;
    } else {
      lines.push_back(line);
      while ((int)word.length() > maxChars) { lines.push_back(word.substring(0, maxChars)); word = word.substring(maxChars); }
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
//  Status bar + token bar widgets
// =============================================================================
void drawBattery(int x, int y) {
  const int w = 16, h = 8;
  int level = (int)M5.Power.getBatteryLevel();
  canvas.drawRoundRect(x, y, w, h, 2, C_MUTED);
  canvas.fillRect(x + w, y + 2, 2, h - 4, C_MUTED);
  if (level >= 0) {
    if (level > 100) level = 100;
    int fw = ((w - 2) * level) / 100;
    canvas.fillRect(x + 1, y + 1, fw, h - 2, level < 20 ? C_LOWBAT : C_ACCENT);
  }
}

void drawWifi(int x, int y) {
  bool up  = WiFi.status() == WL_CONNECTED;
  int rssi = up ? WiFi.RSSI() : -127;
  int bars = !up ? 0 : rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -88 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int bh = 2 + i * 2;
    canvas.fillRect(x + i * 4, y + (8 - bh), 3, bh, i < bars ? C_ACCENT : C_DIM);
  }
}

void drawTokenBar() {
  canvas.fillRect(0, TOKBAR_Y, SCREEN_W, TOKBAR_H, C_STATUS);
  uint32_t used = g_sessIn + g_sessOut;
  if (used == 0) return;
  float frac = (float)used / (float)TOKEN_BUDGET;
  if (frac > 1.0f) frac = 1.0f;
  int w = (int)(SCREEN_W * frac);
  canvas.fillRect(0, TOKBAR_Y, w, TOKBAR_H, frac > 0.9f ? C_LOWBAT : C_ACCENT);
}

void drawStatusBar() {
  canvas.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS);
  canvas.drawFastHLine(0, STATUS_H, SCREEN_W, C_DIM);
  canvas.setTextColor(C_ACCENT);
  canvas.setCursor(5, 3);
  canvas.print(boardName());
  drawBattery(SCREEN_W - 4 - 18, 3);
  drawWifi(SCREEN_W - 4 - 18 - 6 - 16, 3);
  drawTokenBar();
}

// =============================================================================
//  Chat transcript
// =============================================================================
struct Laid { const Msg* m; std::vector<String> lines; int w, h, x; };

void drawChat() {
  std::vector<Laid> items;
  int total = 0;
  for (const auto& m : g_msgs) {
    Laid L; L.m = &m;
    L.lines = wrapText(m.content, MAX_BUB_CHARS);
    int maxw = 1;
    for (auto& ln : L.lines) maxw = max(maxw, (int)ln.length());
    L.w = maxw * CHAR_W + 2 * BUB_PAD;
    L.h = (int)L.lines.size() * LINE_H + 2 * BUB_PAD;
    L.x = (m.role == "user") ? (SCREEN_W - 4 - L.w) : 4;
    items.push_back(L);
    total += L.h + BUB_GAP;
  }
  if (!items.empty()) total -= BUB_GAP;

  int maxScroll = max(0, total - CHAT_H);
  if (g_scrollPx > maxScroll) g_scrollPx = maxScroll;
  if (g_scrollPx < 0) g_scrollPx = 0;

  if (g_msgs.empty()) {
    canvas.setTextColor(C_MUTED);
    const char* t = "Ask Claude anything";
    canvas.setCursor((SCREEN_W - (int)strlen(t) * CHAR_W) / 2, CHAT_TOP + CHAT_H / 2 - 4);
    canvas.print(t);
    return;
  }

  int y = (total <= CHAT_H) ? (CHAT_TOP + (CHAT_H - total)) : (CHAT_TOP - g_scrollPx);

  canvas.setClipRect(0, CHAT_TOP, SCREEN_W, CHAT_H);
  for (auto& L : items) {
    if (y + L.h >= CHAT_TOP && y <= CHAT_TOP + CHAT_H) {
      uint16_t bub, tx;
      if      (L.m->role == "user")  { bub = C_USER_BUB;   tx = C_USER_TX; }
      else if (L.m->role == "error") { bub = C_ERR_BUB;    tx = C_ERR_TX; }
      else if (L.m->role == "info")  { bub = C_STATUS;     tx = C_INFO_TX; }
      else                           { bub = C_CLAUDE_BUB; tx = C_CLAUDE_TX; }
      canvas.fillRoundRect(L.x, y, L.w, L.h, BUB_RAD, bub);
      canvas.setTextColor(tx);
      int ty = y + BUB_PAD;
      for (auto& ln : L.lines) { canvas.setCursor(L.x + BUB_PAD, ty); canvas.print(ln); ty += LINE_H; }
    }
    y += L.h + BUB_GAP;
  }
  canvas.clearClipRect();

  if (g_scrollPx < maxScroll) {
    canvas.fillTriangle(SCREEN_W - 9, CHAT_TOP + CHAT_H - 4,
                        SCREEN_W - 5, CHAT_TOP + CHAT_H - 9,
                        SCREEN_W - 1, CHAT_TOP + CHAT_H - 4, C_DIM);
  }
}

// =============================================================================
//  Input bar
// =============================================================================
void drawInputBar(bool busy) {
  int iy = SCREEN_H - INPUT_H + 1;
  canvas.fillRoundRect(3, iy, SCREEN_W - 6, INPUT_H - 2, 5, C_INPUT_BG);
  int tx = 8;
  int ty = iy + (INPUT_H - 2 - 8) / 2;

  if (busy) {
    canvas.setTextColor(C_ACCENT);
    canvas.setCursor(tx, ty);
    canvas.print("Claude is typing...");
    return;
  }

  int maxChars = ((SCREEN_W - 6) - 2 * 5) / CHAR_W;
  String shown = "> " + g_input;
  if ((int)shown.length() > maxChars)
    shown = "> " + g_input.substring(g_input.length() - (maxChars - 2));

  canvas.setTextColor(C_INPUT_TX);
  canvas.setCursor(tx, ty);
  canvas.print(shown);
  if (g_cursorOn)
    canvas.fillRect(tx + (int)shown.length() * CHAR_W + 1, ty, 5, 8, C_ACCENT);
}

// =============================================================================
//  Full-screen composers
// =============================================================================
void renderChat(bool busy = false) {
  canvas.fillScreen(C_BG);
  drawStatusBar();
  drawChat();
  drawInputBar(busy);
  canvas.pushSprite(0, 0);
}

void renderSetup() {
  canvas.fillScreen(C_BG);
  canvas.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS);
  canvas.drawFastHLine(0, STATUS_H, SCREEN_W, C_DIM);
  canvas.setTextColor(C_ACCENT);
  canvas.setCursor(5, 3);
  canvas.print("Setup");
  String prog = String(g_setupField + 1) + "/3";
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(SCREEN_W - 5 - (int)prog.length() * CHAR_W, 3);
  canvas.print(prog);

  int cardY = STATUS_H + 8;
  int cardH = SCREEN_H - cardY - 16;
  canvas.fillRoundRect(6, cardY, SCREEN_W - 12, cardH, 6, C_INPUT_BG);

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(14, cardY + 8);
  canvas.print(SETUP_LABELS[g_setupField]);

  canvas.setTextColor(C_INPUT_TX);
  std::vector<String> lines = wrapText(g_setupBuf, (SCREEN_W - 12 - 16) / CHAR_W);
  int y = cardY + 8 + LINE_H + 2;
  for (auto& ln : lines) { canvas.setCursor(14, y); canvas.print(ln); y += LINE_H; }
  int curX = 14 + (lines.empty() ? 0 : (int)lines.back().length()) * CHAR_W + 1;
  if (g_cursorOn) canvas.fillRect(curX, y - LINE_H, 5, 8, C_ACCENT);

  canvas.setTextColor(C_MUTED);
  const char* hint = "[ENTER] next   [DEL] erase";
  canvas.setCursor((SCREEN_W - (int)strlen(hint) * CHAR_W) / 2, SCREEN_H - 10);
  canvas.print(hint);

  canvas.pushSprite(0, 0);
}

void renderCard(const String& title, const String& line, uint16_t titleColor) {
  canvas.fillScreen(C_BG);
  drawStatusBar();
  int cardY = 44, cardH = 50;
  canvas.fillRoundRect(20, cardY, SCREEN_W - 40, cardH, 8, C_INPUT_BG);
  canvas.setTextColor(titleColor);
  canvas.setCursor((SCREEN_W - (int)title.length() * CHAR_W) / 2, cardY + 12);
  canvas.print(title);
  canvas.setTextColor(C_MUTED);
  canvas.setCursor((SCREEN_W - (int)line.length() * CHAR_W) / 2, cardY + 28);
  canvas.print(line);
  canvas.pushSprite(0, 0);
}

// =============================================================================
//  Message helpers
// =============================================================================
void addMessage(const char* role, const String& content) {
  g_msgs.push_back({role, content});
  while (g_msgs.size() > (size_t)(MAX_HISTORY_TURNS * 2 + 6)) g_msgs.erase(g_msgs.begin());
  g_scrollPx = 1 << 20;
}

void showTokens() {
  uint32_t tot = g_sessIn + g_sessOut;
  double cost = (g_sessIn / 1e6) * PRICE_IN_PER_MTOK + (g_sessOut / 1e6) * PRICE_OUT_PER_MTOK;
  String s  = "Token usage\n";
  s += "Last:  in " + String(g_lastIn) + "  out " + String(g_lastOut) + "\n";
  s += "Total: in " + String(g_sessIn) + "  out " + String(g_sessOut) + "\n";
  s += "Used:  " + String(tot) + " / " + String((uint32_t)TOKEN_BUDGET) + "\n";
  s += "Est. cost ~$" + String(cost, 4);
  addMessage("info", s);
}

// =============================================================================
//  WiFi
// =============================================================================
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.begin(g_cfg.ssid.c_str(), g_cfg.pass.c_str());

  int dots = 0;
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    M5Cardputer.update();
    if (millis() - start > 20000) return false;
    String d; for (int i = 0; i < (dots % 4); i++) d += ".";
    renderCard("Connecting", g_cfg.ssid + " " + d, C_ACCENT);
    dots++;
    delay(280);
  }
  renderCard("Connected", WiFi.localIP().toString(), C_USER_BUB);
  delay(700);
  return true;
}

// =============================================================================
//  Anthropic API -- streaming (Server-Sent Events)
//
//  Updates the trailing assistant bubble (g_msgs.back()) as text arrives.
//  Returns false and fills `err` on failure.
// =============================================================================
bool streamClaude(String& err) {
  JsonDocument doc;
  doc["model"]      = g_cfg.model;
  doc["max_tokens"] = MAX_TOKENS;
  doc["system"]     = SYSTEM_PROMPT;
  doc["stream"]     = true;

  JsonArray messages = doc["messages"].to<JsonArray>();
  bool started = false;
  for (size_t i = 0; i < g_msgs.size(); i++) {
    const Msg& m = g_msgs[i];
    if (i + 1 == g_msgs.size()) break;             // skip trailing empty placeholder
    if (m.role != "user" && m.role != "assistant") continue;
    if (!started && m.role != "user") continue;
    started = true;
    JsonObject o = messages.add<JsonObject>();
    o["role"]    = m.role;
    o["content"] = m.content;
  }

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);

  HTTPClient https;
  https.setReuse(false);
  https.setTimeout(25000);
  if (!https.begin(client, "https://api.anthropic.com/v1/messages")) { err = "connection failed"; return false; }
  https.addHeader("content-type", "application/json");
  https.addHeader("x-api-key", g_cfg.apiKey);
  https.addHeader("anthropic-version", "2023-06-01");

  int code = https.POST(body);
  if (code != 200) {
    String resp = https.getString();
    JsonDocument e;
    if (deserializeJson(e, resp) == DeserializationError::Ok && e["error"]["message"].is<const char*>())
      err = "HTTP " + String(code) + ": " + e["error"]["message"].as<String>();
    else
      err = "HTTP " + String(code);
    https.end();
    return false;
  }

  WiFiClient* stream = https.getStreamPtr();
  String acc;
  unsigned long lastData = millis();
  unsigned long lastRender = 0;
  bool done = false;

  while (https.connected() && !done && (millis() - lastData < 25000)) {
    while (stream->available()) {
      String line = stream->readStringUntil('\n');
      lastData = millis();
      line.trim();
      if (!line.startsWith("data:")) continue;
      String js = line.substring(5);
      js.trim();

      JsonDocument ev;
      if (deserializeJson(ev, js)) continue;
      const char* t = ev["type"] | "";

      if (strcmp(t, "content_block_delta") == 0) {
        if (ev["delta"]["type"] == "text_delta") {
          acc += ev["delta"]["text"].as<String>();
          g_msgs.back().content = acc;
          if (millis() - lastRender > 90) { g_scrollPx = 1 << 20; renderChat(true); lastRender = millis(); }
        }
      } else if (strcmp(t, "message_start") == 0) {
        g_lastIn = ev["message"]["usage"]["input_tokens"] | 0;
      } else if (strcmp(t, "message_delta") == 0) {
        g_lastOut = ev["usage"]["output_tokens"] | g_lastOut;
      } else if (strcmp(t, "message_stop") == 0) {
        done = true;
        break;
      } else if (strcmp(t, "error") == 0) {
        err = String((const char*)(ev["error"]["message"] | "stream error"));
        https.end();
        return false;
      }
    }
    M5Cardputer.update();
    delay(2);
  }
  https.end();

  if (acc.length() == 0) { err = "no response received"; return false; }
  g_msgs.back().content = acc;
  g_sessIn  += g_lastIn;
  g_sessOut += g_lastOut;
  g_scrollPx = 1 << 20;
  return true;
}

// =============================================================================
//  Setup wizard flow
// =============================================================================
void startSetup() {
  g_screen     = Screen::Setup;
  g_setupField = 0;
  g_setupBuf   = g_cfg.ssid;
  renderSetup();
}
bool setupAdvance() {                       // returns true when finished
  switch (g_setupField) {
    case 0: g_cfg.ssid   = g_setupBuf; break;
    case 1: g_cfg.pass   = g_setupBuf; break;
    case 2: g_cfg.apiKey = g_setupBuf; break;
  }
  g_setupField++;
  if (g_setupField <= 2) {
    g_setupBuf = (g_setupField == 1) ? g_cfg.pass : g_cfg.apiKey;
    renderSetup();
    return false;
  }
  saveConfig();
  return true;
}

// =============================================================================
//  Main flow
// =============================================================================
void goChat() { g_screen = Screen::Chat; renderChat(); }

void submitPrompt() {
  String prompt = g_input;
  prompt.trim();
  if (prompt.length() == 0) return;

  if (prompt == "/setup")  { g_input = ""; startSetup(); return; }
  if (prompt == "/reset")  { g_msgs.clear(); g_input = ""; renderChat(); return; }
  if (prompt == "/tokens") { g_input = ""; showTokens(); renderChat(); return; }

  g_input = "";
  addMessage("user", prompt);
  addMessage("assistant", "");        // placeholder the stream fills in
  renderChat(true);

  String err;
  if (!streamClaude(err)) {
    g_msgs.back().role    = "error";
    g_msgs.back().content = err.length() ? err : "request failed";
  }
  renderChat();
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);

  canvas.setPsram(true);
  canvas.setColorDepth(16);
  canvas.createSprite(SCREEN_W, SCREEN_H);
  canvas.setTextFont(&fonts::Font0);
  canvas.setTextSize(1);

  loadConfig();

  M5Cardputer.update();
  bool forceSetup = M5Cardputer.BtnA.isPressed();   // hold G0 at boot to reconfigure

  if (forceSetup || !configComplete()) { startSetup(); return; }

  if (!connectWiFi()) { addMessage("error", "WiFi failed. Type /setup to reconfigure."); goChat(); return; }
  goChat();
}

void loop() {
  M5Cardputer.update();

  unsigned long now = millis();
  if (now - g_lastBlink > 530) {
    g_cursorOn  = !g_cursorOn;
    g_lastBlink = now;
    if (g_screen == Screen::Chat) renderChat();
    else                          renderSetup();
  }

  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) { delay(5); return; }
  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
  g_cursorOn = true;

  if (g_screen == Screen::Setup) {
    if (status.enter) {
      if (setupAdvance()) {
        g_msgs.clear();
        if (connectWiFi()) goChat();
        else { addMessage("error", "WiFi failed. Type /setup to retry."); goChat(); }
      }
      return;
    }
    if (status.del && g_setupBuf.length() > 0) g_setupBuf.remove(g_setupBuf.length() - 1);
    for (auto c : status.word) g_setupBuf += c;
    renderSetup();
    return;
  }

  // --- Chat ---
  if (status.enter) { submitPrompt(); return; }
  if (status.del && g_input.length() > 0) { g_input.remove(g_input.length() - 1); renderChat(); return; }

  if (status.fn) {                          // Fn + ; / . scrolls the transcript
    bool scrolled = false;
    for (auto c : status.word) {
      if (c == ';') { g_scrollPx -= LINE_H * 2; scrolled = true; }
      if (c == '.') { g_scrollPx += LINE_H * 2; scrolled = true; }
    }
    if (scrolled) { if (g_scrollPx < 0) g_scrollPx = 0; renderChat(); return; }
  }

  for (auto c : status.word) g_input += c;
  renderChat();
}
