#!/usr/bin/env python3
"""Adds the game's update to its auto-generated pack: ge2rb_pack.py <title>

`vitarecomp pack-init` writes packs/<title>/pack.toml from the game as it was added (app0): one version, the base game's. A game's
update (patch0) that changes the main program is another release -- vitarecomp import layers the update over the game only when the
pack lists that release by the SHA-256 of its eboot.bin.elf, and otherwise plays the game without it. So the update's release is added
as a second version, marked recommended (the one to play). An update that leaves the main program alone needs nothing."""
import hashlib
import os
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
    if len(argv) != 1: print(__doc__); return 1
    T = argv[0]
    dump = os.environ.get("ATIV_DUMP") or os.path.join(ROOT, "dump")
    pack = os.path.join(os.environ.get("ATIV_PACKS") or os.path.join(ROOT, "packs"), T, "pack.toml")
    patch0 = os.path.join(dump, "decrypted", T, "patch0")
    if not os.path.isfile(pack): print(f"ge2rb_pack: no {pack} (vitarecomp pack-init {T} first)"); return 1
    if not os.path.isdir(patch0): print("ge2rb_pack: no update (patch0) kept: the pack plays the game as it is"); return 0
    print("ge2rb_pack: " + add_update_version(pack, patch0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
