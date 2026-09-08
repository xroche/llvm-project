#!/usr/bin/env python3
"""Differential-testing oracle harness for Coccinelle semantic patches.

Runs one .cocci rule over a list of C files, one spatch process per file, and
emits a normalised, sorted, machine-comparable finding set plus full
accountability for every file that did not produce a clean result.

Two hazards this guards against:

  * `spatch --dir` prints `EXN: Stack overflow` and exits 0 while skipping
    files.  This harness never uses --dir.  It runs one process per file, and
    every file lands in exactly one status bucket in the manifest.  A file
    spatch declined to handle is counted, not silently dropped.  Passing
    several files in one invocation is also wrong: spatch merges them into a
    single HANDLING group and shares inferred typedefs across them.

  * A zero-finding result is meaningless without a positive control.  The
    harness refuses to run the corpus until the rule has produced at least one
    finding on a control file that is known to match.

Usage:
  cocci-oracle.py --rule scripts/coccinelle/free/put_device.cocci \
                  --control control/ctl_put_device.c \
                  --filelist corpus.txt --out out/put_device -j 8
"""
import argparse, concurrent.futures as cf, json, os, re, subprocess, sys, time

SPATCH_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "..", "coccinelle-src", "spatch")

# A rule with no report script says so and produces no output at all.  Distinct
# from CLEAN, which means the rule ran and matched nothing.
NO_RULES = re.compile(r"^No rules apply\.", re.M)

# A python or ocaml script rule that raises is not a clean zero either.  Both
# report on stdout and the exit code is not reliable, so match the text.
SCRIPT_ERR = re.compile(r"Error in (Python|ocaml) script|Python failure|"
                        r"Ocaml failure|SyntaxError", re.M)

# Unified-diff hunk header, for rules whose only output is the diff: `*` context
# rules and `-`/`+` patch rules carry no position through print_report, so the
# removed-line numbers in the diff are the only finding positions spatch exposes.
HUNK = re.compile(r"^@@ -(?P<old>\d+)(?:,(?P<oldlen>\d+))? \+\d+(?:,\d+)? @@")

# `path:line:col-endcol: message`  -- what coccilib.report.print_report emits.
FINDING = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+)(?:-(?P<endcol>\d+))?: (?P<msg>.*)$")

# Anything here means the whole file's result cannot be trusted, whatever the
# exit code says.  This is the `--dir` hazard: spatch prints EXN and exits 0.
POISON = re.compile(
    r"EXN:"
    r"|Stack overflow"
    r"|Fatal error"
    r"|Out of memory"
    r"|Semantic patch uses python, but Coccinelle has been compiled without"
    r"|Semantic patch uses ocaml, but Coccinelle has been compiled without"
    r"|SPATCH: internal error"
    r"|minus: parse error",
    re.M)

# The C parser recovers from what it cannot parse and moves on, so a file can
# yield a valid-looking empty result while whole functions were never analysed.
# `ERROR-RECOV` / `parse error` / `BAD:!!!!!` are how it says so under
# --verbose-parsing.  Degradation is orthogonal to status: a file can hold both
# a finding and an unanalysed region.
DEGRADED = re.compile(r"^(ERROR-RECOV:|parse error)|BAD:!!!!!", re.M)

def rule_options(rule_path):
    """The `// Options:` line the kernel rule author asked for.

    52 of the 76 kernel rules ask for `--no-includes --include-headers`; taking
    the value from the file rather than hardcoding it keeps the oracle running
    each rule the way coccicheck would.
    """
    for ln in open(rule_path, encoding="utf-8", errors="replace"):
        m = re.match(r"^//[ \t]*Options:[ \t]*(.*)$", ln)
        if m:
            opts = m.group(1).split()
            # api/atomic_as_refcounter.cocci asks for --very-quiet, which
            # suppresses the HANDLING: line this harness counts to prove a file
            # was analysed.  Keeping it turns every result into NOT_HANDLED.
            return [o for o in opts if o not in ("--very-quiet", "-very_quiet")]
    return []

def resolve_spatch(p):
    if os.path.isfile(p):
        return os.path.abspath(p)
    # Known trap: the build leaves spatch.opt but no spatch symlink.
    alt = p + ".opt"
    if os.path.isfile(alt):
        sys.stderr.write("note: %s missing, symlinking from %s\n" % (p, alt))
        os.symlink(os.path.basename(alt), p)
        return os.path.abspath(p)
    sys.exit("spatch not found at %s (and no %s beside it)" % (p, alt))

def diff_findings(out, path):
    """Positions of the lines a diff removes or marks, keyed to the old file.

    Columns are not in the diff, so col is reported as 0.  A `*` context rule
    and a `-` patch rule both print their matched lines as removals, so one
    reader serves both.
    """
    found, old, in_hunk = [], 0, False
    for ln in out.split("\n"):
        m = HUNK.match(ln)
        if m:
            old, in_hunk = int(m.group("old")), True
            continue
        if not in_hunk:
            continue
        if ln.startswith("---") or ln.startswith("+++"):
            in_hunk = False
            continue
        if ln.startswith("-"):
            found.append((path, old, 0, 0, "removed: " + ln[1:].strip()))
            old += 1
        elif ln.startswith("+"):
            pass                      # added lines have no old-file position
        elif ln.startswith(" ") or ln == "":
            old += 1
        else:
            in_hunk = False
    return found

def run_one(spatch, rule, opts, path, root, timeout, mode, finding_mode="report"):
    """One spatch process, one C file.  Returns a per-file record."""
    # `-D <v>` on a patch that declares no `virtual <v>` is a hard error
    # ("virtual rule report not supported", exit 255), so a rule with no virtual
    # rules at all has to be invoked with no -D.
    dmode = [] if mode in (None, "", "none") else ["-D", mode]
    cmd = [spatch, "--quiet", "--verbose-parsing"] + dmode + \
          ["--cocci-file", rule] + opts + [path]
    t0 = time.time()
    try:
        cp = subprocess.run(cmd, cwd=root, capture_output=True, text=True,
                            timeout=timeout, errors="replace")
        out, rc, timedout = cp.stdout + cp.stderr, cp.returncode, False
    except subprocess.TimeoutExpired as e:
        out = ((e.stdout or "") if isinstance(e.stdout, str) else (e.stdout or b"").decode("utf-8", "replace")) \
            + ((e.stderr or "") if isinstance(e.stderr, str) else (e.stderr or b"").decode("utf-8", "replace"))
        rc, timedout = None, True
    secs = time.time() - t0

    findings, handled, bad, perr = [], 0, 0, 0
    for ln in out.split("\n"):
        if ln.startswith("HANDLING:"):
            handled += 1
            continue
        if "BAD:!!!!!" in ln:
            bad += 1
            continue
        if ln.startswith("parse error") or ln.startswith("ERROR-RECOV:"):
            perr += 1
            continue
        m = FINDING.match(ln)
        if m and m.group("file") == path:
            findings.append((path, int(m.group("line")), int(m.group("col")),
                             int(m.group("endcol") or m.group("col")), m.group("msg")))
    if finding_mode == "diff":
        findings = diff_findings(out, path)

    if timedout:
        status = "TIMEOUT"
    elif NO_RULES.search(out):
        # spatch exits 255 on this, so it has to be read before the exit code:
        # a patch with no -/+/* code and no report script has nothing to say.
        status = "NO_RULES"
    elif SCRIPT_ERR.search(out):
        # A raising python or ocaml script rule also exits 255 and is not a
        # clean zero; without this it hides behind EXIT255.
        status = "SCRIPT_ERROR"
    elif rc != 0:
        status = "EXIT%s" % rc
    elif POISON.search(out):
        status = "POISON"
    elif handled != 1:
        # spatch accepted the argument but never announced it: silently skipped.
        status = "NOT_HANDLED"
    elif findings:
        status = "HIT"
    else:
        status = "CLEAN"
    degraded = bool(bad or perr)
    return {"file": path, "status": status, "exit": rc, "secs": round(secs, 3),
            "n_findings": len(findings), "n_unparsed_lines": bad,
            "n_parse_recoveries": perr, "degraded": degraded,
            "findings": findings,
            "diag": "" if status in ("HIT", "CLEAN") else out[-2000:]}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rule", required=True)
    ap.add_argument("--control", action="append", default=[],
                    help="C file the rule MUST flag; the run aborts if it does not")
    ap.add_argument("--filelist", required=True)
    ap.add_argument("--root", default="/home/roche/git/linux")
    ap.add_argument("--out", required=True)
    ap.add_argument("--spatch", default=SPATCH_DEFAULT)
    ap.add_argument("--mode", default="report")
    ap.add_argument("--opts", default=None,
                    help="override the rule's own // Options: line")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--skip-control", action="store_true")
    ap.add_argument("--finding-mode", default="report", choices=["report", "diff"],
                    help="report: coccilib.report.print_report lines (file:line:col). "
                         "diff: removed-line positions from the unified diff, for a "
                         "rule with -/+/* code and no report script (col is 0).")
    a = ap.parse_args()

    spatch = resolve_spatch(a.spatch)
    rule = a.rule if os.path.isabs(a.rule) else os.path.join(a.root, a.rule)
    opts = a.opts.split() if a.opts is not None else rule_options(rule)
    os.makedirs(a.out, exist_ok=True)

    # ---- hazard guard 2: positive control before any negative result --------
    ctl = []
    if a.control and not a.skip_control:
        for c in a.control:
            c = os.path.abspath(c)
            r = run_one(spatch, rule, opts, c, a.root, a.timeout, a.mode, a.finding_mode)
            ctl.append({k: v for k, v in r.items() if k != "diag"})
            print("control %-40s %-12s findings=%d" %
                  (os.path.basename(c), r["status"], r["n_findings"]))
            if r["status"] != "HIT":
                sys.exit("ABORT: positive control %s produced no finding (status %s). "
                         "A zero result from this harness would be unprovable.\n%s"
                         % (c, r["status"], r["diag"]))
    elif not a.skip_control:
        sys.exit("ABORT: no --control given. Refusing to report findings without a "
                 "positive control (pass --skip-control to override).")

    files = [l.strip() for l in open(a.filelist) if l.strip()]
    print("rule    : %s" % os.path.relpath(rule, a.root))
    print("options : %s" % (" ".join(opts) or "(none)"))
    print("mode    : %s" % ("(no -D)" if a.mode in (None, "", "none") else "-D " + a.mode))
    print("corpus  : %d files, %d jobs, %ds timeout" % (len(files), a.jobs, a.timeout))

    t0 = time.time()
    recs = []
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(run_one, spatch, rule, opts, f, a.root, a.timeout, a.mode, a.finding_mode): f
                for f in files}
        done = 0
        for fu in cf.as_completed(futs):
            recs.append(fu.result())
            done += 1
            if done % 100 == 0:
                print("  ... %d/%d (%.0fs)" % (done, len(files), time.time() - t0),
                      file=sys.stderr)
    wall = time.time() - t0
    recs.sort(key=lambda r: r["file"])

    # ---- hazard guard 1: every input file is accounted for ------------------
    assert len(recs) == len(files), "manifest lost files: %d in, %d out" % (len(files), len(recs))

    all_f = sorted({(f, ln, col, ec, msg) for r in recs for (f, ln, col, ec, msg) in r["findings"]})
    with open(os.path.join(a.out, "findings.tsv"), "w") as fh:
        fh.write("file\tline\tcol\tendcol\tmessage\n")
        for f, ln, col, ec, msg in all_f:
            fh.write("%s\t%d\t%d\t%d\t%s\n" % (f, ln, col, ec, msg))
    # The comparison key: message text is tool-specific, position is not.
    norm = sorted({"%s:%d:%d" % (f, ln, col) for f, ln, col, _, _ in all_f})
    with open(os.path.join(a.out, "findings.norm"), "w") as fh:
        fh.write("\n".join(norm) + ("\n" if norm else ""))
    with open(os.path.join(a.out, "manifest.tsv"), "w") as fh:
        fh.write("file\tstatus\tdegraded\texit\tsecs\tn_findings\tn_unparsed_lines\tn_parse_recoveries\n")
        for r in recs:
            fh.write("%s\t%s\t%d\t%s\t%s\t%d\t%d\t%d\n" % (
                r["file"], r["status"], int(r["degraded"]), r["exit"], r["secs"],
                r["n_findings"], r["n_unparsed_lines"], r["n_parse_recoveries"]))
    with open(os.path.join(a.out, "diagnostics.txt"), "w") as fh:
        for r in recs:
            if r["diag"]:
                fh.write("### %s [%s]\n%s\n\n" % (r["file"], r["status"], r["diag"]))

    from collections import Counter
    st = Counter(r["status"] for r in recs)
    degraded_files = sum(1 for r in recs if r["degraded"])
    summary = {
        "rule": os.path.relpath(rule, a.root),
        "spatch": subprocess.run([spatch, "--version"], capture_output=True, text=True).stdout.split("\n")[0],
        "options": opts, "mode": a.mode, "finding_mode": a.finding_mode,
        "controls": ctl,
        "files_in": len(files), "files_accounted": len(recs),
        "status_counts": dict(st),
        "findings_total": len(all_f),
        "findings_normalised_unique": len(norm),
        "files_with_findings": st["HIT"],
        "files_no_rules_apply": st["NO_RULES"],
        "files_script_error": st["SCRIPT_ERROR"],
        "files_skipped_or_failed": st["TIMEOUT"] + st["NOT_HANDLED"] + st["POISON"]
                                   + st["SCRIPT_ERROR"]
                                   + sum(v for k, v in st.items() if k.startswith("EXIT")),
        "files_partially_unparsed": degraded_files,
        "unparsed_lines_total": sum(r["n_unparsed_lines"] for r in recs),
        "parse_recoveries_total": sum(r["n_parse_recoveries"] for r in recs),
        "files_fully_analysed": len(recs) - degraded_files
                                - (st["TIMEOUT"] + st["NOT_HANDLED"] + st["POISON"]),
        "spatch_version_banner": subprocess.run([spatch, "--version"], capture_output=True,
                                                text=True).stdout.strip().split("\n"),
        "wall_seconds": round(wall, 1),
        "cpu_seconds_sum": round(sum(r["secs"] for r in recs), 1),
        "slowest_files": [(r["file"], r["secs"]) for r in sorted(recs, key=lambda r: -r["secs"])[:5]],
    }
    json.dump(summary, open(os.path.join(a.out, "summary.json"), "w"), indent=1)

    print()
    print("=" * 68)
    for k, v in summary.items():
        if k not in ("controls", "slowest_files"):
            print("%-32s %s" % (k, v))
    print("slowest_files:")
    for f, s in summary["slowest_files"]:
        print("   %7.2fs  %s" % (s, f))
    if summary["files_partially_unparsed"]:
        print("\nNOTE: %d of %d file(s) had code the C parser skipped. A zero finding "
              "in those regions is not evidence of absence."
              % (summary["files_partially_unparsed"], len(recs)))
    if summary["files_skipped_or_failed"]:
        print("\nWARNING: %d file(s) did not complete cleanly; see %s/diagnostics.txt"
              % (summary["files_skipped_or_failed"], a.out))

if __name__ == "__main__":
    main()
