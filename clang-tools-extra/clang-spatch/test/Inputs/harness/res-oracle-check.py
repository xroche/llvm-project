#!/usr/bin/env python3
"""Does the checked-in .res still describe what this spatch produces?

Coccinelle's own regression runner compares the transformed file against the
paired .res, so a .res that reproduces is a ground truth maintained by the
tool's authors.  One that does not reproduce is an illustration, and using it
as an oracle would pin an expectation the reference implementation itself does
not meet.  This script separates the two.
"""
import os, re, subprocess, sys, json, concurrent.futures as cf

SRC = "/tmp/claude-1000/-home-roche-git-AlgoliaSaaS/32e3b5b7-d685-4d3a-9c3a-2452dfcf7530/scratchpad/coccinelle-src"
SPATCH = os.path.join(SRC, "spatch")
TIMEOUT = 60

def options_of(rule):
    for ln in open(rule, encoding="utf-8", errors="replace"):
        m = re.match(r"^//[ \t]*Options:[ \t]*(.*)$", ln)
        if m:
            return m.group(1).split()
        m = re.match(r"^[ \t]*#[ \t]*spatch[ \t]+(.*)$", ln)
        if m:
            return m.group(1).split()
    return []

def virtuals_of(rule):
    txt = open(rule, encoding="utf-8", errors="replace").read()
    v = set()
    for m in re.finditer(r"^\s*virtual\s+([A-Za-z_0-9, \t]+)$", txt, re.M):
        v |= {x.strip() for x in m.group(1).split(",") if x.strip()}
    return v

def one(rule):
    base = rule[:-6]
    cfile = base + ".c" if os.path.exists(base + ".c") else (
            base + ".cpp" if os.path.exists(base + ".cpp") else None)
    res = base + ".res"
    rec = {"rule": os.path.relpath(rule, SRC), "c": cfile and os.path.relpath(cfile, SRC),
           "res": os.path.relpath(res, SRC)}
    if not cfile:
        rec["verdict"] = "NO_INPUT"; return rec
    out = "/tmp/rescheck-%d-%s.c" % (os.getpid(), os.path.basename(base))
    v = virtuals_of(rule)
    dmode = ["-D", "report"] if "report" in v else []
    cmd = [SPATCH, "--very-quiet", "--no-show-diff", "-o", out] + dmode + \
          ["--cocci-file", rule] + options_of(rule) + [cfile]
    try:
        cp = subprocess.run(cmd, cwd=SRC, capture_output=True, text=True,
                            timeout=TIMEOUT, errors="replace")
        rec["exit"] = cp.returncode
        rec["stderr_tail"] = (cp.stdout + cp.stderr)[-300:]
    except subprocess.TimeoutExpired:
        rec["verdict"] = "TIMEOUT"; return rec
    if not os.path.exists(out):
        rec["verdict"] = "NO_OUTPUT"; return rec
    got = open(out, encoding="utf-8", errors="replace").read()
    want = open(res, encoding="utf-8", errors="replace").read()
    src = open(cfile, encoding="utf-8", errors="replace").read()
    os.unlink(out)
    if got == want:
        rec["verdict"] = "RES_EXACT"
    elif got.split() == want.split():
        rec["verdict"] = "RES_MODULO_WHITESPACE"
    elif got == src:
        rec["verdict"] = "NO_CHANGE"      # the rule did not fire at all
    else:
        rec["verdict"] = "RES_DIFFERS"
    rec["changed"] = (got != src)
    return rec

if __name__ == "__main__":
    dirs = sys.argv[1:] or ["tests", "demos", "cpptests"]
    rules = []
    for d in dirs:
        for dp, _, fns in os.walk(os.path.join(SRC, d)):
            for fn in sorted(fns):
                if fn.endswith(".cocci") and os.path.exists(os.path.join(dp, fn[:-6] + ".res")):
                    rules.append(os.path.join(dp, fn))
    rules.sort()
    recs = []
    with cf.ThreadPoolExecutor(max_workers=8) as ex:
        for r in ex.map(one, rules):
            recs.append(r)
    json.dump(recs, open("rescheck.json", "w"), indent=1)
    from collections import Counter
    print("pairs with a .res:", len(recs))
    for d in dirs:
        sub = [r for r in recs if r["rule"].startswith(d + "/")]
        print("%-10s %s" % (d, dict(Counter(r["verdict"] for r in sub))))
