"""Assemble the player zip from a finished build (CI runs this; it works locally too).

    python tools/package.py [--build build/ninja] [--out dist/grimdark.zip] [--pdb-out dist/pdb]

Layout inside the zip (one top-level folder, so unzipping anywhere gives a self-contained mod folder):

    grimdark/
      gdlaunch.exe (the player runs this), grimdark.dll, prism.dll, gdinject.exe (dev: inject into a running game)
      assets/          audio/... from the repo + expansion/base room databases built by CMake from data/rooms
                       (the DLL loads these from next to itself)
      README.md, LICENSE, THIRD_PARTY.md
      licenses/prism/  prism's NOTICE + LICENSES (MPL-2.0 attribution for the redistributed prism.dll)

The PDB is NOT in the zip; --pdb-out copies it beside it (the crash log records module+offset, so each released
build's grimdark.pdb must be kept to symbolize a tester's log). No Python dependencies beyond the stdlib.
"""
import argparse, os, shutil, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRISM = os.path.join(ROOT, "third_party", "prism-bin", "prism-sdk-v0.18.1")

def git_describe():
    import subprocess
    try:
        return subprocess.check_output(["git", "describe", "--tags", "--always"], cwd=ROOT, text=True).strip()
    except Exception:
        return "unknown"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=os.path.join(ROOT, "build", "ninja"))
    ap.add_argument("--out", default=os.path.join(ROOT, "dist", "grimdark.zip"))
    ap.add_argument("--pdb-out", default=None, help="directory to copy grimdark.pdb into (default: next to --out)")
    ap.add_argument("--version", default=None, help="written to grimdark/version.txt (the installer compares it to release tags); default: git describe")
    a = ap.parse_args()
    version = a.version or git_describe()

    files = []   # (zip path, source path)
    def add(zpath, src):
        if not os.path.exists(src): sys.exit(f"package: missing {src}")
        files.append((zpath, src))
    def add_tree(zdir, srcdir):
        if not os.path.isdir(srcdir): sys.exit(f"package: missing directory {srcdir}")
        for dp, _, fns in os.walk(srcdir):
            for fn in sorted(fns):
                src = os.path.join(dp, fn)
                add(zdir + "/" + os.path.relpath(src, srcdir).replace(os.sep, "/"), src)

    for name in ("gdlaunch.exe", "grimdark.dll", "prism.dll", "gdinject.exe"):
        add("grimdark/" + name, os.path.join(a.build, name))
    add_tree("grimdark/assets", os.path.join(ROOT, "assets"))   # the repo copy, not the build's mirror of it
    for name in ("rooms_gdx3.db", "rooms.db", "rooms_base.db"):   # built from data/rooms by CMake
        add("grimdark/assets/" + name, os.path.join(a.build, "assets", name))
    add("grimdark/README.md", os.path.join(ROOT, "README.md"))
    add("grimdark/LICENSE", os.path.join(ROOT, "LICENSE"))
    add("grimdark/THIRD_PARTY.md", os.path.join(ROOT, "third_party", "README.md"))
    add("grimdark/licenses/prism/NOTICE", os.path.join(PRISM, "NOTICE"))
    add_tree("grimdark/licenses/prism/LICENSES", os.path.join(PRISM, "LICENSES"))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for zpath, src in files: z.write(src, zpath)
        z.writestr("grimdark/version.txt", version + "\n")
        files.append(("grimdark/version.txt", "(generated)"))
    pdb = os.path.join(a.build, "grimdark.pdb")
    pdb_out = a.pdb_out or os.path.dirname(os.path.abspath(a.out))
    if os.path.exists(pdb):
        os.makedirs(pdb_out, exist_ok=True)
        shutil.copy2(pdb, os.path.join(pdb_out, "grimdark.pdb"))
    else:
        print("package: no grimdark.pdb next to the build (not fatal)")
    print(f"package: {a.out} ({os.path.getsize(a.out) / 1e6:.1f} MB, {len(files)} files)")

if __name__ == "__main__":
    main()
