#!/usr/bin/env python3
"""Assemble the clang-spatch .cocci test corpus in the tool tree.

Copies every patch of the four native corpora with a provenance header, copies
the paired C inputs and checked-in .res expectations, and writes one manifest
row per patch.
"""
import hashlib, json, os, re, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refusals as RF, tier_all
from sel import SEL

LINUX = "/home/roche/git/linux"
SRC = "/tmp/claude-1000/-home-roche-git-AlgoliaSaaS/32e3b5b7-d685-4d3a-9c3a-2452dfcf7530/scratchpad/coccinelle-src"
SCOPE = "/tmp/claude-1000/-home-roche-git-AlgoliaSaaS/32e3b5b7-d685-4d3a-9c3a-2452dfcf7530/scratchpad/cocci-scope"
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = "/home/roche/git/llvm-pathmatch/clang-tools-extra/clang-spatch/test/Inputs"
SPATCH = os.path.join(SRC, "spatch")

CORPORA = [
    # key,            source root,                          subtree, dest under cocci/
    ("kernel",   os.path.join(LINUX, "scripts/coccinelle"), "",         "kernel"),
    ("tests",    SRC,                                       "tests",    "coccinelle/tests"),
    ("demos",    SRC,                                       "demos",    "coccinelle/demos"),
    ("cpptests", SRC,                                       "cpptests", "coccinelle/cpptests"),
]
UPSTREAM = {
    "kernel":   ("Linux kernel", "linux/scripts/coccinelle", "GPL-2.0-only (per-file SPDX)"),
    "tests":    ("Coccinelle", "coccinelle/tests", "GPL-2.0 (coccinelle/COPYING)"),
    "demos":    ("Coccinelle", "coccinelle/demos", "GPL-2.0 (coccinelle/COPYING)"),
    "cpptests": ("Coccinelle", "coccinelle/cpptests", "GPL-2.0 (coccinelle/COPYING)"),
}

# ---------------------------------------------------------------- constructs --
def constructs(path, active):
    """The in-subset constructs the live rules of this file actually use."""
    raw, clean, rules, live, virt = RF.live_rules_sat(path, active)
    live_cocci = [(h, m, b) for h, m, b in live
                  if not h.strip().startswith(("script:", "initialize:", "finalize:"))]
    bodies = "\n".join(b for _, _, b in live_cocci)
    decls = "\n".join(m for _, m, _ in live_cocci)
    hdrs = [" ".join(h.split()) for h, _, _ in live]
    c = []
    for kind in ("expression", "identifier", "statement", "type", "constant", "position"):
        if re.search(r"(?m)^\s*%s\b" % kind, decls):
            c.append("%s mv" % kind)
    if re.search(r"(?m)^\s*[-+*]?\s*\.\.\.", bodies):
        c.append("stmt dots")
    if re.search(r"\bwhen\s*!=", bodies, re.I):
        c.append("when !=")
    if re.search(r"\bwhen\s+any\b", bodies, re.I):
        c.append("when any")
    if re.search(r"\bwhen\s+strict\b", bodies, re.I):
        c.append("when strict")
    if re.search(r"(?m)^\s*[-+*]?\s*\|\s*$", bodies):
        c.append("disjunction")
    if any(re.search(r"(^|\s)exists\s*$", h) for h in hdrs):
        c.append("rule exists")
    if any(re.search(r"(^|\s)forall\s*$", h) for h in hdrs):
        c.append("rule forall")
    if re.search(r"@[A-Za-z_]\w*", bodies):
        c.append("position attach")
    if re.search(r"(?m)^\s*position[^;]*!=", decls):
        c.append("position !=")
    if re.search(r"\b\w+\s*\.\s*\w+\s*;", decls) or "<<" in "\n".join(m for _, m, _ in live):
        c.append("metavar inheritance")
    if virt:
        c.append("virtual + depends on")
    if any(h.startswith("script:python") for h in hdrs):
        c.append("script:python")
    marks = set()
    for _, _, b in live_cocci:
        for ln in b.split("\n"):
            if re.match(r"^-(?!-)", ln): marks.add("-")
            if re.match(r"^\+(?!\+)", ln): marks.add("+")
            if re.match(r"^\*", ln): marks.add("*")
    if marks:
        c.append("markers " + "".join(sorted(marks)))
    return c

# ------------------------------------------------------------------- copying --
def header(key, rel):
    proj, root, lic = UPSTREAM[key]
    return (
        "// clang-spatch test corpus. Copied unmodified below this header.\n"
        "// Provenance: %s/%s\n"
        "// Upstream project: %s. Licence: %s.\n"
        "// Third-party file: do not edit. Line numbers quoted in\n"
        "// corpus-manifest.tsv are line numbers in this file, header included.\n"
        % (root, rel, proj, lic))

def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()[:16]

# ---------------------------------------------------------------- oracle runs --
# One grep token per curated kernel rule, to pick a handful of real kernel files
# that can plausibly match.  drivers/of is added to every list as a fixed set of
# files that mostly cannot, so a run has both hits and clean results.
TOKENS = {
    "kernel-api-err_cast": "ERR_PTR(PTR_ERR",
    "kernel-free-kfreeaddr": "kfree(&",
    "kernel-misc-returnvar": "return ret;",
    "kernel-misc-secs_to_jiffies": "msecs_to_jiffies(",
    "kernel-misc-orplus": "| BIT(",
    "kernel-locks-flags": "spin_lock_irqsave(",
    "kernel-api-atomic_as_refcounter": "atomic_dec_and_test(",
    "kernel-misc-array_size_dup": "array_size(",
    "kernel-free-kfree": "kfree_sensitive(",
    "kernel-api-string_choices": '? "enable" : "disable"',
    "kernel-api-stream_open": "nonseekable_open(",
}
CONTROL = {
    "kernel-api-err_cast": "ctl_err_cast.c",
    "kernel-free-kfreeaddr": "ctl_kfreeaddr.c",
    "kernel-misc-returnvar": "ctl_returnvar.c",
    "kernel-misc-secs_to_jiffies": "ctl_secs_to_jiffies.c",
    "kernel-misc-orplus": "ctl_orplus.c",
    "kernel-locks-flags": "ctl_flags.c",
    "kernel-api-atomic_as_refcounter": "ctl_atomic_as_refcounter.c",
    "kernel-misc-array_size_dup": "ctl_array_size_dup.c",
    "kernel-free-kfree": "ctl_kfree.c",
    "kernel-api-string_choices": "ctl_string_choices.c",
    "kernel-api-stream_open": "ctl_stream_open.c",
}

def kernel_filelist(rid, out, cap=120):
    """Files that can plausibly match, plus a fixed set that mostly cannot.

    A rule cannot match a file that never names the function it is about, so
    grepping for one token loses no coverage and cuts the run by two orders of
    magnitude.  drivers/of is appended unconditionally so every run also
    contains files expected to come back clean.
    """
    tok = TOKENS[rid]
    cp = subprocess.run(["grep", "-rl", "--include=*.c", "-F", tok,
                         "drivers", "fs", "net", "sound", "arch/x86", "kernel", "mm", "lib"],
                        cwd=LINUX, capture_output=True, text=True)
    files = sorted(cp.stdout.split())[:cap]
    fixed = subprocess.run(["bash", "-c", "ls drivers/of/*.c"], cwd=LINUX,
                           capture_output=True, text=True).stdout.split()
    files = sorted(dict.fromkeys(files + sorted(fixed)))
    open(out, "w").write("\n".join(files) + "\n")
    return files

def main():
    for d in ("cocci", "c", "oracle", "harness"):
        os.makedirs(os.path.join(TOOL, d), exist_ok=True)

    curated = {rel_key: (rid, tier, note, orc)
               for rid, tier, key, rel, mode, orc, note in SEL
               for rel_key in [(("kernel" if key == "kernel" else
                                 rel.split("/")[0]), rel if key == "kernel" else rel)]}
    # index curated by (corpus key, relative path)
    cur = {}
    for rid, tier, key, rel, mode, orc, note in SEL:
        ck = "kernel" if key == "kernel" else rel.split("/")[0]
        cr = rel if ck == "kernel" else rel.split("/", 1)[1]
        cur[(ck, cr)] = (rid, tier, note, orc)

    resv = {}
    if os.path.exists(os.path.join(HERE, "rescheck.json")):
        for r in json.load(open(os.path.join(HERE, "rescheck.json"))):
            resv[r["rule"]] = r["verdict"]

    rows = []
    for key, root, sub, dest in CORPORA:
        base = os.path.join(root, sub) if sub else root
        sweep = {r["file"]: r for r in
                 json.load(open(os.path.join(HERE, "sweep-%s.json" %
                            ("kernel" if key == "kernel" else key))))}
        for dp, _, fns in os.walk(base):
            for fn in sorted(fns):
                if not fn.endswith(".cocci"):
                    continue
                srcp = os.path.join(dp, fn)
                rel = os.path.relpath(srcp, base)
                dstp = os.path.join(TOOL, "cocci", dest, rel)
                os.makedirs(os.path.dirname(dstp), exist_ok=True)
                with open(dstp, "w", encoding="utf-8") as fh:
                    fh.write(header(key, rel))
                    fh.write(open(srcp, encoding="utf-8", errors="replace").read())
                sw = sweep.get(srcp, {})
                tier = sw.get("tier", 0)
                mode = sw.get("mode", "?")
                refus = list(sw.get("refusals", []))
                # cpptests selects the C++ front end for the whole file, so no
                # rule in it is compilable whatever else it contains.
                if key == "cpptests":
                    if "C++ front end selected by #spatch --c++" not in refus:
                        refus.insert(0, "C++ front end selected by #spatch --c++")
                    tier = 3
                act, _ = tier_all.auto_active(srcp)
                cons = constructs(srcp, act)

                # paired inputs
                inputs, resf = [], ""
                for ext in (".c", ".cpp"):
                    cand = srcp[:-6] + ext
                    if os.path.exists(cand):
                        d2 = os.path.join(TOOL, "c", dest, os.path.relpath(cand, base))
                        os.makedirs(os.path.dirname(d2), exist_ok=True)
                        shutil.copyfile(cand, d2)
                        inputs.append(os.path.relpath(d2, TOOL))
                cand = srcp[:-6] + ".res"
                if os.path.exists(cand):
                    d2 = os.path.join(TOOL, "c", dest, os.path.relpath(cand, base))
                    os.makedirs(os.path.dirname(d2), exist_ok=True)
                    shutil.copyfile(cand, d2)
                    resf = os.path.relpath(d2, TOOL)

                ck = key if key == "kernel" else key
                crel = rel
                c_entry = cur.get((ck, crel))
                rid = c_entry[0] if c_entry else ""
                note = c_entry[2] if c_entry else ""

                # oracle class
                resrel = os.path.relpath(srcp, SRC) if key != "kernel" else ""
                v = resv.get(resrel, "")
                if resf and v in ("RES_EXACT",):
                    oclass, oref = "res-verified", resf
                elif resf and v == "RES_MODULO_WHITESPACE":
                    oclass, oref = "res-whitespace", resf
                elif resf:
                    oclass, oref = "res-unverified(%s)" % (v or "not-run"), resf
                elif rid in CONTROL:
                    oclass, oref = "spatch-run", "oracle/%s/findings.norm" % rid
                else:
                    oclass, oref = "none", ""

                if tier == 1:
                    exp = "parse all rules, compile all rules, run; no refusals"
                elif tier == 2:
                    exp = ("parse all rules, compile the %d clean rule(s) of %d, "
                           "refuse the rest by name" %
                           (sw.get("n_cocci_rules_clean", 0), sw.get("n_cocci_rules_live", 0)))
                else:
                    exp = "refuse by name, compile nothing, produce no findings"

                rows.append({
                    "path": os.path.relpath(dstp, TOOL),
                    "provenance": "%s/%s" % (UPSTREAM[key][1], rel),
                    "tier": tier,
                    "curated_id": rid,
                    "mode": mode,
                    "rules_live": sw.get("n_live", 0),
                    "cocci_rules_live": sw.get("n_cocci_rules_live", 0),
                    "cocci_rules_compilable": sw.get("n_cocci_rules_clean", 0),
                    "constructs": ", ".join(cons),
                    "expected_outcome": exp,
                    "refusal_names": " | ".join(refus),
                    "oracle_class": oclass,
                    "oracle_ref": oref,
                    "c_inputs": " ".join(inputs),
                    "sha256_16": sha(srcp),
                    "hardness_note": note,
                })
    json.dump(rows, open(os.path.join(HERE, "rows.json"), "w"), indent=1)
    print("copied %d patches" % len(rows))
    from collections import Counter
    print(Counter(r["tier"] for r in rows))
    print(Counter(r["oracle_class"].split("(")[0] for r in rows))

if __name__ == "__main__":
    main()
