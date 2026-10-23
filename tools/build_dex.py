"""Builds dex.bin (all Pokemon species from PokeAPI) for the Pokedex firmware.

Usage: python tools/build_dex.py [out=dex.bin]
API responses and artwork are cached in tools/cache/, so re-runs don't hit PokeAPI
again (PokeAPI fair-use policy asks clients to cache).

dex.bin layout (little endian):
  "PDX1", u16 count, u16 S (sprite side), u32 offset[count]
  entry: u16 id, u8 type1, u8 type2 (255 = none), u16 height_dm, u16 weight_hg,
         u8 stats[6] (hp atk def spa spd spe), u8 len + name, u8 len + genus,
         u16 len + flavor text, S*S RGB565 pixels (on white)
Text is ASCII: the firmware fonts have no accents or gender symbols.
"""
import io, json, os, struct, sys, time, unicodedata, urllib.request
from concurrent.futures import ThreadPoolExecutor
from PIL import Image

API = "https://pokeapi.co/api/v2/"
S = 120
TYPES = ["normal", "fighting", "flying", "poison", "ground", "rock", "bug", "ghost", "steel",
         "fire", "water", "grass", "electric", "psychic", "ice", "dragon", "dark", "fairy"]
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache")


def get(url, raw=False):
    path = os.path.join(CACHE, url.split("://", 1)[1].replace("/", "_").replace("?", "_"))
    if not os.path.exists(path):
        for attempt in range(5):
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "cyd-pokedex-builder"})
                data = urllib.request.urlopen(req, timeout=60).read()
                break
            except Exception:
                if attempt == 4:
                    raise
                time.sleep(2 ** attempt)
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)  # a killed run never leaves a half file in the cache
    with open(path, "rb") as f:
        data = f.read()
    return data if raw else json.loads(data)


def ascii_text(s):
    for a, b in {"’": "'", "‘": "'", "“": '"', "”": '"', "—": " - ",
                 "–": "-", "…": "..."}.items():
        s = s.replace(a, b)
    s = s.replace("♀", " F").replace("♂", " M").replace("é", "e").replace("É", "E")
    s = " ".join(s.replace("­", "").replace("\f", " ").replace("\n", " ").split())
    return unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()


def en(entries, key):
    vals = [e[key] for e in entries if e["language"]["name"] == "en"]
    return ascii_text(vals[-1]) if vals else ""  # last = most recent game


def sprite(p):
    other = p["sprites"]["other"]["official-artwork"]["front_default"]
    url = other or p["sprites"]["front_default"]
    canvas = Image.new("RGB", (S, S), "white")
    if url:
        img = Image.open(io.BytesIO(get(url, raw=True))).convert("RGBA")
        img = img.crop(img.getbbox() or (0, 0, img.width, img.height))
        k = (S - 4) / max(img.size)
        img = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
        canvas.paste(img, ((S - img.width) // 2, (S - img.height) // 2), img)
    out = bytearray()
    for r, g, b in canvas.getdata():
        out += struct.pack("<H", (r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
    return bytes(out)


def entry(i):
    sp = get(f"{API}pokemon-species/{i}/")
    p = get(next(v for v in sp["varieties"] if v["is_default"])["pokemon"]["url"])
    t = [TYPES.index(x["type"]["name"]) for x in sorted(p["types"], key=lambda x: x["slot"])]
    stats = {s["stat"]["name"]: min(255, s["base_stat"]) for s in p["stats"]}
    name = en(sp["names"], "name") or sp["name"].title()
    genus = en(sp["genera"], "genus")
    flavor = en(sp["flavor_text_entries"], "flavor_text").encode()[:1000]
    rec = struct.pack("<HBBHH6B", i, t[0], t[1] if len(t) > 1 else 255, p["height"], p["weight"],
                      *(stats[k] for k in ("hp", "attack", "defense", "special-attack",
                                           "special-defense", "speed")))
    rec += bytes([len(name)]) + name.encode() + bytes([len(genus)]) + genus.encode()
    rec += struct.pack("<H", len(flavor)) + flavor + sprite(p)
    print(f"#{i:04d} {name}", flush=True)
    return rec


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "dex.bin"
    os.makedirs(CACHE, exist_ok=True)
    count = int(urllib.request.urlopen(urllib.request.Request(
        API + "pokemon-species/?limit=1", headers={"User-Agent": "cyd-pokedex-builder"})).read()
        .decode().split('"count":')[1].split(",")[0])
    with ThreadPoolExecutor(8) as ex:
        recs = list(ex.map(entry, range(1, count + 1)))
    off = 8 + 4 * count
    table = b""
    for r in recs:
        table += struct.pack("<I", off)
        off += len(r)
    data = b"PDX1" + struct.pack("<HH", count, S) + table + b"".join(recs)
    with open(out, "wb") as f:
        f.write(data)
    # self-check: every offset points at the entry with the matching id
    for k in range(count):
        o = struct.unpack_from("<I", data, 8 + 4 * k)[0]
        assert struct.unpack_from("<H", data, o)[0] == k + 1
    print(f"{out}: {count} Pokemon, {len(data) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
