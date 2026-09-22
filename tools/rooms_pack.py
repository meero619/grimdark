"""The rooms data in git-friendly form <-> the SQLite db the mod reads (docs/rooms.md "Data layout").

The source of truth is data/rooms/<world>/ (gdx3 = Fangs of Asterkarn, gdx2 = Forgotten Gods, base = base game):

    meta.json                 the meta table + this format's version
    regions/<region>.jsonl    one JSON object per line: {"region": row}, then {"subregion": row}..., {"room": row}...,
                              {"exit": row}..., {"shot": row}... -- sorted, so a title edit is a one-line diff
    grids/<region>.bin        the label / height / overlay RLE blobs, zlib-compressed (a small JSON header first)
    misc.jsonl                rows of the small tables and any shot whose room has no region file (only if non-empty)

The db is a build product: CMake runs `build` into build/ninja/assets/ (the DLL loads it from next to itself) and the
authoring tools work on an unpacked copy in build/rooms/ (`unpack`), which `pack` writes back. Stdlib only, so the
build needs no uv / numpy (gdmap.roomsdb imports numpy lazily).

    uv run tools/rooms_pack.py pack   [--db build/rooms/rooms.db] [--out data/rooms/gdx2] [--force]
    uv run tools/rooms_pack.py unpack [--dir data/rooms/gdx2] [--out build/rooms/rooms.db] [--force]
    uv run tools/rooms_pack.py verify [--db ...] [--dir ...]        exact table-by-table comparison
    python tools/rooms_pack.py build  [--out build/ninja/assets]    all worlds (what CMake runs)
    uv run tools/rooms_pack.py status                                is each working db ahead of / behind its text?
"""
from __future__ import annotations
import argparse
import json
import os
import sqlite3
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from gdmap.roomsdb import SCHEMA  # noqa: E402  (stdlib import: numpy is lazy there)

FORMAT = 1
WORLDS = {"gdx3": "rooms_gdx3.db", "gdx2": "rooms.db", "base": "rooms_base.db"}
DATA = os.path.join(ROOT, "data", "rooms")
WORK = os.path.join(ROOT, "build", "rooms")
GRID_MAGIC = b"GDROOMS1"
GRID_BLOBS = ("labels", "heights", "overlays")
# Per-region line kinds, in file order, with the sort key that makes the output deterministic.
KINDS = [
    ("subregion", "subregions", "region_key", lambda r: (r["key"],)),
    ("room", "rooms", "region_key", lambda r: (r["key"],)),
    ("exit", "exits", "region_key", lambda r: (r["room_a"], r["room_b"] or "", r["x"] or 0, r["z"] or 0, r["width"] or 0, r["cut"] or 0)),
]
MISC = [("terrain_type", "terrain_types"), ("coverage", "coverage")]


def columns(c: sqlite3.Connection, table: str) -> list[str]:
    return [r[1] for r in c.execute(f"PRAGMA table_info({table})")]


def rows_of(c: sqlite3.Connection, table: str) -> list[dict]:
    cols = columns(c, table)
    return [dict(zip(cols, r)) for r in c.execute(f"SELECT {', '.join(cols)} FROM {table}")]


def dumps(obj) -> str:
    return json.dumps(obj, ensure_ascii=False, sort_keys=False, separators=(", ", ": "))


def write_text(path: str, lines: list[str]) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:   # LF always, so the files diff the same on every platform
        f.write(("\n".join(lines) + "\n").encode("utf-8"))


def read_lines(path: str) -> list[dict]:
    with open(path, "rb") as f:
        return [json.loads(line) for line in f.read().decode("utf-8").splitlines() if line.strip()]


def newest_mtime(d: str) -> float:
    m = 0.0
    for dp, _, fns in os.walk(d):
        for fn in fns:
            m = max(m, os.path.getmtime(os.path.join(dp, fn)))
    return m


# ---- pack: db -> text ----
def pack(db_path: str, out: str) -> None:
    c = sqlite3.connect(db_path)
    regions = sorted(rows_of(c, "regions"), key=lambda r: r["key"])
    region_keys = {r["key"] for r in regions}
    by_region: dict[str, list[str]] = {r["key"]: [dumps({"region": r})] for r in regions}
    misc: list[str] = []
    for kind, table, fk, sort in KINDS:
        rows = sorted(rows_of(c, table), key=sort)
        for r in rows:
            by_region[r[fk]].append(dumps({kind: r}))
    shots = sorted(rows_of(c, "shots"), key=lambda r: (r["room_key"], r["path"] or "", r["sha"] or ""))
    for r in shots:
        reg = r["room_key"].split(":", 1)[0]
        (by_region[reg] if reg in region_keys else misc).append(dumps({"shot": r}))
    for kind, table in MISC:
        for r in rows_of(c, table):
            misc.append(dumps({kind: r}))
    # Write, then remove files of regions that no longer exist (a deleted region must not linger in git).
    reg_dir, grid_dir = os.path.join(out, "regions"), os.path.join(out, "grids")
    os.makedirs(reg_dir, exist_ok=True); os.makedirs(grid_dir, exist_ok=True)
    for key, lines in by_region.items():
        write_text(os.path.join(reg_dir, key + ".jsonl"), lines)
    for g in rows_of(c, "grids"):
        header = {k: g[k] for k in ("region_key", "x0", "z0", "w", "h", "cell", "label_keys")}
        header["blobs"] = {}
        streams = []
        for name in GRID_BLOBS:
            blob = g[name]
            if blob is None:
                header["blobs"][name] = -1
            else:
                z = zlib.compress(bytes(blob), 9)
                header["blobs"][name] = len(z)
                streams.append(z)
        hb = dumps(header).encode("utf-8")
        with open(os.path.join(grid_dir, g["region_key"] + ".bin"), "wb") as f:
            f.write(GRID_MAGIC + struct.pack("<I", len(hb)) + hb + b"".join(streams))
    for d, ext in ((reg_dir, ".jsonl"), (grid_dir, ".bin")):
        for fn in os.listdir(d):
            if fn.endswith(ext) and fn[: -len(ext)] not in region_keys:
                os.remove(os.path.join(d, fn))
    meta = {"format": FORMAT, "meta": sorted(rows_of(c, "meta"), key=lambda r: r["key"])}
    write_text(os.path.join(out, "meta.json"), [json.dumps(meta, indent=1, ensure_ascii=False)])
    misc_path = os.path.join(out, "misc.jsonl")
    if misc:
        write_text(misc_path, misc)
    elif os.path.exists(misc_path):
        os.remove(misc_path)
    print(f"packed {db_path} -> {out}: {len(regions)} regions, {sum(len(v) for v in by_region.values())} lines")


# ---- unpack: text -> db ----
def unpack(src: str, out_db: str) -> None:
    meta = json.load(open(os.path.join(src, "meta.json"), encoding="utf-8"))
    if meta.get("format") != FORMAT:
        sys.exit(f"rooms_pack: {src} is format {meta.get('format')}, this tool reads {FORMAT}")
    tmp = out_db + ".tmp"
    if os.path.exists(tmp):
        os.remove(tmp)
    os.makedirs(os.path.dirname(os.path.abspath(out_db)), exist_ok=True)
    c = sqlite3.connect(tmp)
    c.executescript(SCHEMA)
    c.execute("PRAGMA journal_mode=OFF"); c.execute("PRAGMA synchronous=OFF")
    tables = {"region": "regions", "shot": "shots"} | {kind: table for kind, table, _, _ in KINDS} | dict(MISC)
    cols = {t: columns(c, t) for t in set(tables.values()) | {"meta", "grids"}}

    def insert(table: str, r: dict) -> None:
        cs = cols[table]
        for extra in [k for k in r if k not in cs]:   # a column the tools added after this SCHEMA (rooms.area_name once was)
            c.execute(f"ALTER TABLE {table} ADD COLUMN {extra}")
            cs.append(extra)
        c.execute(f"INSERT INTO {table}({', '.join(cs)}) VALUES({', '.join('?' * len(cs))})", [r.get(k) for k in cs])

    for r in meta["meta"]:
        insert("meta", r)
    reg_dir = os.path.join(src, "regions")
    n_lines = 0
    for fn in sorted(os.listdir(reg_dir)):
        if not fn.endswith(".jsonl"):
            continue
        for obj in read_lines(os.path.join(reg_dir, fn)):
            (kind, r), = obj.items()
            insert(tables[kind], r)
            n_lines += 1
    misc_path = os.path.join(src, "misc.jsonl")
    if os.path.exists(misc_path):
        for obj in read_lines(misc_path):
            (kind, r), = obj.items()
            insert(tables[kind], r)
    grid_dir = os.path.join(src, "grids")
    for fn in sorted(os.listdir(grid_dir)):
        if not fn.endswith(".bin"):
            continue
        with open(os.path.join(grid_dir, fn), "rb") as f:
            data = f.read()
        if data[:8] != GRID_MAGIC:
            sys.exit(f"rooms_pack: {fn} is not a grid file")
        (hl,) = struct.unpack("<I", data[8:12])
        header = json.loads(data[12:12 + hl].decode("utf-8"))
        pos = 12 + hl
        row = {k: header[k] for k in ("region_key", "x0", "z0", "w", "h", "cell", "label_keys")}
        for name in GRID_BLOBS:
            n = header["blobs"][name]
            if n < 0:
                row[name] = None
            else:
                row[name] = zlib.decompress(data[pos:pos + n]); pos += n
        insert("grids", row)
    c.commit(); c.close()
    try:
        os.replace(tmp, out_db)
    except PermissionError:
        # The running game holds build/ninja/assets/rooms.db open (the dev loop builds while it runs). SQLite opens
        # with write sharing, so overwriting the bytes in place is allowed where a rename / delete is not -- the same
        # thing the old CMake copy_directory step did every build.
        import shutil
        shutil.copyfile(tmp, out_db)
        os.remove(tmp)
    print(f"unpacked {src} -> {out_db}: {n_lines} lines")


# ---- verify: every table equal as a multiset of typed rows ----
def snapshot(db_path: str) -> dict[str, list]:
    c = sqlite3.connect(db_path)
    out = {}
    for (t,) in c.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"):
        cs = columns(c, t)
        rows = [tuple((type(v).__name__, bytes(v) if isinstance(v, memoryview) else v) for v in r)
                for r in c.execute(f"SELECT {', '.join(cs)} FROM {t}")]
        out[t] = (cs, sorted(rows, key=repr))
    c.close()   # Windows cannot delete an open db
    return out


def verify(db_path: str, src: str) -> bool:
    tmp = os.path.join(WORK, "_verify.db")
    unpack(src, tmp)
    a, b = snapshot(db_path), snapshot(tmp)
    os.remove(tmp)
    ok = True
    for t in sorted(set(a) | set(b)):
        if t not in a or t not in b:
            print(f"  table {t} only in {'db' if t in a else 'text'}"); ok = False; continue
        if a[t][0] != b[t][0]:
            print(f"  table {t}: columns differ {a[t][0]} vs {b[t][0]}"); ok = False; continue
        if a[t][1] != b[t][1]:
            sa, sb = set(a[t][1]), set(b[t][1])
            print(f"  table {t}: {len(a[t][1])} vs {len(b[t][1])} rows; only in db {len(sa - sb)}, only in text {len(sb - sa)}")
            for r in list(sa - sb)[:2]: print("    db  :", r[:6])
            for r in list(sb - sa)[:2]: print("    text:", r[:6])
            ok = False
    print("verify:", "identical" if ok else "DIFFERENT")
    return ok


def world_paths(world: str) -> tuple[str, str]:
    return os.path.join(DATA, world), os.path.join(WORK, WORLDS[world])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for cmd in ("pack", "unpack", "verify"):
        s = sub.add_parser(cmd)
        s.add_argument("--world", default="gdx2", choices=sorted(WORLDS), help="picks the default --db / --dir")
        s.add_argument("--db", default=None); s.add_argument("--dir", default=None)
        s.add_argument("--force", action="store_true")
    s = sub.add_parser("build", help="unpack all worlds into an assets directory (CMake)")
    s.add_argument("--out", default=os.path.join(ROOT, "build", "ninja", "assets"))
    sub.add_parser("status")
    a = ap.parse_args()

    if a.cmd == "build":
        for world, name in WORLDS.items():
            unpack(os.path.join(DATA, world), os.path.join(a.out, name))
        return
    if a.cmd == "status":
        for world, name in WORLDS.items():
            d, db = world_paths(world)
            if not os.path.exists(db):
                print(f"{world}: no working db at {db} (unpack to create it)"); continue
            t_db, t_txt = os.path.getmtime(db), newest_mtime(d)
            print(f"{world}: working db is {'AHEAD of the text (pack it)' if t_db > t_txt else 'behind or equal to the text (unpack refreshes it)'}")
        return
    d, db = world_paths(a.world)
    d = a.dir or d; db = a.db or db
    if a.cmd == "pack":
        if os.path.isdir(d) and os.path.exists(db) and newest_mtime(d) > os.path.getmtime(db) and not a.force:
            sys.exit(f"rooms_pack: {d} changed after {db} was written; unpack first or --force to overwrite the text")
        pack(db, d)
    elif a.cmd == "unpack":
        if os.path.exists(db) and os.path.getmtime(db) > newest_mtime(d) and not a.force:
            sys.exit(f"rooms_pack: {db} is newer than {d}; pack it first or --force to discard its changes")
        unpack(d, db)
    elif a.cmd == "verify":
        sys.exit(0 if verify(db, d) else 1)


if __name__ == "__main__":
    main()
