#!/usr/bin/env python3
"""Tier every .cocci file: 1 = fully inside the subset, 2 = at least one live
cocci rule inside it plus named refusals elsewhere, 3 = no live cocci rule is
compilable."""
import os, re, sys, json, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refusals as RF

def per_rule(path, active=("report",)):
    raw, clean, rules, live, virtuals = RF.live_rules_sat(path, active)
    vhdr = "".join("virtual %s\n" % v for v in sorted(virtuals))
    out = []
    for hdr, mv, body in live:
        txt = vhdr + "@%s@\n%s\n@@\n%s\n" % (hdr, mv, body)
        with tempfile.NamedTemporaryFile("w", suffix=".cocci", delete=False) as fh:
            fh.write(txt); tmp = fh.name
        try:
            r, _, _, _ = RF.analyse(tmp, active=active, all_rules=True)
        finally:
            os.unlink(tmp)
        out.append({"hdr": " ".join(hdr.split()),
                    "script": hdr.strip().startswith(("script:", "initialize:", "finalize:")),
                    "refusals": [n for n, _ in r]})
    return out, virtuals, len(rules), len(live)

def auto_active(path):
    """Which virtuals to switch on when classifying this file.

    A kernel rule ships four alternative behaviours behind `virtual context`,
    `virtual patch`, `virtual org` and `virtual report`, and only the report
    ones are live for a matcher.  A file with no `report` virtual is classified
    with every virtual it declares switched on, so nothing is counted as dead
    just because no mode was selected.
    """
    raw = open(path, encoding="utf-8", errors="replace").read()
    clean = RF.normalise(RF.c1.strip_comments(raw))
    virt = set()
    for m in re.finditer(r"^\s*virtual\s+([A-Za-z_0-9, \t]+)$", clean, re.M):
        virt |= {v.strip() for v in m.group(1).split(",") if v.strip()}
    if "report" in virt:
        return ("report",), "report"
    if virt:
        return tuple(sorted(virt)), "+".join(sorted(virt))
    return ("__no_virtual__",), "none"

def tier(path, active=("report",)):
    allr, virtuals, nt, nl = RF.analyse(path, active=active)
    names = [n for n, _ in allr]
    pr, _, _, _ = per_rule(path, active)
    cocci = [x for x in pr if not x["script"]]
    clean_cocci = [x for x in cocci if not x["refusals"]]
    if not names:
        t = 1
    elif clean_cocci:
        t = 2
    else:
        t = 3
    return {"file": path, "tier": t, "refusals": names,
            "refusals_with_line": [{"name": n, "line": l} for n, l in allr],
            "n_rules": nt, "n_live": nl,
            "n_cocci_rules_live": len(cocci), "n_cocci_rules_clean": len(clean_cocci),
            "per_rule": pr, "virtuals": sorted(virtuals)}

if __name__ == "__main__":
    root = sys.argv[1]
    outp = sys.argv[2]
    paths = []
    if os.path.isdir(root):
        for dp, _, fns in os.walk(root):
            for fn in sorted(fns):
                if fn.endswith(".cocci"):
                    paths.append(os.path.join(dp, fn))
    else:
        paths = [root]
    paths.sort()
    rows = []
    for p in paths:
        try:
            act, label = auto_active(p)
            r = tier(p, active=act)
            r["mode"] = label
            rows.append(r)
        except Exception as e:
            rows.append({"file": p, "tier": 0, "mode": "?",
                         "error": "%s: %s" % (type(e).__name__, e)})
    json.dump(rows, open(outp, "w"), indent=1)
    from collections import Counter
    print(Counter(r["tier"] for r in rows))
    for t in (1, 2, 3, 0):
        sel = [r for r in rows if r["tier"] == t]
        if not sel: continue
        print("\n===== TIER %d (%d)" % (t, len(sel)))
        for r in sel:
            f = r["file"].split("scripts/coccinelle/")[-1].split("coccinelle-src/")[-1]
            if t == 0:
                print("  %-46s %s" % (f, r["error"])); continue
            print("  %-46s live=%d cocci=%d clean=%d | %s" % (
                f, r["n_live"], r["n_cocci_rules_live"], r["n_cocci_rules_clean"],
                "; ".join(r["refusals"])))
