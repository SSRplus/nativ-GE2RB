#!/usr/bin/env python3
"""Adds the game's update to its auto-generated pack: ge2rb_pack.py <title> [--base <title of the same game, another region>] [--hooks]

`vitarecomp pack-init` writes packs/<title>/pack.toml from the game as it was added (app0): one version, the base game's. A game's
update (patch0) that changes the main program is another release -- vitarecomp import layers the update over the game only when the
pack lists that release by the SHA-256 of its eboot.bin.elf, and otherwise plays the game without it. So the update's release is added
as a second version, marked recommended (the one to play). An update that leaves the main program alone needs nothing.

--base <title>: the same game's pack from another region (God Eater 2 Rage Burst: PCSB00875). What does not depend on where the code sits is
taken from it -- LOAD_MODULES (the game's own modules, bin/*.self and modules_psvita/*.suprx) and TEST_FRAMES. Not RUN_ENV (widescreen / frame rates: later).
--hooks (after `vitarecomp analyze` and `hooks-scan`): the base's IHOOKS are addresses in ITS main program, so they are not copied. The same
kinds of hook (frame_dt, aspect) are looked up in THIS copy's own hook candidates (analysis/hook_candidates.json), and used only if there are
as many sites of each kind as the base has: the same code, found again. Otherwise none is set (the game plays at its own 30 fps, 16:9 picture)."""
import hashlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
for d in ("pipeline", "recompiler", "decrypt"): sys.path.insert(0, os.path.join(ROOT, d))
import bootstrap  # noqa: E402
import packconf  # noqa: E402
from vitadecrypt import sfo  # noqa: E402


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()


SEED_KEYS = ("LOAD_MODULES", "TEST_FRAMES")   # (RUN_ENV is left out: its widescreen and frame rate settings need the hooks, which come later)


def set_setting(text, key, line):
    """text with the top-level `line` (KEY = ...) in place of its KEY line, or else after the ELF line (before the first [table])"""
    rows = text.split("\n")
    for i, r in enumerate(rows):
        if r.startswith("[") : break
        if re.match(rf"{key}\s*=", r): rows[i] = line; return "\n".join(rows)
    for i, r in enumerate(rows):
        if r.startswith("[") : break
        if re.match(r"ELF\s*=", r): rows.insert(i + 1, line); return "\n".join(rows)
    raise SystemExit(f"ge2rb_pack: no place for {key} in the pack")


def base_pack_path(base):
    return os.path.join(os.environ.get("ATIV_PACKS") or os.path.join(ROOT, "packs"), base, "pack.toml")


def seed_from_base(pack, base):
    """LOAD_MODULES, RUN_ENV, TEST_FRAMES of the base pack (as written there) into pack; a line saying what was done"""
    bp = base_pack_path(base)
    if not os.path.isfile(bp): return f"no pack for {base}: nothing taken from it"
    rows = {}
    for r in open(bp, encoding="utf-8").read().split("\n"):
        if r.startswith("["): break
        for k in SEED_KEYS:
            if re.match(rf"{k}\s*=", r): rows[k] = r
    text = open(pack, encoding="utf-8").read()
    for k, r in rows.items(): text = set_setting(text, k, r)
    with open(pack, "w", encoding="utf-8", newline="\n") as f: f.write(text)
    packconf.read(pack)
    return f"took from {base}: " + ", ".join(rows) if rows else f"{base} has none of {', '.join(SEED_KEYS)}"


def hooks_from_base(pack, base, cand_json):
    """IHOOKS of this copy: the kinds the base's IHOOKS use, at this copy's own candidate sites, when the counts match; a line saying what was done"""
    bs = packconf.read(base_pack_path(base))[0]
    want = {}
    for w in bs.get("IHOOKS", "").split():
        a, _, k = w.partition(":"); want.setdefault(k, set()).add(a)
    if not want: return f"{base} has no IHOOKS: none set"
    if not os.path.isfile(cand_json): return f"no {cand_json} (vitarecomp hooks-scan first): none set"
    cands = json.load(open(cand_json, encoding="utf-8"))["candidates"]
    mine, out = {}, []
    for k in want:
        mine[k] = sorted({c["addr"] for c in cands if c["kind"] == k and c.get("pastable")}, key=lambda a: int(a, 16))
    note = ", ".join(f"{k}: {len(mine[k])} here / {len(want[k])} in {base}" for k in want)
    if any(len(mine[k]) != len(want[k]) for k in want): return f"the sites do not match ({note}): none set, the game plays at 30 fps and 16:9"
    ih = " ".join(f"{a}:{k}" for k in want for a in mine[k])
    text = open(pack, encoding="utf-8").read()
    text = set_setting(text, "IHOOKS", f'IHOOKS = "{ih}"   # found again from {base}\'s hook kinds (' + note + "): unverified")
    with open(pack, "w", encoding="utf-8", newline="\n") as f: f.write(text)
    packconf.read(pack)
    return f"IHOOKS set ({note}): {ih}"


def add_update_version(pack, patch0):
    """append the update's release to the pack (a file); returns a line saying what was done"""
    settings, versions, _ = packconf.read(pack)
    elf = settings.get("ELF", "eboot.bin.elf")
    e = os.path.join(patch0, elf)
    if not os.path.isfile(e): return "the update has no main program of its own: nothing to add"
    digest = sha256(e)
    for name, x in versions.items():
        if x["sha256"] == digest: return f"the update's main program is version {name} already"
    _name, ver = bootstrap.identity(patch0, sfo.read_sfo_file)
    ver = ver or "1.01"
    if ver in versions: ver += "-update"
    text = open(pack, encoding="utf-8").read()
    text = text.replace('status = "recommended"', 'status = "supported"')
    text = text.rstrip("\n") + f'\n\n[versions."{ver}"]\nsha256 = "{digest}"\nstatus = "recommended"\n'
    with open(pack, "w", encoding="utf-8", newline="\n") as f: f.write(text)
    packconf.read(pack)   # (its shape checked, as pack-init does)
    return f"added version {ver} ({digest[:12]}...): the update's main program"


def main(argv):
    base = ""; hooks = "--hooks" in argv
    args = [a for a in argv if a != "--hooks"]
    if "--base" in args:
        i = args.index("--base"); base = args[i + 1] if i + 1 < len(args) else ""; args = args[:i] + args[i + 2:]
    if len(args) != 1: print(__doc__); return 1
    T = args[0]
    dump = os.environ.get("ATIV_DUMP") or os.path.join(ROOT, "dump")
    pack = os.path.join(os.environ.get("ATIV_PACKS") or os.path.join(ROOT, "packs"), T, "pack.toml")
    patch0 = os.path.join(dump, "decrypted", T, "patch0")
    if not os.path.isfile(pack): print(f"ge2rb_pack: no {pack} (vitarecomp pack-init {T} first)"); return 1
    if hooks:
        if not base: print("ge2rb_pack: --hooks needs --base <title>"); return 1
        print("ge2rb_pack: " + hooks_from_base(pack, base, os.path.join(dump, "decrypted", T, "analysis", "hook_candidates.json")))
        return 0
    if base: print("ge2rb_pack: " + seed_from_base(pack, base))
    if not os.path.isdir(patch0): print("ge2rb_pack: no update (patch0) kept: the pack plays the game as it is"); return 0
    print("ge2rb_pack: " + add_update_version(pack, patch0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
