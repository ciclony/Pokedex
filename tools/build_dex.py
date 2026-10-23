"""Builds dex.bin (all Pokemon species from PokeAPI) for the Pokedex firmware.

Usage: python tools/build_dex.py [out=dex.bin]
API responses and artwork are cached in tools/cache/, so re-runs don't hit PokeAPI
again (PokeAPI fair-use policy asks clients to cache).

dex.bin layout (little endian):
  "PDX3", u16 count, u16 S (max sprite side), u32 names_offset, u32 offset[count]
  entry: u16 id, u8 type1, u8 type2 (255 = none), u16 height_dm, u16 weight_hg,
         u8 stats[6] (hp atk def spa spd spe), u8 n + u16 evolution_chain[n] (species ids),
         u8 len + name, u8 len + genus, u8 len + abilities ("\n"-separated),
         u16 len + flavor text, sprite (S box), thumbnail (THUMB box)
  sprite: u8 w, u8 h, w*h RGB565 pixels (KEY = transparent), cropped to the artwork
  names: every name NUL-terminated, in id order (loaded to RAM for search)
Text is ASCII: the firmware fonts have no accents or gender symbols.
"""
import io, json, os, struct, sys, threading, time, unicodedata, urllib.request
from concurrent.futures import ThreadPoolExecutor
from PIL import Image

API = "https://pokeapi.co/api/v2/"
S = 112      # official artwork on the Pokemon page
THUMB = 52  # and on the home grid cards
KEY = 0xF81F  # magenta: transparent pixel
TYPES = ["normal", "fighting", "flying", "poison", "ground", "rock", "bug", "ghost", "steel",
         "fire", "water", "grass", "electric", "psychic", "ice", "dragon", "dark", "fairy"]
LOCKS = {}
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache")


def get(url, raw=False):
    path = os.path.join(CACHE, url.split("://", 1)[1].replace("/", "_").replace("?", "_"))
    with LOCKS.setdefault(path, threading.Lock()):  # chains are shared by several species
        if not os.path.exists(path):
            download(url, path)
    with open(path, "rb") as f:
        data = f.read()
    return data if raw else json.loads(data)


def download(url, path):
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


def ascii_text(s):
    for a, b in {"\u2019": "'", "\u2018": "'", "\u201c": '"', "\u201d": '"', "\u2014": " - ",
                 "\u2013": "-", "\u2026": "..."}.items():
        s = s.replace(a, b)
    s = s.replace("\u2640", " F").replace("\u2642", " M").replace("\u00e9", "e").replace("\u00c9", "E")
    s = " ".join(s.replace("\u00ad", "").replace("\f", " ").replace("\n", " ").split())
    return unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()


def en(entries, key):
    vals = [e[key] for e in entries if e["language"]["name"] == "en"]
    return ascii_text(vals[-1]) if vals else ""  # last = most recent game


def sprite(p, box):
    # official artwork, like the prototype; the game sprite only if a species has none
    url = p["sprites"]["other"]["official-artwork"]["front_default"] or p["sprites"]["front_default"]
    if not url:
        return bytes([1, 1]) + struct.pack("<H", KEY)
    img = Image.open(io.BytesIO(get(url, raw=True))).convert("RGBA")
    img = img.crop(img.getbbox() or (0, 0, 1, 1))
    k = box / max(img.size)
    img = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
    out = bytearray([img.width, img.height])
    px = img.tobytes()
    for r, g, b, a in zip(px[0::4], px[1::4], px[2::4], px[3::4]):
        c = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
        out += struct.pack("<H", KEY if a < 128 else KEY - 1 if c == KEY else c)
    return bytes(out)


def chain_ids(node):
    yield int(node["species"]["url"].rstrip("/").split("/")[-1])
    for child in node["evolves_to"]:
        yield from chain_ids(child)


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
    evo = list(chain_ids(get(sp["evolution_chain"]["url"])["chain"])) if sp["evolution_chain"] else [i]
    rec += bytes([len(evo)]) + struct.pack(f"<{len(evo)}H", *evo)
    abil = "\n".join(a["ability"]["name"].replace("-", " ").title()
                     for a in sorted(p["abilities"], key=lambda a: a["slot"]) if not a["is_hidden"])
    rec += bytes([len(name)]) + name.encode() + bytes([len(genus)]) + genus.encode()
    rec += bytes([len(abil)]) + abil.encode()
    rec += struct.pack("<H", len(flavor)) + flavor + sprite(p, S) + sprite(p, THUMB)
    print(f"#{i:04d} {name}", flush=True)
    return name, rec


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "dex.bin"
    os.makedirs(CACHE, exist_ok=True)
    count = int(urllib.request.urlopen(urllib.request.Request(
        API + "pokemon-species/?limit=1", headers={"User-Agent": "cyd-pokedex-builder"})).read()
        .decode().split('"count":')[1].split(",")[0])
    with ThreadPoolExecutor(8) as ex:
        names, recs = zip(*ex.map(entry, range(1, count + 1)))
    off = 12 + 4 * count
    table = b""
    for r in recs:
        table += struct.pack("<I", off)
        off += len(r)
    data = b"PDX3" + struct.pack("<HHI", count, S, off) + table + b"".join(recs)
    data += b"".join(n.encode() + b"\0" for n in names)
    with open(out, "wb") as f:
        f.write(data)
    # self-check: every offset points at the entry with the matching id
    for k in range(count):
        o = struct.unpack_from("<I", data, 12 + 4 * k)[0]
        assert struct.unpack_from("<H", data, o)[0] == k + 1
    assert data[off:].split(b"\0")[:count] == [n.encode() for n in names]
    print(f"{out}: {count} Pokemon, {len(data) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
