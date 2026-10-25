#!/usr/bin/env python3
"""Read-only inventory of character models inside RTCW .pk3 files (zip) and mod folders.

Mirrors the checks the RTCWQuest mercenary performs at runtime (g_merc.c / g_vrlogic.h):
  server  : models/players/<m>/wolfanim.cfg, wolfanim.script (or default.script when cfg version <= 1)
  AI      : the animation script defines STATE RELAXED / ALERT / COMBAT
  client  : body_<skin>.skin + body.mds (or lower_/upper_<skin>.skin + lower.md3/upper.md3) and head_<skin>.skin
Only entry NAMES and the two small text files (cfg, script) are read, in memory. Nothing is extracted,
copied or uploaded. Search order follows the engine (files.c FS_AddGameDirectory): within one folder the
pk3 files are sorted alphabetically with "sp_" treated as "zz" (so sp_pak* override pak0), later wins;
folders given later on the command line (mods) override earlier ones.

usage: inventory_rtcw_models.py BASE_DIR [MOD_DIR ...] [--models american partisan ...] [--out FILE]
"""
import argparse, os, re, sys, zipfile

REQUIRED = {"relaxed", "alert", "combat"}
NATIVE = ["american", "partisan", "infantryss", "eliteguard"]   # same order as the mod's preference list


def pak_sort_key(name):
    n = name.lower()
    if n.startswith("sp_"):
        n = "zz" + n[2:]
    return n


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def cfg_version(text):
    toks = strip_comments(text).replace("{", " { ").replace("}", " } ").split()
    for i, t in enumerate(toks):
        if t.lower() == "version":
            if i + 1 < len(toks):
                m = re.match(r"-?\d+", toks[i + 1])
                return int(m.group(0)) if m else 0
            return 0
    return 0


def script_states(text):
    toks = strip_comments(text).replace("{", " { ").replace("}", " } ").split()
    states = set()
    for i, t in enumerate(toks[:-1]):
        if t.lower() == "state" and toks[i + 1].lower() in ("relaxed", "query", "alert", "combat"):
            states.add(toks[i + 1].lower())
    return states


class Vfs:
    """name(lower) -> (source label, reader) with engine precedence."""
    def __init__(self):
        self.files = {}

    def add_folder(self, folder):
        paks = sorted((f for f in os.listdir(folder) if f.lower().endswith(".pk3")), key=pak_sort_key)
        for pk in paks:                                  # later wins (searched first in the engine)
            path = os.path.join(folder, pk)
            try:
                z = zipfile.ZipFile(path)                # read-only
            except Exception as e:
                print("WARNING: cannot read %s: %s" % (pk, e), file=sys.stderr)
                continue
            for info in z.infolist():
                if info.is_dir():
                    continue
                self.files[info.filename.lower()] = (os.path.basename(folder) + "/" + pk, z, info)
        # loose files in the folder override pk3 files of the same folder
        for root, _, names in os.walk(folder):
            for n in names:
                full = os.path.join(root, n)
                rel = os.path.relpath(full, folder).replace(os.sep, "/").lower()
                if rel.startswith("models/"):
                    self.files[rel] = (os.path.basename(folder) + "/(loose)", None, full)

    def exists(self, name):
        return name.lower() in self.files

    def source(self, name):
        return self.files[name.lower()][0]

    def read(self, name):
        src, z, info = self.files[name.lower()]
        data = z.read(info) if z is not None else open(info, "rb").read()
        return data.decode("latin-1")

    def listdir_models(self):
        out = set()
        for n in self.files:
            m = re.match(r"models/players/([^/]+)/", n)
            if m:
                out.add(m.group(1))
        return sorted(out)

    def skins(self, model, prefix):
        pat = re.compile(r"^models/players/%s/%s(.+)\.skin$" % (re.escape(model), re.escape(prefix)))
        res = []
        for n in self.files:
            m = pat.match(n)
            if m:
                res.append(m.group(1))
        return sorted(res)


def pick(names):
    if not names:
        return None
    return "default" if "default" in names else names[0]


def evaluate(v, model):
    r = {"model": model, "missing": [], "info": []}
    base = "models/players/%s/" % model
    cfg = base + "wolfanim.cfg"
    has_cfg = v.exists(cfg)
    version = cfg_version(v.read(cfg)) if has_cfg else 0
    script_name = None
    if v.exists(base + "wolfanim.script"):
        script_name = base + "wolfanim.script"
    elif has_cfg and version <= 1 and v.exists("models/players/default.script"):
        script_name = "models/players/default.script"
    states = script_states(v.read(script_name)) if script_name else set()
    if not has_cfg:
        r["missing"].append(cfg)
    if not script_name:
        r["missing"].append(base + "wolfanim.script")
    elif not REQUIRED <= states:
        r["missing"].append("animation script %s lacks AI states %s" % (script_name, sorted(REQUIRED - states)))
    body = v.skins(model, "body_")
    lower, upper = v.skins(model, "lower_"), v.skins(model, "upper_")
    head = v.skins(model, "head_")
    skeletal = bool(body) and v.exists(base + "body.mds")
    common = [s for s in lower if s in upper]
    legacy = bool(common) and v.exists(base + "lower.md3") and v.exists(base + "upper.md3")
    if not skeletal and not legacy:
        if body and not v.exists(base + "body.mds"):
            r["missing"].append(base + "body.mds")
        elif not body and not common:
            r["missing"].append(base + "body_<skin>.skin (or lower_/upper_<skin>.skin)")
        else:
            r["missing"].append("legacy body parts (lower.md3/upper.md3)")
    if not head:
        r["missing"].append(base + "head_<skin>.skin")
    r["bodySkin"] = pick(body) or pick(common)
    r["headSkin"] = pick(head)
    r["version"] = version
    r["script"] = script_name
    r["sources"] = sorted({v.source(base + f) for f in ("wolfanim.cfg", "wolfanim.script") if v.exists(base + f)})
    r["ok"] = not r["missing"]
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folders", nargs="+", help="base folder with pk3 files, then mod/mission-pack folders (override earlier ones)")
    ap.add_argument("--models", nargs="*", help="models to list in detail (default: native pairings + every viable one)")
    ap.add_argument("--out", default="rtcw_model_inventory.txt")
    a = ap.parse_args()

    v = Vfs()
    for f in a.folders:
        v.add_folder(f)
    lines = ["RTCW model inventory (file names only). Folders (low -> high priority): " + ", ".join(a.folders), ""]
    allm = v.listdir_models()
    lines.append("model folders found: " + ", ".join(allm))
    lines.append("")
    viable = []
    for m in allm:
        r = evaluate(v, m)
        detail = a.models is None or m in a.models or m in NATIVE
        if r["ok"]:
            viable.append(m)
        if detail or r["ok"]:
            tag = "COMPLETE" if r["ok"] else "INCOMPLETE"
            lines.append("== %s: %s%s" % (m, tag, " (native pairing)" if m in NATIVE else ""))
            if r["ok"]:
                lines.append("   body skin '%s', head skin '%s', cfg version %d, script %s, from %s"
                             % (r["bodySkin"], r["headSkin"], r["version"], r["script"], ", ".join(r["sources"])))
            for mi in r["missing"]:
                lines.append("   missing: " + mi)
    for m in (a.models or []):
        if m not in allm:
            lines.append("== %s: NOT PRESENT (no models/players/%s/ entries in any pk3 or folder)" % (m, m))
    lines.append("")
    lines.append("complete models: " + (", ".join(viable) if viable else "none"))
    lines.append("The mod prefers (in order): " + ", ".join(NATIVE) + ". Any other complete model can be forced in game with:  merc type <model>")
    lines.append("NOTE: complete means 'files the engine needs exist'. Check that the model is a humanoid soldier before forcing it.")
    text = "\n".join(lines)
    print(text)
    open(a.out, "w", encoding="utf-8").write(text + "\n")
    print("\nReport written to " + os.path.abspath(a.out))


if __name__ == "__main__":
    main()
