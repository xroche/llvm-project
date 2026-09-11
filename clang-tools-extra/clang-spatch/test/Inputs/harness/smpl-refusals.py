#!/usr/bin/env python3
"""Name every construct in a .cocci file that falls outside the documented SmPL
subset (smpl-subset-grammar.md sections 1 and 2).

Operates on the rules that are live for a given -D mode, because a rule gated on
a virtual that is not set never fires and its constructs never reach the matcher.
Returns a sorted list of canonical refusal names, matching the vocabulary of
clang-spatch's Refusal::Construct field.
"""
import os, re, sys, importlib.util

SCOPE = "/tmp/claude-1000/-home-roche-git-AlgoliaSaaS/32e3b5b7-d685-4d3a-9c3a-2452dfcf7530/scratchpad/cocci-scope"
spec = importlib.util.spec_from_file_location("c2", os.path.join(SCOPE, "classify2.py"))
c2 = importlib.util.module_from_spec(spec); spec.loader.exec_module(c2)
c1 = c2.c1

SUBSET_MV = {"expression", "identifier", "statement", "type", "constant", "position"}

# metavariable kinds outside the six, longest-first
OUT_MV = [
    ("local idexpression", "local idexpression metavariable"),
    ("global idexpression", "global idexpression metavariable"),
    ("local function", "local function metavariable"),
    ("expression list", "expression list metavariable"),
    ("identifier list", "identifier list metavariable"),
    ("parameter list", "parameter list metavariable"),
    ("initialiser list", "initialiser list metavariable"),
    ("initializer list", "initializer list metavariable"),
    ("statement list", "statement list metavariable"),
    ("field list", "field list metavariable"),
    ("format list", "format list metavariable"),
    ("declaration name", "declaration name metavariable"),
    ("assignment operator", "assignment operator metavariable"),
    ("binary operator", "binary operator metavariable"),
    ("fresh identifier", "fresh identifier"),
    ("attribute name", "attribute name declaration"),
    ("iterator name", "iterator name declaration"),
    ("declarer name", "declarer name declaration"),
    ("idexpression", "idexpression metavariable"),
    ("metavariable", "metavariable (any kind)"),
    ("initialiser", "initialiser metavariable"),
    ("initializer", "initializer metavariable"),
    ("declaration", "declaration metavariable"),
    ("parameter", "parameter metavariable"),
    ("attribute", "attribute metavariable"),
    ("pragmainfo", "pragmainfo metavariable"),
    ("comments", "comments metavariable"),
    ("iterator", "iterator metavariable"),
    ("declarer", "declarer metavariable"),
    ("typedef", "typedef declaration"),
    ("symbol", "symbol declaration"),
    ("function", "function metavariable"),
    ("format", "format metavariable"),
    ("field", "field metavariable"),
    ("error", "error metavariable"),
    ("operator", "operator metavariable"),
]

OPCHARS = set("&|+-*/%^<>=!?:~")

def _classify_dots(core, ctx=None):
    """Name every `...` in a line that is not a statement-level ellipsis.

    The subset admits statement-level dots only.  Argument, parameter,
    expression, initialiser and field level dots are all separate refusals, and
    the level is decided by the neighbouring non-space characters rather than by
    a whole-line pattern, because one line can carry dots at two levels
    (`f(...,x,...)` inside a `when != ` tail, for instance).
    """
    names = set()
    # nested-dot delimiters are reported by the caller; blank them out first
    t = re.sub(r"<\+?\.\.\.", "  ", core)
    t = re.sub(r"\.\.\.\+?>", "  ", t)
    i = 0
    while True:
        j = t.find("...", i)
        if j < 0:
            break
        i = j + 3
        pre = t[:j].rstrip()
        post = t[j + 3:].lstrip()
        pc = pre[-1] if pre else ""
        fc = post[0] if post else ""
        if pc == "(" or fc == ")" or pc == "," or fc == ",":
            # `f(...)`, `f(x, ...)`, `T f(...)` -- the parameter form is only
            # distinguishable with a declaration parse, so both land here.
            names.add("argument-level or parameter-level ellipsis")
        elif pc == "{" and re.search(r"=\s*\{$", pre):
            names.add("initialiser-level ellipsis")
        elif pc == "{" and re.search(r"\b(struct|union)\b", pre):
            names.add("field-level ellipsis")
        elif pc == "{" and re.search(r"\benum\b", pre):
            names.add("enumerator-level ellipsis")
        elif not pre and ctx == "record":
            names.add("field-level ellipsis")
        elif not pre and ctx == "enum":
            names.add("enumerator-level ellipsis")
        elif not pre and ctx == "initialiser":
            names.add("initialiser-level ellipsis")
        elif (pc in OPCHARS) or (fc in OPCHARS):
            names.add("expression-level ellipsis")
    return sorted(names)

def _mark_strip(s):
    return re.sub(r"^\s*[-+*?]+\s*", "", s)

def normalise(clean):
    """Put a rule header, its declarations and the closing @@ on separate lines.

    `@r@ expression E; @@` and `@@ statement s1; @@` are legal SmPL that a
    line-oriented reader silently drops: the header regex needs the line to end
    at the closing @.  1 kernel rule and 30-odd Coccinelle tests are written
    that way.  Line numbers shift after this, so refusal lines are reported
    against the normalised text.
    """
    out = []
    for ln in clean.split("\n"):
        m = re.match(r"^(@[^@\n]*@)[ \t]+(\S.*)$", ln)
        if m:
            head, rest = m.group(1), m.group(2)
            out.append(head)
            m2 = re.match(r"^(.*?)[ \t]*@@[ \t]*$", rest)
            if m2:
                if m2.group(1).strip():
                    for d in m2.group(1).split(";"):
                        if d.strip():
                            out.append(d.strip() + ";")
                out.append("@@")
            else:
                out.append(rest)
            continue
        out.append(ln)
    return "\n".join(out)

def live_rules_sat(path, active=("report",)):
    """Rules that can fire with only `active` virtuals set.

    A dependency on a rule name is unknown at classification time, so the rule
    counts as live if the expression is satisfiable for some assignment of the
    rule names.  classify2.eval_depends pins them to True instead, which drops
    every rule guarded by `depends on !<rulename>` -- the suppression idiom
    misc/struct_size.cocci is built on.
    """
    raw = open(path, encoding="utf-8", errors="replace").read()
    clean = normalise(c1.strip_comments(raw))
    rules = c1.split_rules(clean)
    virtuals = set()
    for m in re.finditer(r"^\s*virtual\s+([A-Za-z_0-9, \t]+)$", clean, re.M):
        virtuals |= {v.strip() for v in m.group(1).split(",") if v.strip()}
    live = []
    for hdr, mv, body in rules:
        dm = re.search(r"depends on\s+(.*?)\s*$", hdr)
        expr = dm.group(1) if dm else ""
        if _satisfiable(expr, virtuals, set(active)):
            live.append((hdr, mv, body))
    return raw, clean, rules, live, virtuals

def _satisfiable(expr, virtuals, active):
    if not expr.strip():
        return True
    names = [n for n in dict.fromkeys(re.findall(r"[A-Za-z_]\w*", expr))
             if n not in ("ever", "never", "exists", "forall", "file", "in")
             and n not in virtuals]
    if len(names) > 12:
        return True
    import itertools
    for bits in itertools.product([True, False], repeat=len(names)):
        env = dict(zip(names, bits))
        toks, out = re.findall(r"\(|\)|&&|\|\||!|[A-Za-z_]\w*", expr), []
        for t in toks:
            if re.match(r"^[A-Za-z_]\w*$", t):
                if t in ("ever", "never", "exists", "forall"):
                    out.append("True")
                elif t in virtuals:
                    out.append("True" if t in active else "False")
                else:
                    out.append("True" if env.get(t, True) else "False")
            elif t == "&&": out.append(" and ")
            elif t == "||": out.append(" or ")
            elif t == "!":  out.append(" not ")
            else:           out.append(t)
        try:
            if eval("".join(out)):
                return True
        except Exception:
            return True
    return False

def analyse(path, active=("report",), all_rules=False):
    raw, clean, rules, live, virtuals = live_rules_sat(path, active)
    use = rules if all_rules else live
    R = {}                      # name -> first line number seen
    lines = clean.split("\n")

    def add(name, ln=0):
        if name not in R:
            R[name] = ln

    def lineno(needle, start=0):
        for i, l in enumerate(lines[start:], start + 1):
            if needle in l:
                return i
        return 0

    # ---- file-level ------------------------------------------------------
    if re.search(r'^\s*using\s+"', clean, re.M):
        add('using "..." isomorphism file', lineno("using \""))
    m = re.search(r"^[ \t]*#[ \t]*spatch\b", clean, re.M)
    if m:
        add("#spatch embedded options", clean[:m.start()].count("\n") + 1)
    if re.search(r"\bvirtual\s*\.\s*\w+", clean):
        add("virtual.x command-line metavariable value",
            lineno("virtual."))

    for hdr, mv, body in use:
        h = " ".join(hdr.split())
        hln = lineno("@" + hdr.rstrip()) if hdr.strip() else 0

        # ---- rule header -------------------------------------------------
        if h.startswith("script:ocaml"):
            add("script:ocaml rule", hln)
        if h.startswith("initialize:ocaml") or h.startswith("finalize:ocaml"):
            add("@initialize:ocaml@ / @finalize:ocaml@ rule", hln)
        if h.startswith("initialize:python") or h.startswith("finalize:python"):
            add("@initialize:python@ / @finalize:python@ rule", hln)
        dm = re.search(r"depends on\s+(.*)$", h)
        if dm:
            e = dm.group(1).strip()
            # A boolean over virtuals only is how all 76 kernel rules select
            # their output mode; the scoping analysis counts it as base.  Mixing
            # a rule name into the boolean is a different construct, because the
            # dependency then has to be evaluated per match.
            enames = {n for n in re.findall(r"[A-Za-z_]\w*", e)
                      if n not in ("ever", "never", "exists", "forall", "file", "in")}
            rulenames = enames - virtuals
            if rulenames and re.search(r"!|&&|\|\|", e):
                if "!" in e:
                    add("depends on negated rule", hln)
                if "&&" in e or "||" in e:
                    add("depends on boolean expression over a rule name", hln)
            if re.match(r"^file in\b", e):
                add("depends on file in", hln)
            if re.match(r"^(ever|never)\b", e):
                add("depends on ever / never", hln)
        if re.search(r"\bextends\s+\w+", h):
            add("extends", hln)
        if re.search(r"\bdisable\s+\w+", h):
            add("disable <isomorphism>", hln)
        if "generated" in h.split():
            add("@generated@ rule", hln)
        if re.search(r"(^|\s)(expression|identifier|type)\s*$", h) and not h.startswith("script:"):
            add("expression / identifier / type rule kind", hln)

        script = h.startswith("script:") or h.startswith("initialize:") or h.startswith("finalize:")

        # ---- metavariable declarations ------------------------------------
        if not script:
            for stmt in mv.split(";"):
                s = " ".join(stmt.split())
                if not s:
                    continue
                sln = lineno(stmt.strip()) if stmt.strip() else 0
                if s.startswith("?") or s.startswith("+"):
                    add("arity prefix on a metavariable declaration", sln)
                if "=~" in s or "!~" in s:
                    add("regex constraint on a metavariable", sln)
                if re.search(r"\[\s*\]", s):
                    add("array-typed metavariable declaration", sln)
                if "<=" in s:
                    add("metavariable bound to a subterm of an inherited one (<=)", sln)
                    continue
                if re.search(r"(!=|=)\s*\{", s):
                    add("set-valued metavariable constraint", sln)
                elif re.search(r"^(?:expression|identifier|constant|type|statement)\b[^:]*?(!=|=)\s*[^{\s]", s) \
                        and not re.search(r"script\s*:", s) and "position" not in s:
                    add("value constraint on a metavariable", sln)
                if re.match(r"^position\b", s) and "!=" in s and "." not in s.split("!=")[1]:
                    pass
                if re.search(r"script\s*:\s*python", s):
                    add("python constraint on a metavariable", sln)
                if re.search(r"script\s*:\s*ocaml", s):
                    add("ocaml constraint on a metavariable", sln)
                if re.match(r"^position\s+any\b", s):
                    add("position any", sln)
                hit = None
                for k, name in OUT_MV:
                    if re.match(re.escape(k) + r"(\s|$)", s):
                        hit = name
                        break
                if hit:
                    add(hit, sln)
                    continue
                first = s.split()[0]
                if first in SUBSET_MV:
                    # `expression E : struct *;` type-restricted form
                    if ":" in s and not re.search(r"script\s*:", s):
                        add("type-restricted metavariable declaration", sln)
                elif first == "virtual":
                    pass
                elif re.match(r"^(const |volatile )*(int|char|void|long|short|unsigned|"
                              r"signed|float|double|_Bool|struct|union|enum)\b", s):
                    add("metavariable restricted to a concrete C type", sln)
                elif re.match(r"^[A-Za-z_][\w.]*[\s\*]+[A-Za-z_]", s):
                    add("metavariable typed by another metavariable", sln)

        # ---- script bodies ------------------------------------------------
        if script:
            for ln0 in mv.split("\n"):
                s = ln0.strip().rstrip(";")
                if not s:
                    continue
                if "<<" not in s and re.match(r"^[A-Za-z_]\w*$", s):
                    add("script output variable (declared without <<)", lineno(ln0.strip()))
                if re.match(r"^\(.*,.*\)\s*<<", s):
                    add("script (str, ast) binding", lineno(ln0.strip()))
                if re.search(r"<<[^=]*=", s):
                    add("script binding with a default value", lineno(ln0.strip()))
                if re.search(r"<<\s*merge\s*\.", s):
                    add("script merge variable", lineno(ln0.strip()))
            for ln0 in body.split("\n"):
                s = ln0.strip()
                if not s or s.startswith("#"):
                    continue
                if re.match(r"^(msg\w*|m)\s*=", s):
                    continue
                if re.match(r"^coccilib\.report\.print_report\(", s):
                    continue
                if re.match(r"^coccilib\.org\.print_todo\(", s):
                    continue
                if re.match(r"^print\s*\(", s) or re.match(r"^print\s+", s):
                    continue
                if re.match(r"^(cocci|coccilib)\.", s):
                    add("coccilib call other than print_report/print_todo",
                        lineno(s))
                    continue
                add("script:python body beyond print_report/print_todo/print", lineno(s))
            continue

        # ---- body constructs ---------------------------------------------
        bracectx = []
        for i, ln0 in enumerate(body.split("\n")):
            s = ln0.rstrip()
            core = _mark_strip(s)
            ctx = bracectx[-1] if bracectx else None
            bln = lineno(s.strip()) if s.strip() else 0
            if re.match(r"^\s*[-+*]?\s*\?", s) or re.match(r"^\?", s):
                add("? optional line marker", bln)
            if re.match(r"^\+\+", s):
                add("++ line marker", bln)
            if re.match(r"^(---|\+\+\+)\s", s):
                add("--- / +++ filespec header", bln)
            if r"\(" in s or r"\|" in s or r"\)" in s:
                add(r"backslash disjunction \( \| \)", bln)
            if re.search(r"<\+?\.\.\.", s) or re.search(r"\.\.\.\+?>", s):
                add("nested dots <... ...>", bln)
            for name in _classify_dots(core, ctx):
                add(name, bln)
            for ch_i, ch in enumerate(core):
                if ch == "{":
                    pre_b = core[:ch_i].rstrip()
                    if re.search(r"=\s*$", pre_b):
                        bracectx.append("initialiser")
                    elif re.search(r"\benum\b", pre_b):
                        bracectx.append("enum")
                    elif re.search(r"\b(struct|union)\b", pre_b) or \
                         re.match(r"^[A-Za-z_]\w*(\s*\*)*\s*$", pre_b):
                        bracectx.append("record")
                    else:
                        bracectx.append("block")
                elif ch == "}" and bracectx:
                    bracectx.pop()
            if re.search(r"\bwhen\b", s) and re.search(r"\([^()]*\bwhen\b", s):
                add("argument-level when", bln)
            if re.search(r"\bwhen\s+forall\b", s):
                add("when forall", bln)
            if re.search(r"\bwhen\s+exists\b", s):
                add("when exists", bln)
            if re.search(r"\bwhen\s*==", s):
                add("when == code", bln)
            if re.search(r"\bwhen\s*!=\s*(true|false)\b", s):
                add("when != true / when != false", bln)
            if s.endswith("\\"):
                add("backslash line continuation", bln)
            if re.match(r"^\s*[-+*]?\s*&\s*$", s) or re.search(r"^\s*\\&", s):
                add("conjunction ( & )", bln)
            if re.match(r"^\s*[-+*]?\s*#(include|define|undef|pragma|if|ifdef|else|endif)", s):
                add("preprocessor directive pattern", bln)
            if re.search(r"\b(template|namespace|class|typename|decltype|operator|throw|"
                         r"catch|constexpr|nullptr|co_return|co_await|co_yield|static_cast|"
                         r"dynamic_cast|reinterpret_cast|const_cast)\b", core) \
               or re.search(r"\b(public|private|protected)\s*:", core) \
               or re.search(r"\bnew\s+[A-Za-z_]", core) or re.search(r"\bdelete\s+[A-Za-z_]", core) \
               or "::" in core or "[[" in core:
                add("C++ construct in the pattern", bln)
            if re.match(r"^\s*[-+*]?\s*(switch|case|default)\b", core):
                add("switch / case pattern", bln)
            if re.match(r"^\s*[-+*]?\s*goto\b", core):
                add("goto pattern", bln)
            if re.match(r"^\s*[-+*]?\s*(do|while)\b.*\{?\s*$", core) and re.match(r"^\s*do\b", core):
                add("do/while pattern", bln)
            # position attachments in refused places
            for m in re.finditer(r"@[A-Za-z_]\w*", s):
                pre = s[:m.start()].rstrip()
                if pre.endswith(("...", ">", "|", "(", ";", "{", "}", ",")):
                    add("position attached to an ellipsis, a delimiter or a separator", bln)
                elif pre and pre[-1] in OPCHARS and not pre.endswith("..."):
                    add("position attached to an operator", bln)
                if s[:m.start()] != pre and pre:
                    add("whitespace between token and @", bln)
            if re.search(r"\bwhen\b.*@[A-Za-z_]", s):
                add("position inside when code", bln)
            if not re.match(r"^\s*(if|while|for|switch|return|do|else)\b", core) and (
                    re.match(r"^\s*[A-Za-z_]\w*\s*\([^;]*\)\s*\{\s*$", core) or
                    re.match(r"^\s*[A-Za-z_][\w \t\*]*[\s\*]+[A-Za-z_]\w*\s*\([^;]*\)\s*\{?\s*$", core)):
                add("function definition pattern", bln)

        # function definition across lines: `T f(...) {` handled above; prototype:
        for ln0 in body.split("\n"):
            core = _mark_strip(ln0.rstrip())
            if re.match(r"^[A-Za-z_][\w \t\*]*\s+[A-Za-z_]\w*\s*\([^)]*\)\s*;\s*$", core):
                add("function prototype pattern", lineno(ln0.strip()))

    # `virtual` itself: in the base per the scoping analysis, flagged separately
    return sorted(R.items(), key=lambda kv: (kv[1], kv[0])), virtuals, len(rules), len(live)

if __name__ == "__main__":
    for p in sys.argv[1:]:
        r, v, nt, nl = analyse(p)
        print("== %s  (rules %d, live-report %d)" % (p, nt, nl))
        for name, ln in r:
            print("   %-58s line %s" % (name, ln))
