// Pokedex for the ESP32-2432S028 ("Cheap Yellow Display", two-USB ST7789 version).
// Reads every Pokemon from /dex.bin on the microSD (built by tools/build_dex.py
// from PokeAPI). Look based on the "Pokedex Prototype v2.0" Figma community file
// by Ricardo Schiniegoski (see design/DESIGN.md), squeezed into 320x240.
//   Home: grid of Pokemon (swipe up/down to page), search bar, sort by # / A-Z.
//   Search: by name or number.
//   Pokemon: tap left/right of the sprite for previous/next (hold to scroll),
//   tap an evolution to open it, "<" to go home.
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
// prototype palette
static const uint16_t RED = C(0xDC0A2D), WHITE = C(0xFFFFFF), DARK = C(0x212121), MEDIUM = C(0x666666),
                      LIGHT = C(0xE0E0E0), BACK = C(0xEFEFEF);
static const uint16_t KEY = 0xF81F;  // transparent sprite pixel (tools/build_dex.py)

// same order as TYPES in tools/build_dex.py
static const char *TYPE_NAME[18] = {"Normal", "Fighting", "Flying", "Poison", "Ground", "Rock",
                                    "Bug", "Ghost", "Steel", "Fire", "Water", "Grass",
                                    "Electric", "Psychic", "Ice", "Dragon", "Dark", "Fairy"};
static const uint16_t TYPE_COL[18] = {C(0xAAA67F), C(0xC12239), C(0xA891EC), C(0xA43E9E), C(0xDEC16B), C(0xB69E31),
                                      C(0xA7B723), C(0x70559B), C(0xB7B9D0), C(0xF57D31), C(0x6493EB), C(0x74CB48),
                                      C(0xF9CF30), C(0xFB5584), C(0x9AD6DF), C(0x7037FF), C(0x75574C), C(0xE69EAC)};
static const char *STAT_NAME[6] = {"HP", "ATK", "DEF", "SATK", "SDEF", "SPD"};

static const uint8_t *F_TITLE = u8g2_font_helvB12_tr, *F_HEAD = u8g2_font_helvB10_tr, *F_BOLD = u8g2_font_helvB08_tr,
                     *F_BODY = u8g2_font_helvR08_tr, *F_TINY = u8g2_font_5x7_tr;

static SPIClass sdSpi(VSPI);
static File dex;
static uint16_t count, side;
static uint16_t *spriteBuf;
static char **names;     // names[id], 1-based, kept in RAM for search and the grid
static uint16_t *order;  // every id, by number or by name
static uint16_t *view;   // what the grid shows: order filtered by the search
static int nView;
static Preferences prefs;

static const int MAX_EVO = 12;
struct Mon {
  uint16_t id, height, weight, evo[MAX_EVO];
  uint8_t type[2], stat[6], nEvo, sw, sh;
  char name[64], genus[64], abil[128], flavor[1001];
};
static Mon mon, card;  // card: scratch entry for the home grid

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

static bool readStr(char *dst, size_t cap) {
  uint8_t len;
  if (dex.read(&len, 1) != 1 || len >= cap || dex.read((uint8_t *)dst, len) != len) return false;
  dst[len] = 0;
  return true;
}

// loads entry id into m; its artwork (or, for the grid, its thumbnail) goes to spriteBuf
static bool loadMon(uint16_t id, Mon &m, bool thumb = false) {
  uint32_t off;
  uint8_t h[15];
  uint16_t fl;
  if (!dex.seek(12 + 4UL * (id - 1)) || dex.read((uint8_t *)&off, 4) != 4 || !dex.seek(off) ||
      dex.read(h, 15) != 15)
    return false;
  memcpy(&m.id, h, 2);
  m.type[0] = h[2];
  m.type[1] = h[3];
  memcpy(&m.height, h + 4, 2);
  memcpy(&m.weight, h + 6, 2);
  memcpy(m.stat, h + 8, 6);
  m.nEvo = h[14];
  if (m.nEvo > MAX_EVO || dex.read((uint8_t *)m.evo, m.nEvo * 2) != m.nEvo * 2u) return false;
  for (int i = 0; i < m.nEvo; i++)
    if (m.evo[i] < 1 || m.evo[i] > count) return false;
  if (!readStr(m.name, sizeof(m.name)) || !readStr(m.genus, sizeof(m.genus)) || !readStr(m.abil, sizeof(m.abil)))
    return false;
  if (dex.read((uint8_t *)&fl, 2) != 2 || fl >= sizeof(m.flavor) || dex.read((uint8_t *)m.flavor, fl) != fl) return false;
  m.flavor[fl] = 0;
  if (dex.read(&m.sw, 1) != 1 || dex.read(&m.sh, 1) != 1) return false;
  if (thumb && (!dex.seek(dex.position() + (size_t)m.sw * m.sh * 2) || dex.read(&m.sw, 1) != 1 || dex.read(&m.sh, 1) != 1))
    return false;
  if (!m.sw || !m.sh || m.sw > side || m.sh > side) return false;
  size_t px = (size_t)m.sw * m.sh * 2;
  return m.id == id && m.type[0] < 18 && dex.read((uint8_t *)spriteBuf, px) == px;
}

static bool loadNames(uint32_t at) {
  uint32_t len = dex.size() - at;
  char *blk = (char *)malloc(len + 1);
  names = (char **)calloc(count + 1, sizeof(char *));
  order = (uint16_t *)malloc(count * 2);
  view = (uint16_t *)malloc(count * 2);
  if (!blk || !names || !order || !view || !dex.seek(at) || dex.read((uint8_t *)blk, len) != len) return false;
  blk[len] = 0;
  char *p = blk;
  for (int i = 1; i <= count; i++) {
    if (p >= blk + len) return false;
    names[i] = p;
    p += strlen(p) + 1;
    order[i - 1] = i;
  }
  return true;
}

// ---------- drawing helpers ----------

// a blended toward b by t/255
static uint16_t mix(uint16_t a, uint16_t b, int t) {
  int r = ((a >> 11) * (255 - t) + (b >> 11) * t) / 255;
  int g = (((a >> 5) & 63) * (255 - t) + ((b >> 5) & 63) * t) / 255;
  int bl = ((a & 31) * (255 - t) + (b & 31) * t) / 255;
  return r << 11 | g << 5 | bl;
}

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

static void centered(int cx, int y, const uint8_t *font, uint16_t col, const char *s) {
  textAt(cx - textW(font, s) / 2, y, font, col, s);
}

static void pill(int x, int y, int w, int h, uint16_t bg, uint16_t fg, const uint8_t *font, const char *s) {
  gfx->fillRoundRect(x, y, w, h, h / 2 < 8 ? h / 2 : 8, bg);
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->setFont(font);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  textAt(x + (w - tw) / 2 - x1, y + (h - th) / 2 - y1, font, fg, s);
}

static bool inside(int x, int y, int bx, int by, int bw, int bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

// spriteBuf (w x h) centered on (cx, cy), nearest-neighbour scaled to fit box
// (whole multiples up to maxK when it is smaller); transparent pixels are skipped
static void drawSprite(int w, int h, int cx, int cy, int box, int maxK) {
  int big = max(w, h), num = 1, den = 1;
  if (big <= box) num = constrain(box / big, 1, maxK);
  else num = box, den = big;
  int ow = w * num / den, oh = h * num / den, x0 = cx - ow / 2, y0 = cy - oh / 2;
  static uint16_t line[128];
  for (int oy = 0; oy < oh; oy++) {
    const uint16_t *src = spriteBuf + (oy * den / num) * w;
    for (int ox = 0; ox < ow; ox++) line[ox] = src[ox * den / num];
    for (int a = 0; a < ow;) {  // runs of opaque pixels
      if (line[a] == KEY) {
        a++;
        continue;
      }
      int b = a;
      while (b < ow && line[b] != KEY) b++;
      gfx->draw16bitRGBBitmap(x0 + a, y0 + oy, line + a, b - a, 1);
      a = b;
    }
  }
}

static void pokeball(int cx, int cy, int r, uint16_t fg, uint16_t bg) {
  gfx->fillCircle(cx, cy, r, fg);
  gfx->fillCircle(cx, cy, r * 4 / 5, bg);
  gfx->fillRect(cx - r, cy - r / 10, 2 * r + 1, r / 5 + 1, fg);
  gfx->fillCircle(cx, cy, r * 3 / 10, fg);
  gfx->fillCircle(cx, cy, r * 3 / 20, bg);
}

// the prototype's solid pokeball icon, white on red
static void logo(int cx, int cy) {
  gfx->fillCircle(cx, cy, 10, WHITE);
  gfx->fillRect(cx - 10, cy - 1, 21, 3, RED);
  gfx->fillCircle(cx, cy, 5, RED);
  gfx->fillCircle(cx, cy, 2, WHITE);
}

// word-wrapped text from baseline y, line height lh, stops before maxY
static void wrapped(const char *s, int x, int y, int w, int lh, int maxY, uint16_t col) {
  char line[120] = "", cand[120];
  while (*s && y <= maxY) {
    while (*s == ' ') s++;
    const char *e = s;
    while (*e && *e != ' ') e++;
    snprintf(cand, sizeof(cand), "%s%s%.*s", line, *line ? " " : "", (int)(e - s), s);
    if (*line && textW(F_BODY, cand) > w) {
      textAt(x, y, F_BODY, col, line);
      y += lh;
      line[0] = 0;
      continue;  // retry the same word on the new line
    }
    strcpy(line, cand);
    s = e;
  }
  if (*line && y <= maxY) textAt(x, y, F_BODY, col, line);
}

static void message(const char *a, const char *b) {
  gfx->fillScreen(RED);
  logo(18, 15);
  textAt(34, 21, F_TITLE, WHITE, "Pokedex");
  gfx->fillRoundRect(4, 32, LCD_W - 8, LCD_H - 36, 8, WHITE);
  textAt(16, 100, F_TITLE, DARK, a);
  textAt(16, 124, u8g2_font_helvR10_tr, MEDIUM, b);
}

// ---------- screens ----------

enum Screen { HOME, SEARCH, MON };
static Screen screen;
static uint16_t cur = 1;

// HOME: grid of cards in a white panel, paged by swiping. SEARCH is the same
// screen with the keyboard open: one row of cards above it, filtered live.
static const int CARD_W = 98, CARD_H = 80, GRID_X = 10;
static int page, gridY, perPage;
static bool byName;
static char query[13];
static const char *KB_ROWS[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
static const int KB_Y = 122, KB_ROW_H = 29, KB_OFF[4] = {0, 0, 16, 16};

static int pages() { return max(1, (nView + perPage - 1) / perPage); }

// lowercase letters and digits only, so "mrmime" finds "Mr. Mime" and "hooh" finds "Ho-Oh"
static void squash(const char *s, char *out, size_t cap) {
  size_t n = 0;
  for (; *s && n + 1 < cap; s++)
    if (isalnum((unsigned char)*s)) out[n++] = tolower((unsigned char)*s);
  out[n] = 0;
}

// numbers match by prefix ("4" -> #4, #40...), names anywhere in the name
static void filter() {
  char q[16], n[64];
  squash(query, q, sizeof(q));
  bool num = *q && strspn(q, "0123456789") == strlen(q);
  nView = 0;
  for (int i = 0; i < count; i++) {
    uint16_t id = order[i];
    if (num) snprintf(n, sizeof(n), "%u", id);
    else squash(names[id], n, sizeof(n));
    char *at = strstr(n, q);
    if (num ? at == n : at != nullptr) view[nView++] = id;
  }
  page = 0;
}

static void drawCard(int slot, uint16_t id) {
  int x = GRID_X + slot % 3 * (CARD_W + 4), y = gridY + slot / 3 * (CARD_H + 4);
  char buf[8];
  gfx->fillRoundRect(x, y, CARD_W, CARD_H, 8, LIGHT);  // 1px border, like the card shadow
  gfx->fillRoundRect(x + 1, y + 1, CARD_W - 2, CARD_H - 2, 7, WHITE);
  gfx->fillRoundRect(x + 1, y + 36, CARD_W - 2, CARD_H - 37, 7, BACK);
  snprintf(buf, sizeof(buf), "#%03u", id);
  textAt(x + CARD_W - 6 - textW(F_TINY, buf), y + 10, F_TINY, MEDIUM, buf);
  if (loadMon(id, card, true)) drawSprite(card.sw, card.sh, x + CARD_W / 2, y + 38, 52, 1);
  centered(x + CARD_W / 2, y + CARD_H - 6, textW(F_BODY, names[id]) > CARD_W - 6 ? F_TINY : F_BODY, DARK, names[id]);
}

static void drawGrid() {
  gfx->fillRoundRect(4, gridY - 4, LCD_W - 8, perPage / 3 * (CARD_H + 4) + 4, 8, WHITE);
  if (!nView) centered(LCD_W / 2, gridY + 40, u8g2_font_helvR10_tr, MEDIUM, "No Pokemon found");
  for (int i = 0; i < perPage && page * perPage + i < nView; i++) drawCard(i, view[page * perPage + i]);
  if (pages() > 1) {  // scrollbar
    int track = perPage / 3 * (CARD_H + 4) - 4, len = max(6, track / pages());
    gfx->fillRect(LCD_W - 7, gridY + (track - len) * page / (pages() - 1), 2, len, MEDIUM);
  }
}

static void drawSearchBar(int y, bool typing) {
  char buf[20];
  gfx->fillRoundRect(8, y, 264, 26, 13, WHITE);
  gfx->drawCircle(23, y + 11, 5, RED);
  gfx->drawLine(27, y + 15, 31, y + 19, RED);
  snprintf(buf, sizeof(buf), "%s%s", query, typing ? "|" : "");
  if (*buf) textAt(38, y + 17, F_BODY, DARK, buf);
  else textAt(38, y + 17, F_BODY, MEDIUM, "Search");
  if (*query) {  // clear
    gfx->drawLine(252, y + 9, 259, y + 16, RED);
    gfx->drawLine(252, y + 16, 259, y + 9, RED);
  }
  gfx->fillCircle(294, y + 13, 13, WHITE);
  centered(294, y + 18, F_HEAD, RED, byName ? "A" : "#");
  if (byName) gfx->drawFastHLine(289, y + 20, 10, RED);
}

static void drawHome() {
  gfx->fillScreen(RED);
  if (screen == SEARCH) {
    drawSearchBar(4, true);
    char k[2] = "";
    for (int r = 0; r < 4; r++)
      for (int i = 0; KB_ROWS[r][i]; i++) {
        k[0] = KB_ROWS[r][i];
        pill(KB_OFF[r] + i * 32 + 1, KB_Y + r * KB_ROW_H + 1, 30, KB_ROW_H - 3, WHITE, DARK, F_HEAD, k);
      }
    pill(241, KB_Y + 3 * KB_ROW_H + 1, 38, KB_ROW_H - 3, BACK, RED, F_BOLD, "DEL");
    pill(281, KB_Y + 3 * KB_ROW_H + 1, 38, KB_ROW_H - 3, BACK, RED, F_BOLD, "OK");
  } else {
    logo(18, 15);
    textAt(34, 21, F_TITLE, WHITE, "Pokedex");
    drawSearchBar(30, false);
  }
  drawGrid();
}

static void openHome(bool search) {
  Screen s = search ? SEARCH : HOME;
  screen = s;
  gridY = s == SEARCH ? 38 : 66;
  perPage = s == SEARCH ? 3 : 6;
  page = 0;
  for (int i = 0; i < nView; i++)
    if (view[i] == cur) page = i / perPage;
  drawHome();
}

static void goHome() { openHome(false); }

static int byNameCmp(const void *a, const void *b) {
  return strcasecmp(names[*(const uint16_t *)a], names[*(const uint16_t *)b]);
}

static void show(uint16_t id);

// taps shared by both modes; bar = y of the search bar
static bool tapBar(int x, int y, int bar) {
  if (!inside(x, y, 0, bar - 2, LCD_W, 30)) return false;
  if (x >= 278) {  // sort
    byName = !byName;
    if (byName) qsort(order, count, 2, byNameCmp);
    else
      for (int i = 0; i < count; i++) order[i] = i + 1;
  } else if (*query && x >= 244) {  // clear
    query[0] = 0;
  } else {
    openHome(true);
    return true;
  }
  filter();
  drawHome();
  return true;
}

static void tapCard(int x, int y) {
  for (int i = 0; i < perPage && page * perPage + i < nView; i++)
    if (inside(x, y, GRID_X + i % 3 * (CARD_W + 4), gridY + i / 3 * (CARD_H + 4), CARD_W, CARD_H))
      show(view[page * perPage + i]);
}

static void releaseHome(int x, int y, int dx, int dy) {
  if (abs(dy) > 45 && abs(dy) > 2 * abs(dx) && y >= gridY - 4) {  // swipe: finger up = next page
    int p = constrain(page + (dy < 0 ? 1 : -1), 0, pages() - 1);
    if (p != page) {
      page = p;
      drawGrid();
    }
    return;
  }
  if (!tapBar(x, y, 30)) tapCard(x, y);
}

static void tapSearch(int x, int y) {
  if (tapBar(x, y, 4)) return;
  if (y < KB_Y) {
    tapCard(x, y);
    return;
  }
  size_t len = strlen(query);
  int r = min((y - KB_Y) / KB_ROW_H, 3);
  if (r == 3 && x >= 280) {
    goHome();
    return;
  }
  if (r == 3 && x >= 240) {
    if (!len) return;
    query[len - 1] = 0;
  } else {
    int i = (x - KB_OFF[r]) / 32;
    if (x < KB_OFF[r] || i >= (int)strlen(KB_ROWS[r]) || len >= sizeof(query) - 1) return;
    query[len] = KB_ROWS[r][i];
    query[len + 1] = 0;
  }
  filter();
  drawSearchBar(4, true);
  drawGrid();
}

// MON: type-coloured page, sprite + evolutions on the left, white card on the right
static int chipX[MAX_EVO], chipY[MAX_EVO], chipW[MAX_EVO];
static const int CHIP_H = 14, CARD_X = 128, CARD_CX = CARD_X + 94;

static void drawMon() {
  char buf[48];
  uint16_t tc = TYPE_COL[mon.type[0]], soft = mix(tc, WHITE, 60);
  gfx->fillScreen(tc);
  for (int d = 0; d < 2; d++) {  // back arrow
    gfx->drawLine(7, 14 + d, 19, 14 + d, WHITE);
    gfx->drawLine(7, 14 + d, 12, 9 + d, WHITE);
    gfx->drawLine(7, 14 + d, 12, 19 + d, WHITE);
  }
  textAt(28, 20, F_TITLE, WHITE, mon.name);
  snprintf(buf, sizeof(buf), "#%03u", mon.id);
  textAt(LCD_W - 8 - textW(F_BOLD, buf), 18, F_BOLD, WHITE, buf);

  pokeball(64, 88, 54, soft, tc);  // watermark behind the sprite
  drawSprite(mon.sw, mon.sh, 64, 88, 112, 4);
  for (int d = 0; d < 2; d++) {  // chevrons
    gfx->drawLine(10 + d, 88, 14 + d, 84, WHITE);
    gfx->drawLine(10 + d, 88, 14 + d, 92, WHITE);
    gfx->drawLine(118 - d, 88, 114 - d, 84, WHITE);
    gfx->drawLine(118 - d, 88, 114 - d, 92, WHITE);
  }

  // evolution family under the sprite: one column, or two when it is long (Eevee)
  if (mon.nEvo <= 1) {
    centered(64, 170, F_BODY, WHITE, "No evolutions");
  } else {
    int cols = mon.nEvo > 5 ? 2 : 1, w = cols == 2 ? 58 : 116;
    for (int i = 0; i < mon.nEvo; i++) {
      chipX[i] = 6 + i % cols * (w + 4);
      chipY[i] = 152 + i / cols * (CHIP_H + 2);
      chipW[i] = w;
      bool me = mon.evo[i] == mon.id;
      const char *n = names[mon.evo[i]];
      pill(chipX[i], chipY[i], w, CHIP_H, me ? WHITE : soft, me ? tc : WHITE,
           textW(F_BOLD, n) > w - 6 ? F_TINY : F_BOLD, n);
    }
  }

  gfx->fillRoundRect(CARD_X, 28, LCD_W - CARD_X - 4, LCD_H - 32, 8, WHITE);
  // type pills
  int pw[2], total = 0, nt = mon.type[1] < 18 ? 2 : 1;
  for (int i = 0; i < nt; i++) total += (pw[i] = textW(F_BOLD, TYPE_NAME[mon.type[i]]) + 16);
  for (int i = 0, x = CARD_CX - (total + 8 * (nt - 1)) / 2; i < nt; x += pw[i] + 8, i++)
    pill(x, 34, pw[i], 16, TYPE_COL[mon.type[i]], WHITE, F_BOLD, TYPE_NAME[mon.type[i]]);

  centered(CARD_CX, 68, F_HEAD, tc, "About");
  const int colW = 62, cx[3] = {CARD_CX - colW, CARD_CX, CARD_CX + colW};
  snprintf(buf, sizeof(buf), "%u.%u kg", mon.weight / 10, mon.weight % 10);
  centered(cx[0] + 6, 86, F_BODY, DARK, buf);
  int bx = cx[0] - textW(F_BODY, buf) / 2 - 6;  // weight icon: a bag
  gfx->drawRoundRect(bx - 4, 79, 9, 8, 2, DARK);
  gfx->drawCircle(bx, 78, 2, DARK);
  snprintf(buf, sizeof(buf), "%u.%u m", mon.height / 10, mon.height % 10);
  centered(cx[1] + 6, 86, F_BODY, DARK, buf);
  bx = cx[1] - textW(F_BODY, buf) / 2 - 6;  // height icon: a ruler
  gfx->drawRect(bx - 3, 77, 7, 10, DARK);
  for (int t = 0; t < 3; t++) gfx->drawFastHLine(bx - 3, 79 + t * 3, 3, DARK);
  char ab[128];
  strcpy(ab, mon.abil);
  int ay = strchr(ab, '\n') ? 81 : 86;
  for (char *a = strtok(ab, "\n"); a && ay <= 92; a = strtok(nullptr, "\n"), ay += 10)
    centered(cx[2], ay, textW(F_BODY, a) > colW - 2 ? F_TINY : F_BODY, DARK, a);
  centered(cx[0], 102, F_TINY, MEDIUM, "Weight");
  centered(cx[1], 102, F_TINY, MEDIUM, "Height");
  centered(cx[2], 102, F_TINY, MEDIUM, "Abilities");
  gfx->drawFastVLine(CARD_CX - colW / 2, 74, 30, LIGHT);
  gfx->drawFastVLine(CARD_CX + colW / 2, 74, 30, LIGHT);

  wrapped(mon.flavor, CARD_X + 8, 117, LCD_W - CARD_X - 20, 11, 150, DARK);

  centered(CARD_CX, 167, F_HEAD, tc, "Base Stats");
  const int sx = CARD_X + 38;
  gfx->drawFastVLine(sx, 172, 60, LIGHT);
  for (int i = 0; i < 6; i++) {
    int y = 172 + i * 10, v = mon.stat[i];
    textAt(sx - 4 - textW(F_BOLD, STAT_NAME[i]), y + 8, F_BOLD, tc, STAT_NAME[i]);
    snprintf(buf, sizeof(buf), "%03d", v);
    textAt(sx + 5, y + 8, F_BODY, DARK, buf);
    gfx->fillRoundRect(sx + 26, y + 3, 112, 4, 2, mix(tc, WHITE, 200));
    gfx->fillRoundRect(sx + 26, y + 3, max(4, v * 112 / 255), 4, 2, tc);
  }
}

static void show(uint16_t id) {
  cur = id;
  screen = MON;
  if (loadMon(id, mon)) drawMon();
  else {
    char buf[40];
    snprintf(buf, sizeof(buf), "Entry #%u is damaged", id);
    message(buf, "Rebuild dex.bin with tools/build_dex.py");
  }
}

// evolution chip under (x, y), or 0
static uint16_t chipAt(int x, int y) {
  for (int i = 0; mon.nEvo > 1 && i < mon.nEvo; i++)
    if (inside(x, y, chipX[i], chipY[i] - 1, chipW[i], CHIP_H + 2)) return mon.evo[i];
  return 0;
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
  if (!dex || dex.read(h, 12) != 12 || memcmp(h, "PDX3", 4) != 0) {
    message("dex.bin missing or outdated", "Copy a new dex.bin to the card");
    return false;
  }
  uint32_t namesAt;
  memcpy(&count, h + 4, 2);
  memcpy(&side, h + 6, 2);
  memcpy(&namesAt, h + 8, 4);
  spriteBuf = (uint16_t *)malloc((size_t)side * side * 2);
  if (count == 0 || side == 0 || side > 128 || !spriteBuf || namesAt >= dex.size() || !loadNames(namesAt)) {
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
    filter();
    goHome();
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
  static Screen pressedOn;
  static int x0, y0, lx, ly, hx[3], hy[3], n;
  int x, y;
  uint32_t now = millis();
  if (!touchXY(&x, &y)) {
    // resistive panels drop pressure for an instant: a release needs 3 empty reads
    if (down && ++miss >= 3) {
      down = false;
      if (screen == HOME && pressedOn == HOME) releaseHome(x0, y0, hx[0] - x0, hy[0] - y0);  // home acts on release: it tells taps from swipes
      else if (screen == MON) prefs.putUShort("cur", cur);  // on release only: spares the flash while scrolling
    }
    delay(10);
    return;
  }
  miss = 0;
  lx = x;
  ly = y;
  // the last readings before the finger lifts are noise (pressure fading), so a
  // swipe is measured to the sample 3 reads back; a short tap never reaches it
  if (n < 3) n++;
  hx[0] = n == 3 ? hx[1] : x0;
  hy[0] = n == 3 ? hy[1] : y0;
  hx[1] = hx[2];
  hy[1] = hy[2];
  hx[2] = x;
  hy[2] = y;
  if (!down) {  // new press
    down = true;
    holdNav = false;
    x0 = x;
    y0 = y;
    n = 0;
    hx[0] = hx[1] = hx[2] = x;
    hy[0] = hy[1] = hy[2] = y;
    pressedOn = screen;
    if (screen == SEARCH) tapSearch(x, y);
    else if (screen == MON) {
      if (y < 28 && x < 140) goHome();
      else if (uint16_t id = chipAt(x, y)) {
        if (id != cur) show(id);
      } else if (y >= 28 && y < 148 && x < CARD_X) {
        holdNav = true;
        nextMs = now + 450;
        steps = 0;
        show(x < 64 ? (cur > 1 ? cur - 1 : count) : (cur < count ? cur + 1 : 1));
      }
    }
    return;
  }
  if (!holdNav || screen != MON || (int32_t)(now - nextMs) < 0) return;
  nextMs = now + 110;  // held: repeat, jumping by 10 after a while
  int step = ++steps > 15 ? 10 : 1;
  int id = cur + (lx < 64 ? -step : step);
  if (id < 1) id += count;
  if (id > count) id -= count;
  show(id);
}
