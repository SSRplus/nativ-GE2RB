#!/usr/bin/env python3
"""The game's files for an iPhone/iPad, apart from the app: stage_gamedata.py <title> <out folder> -- the folder the player copies
into the app's folder in Files (runtime/platform/ios/ios_boot.c reads it): ge2rb.cfg, the memory images (game.img, one per module),
fs/app0 (the game's own files, its update layered over them) and shaders.txt when the pack ships one. Private: it holds the game.

Run after `vitarecomp recompile <title>` (the images) from the repository's Python, with the same ATIV_DUMP as the pipeline."""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "pipeline"))
import vitarecomp as V  # noqa: E402


def cfg_lines(g, shaders):
    """ge2rb.cfg: what `vitarecomp run` gives the game (play_fixed_env, RECOMP_MODULES, RECOMP_LOAD_MODULES) as KEY=VALUE, the folder as @DATA@"""
    L = ["# written by dev/ios/stage_gamedata.py: the game's files for the iOS app (runtime/platform/ios/ios_boot.c)",
         f"IMAGE={g.BASE}.img", f"ENTRY={g.entry()}", "BUDGET=100000000000"]
    fixed = V.play_fixed_env(g, "@DATA@/shaders.txt" if shaders else None, None)
    L += [f"{k}={v}" for k, v in fixed if k != "RECOMP_ICON"]
    if g.MODS: L.append("RECOMP_MODULES=" + ",".join("@DATA@/" + m + ".img" for m in g.MODS))
    if g.LMODS: L.append("RECOMP_LOAD_MODULES=" + ";".join(f"{g.LPATH[m]}=@DATA@/{m}.img" for m in g.LMODS))
    return L


def clone_tree(src, dst):
    """src copied to dst: APFS clones (cp -c: no second copy on the disk), else a plain copy; links followed"""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if sys.platform == "darwin" and subprocess.run(["cp", "-cR", src, dst]).returncode == 0: return
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst, symlinks=False)


def main(argv):
    if len(argv) != 2: print(__doc__); return 1
    T, out = argv[0], os.path.abspath(argv[1])
    g = V.Game(T, "export")
    app0 = os.path.realpath(os.path.join(g.OUT, "fs", "app0"))
    if not os.path.isdir(app0): V.die(f"{T}: no game files at {app0} (vitarecomp import {T})")
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    names = [g.BASE + ".img"] + [m + ".img" for m in g.ALL]
    for n in names:
        s = os.path.join(g.OUT, n)
        if not os.path.isfile(s): V.die(f"{T}: {s} is missing (vitarecomp recompile {T})")
        shutil.copy2(s, os.path.join(out, n))
    shaders = os.path.join(V.PACKS, g.PACK, "shaders.txt")
    if os.path.isfile(shaders): shutil.copy2(shaders, os.path.join(out, "shaders.txt"))
    for d in ("captures",): os.makedirs(os.path.join(out, d), exist_ok=True)
    clone_tree(app0, os.path.join(out, "fs", "app0"))
    with open(os.path.join(out, "ge2rb.cfg"), "w", encoding="utf-8", newline="\n") as f: f.write("\n".join(cfg_lines(g, os.path.isfile(shaders))) + "\n")
    n = sum(len(fs) for _r, _d, fs in os.walk(os.path.join(out, "fs", "app0")))
    print(f"gamedata: {out}: {len(names)} image(s), {n} game file(s), ge2rb.cfg")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
