// Pokedex for the ESP32-2432S028 ("Cheap Yellow Display", two-USB ST7789 version).
// Reads every Pokemon from /dex.bin on the microSD (built by tools/build_dex.py
// from PokeAPI). Tap the right half for the next Pokemon, the left half for the
// previous one; hold to scroll faster.
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
                      TRACK = C(0xE4E4E4);

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
static Preferences prefs;

struct Mon {
  uint16_t id, height, weight;
  uint8_t type[2], stat[6];
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

// screen x of the finger, or -1 if not touched
static int touchX() {
  if (digitalRead(TP_INT) == HIGH) return -1;
  digitalWrite(TP_CS, LOW);
  int z = tpConv(0xB1) + 4095 - tpConv(0xC1);
  int sx = 0;
  if (z >= 400) {
    tpConv(0x91);  // first sample is noisy
    for (int i = 0; i < 4; i++) sx += tpConv(0x91);
  }
  tpConv(0xD0);  // PD=00 re-enables PENIRQ
  digitalWrite(TP_CS, HIGH);
  if (z < 400) return -1;
  return constrain((sx / 4 - TP_X_MIN) * LCD_W / (TP_X_MAX - TP_X_MIN), 0, LCD_W - 1);
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
  if (!dex.seek(8 + 4UL * (id - 1)) || dex.read((uint8_t *)&off, 4) != 4 || !dex.seek(off) ||
      dex.read(h, 15) != 15)
    return false;
  memcpy(&mon.id, h, 2);
  mon.type[0] = h[2];
  mon.type[1] = h[3];
  memcpy(&mon.height, h + 4, 2);
  memcpy(&mon.weight, h + 6, 2);
  memcpy(mon.stat, h + 8, 6);
  if (!readStr(mon.name, sizeof(mon.name), h[14])) return false;
  if (dex.read(&n, 1) != 1 || !readStr(mon.genus, sizeof(mon.genus), n)) return false;
  if (dex.read((uint8_t *)&fl, 2) != 2 || !readStr(mon.flavor, sizeof(mon.flavor), fl)) return false;
  size_t px = (size_t)side * side * 2;
  return mon.id == id && mon.type[0] < 18 && dex.read((uint8_t *)spriteBuf, px) == px;
}

// ---------- drawing ----------

static void textAt(int x, int y, const uint8_t *font, uint16_t col, const char *s) {
  gfx->setFont(font);
  gfx->setTextColor(col);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static int textW(const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// word-wrapped text from baseline y, line height lh, stops before maxY
static void wrapped(const char *s, int x, int y, int w, int lh, int maxY) {
  gfx->setFont(u8g2_font_helvR10_tr);
  gfx->setTextColor(INK);
  char line[120] = "", cand[120];
  while (*s && y <= maxY) {
    while (*s == ' ') s++;
    const char *e = s;
    while (*e && *e != ' ') e++;
    snprintf(cand, sizeof(cand), "%s%s%.*s", line, *line ? " " : "", (int)(e - s), s);
    if (*line && textW(cand) > w) {
      gfx->setCursor(x, y);
      gfx->print(line);
      y += lh;
      line[0] = 0;
      continue;  // retry the same word on the new line
    }
    strcpy(line, cand);
    s = e;
  }
  if (*line && y <= maxY) {
    gfx->setCursor(x, y);
    gfx->print(line);
  }
}

static void typeBadge(int x, uint8_t t) {
  gfx->fillRoundRect(x, 48, 58, 16, 4, TYPE_COL[t]);
  gfx->setFont(u8g2_font_helvB08_tr);
  textAt(x + (58 - textW(TYPE_NAME[t])) / 2, 60, u8g2_font_helvB08_tr, WHITE, TYPE_NAME[t]);
}

static void message(const char *a, const char *b) {
  gfx->fillScreen(WHITE);
  gfx->fillRect(0, 0, LCD_W, 24, RED);
  textAt(6, 18, u8g2_font_helvB12_tr, WHITE, "POKEDEX");
  textAt(10, 100, u8g2_font_helvB12_tr, INK, a);
  textAt(10, 124, u8g2_font_helvR10_tr, GREY, b);
}

static void drawMon() {
  char buf[48];
  gfx->fillScreen(WHITE);
  gfx->fillRect(0, 0, LCD_W, 24, RED);
  snprintf(buf, sizeof(buf), "#%04u", mon.id);
  textAt(6, 18, u8g2_font_helvB12_tr, WHITE, buf);
  textAt(66, 18, u8g2_font_helvB12_tr, WHITE, mon.name);
  snprintf(buf, sizeof(buf), "%u/%u", mon.id, count);
  gfx->setFont(u8g2_font_helvR08_tr);
  textAt(LCD_W - 6 - textW(buf), 17, u8g2_font_helvR08_tr, WHITE, buf);

  gfx->draw16bitRGBBitmap(4, 28, spriteBuf, side, side);

  const int x0 = 132;
  textAt(x0, 41, u8g2_font_helvR08_tr, GREY, mon.genus);
  typeBadge(x0, mon.type[0]);
  if (mon.type[1] < 18) typeBadge(x0 + 64, mon.type[1]);
  snprintf(buf, sizeof(buf), "HT %u.%u m    WT %u.%u kg", mon.height / 10, mon.height % 10, mon.weight / 10,
           mon.weight % 10);
  textAt(x0, 80, u8g2_font_helvR08_tr, INK, buf);
  for (int i = 0; i < 6; i++) {
    int y = 86 + i * 11, v = mon.stat[i];
    textAt(x0, y + 8, u8g2_font_helvB08_tr, INK, STAT_NAME[i]);
    snprintf(buf, sizeof(buf), "%d", v);
    gfx->setFont(u8g2_font_helvR08_tr);
    textAt(x0 + 46 - textW(buf), y + 8, u8g2_font_helvR08_tr, INK, buf);
    gfx->fillRect(x0 + 52, y + 2, 132, 7, TRACK);
    gfx->fillRect(x0 + 52, y + 2, v * 132 / 255, 7, v < 60 ? C(0xF34444) : v < 90 ? C(0xFFC631) : C(0x4CAF50));
  }
  gfx->drawFastHLine(4, 153, LCD_W - 8, TRACK);
  wrapped(mon.flavor, 6, 170, LCD_W - 12, 14, 236);
}

static uint16_t cur = 1;

static void show(uint16_t id) {
  cur = id;
  if (loadMon(id)) drawMon();
  else {
    char buf[40];
    snprintf(buf, sizeof(buf), "Entry #%u is damaged", id);
    message(buf, "Rebuild dex.bin with tools/build_dex.py");
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
  uint8_t h[8];
  if (!dex || dex.read(h, 8) != 8 || memcmp(h, "PDX1", 4) != 0) {
    message("dex.bin not found", "Copy dex.bin to the root of the card");
    return false;
  }
  memcpy(&count, h + 4, 2);
  memcpy(&side, h + 6, 2);
  spriteBuf = (uint16_t *)malloc((size_t)side * side * 2);
  if (count == 0 || side == 0 || side > 200 || !spriteBuf) {
    message("dex.bin is invalid", "Rebuild it with tools/build_dex.py");
    return false;
  }
  Serial.printf("dex.bin: %u Pokemon\n", count);
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
  if (ready) show(constrain(prefs.getUShort("cur", 1), 1, count));
}

void loop() {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("PUT ")) serialPut(line);
  }
  if (!ready) return;

  static uint32_t pressMs, nextMs;
  static uint16_t steps;
  int x = touchX();
  uint32_t now = millis();
  if (x < 0) {
    if (pressMs) prefs.putUShort("cur", cur);  // on release only: spares the flash while scrolling
    pressMs = 0;
    delay(10);
    return;
  }
  if (!pressMs) {  // new press
    pressMs = now;
    nextMs = now + 450;
    steps = 0;
  } else if ((int32_t)(now - nextMs) < 0) {
    return;
  } else {  // held: repeat, jumping by 10 after a while
    nextMs = now + 110;
    steps++;
  }
  int step = steps > 15 ? 10 : 1;
  int id = cur + (x < LCD_W / 2 ? -step : step);
  if (id < 1) id += count;
  if (id > count) id -= count;
  show(id);
}
