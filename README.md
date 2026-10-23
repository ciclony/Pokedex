# Pokedex for the ESP32-2432S028 (CYD)

Standalone Pokedex firmware for the "Cheap Yellow Display" (two-USB / ST7789 version).
Every Pokemon from [PokeAPI](https://pokeapi.co/) (1025 species) lives in one file,
`dex.bin`, on the microSD card. No Wi-Fi needed.

Each entry shows number, name, category, types, height, weight, base stats,
the latest English Pokedex description and the official artwork.

**Controls:** tap the right half of the screen for the next Pokemon, the left half
for the previous one. Hold to scroll; after a couple of seconds it jumps 10 at a time.
The last Pokemon viewed is remembered across reboots.

## 1. Build dex.bin

```
pip install pillow pyserial
python tools/build_dex.py dex.bin
```

Downloads ~300 MB from PokeAPI the first time (cached in `tools/cache/`, so re-runs
are offline) and writes `dex.bin` (~30 MB).

## 2. Flash the firmware

Arduino core esp32 3.x, libraries **GFX Library for Arduino** and **U8g2**.

```
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload --fqbn esp32:esp32:esp32 -p COM3 .
```

If the upload fails with "Wrong boot mode", hold BOOT, tap RST, release BOOT and
upload with `--before no-reset`.

For the single micro-USB board (ILI9341), comment out `#define CYD_ST7789` in
`pin_config.h`.

## 3. Put dex.bin on the card

Either copy `dex.bin` to the root of a FAT32 microSD with a card reader, or send
it over USB with the firmware running (~5 min):

```
python tools/send_dex.py COM3 dex.bin
```

The card is only replaced once the whole file arrived.

## dex.bin format

See the docstring in `tools/build_dex.py`.

Data from PokeAPI. Pokemon and artwork are (c) Nintendo / Game Freak / The Pokemon Company;
this is a non-commercial fan project.
