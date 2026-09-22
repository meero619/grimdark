"""Where the game's data files are, with the expansion overlay (2026-09-14).

Each expansion ships a COMPLETE replacement of the shared files it touches, not a patch: `gdx1/resources/Levels.arc`,
`gdx2/resources/Levels.arc`, and `gdx3/resources/Levels.arc` each hold a full `world001.map` (the base chunks are
recompiled with the expansion's props and reworks, three dungeons move, the cut
Prospect Hill corner is replaced by Gloomwald), and `GDX1.arz` / `GDX2.arz` add + override database records. The
game mounts the layers base < gdx1 < gdx2 < gdx3, last wins per file / per record. Every offline tool reads through
here so it sees the same world the running game does.

`GRIMDARK_GAME_DIR` overrides the install path; `GRIMDARK_GAME_LAYERS` (comma list of base,gdx1,gdx2,gdx3) forces a
layer set, e.g. `base` to build the base-game rooms db on a full install."""
from __future__ import annotations

import os

GAME_DIR = os.environ.get("GRIMDARK_GAME_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Grim Dawn")
# (layer name, database file); the resources live in <layer>/resources
LAYERS = (("base", "database/database.arz"), ("gdx1", "gdx1/database/GDX1.arz"),
          ("gdx2", "gdx2/database/GDX2.arz"), ("gdx3", "gdx3/database/GDX3.arz"))


def layer_dir(layer: str) -> str:
    return GAME_DIR if layer == "base" else os.path.join(GAME_DIR, layer)


def installed() -> list[str]:
    """The mounted layers in overlay order (base first). An expansion counts when its Levels.arc is on disk
    (Steam removes the folder when the DLC is disabled)."""
    forced = os.environ.get("GRIMDARK_GAME_LAYERS")
    if forced:
        return [l.strip() for l in forced.split(",") if l.strip()]
    out = []
    for layer, _ in LAYERS:
        if os.path.exists(os.path.join(layer_dir(layer), "resources", "Levels.arc")):
            out.append(layer)
    return out


def map_id() -> str:
    """The world the game mounts: the highest installed layer's world001.map."""
    return installed()[-1]


def levels_arc() -> str:
    return os.path.join(layer_dir(map_id()), "resources", "Levels.arc")


def arz_paths() -> list[str]:
    """Database files in overlay order (base first; a later record overrides an earlier one by path)."""
    have = set(installed())
    return [os.path.join(GAME_DIR, f) for layer, f in LAYERS if layer in have]


def arc_paths(name: str) -> list[str]:
    """Every installed `<layer>/resources/<name>` in overlay order (some layers lack a file: gdx1 has no System.arc)."""
    out = []
    for layer in installed():
        p = os.path.join(layer_dir(layer), "resources", name)
        if os.path.exists(p):
            out.append(p)
    return out


def text_tags(name: str = "Text_EN.arc") -> dict[str, str]:
    """{tag -> localized text} over every entry of every installed Text arc, later layers overriding (the DLC
    zone names tagGDX1Rift* / tagGDX2Rift* live only in the expansion arcs)."""
    from .arc import Arc
    tags: dict[str, str] = {}
    for p in arc_paths(name):
        a = Arc(p)
        for entry in a.names():
            for line in a.read(entry).decode("utf-8-sig", errors="replace").splitlines():
                if "=" in line:
                    k, _, v = line.partition("=")
                    tags[k.strip()] = v.strip()
    return tags
