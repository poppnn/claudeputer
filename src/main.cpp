// =============================================================================
//  Claudeputer — talk to Claude from an M5Stack Cardputer over WiFi
//
//  Simple first version:
//    - connects to WiFi
//    - type a prompt on the Cardputer keyboard, press ENTER
//    - the prompt is sent to the Anthropic Messages API
//    - the reply is shown on screen, scrollable with the ; / . keys
//    - press ` (backtick, top-left) to go back and type a new prompt
//
//  Copy src/config.h.example -> src/config.h and fill in your secrets first.
// =============================================================================

#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>

#include "config.h"

// ---- Layout constants (text size 1: ~6px wide, ~8px tall glyphs) ------------
static const int   SCREEN_W     = 240;
static const int   SCREEN_H     = 135;
static const int   CHAR_W       = 6;
static const int   LINE_H       = 9;     // glyph height + 1px spacing
static const int   COLS         = SCREEN_W / CHAR_W;        // ~40 chars/line
static const int   HEADER_H     = 12;
static const int   BODY_ROWS    = (SCREEN_H - HEADER_H) / LINE_H;

// ---- Colours ----------------------------------------------------------------
#define COL_BG      TFT_BLACK
#define COL_HEADER  0x5AEB          // soft grey-blue
#define COL_USER    TFT_GREENYELLOW
#define COL_CLAUDE  TFT_WHITE
#define COL_ACCENT  0xFD20          // orange (Claude-ish)
#define COL_ERROR   TFT_RED

// ---- Conversation state -----------------------------------------------------
struct Turn {
  String role;       // "user" or "assistant"
  String content;
};
std::vector<Turn> g_history;

String g_input;                    // current prompt being typed
String g_lastReply;                // last assistant reply (for the viewer)
int    g_scroll = 0;               // scroll offset (in wrapped lines)

enum class Screen { INPUT, THINKING, VIEW, ERROR };
Screen g_screen = Screen::INPUT;

// =============================================================================
//  Small UI helpers
// =============================================================================
void drawHeader(const String& title, uint16_t color = COL_HEADER) {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 0, SCREEN_W, HEADER_H, COL_BG);
  d.setTextSize(1);
  d.setTextColor(color, COL_BG);
  d.setCursor(2, 2);
  d.print(title);
  d.drawFastHLine(0, HEADER_H - 1, SCREEN_W, color);
}

// Wrap a string into display lines, honouring existing newlines.
std::vector<String> wrapText(const String& text, int maxChars) {
  std::vector<String> lines;
  String line;
  String word;

  auto pushWord = [&]() {
    if (word.length() == 0) return;
    if (line.length() == 0) {
      // very long word: hard-split it
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
    if (c == '\n') {
      pushWord();
      lines.push_back(line);
      line = "";
    } else if (c == ' ') {
      pushWord();
    } else if (c == '\r') {
      // ignore
    } else {
      word += c;
    }
  }
  pushWord();
  lines.push_back(line);
  return lines;
}

// =============================================================================
//  Screen renderers
// =============================================================================
void renderInput() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("Claudeputer  [ENTER] send", COL_ACCENT);

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

  String hdr = "Claude  [;/.] scroll  [`] new";
  drawHeader(hdr, COL_HEADER);

  d.setTextColor(COL_CLAUDE, COL_BG);
  int y = HEADER_H + 2;
  for (int i = g_scroll; i < total && i < g_scroll + BODY_ROWS; i++) {
    d.setCursor(2, y);
    d.print(lines[i]);
    y += LINE_H;
  }

  // tiny scrollbar
  if (maxScroll > 0) {
    int barH = max(6, (BODY_ROWS * (SCREEN_H - HEADER_H)) / total);
    int barY = HEADER_H + (g_scroll * (SCREEN_H - HEADER_H - barH)) / maxScroll;
    d.fillRect(SCREEN_W - 2, barY, 2, barH, COL_ACCENT);
  }
}

void renderError(const String& msg) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("Error  [`] back", COL_ERROR);
  d.setTextColor(COL_ERROR, COL_BG);
  std::vector<String> lines = wrapText(msg, COLS);
  int y = HEADER_H + 2;
  for (int i = 0; i < (int)lines.size() && i < BODY_ROWS; i++) {
    d.setCursor(2, y);
    d.print(lines[i]);
    y += LINE_H;
  }
}

// =============================================================================
//  WiFi
// =============================================================================
bool connectWiFi() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("Claudeputer", COL_ACCENT);
  d.setTextColor(COL_CLAUDE, COL_BG);
  d.setCursor(2, HEADER_H + 4);
  d.print("WiFi: ");
  d.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int dots = 0;
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    M5Cardputer.update();
    if (millis() - start > 20000) return false;   // 20s timeout
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
// Returns true on success; fills `out` with the reply text (or an error msg).
bool askClaude(const String& prompt, String& out) {
  // Build the messages array: history + new user turn.
  JsonDocument doc;
  doc["model"]      = CLAUDE_MODEL;
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
  client.setInsecure();               // simple v1: skip cert validation
  client.setTimeout(20000);

  HTTPClient https;
  https.setReuse(false);
  if (!https.begin(client, "https://api.anthropic.com/v1/messages")) {
    out = "https.begin() failed";
    return false;
  }
  https.addHeader("content-type", "application/json");
  https.addHeader("x-api-key", ANTHROPIC_API_KEY);
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
  if (e) {
    out = String("JSON parse: ") + e.c_str();
    return false;
  }

  // Concatenate all text content blocks.
  String text;
  for (JsonObject block : res["content"].as<JsonArray>()) {
    if (block["type"] == "text") text += block["text"].as<String>();
  }
  if (text.length() == 0) text = "(empty response)";
  out = text;
  return true;
}

void trimHistory() {
  // Keep the last MAX_HISTORY_TURNS*2 messages.
  int maxMsgs = MAX_HISTORY_TURNS * 2;
  while ((int)g_history.size() > maxMsgs) {
    g_history.erase(g_history.begin());
  }
}

// =============================================================================
//  Main flow
// =============================================================================
void submitPrompt() {
  String prompt = g_input;
  prompt.trim();
  if (prompt.length() == 0) return;

  g_screen = Screen::THINKING;
  renderThinking();

  String reply;
  bool ok = askClaude(prompt, reply);

  if (ok) {
    g_history.push_back({"user", prompt});
    g_history.push_back({"assistant", reply});
    trimHistory();

    g_input    = "";
    g_lastReply = reply;
    g_scroll   = 0;
    g_screen   = Screen::VIEW;
    renderView();
  } else {
    g_screen = Screen::ERROR;
    renderError(reply);
  }
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);          // true = init keyboard
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextFont(&fonts::Font0);

  if (!connectWiFi()) {
    g_screen = Screen::ERROR;
    renderError("WiFi connection failed. Check WIFI_SSID / WIFI_PASSWORD in config.h, then reset.");
    return;
  }

  g_screen = Screen::INPUT;
  renderInput();
}

void loop() {
  M5Cardputer.update();

  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
    delay(5);
    return;
  }

  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

  switch (g_screen) {
    case Screen::INPUT: {
      if (status.enter) {
        submitPrompt();
        break;
      }
      if (status.del && g_input.length() > 0) {
        g_input.remove(g_input.length() - 1);
      }
      for (auto c : status.word) {
        g_input += c;
      }
      renderInput();
      break;
    }

    case Screen::VIEW: {
      bool changed = false;
      for (auto c : status.word) {
        if (c == '`') {                 // back to input
          g_screen = Screen::INPUT;
          renderInput();
          return;
        }
        if (c == ';') { g_scroll -= 1; changed = true; }   // up
        if (c == '.') { g_scroll += 1; changed = true; }   // down
        if (c == ' ') { g_scroll += BODY_ROWS - 1; changed = true; } // page down
      }
      if (changed) renderView();
      break;
    }

    case Screen::ERROR: {
      for (auto c : status.word) {
        if (c == '`') {
          g_screen = Screen::INPUT;
          renderInput();
          return;
        }
      }
      break;
    }

    case Screen::THINKING:
    default:
      break;
  }
}
