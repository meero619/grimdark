"""Read Grim Dawn's database.arz offline (format 2 version 3: LZ4 records, shared string table) and print the
records whose path matches a regex, optionally only the fields whose name matches another regex.
Usage: uv run --with lz4 tools/arz.py <record-path-regex> [field-regex] [--max N]
  e.g. uv run --with lz4 tools/arz.py "records/skills/playerclass01/.*\\.dbr$" "range|radius|distance"
Format per reference/GDCommunityLauncher/extractor (ARZExtractor.cpp).

Expansion overlay (2026-09-14): `load()` reads every installed database (database.arz, then GDX1.arz, GDX2.arz --
tools/gdmap/gamefiles.py) and a later record overrides an earlier one by path, like the game. The returned
`d`/`strings` are a `Layered` view so `decode(d, strings, off, csz, dsz)` keeps working unchanged; set `P` to
one file's path to read that file alone."""
import os, re, struct, sys, lz4.block
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
DEFAULT = r"C:\Program Files (x86)\Steam\steamapps\common\Grim Dawn\database\database.arz"
P = DEFAULT


class Layered:
    """Several arz files as one address space: record offsets are shifted by the file's base, slicing and the
    string table resolve through the file the offset falls in."""
    def __init__(self):
        self.parts = []   # (base, d, strings)
        self.total = 0

    def add(self, d, strings):
        base = self.total
        self.parts.append((base, d, strings))
        self.total += len(d)
        return base

    def _part(self, off):
        for base, d, strings in reversed(self.parts):
            if off >= base:
                return base, d, strings
        raise IndexError(off)

    def __getitem__(self, key):
        base, d, _ = self._part(key.start)
        return d[key.start - base:key.stop - base]

    def strings_at(self, off):
        return self._part(off)[2]


def _load_one(path):
    d = open(path, "rb").read()
    fmt, ver, rec_start, rec_size, rec_count, str_start, str_size = struct.unpack_from("<HHIIIII", d, 0)
    assert (fmt, ver) == (2, 3), (fmt, ver)
    strings = []
    o = str_start
    n = struct.unpack_from("<I", d, o)[0]; o += 4
    for _ in range(n):
        ln = struct.unpack_from("<I", d, o)[0]; o += 4
        strings.append(d[o:o + ln].decode("utf-8", errors="replace")); o += ln
    recs = []
    o = rec_start
    for _ in range(rec_count):
        fid = struct.unpack_from("<I", d, o)[0]; o += 4
        ln = struct.unpack_from("<I", d, o)[0]; o += 4
        rname = d[o:o + ln].decode(errors="replace"); o += ln
        off, csz, dsz = struct.unpack_from("<III", d, o); o += 12
        o += 8  # data (timestamp)
        recs.append((strings[fid], rname, off, csz, dsz))
    return d, strings, recs


def load():
    """(d, strings, recs) over the installed expansion overlay, or over `P` alone if it was changed."""
    if P != DEFAULT:
        return _load_one(P)
    from gdmap import gamefiles
    paths = gamefiles.arz_paths()
    if len(paths) == 1:
        return _load_one(paths[0])
    L = Layered()
    by_path = {}
    for p in paths:
        d, strings, recs = _load_one(p)
        base = L.add(d, strings)
        for path, rname, off, csz, dsz in recs:
            by_path[path.lower()] = (path, rname, off + base, csz, dsz)
    return L, L, list(by_path.values())


def decode(d, strings, off, csz, dsz):
    if isinstance(strings, Layered):
        strings = strings.strings_at(off)
    raw = lz4.block.decompress(d[off + 24:off + 24 + csz], uncompressed_size=dsz)
    i, out = 0, {}
    while i < len(raw):
        typ, cnt, key = struct.unpack_from("<HHI", raw, i); i += 8
        vals = []
        for _ in range(cnt):
            if typ == 1: vals.append(struct.unpack_from("<f", raw, i)[0])
            elif typ == 2: vals.append(strings[struct.unpack_from("<I", raw, i)[0]])
            elif typ == 3: vals.append(bool(struct.unpack_from("<i", raw, i)[0]))
            else: vals.append(struct.unpack_from("<i", raw, i)[0])
            i += 4
        out[strings[key]] = vals
    return out

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    mx = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 50
    path_rx = re.compile(args[0], re.I)
    field_rx = re.compile(args[1], re.I) if len(args) > 1 else None
    d, strings, recs = load()
    shown = 0
    for path, rname, off, csz, dsz in recs:
        if not path_rx.search(path): continue
        rec = decode(d, strings, off, csz, dsz)
        lines = [f"  {k} = {v if len(v) > 1 else v[0]}" for k, v in sorted(rec.items()) if not field_rx or field_rx.search(k)]
        if field_rx and not lines: continue
        print(f"=== {path} ({rec.get('templateName', ['?'])[0]})")
        for l in lines: print(l)
        shown += 1
        if shown >= mx: print(f"... (--max {mx})"); break
