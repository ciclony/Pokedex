// Pokedex for the ESP32-2432S028 ("Cheap Yellow Display", two-USB ST7789 version).
// Reads every Pokemon from /dex.bin on the microSD (built by tools/build_dex.py
// from PokeAPI). Menu: browse, search by number, search by name. On a Pokemon:
// tap the right/left half for next/previous (hold to scroll), tap an evolution
// to jump to it, tap MENU to go back.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <U8g2lib.h>  // fonts only
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include "pin_config.h"

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, LCD_MISO, HSPI);
#ifdef CYD_ST7789
Arduino_GFX *gfx = new Arduino_ST7789(bus, GFX_NOT_DEFINED, 1, false, 240, 320);
#else
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, 1);
#endif

static constexpr uint16_t C(uint32_t h) { return ((h >> 8) & 0xF800) | ((h >> 5) & 0x07E0) | ((h >> 3) & 0x1F); }
static const uint16_t RED = C(0xDC0A2D), WHITE = C(0xFFFFFF), INK = C(0x222222), GREY = C(0x707070),
                      TRACK = C(0xE4E4E4), BLUE = C(0x3B4CCA);

// same order as TYPES in tools/build_dex.py
static const char *TYPE_NAME[18] = {"NORMAL", "FIGHT", "FLYING", "POISON", "GROUND", "ROCK", "BUG", "GHOST", "STEEL",
                                    "FIRE", "WATER", "GRASS", "ELECTR", "PSYCHC", "ICE", "DRAGON", "DARK", "FAIRY"};
static const uint16_t TYPE_COL[18] = {C(0xA8A77A), C(0xC22E28), C(0xA98FF3), C(0xA33EA1), C(0xE2BF65), C(0xB6A136),
                                      C(0xA6B91A), C(0x735797), C(0xB7B7CE), C(0xEE8130), C(0x6390F0), C(0x7AC74C),
                                      C(0xF7D02C), C(0xF95587), C(0x96D9D6), C(0x6F35FC), C(0x705746), C(0xD685AD)};
static const char *STAT_NAME[6] = {"HP", "ATK", "DEF", "SPA", "SPD", "SPE"};

static SPIClass sdSpi(VSPI);
static File dex;
static uint16_t count, side;
static uint16_t *spriteBuf;
static char **names;  // names[id], 1-based, kept in RAM for the name search
static Preferences prefs;

static const int MAX_EVO = 12;
struct Mon {
  uint16_t id, height, weight, evo[MAX_EVO];
  uint8_t type[2], stat[6], nEvo;
  char name[64], genus[64], flavor[1001];
};
static Mon mon;

// ---------- touch: XPT2046, bit-banged (both hardware SPI buses are taken) ----------

static uint16_t tpConv(uint8_t cmd) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TP_MOSI, (cmd >> i) & 1);
    delayMicroseconds(1);
    digitalWrite(TP_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(TP_CLK, LOW);
  }
  digitalWrite(TP_MOSI, LOW);
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    delayMicroseconds(1);
    digitalWrite(TP_CLK, HIGH);
    delayMicroseconds(1);
    v = (v << 1) | digitalRead(TP_MISO);
    digitalWrite(TP_CLK, LOW);
  }
  return (v >> 3) & 0x0FFF;
}

// screen coordinates of the finger; false if not touched
static bool touchXY(int *x, int *y) {
  if (digitalRead(TP_INT) == HIGH) return false;
  digitalWrite(TP_CS, LOW);
  int z = tpConv(0xB1) + 4095 - tpConv(0xC1);
  int sx = 0, sy = 0;
  if (z >= 400) {
    tpConv(0x91);  // first sample is noisy
    for (int i = 0; i < 4; i++) {
      sx += tpConv(0x91);
      sy += tpConv(0xD1);
    }
  }
  tpConv(0xD0);  // PD=00 re-enables PENIRQ
  digitalWrite(TP_CS, HIGH);
  if (z < 400) return false;
  *x = constrain((sx / 4 - TP_X_MIN) * LCD_W / (TP_X_MAX - TP_X_MIN), 0, LCD_W - 1);
  *y = constrain((sy / 4 - TP_Y_MIN) * LCD_H / (TP_Y_MAX - TP_Y_MIN), 0, LCD_H - 1);
  return true;
}

// ---------- dex.bin ----------

static bool readStr(char *dst, size_t cap, size_t len) {
  if (len >= cap || dex.read((uint8_t *)dst, len) != len) return false;
  dst[len] = 0;
  return true;
}

static bool loadMon(uint16_t id) {
  uint32_t off;
  uint8_t h[15], n;
  uint16_t fl;
  if (!dex.seek(12 + 4UL * (id - 1)) || dex.read((uint8_t *)&off, 4) != 4 || !dex.seek(off) ||
      dex.read(h, 15) != 15)
    return false;
  memcpy(&mon.id, h, 2);
  mon.type[0] = h[2];
  mon.type[1] = h[3];
  memcpy(&mon.height, h + 4, 2);
  memcpy(&mon.weight, h + 6, 2);
  memcpy(mon.stat, h + 8, 6);
  mon.nEvo = h[14];
  if (mon.nEvo > MAX_EVO || dex.read((uint8_t *)mon.evo, mon.nEvo * 2) != mon.nEvo * 2u) return false;
  for (int i = 0; i < mon.nEvo; i++)
    if (mon.evo[i] < 1 || mon.evo[i] > count) return false;
  if (dex.read(&n, 1) != 1 || !readStr(mon.name, sizeof(mon.name), n)) return false;
  if (dex.read(&n, 1) != 1 || !readStr(mon.genus, sizeof(mon.genus), n)) return false;
  if (dex.read((uint8_t *)&fl, 2) != 2 || !readStr(mon.flavor, sizeof(mon.flavor), fl)) return false;
  size_t px = (size_t)side * side * 2;
  return mon.id == id && mon.type[0] < 18 && dex.read((uint8_t *)spriteBuf, px) == px;
}

static bool loadNames(uint32_t at) {
  uint32_t len = dex.size() - at;
  char *blk = (char *)malloc(len + 1);
  names = (char **)calloc(count + 1, sizeof(char *));
  if (!blk || !names || !dex.seek(at) || dex.read((uint8_t *)blk, len) != len) return false;
  blk[len] = 0;
  char *p = blk;
  for (int i = 1; i <= count; i++) {
    if (p >= blk + len) return false;
    names[i] = p;
    p += strlen(p) + 1;
  }
  return true;
}

// ---------- drawing helpers ----------

static void textAt(int x, int y, const uint8_t *font, uint16_t col, const char *s) {
  gfx->setFont(font);
  gfx->setTextColor(col);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static int textW(const uint8_t *font, const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setFont(font);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

static void button(int x, int y, int w, int h, uint16_t bg, uint16_t fg, const uint8_t *font, const char *s) {
  gfx->fillRoundRect(x, y, w, h, 5, bg);
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->setFont(font);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  textAt(x + (w - tw) / 2 - x1, y + (h - th) / 2 - y1, font, fg, s);
}

static bool inside(int x, int y, int bx, int by, int bw, int bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

// red title bar; back = "<" button on the left that returns to the menu
static void header(const char *title, bool back) {
  gfx->fillRect(0, 0, LCD_W, 24, RED);
  if (back) textAt(8, 18, u8g2_font_helvB12_tr, WHITE, "<");
  textAt(back ? 28 : 6, 18, u8g2_font_helvB12_tr, WHITE, title);
}

// word-wrapped text from baseline y, line height lh, stops before maxY
static void wrapped(const char *s, int x, int y, int w, int lh, int maxY) {
  const uint8_t *font = u8g2_font_helvR08_tr;
  char line[120] = "", cand[120];
  while (*s && y <= maxY) {
    while (*s == ' ') s++;
    const char *e = s;
    while (*e && *e != ' ') e++;
    snprintf(cand, sizeof(cand), "%s%s%.*s", line, *line ? " " : "", (int)(e - s), s);
    if (*line && textW(font, cand) > w) {
      textAt(x, y, font, INK, line);
      y += lh;
      line[0] = 0;
      continue;  // retry the same word on the new line
    }
    strcpy(line, cand);
    s = e;
  }
  if (*line && y <= maxY) textAt(x, y, font, INK, line);
}

static void message(const char *a, const char *b) {
  gfx->fillScreen(WHITE);
  header("POKEDEX", false);
  textAt(10, 100, u8g2_font_helvB12_tr, INK, a);
  textAt(10, 124, u8g2_font_helvR10_tr, GREY, b);
}

// ---------- screens ----------

enum Screen { MENU, NUMBER, NAME, MON };
static Screen screen;
static uint16_t cur = 1;

// MENU
static const int MENU_Y[3] = {40, 104, 168}, MENU_H = 52;

static void drawMenu() {
  char buf[48];
  gfx->fillScreen(WHITE);
  header("POKEDEX", false);
  snprintf(buf, sizeof(buf), "Browse  (#%04u %s)", cur, names[cur]);
  const char *label[3] = {buf, "Search by number", "Search by name"};
  for (int i = 0; i < 3; i++) button(20, MENU_Y[i], 280, MENU_H, BLUE, WHITE, u8g2_font_helvB12_tr, label[i]);
}

// MON
static int chipX[MAX_EVO], chipY[MAX_EVO], chipW[MAX_EVO];
static const int CHIP_H = 16;

static void drawMon() {
  char buf[48];
  gfx->fillScreen(WHITE);
  gfx->fillRect(0, 0, LCD_W, 24, RED);
  snprintf(buf, sizeof(buf), "#%04u", mon.id);
  textAt(6, 18, u8g2_font_helvB12_tr, WHITE, buf);
  textAt(66, 18, u8g2_font_helvB12_tr, WHITE, mon.name);
  button(258, 3, 58, 18, WHITE, RED, u8g2_font_helvB08_tr, "MENU");

  gfx->draw16bitRGBBitmap(4, 28, spriteBuf, side, side);

  const int x0 = 132;
  textAt(x0, 41, u8g2_font_helvR08_tr, GREY, mon.genus);
  for (int i = 0; i < 2; i++)
    if (mon.type[i] < 18) button(x0 + 64 * i, 48, 58, 16, TYPE_COL[mon.type[i]], WHITE, u8g2_font_helvB08_tr, TYPE_NAME[mon.type[i]]);
  snprintf(buf, sizeof(buf), "HT %u.%u m    WT %u.%u kg", mon.height / 10, mon.height % 10, mon.weight / 10,
           mon.weight % 10);
  textAt(x0, 80, u8g2_font_helvR08_tr, INK, buf);
  for (int i = 0; i < 6; i++) {
    int y = 86 + i * 11, v = mon.stat[i];
    textAt(x0, y + 8, u8g2_font_helvB08_tr, INK, STAT_NAME[i]);
    snprintf(buf, sizeof(buf), "%d", v);
    textAt(x0 + 46 - textW(u8g2_font_helvR08_tr, buf), y + 8, u8g2_font_helvR08_tr, INK, buf);
    gfx->fillRect(x0 + 52, y + 2, 132, 7, TRACK);
    gfx->fillRect(x0 + 52, y + 2, v * 132 / 255, 7, v < 60 ? C(0xF34444) : v < 90 ? C(0xFFC631) : C(0x4CAF50));
  }
  gfx->drawFastHLine(4, 153, LCD_W - 8, TRACK);

  // evolution family: one chip per species, wraps to a second row (Eevee has 9)
  int x = 6, y = 157;
  if (mon.nEvo <= 1) {
    textAt(x, y + 12, u8g2_font_helvR08_tr, GREY, "Does not evolve");
  } else {
    for (int i = 0; i < mon.nEvo; i++) {
      const char *n = names[mon.evo[i]];
      int w = textW(u8g2_font_helvB08_tr, n) + 12;
      if (x + w > LCD_W - 6) {
        x = 6;
        y += CHIP_H + 4;
      }
      chipX[i] = x;
      chipY[i] = y;
      chipW[i] = w;
      bool me = mon.evo[i] == mon.id;
      button(x, y, w, CHIP_H, me ? RED : TRACK, me ? WHITE : INK, u8g2_font_helvB08_tr, n);
      x += w + 4;
    }
  }
  wrapped(mon.flavor, 6, y + CHIP_H + 14, LCD_W - 12, 12, 237);
}

static void show(uint16_t id) {
  cur = id;
  screen = MON;
  if (loadMon(id)) drawMon();
  else {
    char buf[40];
    snprintf(buf, sizeof(buf), "Entry #%u is damaged", id);
    message(buf, "Rebuild dex.bin with tools/build_dex.py");
    button(258, 3, 58, 18, WHITE, RED, u8g2_font_helvB08_tr, "MENU");
  }
}

// evolution chip under (x, y), or 0
static uint16_t chipAt(int x, int y) {
  for (int i = 0; mon.nEvo > 1 && i < mon.nEvo; i++)
    if (inside(x, y, chipX[i], chipY[i] - 2, chipW[i], CHIP_H + 4)) return mon.evo[i];
  return 0;
}

// NUMBER: keypad
static char numBuf[5];
static const char *NUM_KEYS[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "GO"};

static void drawNumberField() {
  char buf[16];
  gfx->fillRect(0, 26, LCD_W, 48, WHITE);
  snprintf(buf, sizeof(buf), "#%s_", numBuf);
  textAt(20, 60, u8g2_font_helvB18_tr, INK, buf);
  int n = atoi(numBuf);
  textAt(130, 58, u8g2_font_helvB12_tr, GREY, n >= 1 && n <= count ? names[n] : *numBuf ? "(no Pokemon)" : "");
}

static void drawNumber() {
  gfx->fillScreen(WHITE);
  header("Search by number", true);
  for (int i = 0; i < 12; i++)
    button(20 + (i % 3) * 95, 78 + (i / 3) * 40, 90, 36, i == 11 ? RED : BLUE, WHITE, u8g2_font_helvB12_tr, NUM_KEYS[i]);
  drawNumberField();
}

static void tapNumber(int x, int y) {
  if (y < 24 && x < 120) {
    screen = MENU;
    drawMenu();
    return;
  }
  if (y < 78 || x < 20 || x >= 305) return;
  int k = (y - 78) / 40 * 3 + (x - 20) / 95;
  if (k < 0 || k > 11) return;
  size_t len = strlen(numBuf);
  if (k == 9) {
    if (len) numBuf[len - 1] = 0;
  } else if (k == 11) {
    int n = atoi(numBuf);
    if (n >= 1 && n <= count) {
      show(n);
      return;
    }
  } else if (len < 4) {
    numBuf[len] = NUM_KEYS[k][0];
    numBuf[len + 1] = 0;
  }
  drawNumberField();
}

// NAME: keyboard + up to 8 matches
static char nameBuf[13];
static uint16_t hits[8];
static int nHits;
static const char *KB_ROWS[3] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
static const int KB_Y = 150, KB_ROW_H = 30;

// lowercase letters and digits only, so "mrmime" finds "Mr. Mime" and "hooh" finds "Ho-Oh"
static void squash(const char *s, char *out, size_t cap) {
  size_t n = 0;
  for (; *s && n + 1 < cap; s++)
    if (isalnum((unsigned char)*s)) out[n++] = tolower((unsigned char)*s);
  out[n] = 0;
}

static void searchNames() {
  char q[16], n[64];
  squash(nameBuf, q, sizeof(q));
  nHits = 0;
  if (!*q) return;
  for (int pass = 0; pass < 2; pass++)  // prefix matches first, then the rest
    for (int i = 1; i <= count && nHits < 8; i++) {
      squash(names[i], n, sizeof(n));
      char *at = strstr(n, q);
      if (at && (pass == 0) == (at == n)) hits[nHits++] = i;
    }
}

static void drawNameResults() {
  char buf[48];
  gfx->fillRect(0, 26, LCD_W, KB_Y - 28, WHITE);
  snprintf(buf, sizeof(buf), "%s_", nameBuf);
  textAt(8, 44, u8g2_font_helvB12_tr, INK, buf);
  if (*nameBuf && !nHits) textAt(8, 70, u8g2_font_helvR10_tr, GREY, "No Pokemon found");
  for (int i = 0; i < nHits; i++) {
    snprintf(buf, sizeof(buf), "#%04u %s", hits[i], names[hits[i]]);
    button(4 + (i % 2) * 158, 52 + (i / 2) * 24, 154, 22, TRACK, INK, u8g2_font_helvB08_tr, buf);
  }
}

static void drawName() {
  gfx->fillScreen(WHITE);
  header("Search by name", true);
  char k[2] = "";
  for (int r = 0; r < 3; r++)
    for (int i = 0; KB_ROWS[r][i]; i++) {
      k[0] = KB_ROWS[r][i];
      button((r ? 16 : 0) + i * 32 + 1, KB_Y + r * KB_ROW_H, 30, KB_ROW_H - 3, BLUE, WHITE, u8g2_font_helvB12_tr, k);
    }
  button(16 + 7 * 32 + 1, KB_Y + 2 * KB_ROW_H, 78, KB_ROW_H - 3, RED, WHITE, u8g2_font_helvB12_tr, "DEL");
  drawNameResults();
}

static void tapName(int x, int y) {
  if (y < 24 && x < 120) {
    screen = MENU;
    drawMenu();
    return;
  }
  size_t len = strlen(nameBuf);
  if (y >= KB_Y) {
    int r = min((y - KB_Y) / KB_ROW_H, 2), i = (x - (r ? 16 : 0)) / 32;
    if (x < (r ? 16 : 0)) return;
    if (r == 2 && i >= 7) {
      if (len) nameBuf[len - 1] = 0;
    } else if (i < (int)strlen(KB_ROWS[r]) && len < sizeof(nameBuf) - 1) {
      nameBuf[len] = KB_ROWS[r][i];
      nameBuf[len + 1] = 0;
    } else {
      return;
    }
    searchNames();
    drawNameResults();
    return;
  }
  for (int i = 0; i < nHits; i++)
    if (inside(x, y, 4 + (i % 2) * 158, 52 + (i / 2) * 24, 154, 22)) {
      show(hits[i]);
      return;
    }
}

static void tapMenu(int x, int y) {
  for (int i = 0; i < 3; i++) {
    if (!inside(x, y, 20, MENU_Y[i], 280, MENU_H)) continue;
    if (i == 0) show(cur);
    if (i == 1) {
      screen = NUMBER;
      numBuf[0] = 0;
      drawNumber();
    }
    if (i == 2) {
      screen = NAME;
      nameBuf[0] = 0;
      nHits = 0;
      drawName();
    }
  }
}

// ---------- USB upload: "PUT dex.bin <bytes>\n" + raw data, 2 KB blocks acked with '#' ----------

static void serialPut(const String &line) {
  uint32_t size = line.substring(line.lastIndexOf(' ') + 1).toInt();
  if (!line.startsWith("PUT dex.bin ") || size == 0) {
    Serial.println("ERR");
    return;
  }
  dex.close();
  SD.remove("/dex.tmp");
  File f = SD.open("/dex.tmp", FILE_WRITE);
  if (!f) {
    Serial.println("ERR");
    return;
  }
  message("Receiving dex.bin...", "Keep the USB cable connected");
  Serial.println("OK");
  static uint8_t buf[2048];
  uint32_t left = size;
  while (left) {
    size_t n = Serial.readBytes(buf, left > sizeof(buf) ? sizeof(buf) : left);
    if (n == 0 || f.write(buf, n) != n) break;  // timeout or card full
    left -= n;
    Serial.println("#");
  }
  f.close();
  // the old file is replaced only once the new one arrived whole
  if (left == 0) {
    SD.remove("/dex.bin");
    SD.rename("/dex.tmp", "/dex.bin");
  }
  Serial.println(left == 0 ? "DONE" : "ERR");
  ESP.restart();
}

// ---------- main ----------

static bool openDex() {
  if (!SD.begin(SD_CS, sdSpi, 20000000) && !SD.begin(SD_CS, sdSpi, 4000000)) {
    message("No microSD card", "Insert a FAT32 card with dex.bin");
    return false;
  }
  dex = SD.open("/dex.bin", FILE_READ);
  uint8_t h[12];
  if (!dex || dex.read(h, 12) != 12 || memcmp(h, "PDX2", 4) != 0) {
    message("dex.bin not found or outdated", "Copy a new dex.bin to the card root");
    return false;
  }
  uint32_t namesAt;
  memcpy(&count, h + 4, 2);
  memcpy(&side, h + 6, 2);
  memcpy(&namesAt, h + 8, 4);
  spriteBuf = (uint16_t *)malloc((size_t)side * side * 2);
  if (count == 0 || side == 0 || side > 200 || !spriteBuf || namesAt >= dex.size() || !loadNames(namesAt)) {
    message("dex.bin is invalid", "Rebuild it with tools/build_dex.py");
    return false;
  }
  Serial.printf("dex.bin: %u Pokemon, heap %u\n", count, ESP.getFreeHeap());
  return true;
}

static bool ready;

void setup() {
  Serial.begin(921600);
  Serial.setTimeout(5000);
  ledcAttach(LCD_BL, 5000, 8);
  ledcWrite(LCD_BL, 255);
  gfx->begin(40000000);
  pinMode(TP_CS, OUTPUT);
  digitalWrite(TP_CS, HIGH);
  pinMode(TP_CLK, OUTPUT);
  digitalWrite(TP_CLK, LOW);
  pinMode(TP_MOSI, OUTPUT);
  pinMode(TP_MISO, INPUT);
  pinMode(TP_INT, INPUT);
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  prefs.begin("dex");
  ready = openDex();
  if (ready) {
    cur = constrain(prefs.getUShort("cur", 1), 1, count);
    screen = MENU;
    drawMenu();
  }
}

void loop() {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("PUT ")) serialPut(line);
  }
  if (!ready) return;

  static uint32_t nextMs;
  static uint16_t steps;
  static uint8_t miss = 3;
  static bool down, holdNav;
  int x, y;
  uint32_t now = millis();
  if (!touchXY(&x, &y)) {
    // resistive panels drop pressure for an instant: a release needs 3 empty reads
    if (down && ++miss >= 3) {
      down = false;
      if (screen == MON) prefs.putUShort("cur", cur);  // on release only: spares the flash while scrolling
    }
    delay(10);
    return;
  }
  miss = 0;
  if (!down) {  // new press
    down = true;
    holdNav = false;
    if (screen == MENU) tapMenu(x, y);
    else if (screen == NUMBER) tapNumber(x, y);
    else if (screen == NAME) tapName(x, y);
    else if (y < 24 && x >= 250) {
      screen = MENU;
      drawMenu();
    } else if (uint16_t id = chipAt(x, y)) {
      if (id != cur) show(id);
    } else {
      holdNav = true;
      nextMs = now + 450;
      steps = 0;
      show(x < LCD_W / 2 ? (cur > 1 ? cur - 1 : count) : (cur < count ? cur + 1 : 1));
    }
    return;
  }
  if (!holdNav || screen != MON || (int32_t)(now - nextMs) < 0) return;
  nextMs = now + 110;  // held: repeat, jumping by 10 after a while
  int step = ++steps > 15 ? 10 : 1;
  int id = cur + (x < LCD_W / 2 ? -step : step);
  if (id < 1) id += count;
  if (id > count) id -= count;
  show(id);
}
