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
#include <WebServer.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <vector>
#include <cstring>
#include "anthropic_ca.h"

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
// microSD pins (Cardputer 1.1 & ADV share the StampS3 pinout). Override in
// config.h if your board differs.
#ifndef SD_SCK_PIN
  #define SD_SCK_PIN  40
#endif
#ifndef SD_MISO_PIN
  #define SD_MISO_PIN 39
#endif
#ifndef SD_MOSI_PIN
  #define SD_MOSI_PIN 14
#endif
#ifndef SD_CS_PIN
  #define SD_CS_PIN   12
#endif
// Define TLS_INSECURE in config.h to skip certificate validation (debug only).

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

// ---- Screens ----------------------------------------------------------------
// Setup is split into 3 steps: pick a scanned WiFi -> type password -> import
// the API key (web form / SD file / typing).
enum class Screen { Scan, Pass, ApiKey, Chat, Model };
Screen g_screen = Screen::Chat;

// ---- WiFi scan / setup ------------------------------------------------------
struct Net { String ssid; int rssi; bool locked; };
std::vector<Net> g_nets;
int    g_netSel = 0, g_netTop = 0;
String g_selSsid;
String g_passBuf;
String g_apiBuf;

// ---- Model picker -----------------------------------------------------------
static const char* MODELS[] = {
  "claude-haiku-4-5",
  "claude-sonnet-4-6",
  "claude-opus-4-8",
};
static const int N_MODELS = sizeof(MODELS) / sizeof(MODELS[0]);
int g_modelSel = 0;

// ---- SD card ----------------------------------------------------------------
bool g_sdReady = false;

// ---- API-key web import -----------------------------------------------------
WebServer g_web(80);
bool g_webUp = false;
volatile bool g_apiImported = false;

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

static void setupHeader(const String& title) {
  canvas.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS);
  canvas.drawFastHLine(0, STATUS_H, SCREEN_W, C_DIM);
  canvas.setTextColor(C_ACCENT);
  canvas.setCursor(5, 3);
  canvas.print(title);
}

// Step 1: pick a scanned WiFi network.
void renderScan() {
  canvas.fillScreen(C_BG);
  setupHeader("Select WiFi");
  String cnt = String((int)g_nets.size()) + " found";
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(SCREEN_W - 5 - (int)cnt.length() * CHAR_W, 3);
  canvas.print(cnt);

  if (g_nets.empty()) {
    canvas.setTextColor(C_MUTED);
    const char* t = "No networks - [DEL] rescan";
    canvas.setCursor((SCREEN_W - (int)strlen(t) * CHAR_W) / 2, STATUS_H + 32);
    canvas.print(t);
    canvas.pushSprite(0, 0);
    return;
  }

  const int rowH = 14;
  int top = STATUS_H + 4;
  int rows = (SCREEN_H - 11 - top) / rowH;
  if (g_netSel < g_netTop) g_netTop = g_netSel;
  if (g_netSel >= g_netTop + rows) g_netTop = g_netSel - rows + 1;

  for (int i = g_netTop; i < (int)g_nets.size() && i < g_netTop + rows; i++) {
    int rowY = top + (i - g_netTop) * rowH;
    bool sel = (i == g_netSel);
    if (sel) canvas.fillRoundRect(4, rowY - 1, SCREEN_W - 8, rowH - 1, 4, C_INPUT_BG);
    canvas.setTextColor(sel ? C_ACCENT : C_CLAUDE_TX);
    canvas.setCursor(10, rowY + 2);
    String s = g_nets[i].ssid;
    if (s.length() > 26) s = s.substring(0, 26);
    canvas.print(s);
    int rx = SCREEN_W - 14;
    int bars = g_nets[i].rssi >= -60 ? 3 : g_nets[i].rssi >= -72 ? 2 : 1;
    for (int b = 0; b < 3; b++) {
      int bh = 2 + b * 2;
      canvas.fillRect(rx + b * 3, rowY + 2 + (8 - bh), 2, bh, b < bars ? (sel ? C_ACCENT : C_MUTED) : C_DIM);
    }
  }

  canvas.setTextColor(C_MUTED);
  const char* h = "[;/.] move  [ENTER] ok  [DEL] rescan";
  canvas.setCursor((SCREEN_W - (int)strlen(h) * CHAR_W) / 2, SCREEN_H - 9);
  canvas.print(h);
  canvas.pushSprite(0, 0);
}

// Step 2: type the WiFi password for the chosen network.
void renderPass() {
  canvas.fillScreen(C_BG);
  setupHeader("WiFi password");
  int cardY = STATUS_H + 8;
  int cardH = SCREEN_H - cardY - 16;
  canvas.fillRoundRect(6, cardY, SCREEN_W - 12, cardH, 6, C_INPUT_BG);
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(14, cardY + 8);
  String lbl = g_selSsid; if (lbl.length() > 30) lbl = lbl.substring(0, 30);
  canvas.print(lbl);

  canvas.setTextColor(C_INPUT_TX);
  std::vector<String> lines = wrapText(g_passBuf, (SCREEN_W - 12 - 16) / CHAR_W);
  int y = cardY + 8 + LINE_H + 4;
  for (auto& ln : lines) { canvas.setCursor(14, y); canvas.print(ln); y += LINE_H; }
  int curX = 14 + (lines.empty() ? 0 : (int)lines.back().length()) * CHAR_W + 1;
  if (g_cursorOn) canvas.fillRect(curX, y - LINE_H, 5, 8, C_ACCENT);

  canvas.setTextColor(C_MUTED);
  const char* h = "[ENTER] connect   [`] back";
  canvas.setCursor((SCREEN_W - (int)strlen(h) * CHAR_W) / 2, SCREEN_H - 10);
  canvas.print(h);
  canvas.pushSprite(0, 0);
}

// Step 3: import the API key (web form / SD file / typing).
void renderApi() {
  canvas.fillScreen(C_BG);
  setupHeader("Import API key");
  int y = STATUS_H + 7;
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(8, y); canvas.print("On a phone/PC on this WiFi,"); y += LINE_H;
  canvas.setCursor(8, y); canvas.print("open & paste your key:");      y += LINE_H + 1;
  canvas.setTextColor(C_ACCENT);
  canvas.setCursor(8, y); canvas.print("http://" + WiFi.localIP().toString() + "/"); y += LINE_H + 4;

  canvas.fillRoundRect(6, y, SCREEN_W - 12, 22, 5, C_INPUT_BG);
  int maxc = (SCREEN_W - 12 - 12) / CHAR_W - 1;
  String disp;
  if (g_apiBuf.length() == 0)         disp = "(or type it here)";
  else if ((int)g_apiBuf.length() > maxc) disp = "..." + g_apiBuf.substring(g_apiBuf.length() - (maxc - 3));
  else                                disp = g_apiBuf;
  canvas.setTextColor(g_apiBuf.length() ? C_INPUT_TX : C_DIM);
  canvas.setCursor(12, y + 7); canvas.print(disp);
  if (g_cursorOn && g_apiBuf.length()) canvas.fillRect(12 + (int)disp.length() * CHAR_W + 1, y + 6, 5, 8, C_ACCENT);

  canvas.setTextColor(C_MUTED);
  const char* h = "[ENTER] save   (SD apikey.txt auto)";
  canvas.setCursor((SCREEN_W - (int)strlen(h) * CHAR_W) / 2, SCREEN_H - 10);
  canvas.print(h);
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

void showHelp() {
  addMessage("info", "Commands:\n/setup  /reset  /tokens\n/model  /save  /load\n/sd  /help");
}

// =============================================================================
//  Model picker
// =============================================================================
void renderModel() {
  canvas.fillScreen(C_BG);
  canvas.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS);
  canvas.drawFastHLine(0, STATUS_H, SCREEN_W, C_DIM);
  canvas.setTextColor(C_ACCENT);
  canvas.setCursor(5, 3);
  canvas.print("Select model");

  int y = STATUS_H + 12;
  for (int i = 0; i < N_MODELS; i++) {
    int rowY = y + i * 16;
    bool sel = (i == g_modelSel);
    if (sel) canvas.fillRoundRect(6, rowY - 3, SCREEN_W - 12, 14, 4, C_INPUT_BG);
    canvas.setTextColor(sel ? C_ACCENT : C_CLAUDE_TX);
    canvas.setCursor(12, rowY);
    canvas.print(sel ? ">" : " ");
    canvas.setCursor(24, rowY);
    canvas.print(MODELS[i]);
  }
  canvas.setTextColor(C_MUTED);
  const char* h = "[;/.] move  [ENTER] ok  [`] cancel";
  canvas.setCursor((SCREEN_W - (int)strlen(h) * CHAR_W) / 2, SCREEN_H - 10);
  canvas.print(h);
  canvas.pushSprite(0, 0);
}
void startModel() {
  g_modelSel = 0;
  for (int i = 0; i < N_MODELS; i++) if (g_cfg.model == MODELS[i]) { g_modelSel = i; break; }
  g_screen = Screen::Model;
  renderModel();
}

// =============================================================================
//  microSD: save / load conversations
// =============================================================================
bool initSD() {
  if (g_sdReady) return true;
  static bool spiUp = false;
  if (!spiUp) { SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN); spiUp = true; }
  g_sdReady = SD.begin(SD_CS_PIN, SPI, 25000000) || SD.begin(SD_CS_PIN, SPI, 4000000);
  return g_sdReady;
}

void showSD() {
  if (!initSD()) { addMessage("info", "No SD card detected"); return; }
  uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
  addMessage("info", "SD ready: " + String((uint32_t)mb) + " MB. Use /save and /load.");
}

void saveConversation() {
  if (!initSD()) { addMessage("info", "No SD card detected"); return; }
  if (!SD.exists("/claudeputer")) SD.mkdir("/claudeputer");

  String path;
  char buf[40];
  for (int n = 1; n < 1000; n++) {
    snprintf(buf, sizeof(buf), "/claudeputer/chat-%03d.json", n);
    if (!SD.exists(buf)) { path = buf; break; }
  }
  if (path.length() == 0) { addMessage("info", "SD chat slots full"); return; }

  File f = SD.open(path, FILE_WRITE);
  if (!f) { addMessage("info", "SD write failed"); return; }
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& m : g_msgs) {
    if (m.role != "user" && m.role != "assistant") continue;
    JsonObject o = arr.add<JsonObject>();
    o["role"]    = m.role;
    o["content"] = m.content;
  }
  serializeJson(doc, f);
  f.close();
  addMessage("info", "Saved " + path);
}

void loadConversation() {
  if (!initSD()) { addMessage("info", "No SD card detected"); return; }

  String path;
  char buf[40];
  for (int n = 999; n >= 1; n--) {
    snprintf(buf, sizeof(buf), "/claudeputer/chat-%03d.json", n);
    if (SD.exists(buf)) { path = buf; break; }
  }
  if (path.length() == 0) { addMessage("info", "No saved chats found"); return; }

  File f = SD.open(path, FILE_READ);
  if (!f) { addMessage("info", "SD read failed"); return; }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) { addMessage("info", "Parse error in " + path); return; }

  g_msgs.clear();
  for (JsonObject o : doc.as<JsonArray>())
    addMessage(o["role"] | "assistant", String((const char*)(o["content"] | "")));
  addMessage("info", "Loaded " + path);
}

// =============================================================================
//  WiFi + time
// =============================================================================
// TLS certificate validation needs a real clock (otherwise the cert looks
// "not yet valid"). Pull the time over SNTP once WiFi is up.
void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  struct tm t;
  unsigned long start = millis();
  while (millis() - start < 8000) {
    if (getLocalTime(&t, 200) && t.tm_year > (2020 - 1900)) return;
    renderCard("Syncing clock", "NTP...", C_ACCENT);
    M5Cardputer.update();
    delay(200);
  }
}

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
  delay(500);
  syncTime();
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
#ifdef TLS_INSECURE
  client.setInsecure();                       // debug only -- no cert validation
#else
  client.setCACert(ANTHROPIC_ROOT_CA);        // validate api.anthropic.com
#endif
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
//  Setup flow: scan WiFi -> password -> import API key
// =============================================================================
void goChat() { g_screen = Screen::Chat; renderChat(); }

void scanNetworks() {
  renderCard("Scanning", "WiFi networks...", C_ACCENT);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  int n = WiFi.scanNetworks();
  g_nets.clear();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (auto& e : g_nets) if (e.ssid == s) { dup = true; break; }
    if (dup) continue;
    Net net;
    net.ssid   = s;
    net.rssi   = WiFi.RSSI(i);
    net.locked = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    g_nets.push_back(net);
  }
  WiFi.scanDelete();
  g_netSel = 0;
  g_netTop = 0;
}

void startSetup() {
  g_screen = Screen::Scan;
  scanNetworks();
  renderScan();
}

// --- API key import over a tiny web form (paste from your phone/PC) ---
void stopWeb() { if (g_webUp) { g_web.stop(); g_webUp = false; } }

void startWeb() {
  g_apiImported = false;
  g_web.on("/", HTTP_GET, []() {
    g_web.send(200, "text/html",
      "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>Claudeputer setup</title>"
      "<body style='font-family:system-ui;background:#111;color:#eee;margin:0;padding:24px'>"
      "<h2 style='color:#d97706'>Claudeputer</h2>"
      "<p>Paste your Anthropic API key:</p>"
      "<form method=POST action=/save>"
      "<input name=key autocomplete=off autocapitalize=off spellcheck=false "
      "style='width:100%;box-sizing:border-box;padding:12px;font-size:16px;border-radius:8px;border:1px solid #444;background:#1c1c22;color:#eee'>"
      "<button style='margin-top:14px;padding:12px 20px;font-size:16px;border:0;border-radius:8px;background:#d97706;color:#fff'>Save</button>"
      "</form></body>");
  });
  g_web.on("/save", HTTP_POST, []() {
    String k = g_web.arg("key"); k.trim();
    if (k.length()) { g_apiBuf = k; g_apiImported = true; }
    g_web.send(200, "text/html",
      "<body style='font-family:system-ui;background:#111;color:#eee;padding:24px'>"
      "<h2 style='color:#d97706'>Saved &#10003;</h2>"
      "<p>Close this tab and return to the Cardputer.</p></body>");
  });
  g_web.begin();
  g_webUp = true;
}

void startApi() {
  g_apiBuf = g_cfg.apiKey;                         // prefill (reconfigure)
  if (initSD() && SD.exists("/claudeputer/apikey.txt")) {   // auto-import from SD
    File f = SD.open("/claudeputer/apikey.txt", FILE_READ);
    if (f) { String k = f.readStringUntil('\n'); k.trim(); if (k.length()) g_apiBuf = k; f.close(); }
  }
  startWeb();
  g_screen = Screen::ApiKey;
  renderApi();
}

void finalizeSetup() {
  g_cfg.apiKey = g_apiBuf;
  saveConfig();
  stopWeb();
  g_msgs.clear();
  addMessage("info", "Setup done. WiFi: " + g_cfg.ssid + "   Model: " + g_cfg.model);
  goChat();
}

// =============================================================================
//  Main flow
// =============================================================================
void submitPrompt() {
  String prompt = g_input;
  prompt.trim();
  if (prompt.length() == 0) return;

  if (prompt == "/setup")  { g_input = ""; startSetup(); return; }
  if (prompt == "/reset")  { g_msgs.clear(); g_input = ""; renderChat(); return; }
  if (prompt == "/tokens") { g_input = ""; showTokens(); renderChat(); return; }
  if (prompt == "/model")  { g_input = ""; startModel(); return; }
  if (prompt == "/save")   { g_input = ""; saveConversation(); renderChat(); return; }
  if (prompt == "/load")   { g_input = ""; loadConversation(); renderChat(); return; }
  if (prompt == "/sd")     { g_input = ""; showSD(); renderChat(); return; }
  if (prompt == "/help")   { g_input = ""; showHelp(); renderChat(); return; }

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

  // Serve the API-key import page while it is running.
  if (g_webUp) {
    g_web.handleClient();
    if (g_apiImported) { finalizeSetup(); return; }
  }

  unsigned long now = millis();
  if (now - g_lastBlink > 530) {
    g_cursorOn  = !g_cursorOn;
    g_lastBlink = now;
    if      (g_screen == Screen::Chat)   renderChat();
    else if (g_screen == Screen::Pass)   renderPass();
    else if (g_screen == Screen::ApiKey) renderApi();
    // Scan & Model lists are static -- no cursor to blink.
  }

  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) { delay(5); return; }
  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
  g_cursorOn = true;

  // ---- Setup step 1: WiFi scan list ----
  if (g_screen == Screen::Scan) {
    if (status.del) { scanNetworks(); renderScan(); return; }       // rescan
    if (status.enter && !g_nets.empty()) {
      g_selSsid = g_nets[g_netSel].ssid;
      g_passBuf = "";
      if (!g_nets[g_netSel].locked) {                               // open network: no password
        g_cfg.ssid = g_selSsid; g_cfg.pass = "";
        if (connectWiFi()) startApi();
        else { renderCard("WiFi failed", "try again", C_ERR_TX); delay(1200); renderScan(); }
      } else {
        g_screen = Screen::Pass; renderPass();
      }
      return;
    }
    bool moved = false;
    for (auto c : status.word) {
      if (c == ';') { if (g_netSel > 0) g_netSel--; moved = true; }
      if (c == '.') { if (g_netSel < (int)g_nets.size() - 1) g_netSel++; moved = true; }
    }
    if (moved) renderScan();
    return;
  }

  // ---- Setup step 2: WiFi password ----
  if (g_screen == Screen::Pass) {
    if (status.enter) {
      g_cfg.ssid = g_selSsid; g_cfg.pass = g_passBuf;
      if (connectWiFi()) startApi();
      else { renderCard("WiFi failed", "wrong password?", C_ERR_TX); delay(1200); renderPass(); }
      return;
    }
    if (status.del && g_passBuf.length() > 0) g_passBuf.remove(g_passBuf.length() - 1);
    bool back = false;
    for (auto c : status.word) { if (c == '`') back = true; else g_passBuf += c; }
    if (back) { startSetup(); return; }                              // back to scan
    renderPass();
    return;
  }

  // ---- Setup step 3: API key import ----
  if (g_screen == Screen::ApiKey) {
    if (status.enter) {
      String k = g_apiBuf; k.trim();
      if (k.length()) { g_apiBuf = k; finalizeSetup(); }
      return;
    }
    if (status.del && g_apiBuf.length() > 0) g_apiBuf.remove(g_apiBuf.length() - 1);
    for (auto c : status.word) g_apiBuf += c;
    renderApi();
    return;
  }

  if (g_screen == Screen::Model) {
    if (status.enter) {
      g_cfg.model = MODELS[g_modelSel];
      saveConfig();
      addMessage("info", "Model set: " + g_cfg.model);
      goChat();
      return;
    }
    if (status.del) { goChat(); return; }              // cancel
    bool moved = false;
    for (auto c : status.word) {
      if (c == ';') { g_modelSel = (g_modelSel - 1 + N_MODELS) % N_MODELS; moved = true; }
      if (c == '.') { g_modelSel = (g_modelSel + 1) % N_MODELS;            moved = true; }
      if (c == '`') { goChat(); return; }               // cancel
    }
    if (moved) renderModel();
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
