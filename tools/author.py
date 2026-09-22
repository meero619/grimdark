"""The authoring CLI over the working rooms db (build/rooms/rooms.db, see tools/rooms_pack.py), used by the rooms authoring workflow's agents (docs/rooms.md M5) and
by hand. Everything an agent needs to read or write goes through here; agents never open the db.

  uv run tools/author.py list <region> [--status shot] [--subregion K]   rooms: key, status, cls, area, title, subregion
  uv run tools/author.py facts <room_key>         everything known about a room: meta.json facts, shots, exits,
                                                  sub-region, neighbours' titles (JSON)
  uv run tools/author.py export <region>          build/rooms/<region>_rooms.json: rooms + exits + sub-regions
                                                  (for the sub-region agent) and the db floor plan PNG path
  uv run tools/author.py subregion <region> <key> --name "The prison" [--summary "..."]
  uv run tools/author.py assign <room_key> <subregion_key>
  uv run tools/author.py describe <room_key> --title "..." --body "..."    (status -> described)
  uv run tools/author.py verify <room_key> [--fail "reason"]               (status -> verified | described)
  uv run tools/author.py status <region>          counts per status and per sub-region"""
from __future__ import annotations

import argparse
import threading
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gdmap.roomsdb import RoomsDb  # noqa: E402

DB = "build/rooms/rooms.db"   # the unpacked working copy (tools/rooms_pack.py unpack); pack after editing
SHOTS = "build/shots"


def shot_dir(room_key: str) -> str:
    return os.path.join(SHOTS, room_key.split(":", 1)[0], room_key.replace(":", "_"))


def cmd_list(db, a):
    rows = db.rooms(a.region)
    for r in rows:
        if a.status and r["status"] != a.status:
            continue
        if a.subregion and r["subregion_key"] != a.subregion:
            continue
        if getattr(a, "keys_only", False):
            print(r["key"]); continue
        print(f"{r['key']:32s} {r['status']:10s} {r['cls']:8s} {r['area']:6.0f} m2  sub={r['subregion_key'] or '-':18s} title={r['title'] or '-'}")


def build_facts(db, key):
    """Everything the describer needs about a room, as a dict (cmd_facts prints it; describe_or.py imports it)."""
    region_key = key.split(":", 1)[0]
    room = next((r for r in db.rooms(region_key) if r["key"] == key), None)
    if not room:
        return {"error": "no such room"}
    a = type("A", (), {"key": key})()   # shim so the existing shot_dir(a.key) references below keep working
    meta_path = os.path.join(shot_dir(a.key), "meta.json")
    meta = json.load(open(meta_path, encoding="utf-8")) if os.path.exists(meta_path) else {}
    region = db.c.execute("SELECT name FROM regions WHERE key=?", (region_key,)).fetchone()
    sub = db.c.execute("SELECT name, summary FROM subregions WHERE key=?", (room["subregion_key"] or "",)).fetchone()
    neighbours = []
    for ra, rb, x, z, width, cut in db.c.execute("SELECT room_a, room_b, x, z, width, cut FROM exits WHERE region_key=? AND (room_a=? OR room_b=?)",
                                                  (region_key, a.key, a.key)):
        other = rb if ra == a.key else ra
        o = db.c.execute("SELECT title, cls, subregion_key, anchor_x, anchor_z FROM rooms WHERE key=?", (other,)).fetchone()
        dx, dz = x - room["anchor_x"], z - room["anchor_z"]
        bearing = ["north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"][int(((__import__("math").degrees(__import__("math").atan2(dx, -dz)) % 360) + 22.5) // 45) % 8]
        neighbours.append({"room": other, "title": o[0] if o else None, "cls": o[1] if o else None, "subregion": o[2] if o else None,
                           "bearing": bearing, "width": width, "cap_cut": bool(cut)})
    shots = sorted(f for f in os.listdir(shot_dir(a.key)) if f.endswith("_overlay.png")) if os.path.isdir(shot_dir(a.key)) else []
    # the agents see a downscaled copy (image tokens): 1100 px wide, made on first use
    from PIL import Image
    small = []
    for f in shots:
        src = os.path.join(shot_dir(a.key), f); dst = src.replace("_overlay.png", "_small.png")
        if not os.path.exists(dst):
            im = Image.open(src); im.resize((1100, int(im.height * 1100 / im.width)), Image.LANCZOS).save(dst)
        small.append(dst)
    shots = [os.path.basename(s) for s in small]
    # Collapse the per-sample entity lists to a deduped, nearest-first list of fixtures. The describer only needs
    # WHAT is nearby (the decoration/decal record names and any NPC/monster label) to name the place -- the ids,
    # coordinates and per-sample-point split were ~60% of the facts JSON (10 KB) and pure cache_creation noise
    # (2026-08-24). Record name is the signal; the shot image carries the spatial layout. Drop the player self.
    fixtures = {}
    for s in meta.get("samples", []):
        for e in s.get("entities", []):
            if e.get("cls") == "Player":
                continue
            rec = e.get("record") or e.get("label")
            if not rec:
                continue
            d = round(e.get("dist", 0.0), 1)
            cur = fixtures.get(rec)
            if cur is None or d < cur["dist"]:
                fixtures[rec] = {"record": rec, "cls": e.get("cls"), "label": e.get("label") or None, "dist": d}
    nearby = sorted(fixtures.values(), key=lambda e: e["dist"])
    out = {"room": room, "region_name": region[0] if region else region_key, "subregion": {"key": room["subregion_key"], "name": sub[0] if sub else None, "summary": sub[1] if sub else None},
           "terrain": meta.get("terrain", {}), "nearby": nearby,
           "shots": [os.path.abspath(os.path.join(shot_dir(a.key), f)) for f in shots], "exits": neighbours}
    return out


def cmd_facts(db, a):
    print(json.dumps(build_facts(db, a.key), indent=1))


def cmd_export(db, a):
    rooms = db.rooms(a.region)
    exits = [dict(zip(("room_a", "room_b", "x", "z", "width", "cut"), row)) for row in
             db.c.execute("SELECT room_a, room_b, x, z, width, cut FROM exits WHERE region_key=?", (a.region,))]
    subs = [dict(zip(("key", "name", "summary"), row)) for row in db.c.execute("SELECT key, name, summary FROM subregions WHERE region_key=?", (a.region,))]
    # the floor plan's ids are the rooms' order by (anchor z, x) = db.rooms() order
    for i, r in enumerate(rooms):
        r["plan_id"] = i
        meta_path = os.path.join(shot_dir(r["key"]), "meta.json")
        if os.path.exists(meta_path):
            meta = json.load(open(meta_path, encoding="utf-8"))
            r["terrain"] = meta.get("terrain", {})
            labels = sorted({e["label"] for s in meta.get("samples", []) for e in s["entities"] if e.get("label")})
            r["landmarks"] = labels[:12]
    name = db.c.execute("SELECT name FROM regions WHERE key=?", (a.region,)).fetchone()
    out = {"region": a.region, "name": name[0] if name else a.region, "plan_png": os.path.abspath(f"build/rooms/{a.region}_db.png"),
           "rooms": rooms, "exits": exits, "subregions": subs}
    path = f"build/rooms/{a.region}_rooms.json"
    os.makedirs("build/rooms", exist_ok=True)
    json.dump(out, open(path, "w", encoding="utf-8"), indent=1)
    print(path, len(rooms), "rooms", len(exits), "exits", len(subs), "sub-regions")


def cmd_subregion(db, a):
    db.c.execute("INSERT INTO subregions(key, region_key, name, summary) VALUES(?,?,?,?) ON CONFLICT(key) DO UPDATE SET name=excluded.name, summary=COALESCE(excluded.summary, subregions.summary)",
                 (a.key, a.region, a.name, a.summary))
    db.c.commit(); print("ok")


def cmd_assign(db, a):
    n = db.c.execute("UPDATE rooms SET subregion_key=? WHERE key=?", (a.subregion, a.key)).rowcount
    db.c.commit(); print("ok" if n else "no such room")


_SAVE_LOCK = threading.Lock()   # describe_or.py saves from 32 worker threads: the read-then-write below must be atomic


def save_description(db, key, title, body):
    """Store a room's title/body, mechanically de-duplicating the title within its sub-region with a trailing
    " N" suffix. Returns the final title, or None if the room does not exist. Shared by the CLI and describe_or.py.
    Serialized: without the lock two parallel workers both saw the title free and wrote it twice (61 same-sub-region
    duplicates in the DLC regions, found 2026-09-20)."""
    with _SAVE_LOCK:
        return _save_description_locked(db, key, title, body)


def _save_description_locked(db, key, title, body):
    region_key = key.split(":", 1)[0]
    room = next((r for r in db.rooms(region_key) if r["key"] == key), None)
    if room is None:
        return None
    base = re.sub(r"\s+\d+$", "", title.strip().lower()).strip()
    existing = {t.lower() for (t,) in db.c.execute(
        "SELECT title FROM rooms WHERE region_key=? AND subregion_key IS ? AND key<>? AND title IS NOT NULL",
        (region_key, room["subregion_key"], key))}
    final, k = base, 2
    while final in existing:
        final = f"{base} {k}"; k += 1
    db.c.execute("UPDATE rooms SET title=?, body=?, status='described' WHERE key=?", (final, body.strip(), key))
    db.c.commit()
    return final


def strip_suffix(title: str) -> str:
    return re.sub(r"\s+\d+$", "", (title or "").strip()).strip()


def retitle_plan(db) -> list[tuple[str, str, str, str]]:
    """The mechanical title clean-up of 2026-09-20 (docs/rooms.md "Duplicate titles"): titles must be unique within a
    sub-region and the " N" suffix must mean "a twin in this sub-region". Historically the describer ran before the
    sub-region pass in most regions (a null sub-region = region-wide numbering that later straddled the painted
    sub-regions; 1410 of 1980 base-db suffixes had no base in their sub-region), and parallel workers raced the
    dedupe (same-sub-region duplicates). Per (region, sub-region, base title) group, orphans excluded:
      one room -> the bare base; several -> keep them if they already read base, base 2 .. base n; else the room
      that is bare stays bare (if exactly one) and the rest take 2.. in anchor (x, z) order -- deterministic, so the
      base and DLC dbs agree wherever their twin sets agree.
    Returns (key, old_title, new_title, why) for every room whose title changes."""
    rows = db.c.execute("SELECT key, region_key, subregion_key, title, anchor_x, anchor_z FROM rooms "
                        "WHERE title IS NOT NULL AND title<>'' AND status<>'orphan'").fetchall()
    groups: dict[tuple, list] = {}
    for key, reg, sub, title, ax, az in rows:
        groups.setdefault((reg, sub or "", strip_suffix(title).lower()), []).append((key, title, ax, az))
    changes = []
    for (reg, sub, base), members in groups.items():
        members.sort(key=lambda m: (m[2], m[3], m[0]))
        n = len(members)
        if n == 1:
            key, title, _, _ = members[0]
            if title != base: changes.append((key, title, base, "stale suffix lifted"))
            continue
        want = {base} | {f"{base} {k}" for k in range(2, n + 1)}
        have = [m[1] for m in members]
        if set(have) == want and len(set(have)) == n:
            continue   # already a clean contiguous numbering
        bare = [m for m in members if m[1] == base]
        order = ([bare[0]] + [m for m in members if m is not bare[0]]) if len(bare) == 1 else members
        why = "same-sub-region duplicates renumbered" if len(set(have)) < n else "twins renumbered"
        for i, (key, title, _, _) in enumerate(order):
            new = base if i == 0 else f"{base} {i + 1}"
            if new != title: changes.append((key, title, new, why))
    return changes


def cmd_retitle(db, a):
    changes = retitle_plan(db)
    from collections import Counter
    why = Counter(c[3] for c in changes)
    print(f"{len(changes)} title changes in {a.db}: " + ", ".join(f"{k} {v}" for k, v in why.items()))
    for key, old, new, w in changes[: a.show]:
        print(f"  {key:45s} {old!r} -> {new!r}  ({w})")
    if len(changes) > a.show: print(f"  ... {len(changes) - a.show} more (--show N)")
    if not a.write:
        print("dry run; --write applies"); return
    for key, _, new, _ in changes:
        db.c.execute("UPDATE rooms SET title=? WHERE key=?", (new, key))
    db.c.commit()
    left = db.c.execute("SELECT count(*) FROM (SELECT 1 FROM rooms WHERE title<>'' AND status<>'orphan' "
                        "GROUP BY region_key, subregion_key, title HAVING count(*)>1)").fetchone()[0]
    print(f"written; same-sub-region duplicates left: {left}")


def cmd_describe(db, a):
    print("ok" if save_description(db, a.key, a.title, a.body) else "no such room")


def cmd_verify(db, a):
    if a.fail:
        n = db.c.execute("UPDATE rooms SET status='described' WHERE key=?", (a.key,)).rowcount
        print(f"kept as described ({a.fail})" if n else "no such room")
    else:
        n = db.c.execute("UPDATE rooms SET status='verified' WHERE key=? AND title IS NOT NULL", (a.key,)).rowcount
        print("ok" if n else "no such room or no title")
    db.c.commit()


BANNED = ["exit", "way out", "ways out", "leads ", "lead north", "lead south", "lead east", "lead west", "carries on", "carry on",
          "runs on to", "runs north", "runs south", "runs east", "runs west", "opens north", "opens south", "opens east", "opens west",
          "to the north", "to the south", "to the east", "to the west", "northward", "southward", "eastward", "westward",
          "locked", "looted", "alive", "spawn", "quest", "you are", "this room"]   # "dead" accepted for now (fix later)
ADJ_SOUP = ["weathered", "splintered", "mist-filled", "forcing up", "hemmed in", "threads between", "scribed", "shoulders the way",
            "beaten down", "trodden down", "crowding", "scattered papers", "scatter of"]


def check_room(db, key: str) -> list[str]:
    region_key = key.split(":", 1)[0]
    room = next((r for r in db.rooms(region_key) if r["key"] == key), None)
    if not room:
        return ["no such room"]
    title, body = (room["title"] or "").strip(), (room["body"] or "").strip()
    problems = []
    core_title = re.sub(r"\s+\d+$", "", title).strip()   # ignore the mechanical " N" de-dup suffix in these checks
    words = core_title.split()
    if not (2 <= len(words) <= 6): problems.append(f"title must be 2-6 words (has {len(words)})")
    if title != title.lower(): problems.append("title must be lowercase")
    if any(ch.isdigit() for ch in core_title): problems.append("title contains digits")
    sub_name = (db.c.execute("SELECT name FROM subregions WHERE key=?", (room["subregion_key"] or "",)).fetchone() or [""])[0] or ""
    region_name = (db.c.execute("SELECT name FROM regions WHERE key=?", (region_key,)).fetchone() or [""])[0] or ""
    for n in (sub_name, region_name):
        core = n.lower().removeprefix("the ").strip()
        if core and core in core_title.lower(): problems.append(f"title repeats '{n}'")
    # duplicate titles are now de-duplicated mechanically at describe time (a " N" suffix), so no dup check here
    sentences = [s for s in __import__("re").split(r"[.!?]+", body) if s.strip()]
    if not (1 <= len(sentences) <= 2): problems.append(f"body must be 1-2 sentences (has {len(sentences)})")
    if len(body.split()) > 40: problems.append(f"body over 40 words ({len(body.split())})")
    low = " " + body.lower() + " "
    for b in BANNED:
        if b in low: problems.append(f"banned phrase '{b.strip()}'")
    for b in ADJ_SOUP:
        if b in low: problems.append(f"thesaurus phrase '{b}'")
    meta_path = os.path.join(shot_dir(key), "meta.json")
    if os.path.exists(meta_path):
        meta = json.load(open(meta_path, encoding="utf-8"))
        names = {e["label"] for s in meta.get("samples", []) for e in s["entities"] if e.get("label") and e["cls"] in ("Npc", "Monster", "Player")}
        for n in names:
            if n and n.lower() in low: problems.append(f"names a unit '{n}'")
    for (other,) in db.c.execute("SELECT CASE WHEN room_a=? THEN room_b ELSE room_a END FROM exits WHERE region_key=? AND (room_a=? OR room_b=?)", (key, region_key, key, key)):
        t = (db.c.execute("SELECT title FROM rooms WHERE key=?", (other,)).fetchone() or [None])[0]
        if t and t.lower() in low: problems.append(f"mentions neighbour '{t}'")
    return problems


def cmd_check(db, a):
    problems = check_room(db, a.key)
    if problems:
        db.c.execute("UPDATE rooms SET status='described' WHERE key=? AND title IS NOT NULL", (a.key,))
        print("FAIL: " + "; ".join(problems))
    else:
        db.c.execute("UPDATE rooms SET status='verified' WHERE key=? AND title IS NOT NULL", (a.key,))
        print("ok")
    db.c.commit()


def cmd_status(db, a):
    print(dict(db.c.execute("SELECT status, COUNT(*) FROM rooms WHERE region_key=? GROUP BY status", (a.region,)).fetchall()))
    for key, name, n in db.c.execute("SELECT s.key, s.name, COUNT(r.key) FROM subregions s LEFT JOIN rooms r ON r.subregion_key=s.key WHERE s.region_key=? GROUP BY s.key", (a.region,)):
        print(f"  {key:24s} {name:30s} {n} rooms")
    un = db.c.execute("SELECT COUNT(*) FROM rooms WHERE region_key=? AND subregion_key IS NULL", (a.region,)).fetchone()[0]
    print("  unassigned rooms:", un)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DB, help="rooms database (rooms_gdx3.db = Fangs, rooms.db = Forgotten Gods, rooms_base.db = base)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("list"); s.add_argument("region"); s.add_argument("--status"); s.add_argument("--subregion"); s.add_argument("--keys-only", action="store_true"); s.set_defaults(fn=cmd_list)
    s = sub.add_parser("facts"); s.add_argument("key"); s.set_defaults(fn=cmd_facts)
    s = sub.add_parser("export"); s.add_argument("region"); s.set_defaults(fn=cmd_export)
    s = sub.add_parser("subregion"); s.add_argument("region"); s.add_argument("key"); s.add_argument("--name", required=True); s.add_argument("--summary"); s.set_defaults(fn=cmd_subregion)
    s = sub.add_parser("assign"); s.add_argument("key"); s.add_argument("subregion"); s.set_defaults(fn=cmd_assign)
    s = sub.add_parser("describe"); s.add_argument("key"); s.add_argument("--title", required=True); s.add_argument("--body", required=True); s.set_defaults(fn=cmd_describe)
    s = sub.add_parser("verify"); s.add_argument("key"); s.add_argument("--fail"); s.set_defaults(fn=cmd_verify)
    s = sub.add_parser("status"); s.add_argument("region"); s.set_defaults(fn=cmd_status)
    s = sub.add_parser("check"); s.add_argument("key"); s.set_defaults(fn=cmd_check)
    s = sub.add_parser("retitle", help="lift stale ' N' suffixes and renumber true same-sub-region twins (dry run without --write)")
    s.add_argument("--write", action="store_true"); s.add_argument("--show", type=int, default=40); s.set_defaults(fn=cmd_retitle)
    a = ap.parse_args()
    a.fn(RoomsDb(a.db), a)


if __name__ == "__main__":
    main()
