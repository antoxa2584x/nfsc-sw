"""Mark the lifted code's poll loops so a spinning guest thread gives way.

An Xbox title waits on hardware and on other threads with loops like
D3D_BlockOnTime's last one:

    loc_002E9057: ;
        ecx = MEM32(edx);
        ...
        if (CMP_B(_fa, _fb)) goto loc_002E9057;

A single-core console time-slices that thread against whoever will end the
wait. A host runs it flat out on a core of its own, which on a Switch is a
third of the machine spent re-reading one word, while the pushbuffer thread
it waits for runs on what is left.

This pass finds the loops that can only be waiting -- one block, a backward
goto to its own label, no stores, no calls, and memory read only at addresses
the loop does not change -- and puts RECOMP_SPIN_HINT() on the back edge
(recomp_types.h: a CPU pause/yield hint, and a host thread yield every 64
turns). A loop that walks memory (strlen, memchr) changes its address
registers and is left alone, as is anything that writes.

Like mmio_rewrite, it works on the C text.
"""

import re

_LABEL = re.compile(r"^(loc_[0-9A-Fa-f]+): ;\s*$")
_ANY_LABEL = re.compile(r"^\s*loc_[0-9A-Fa-f]+: ;")
_REGS = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp")
_REG_WORD = re.compile(r"(?<![A-Za-z0-9_])(" + "|".join(_REGS) + r")(?![A-Za-z0-9_])")
_REG_ASSIGN = re.compile(r"(?<![A-Za-z0-9_.>])(" + "|".join(_REGS) +
                         r")\s*(?:<<=|>>=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|=(?!=)|\+\+|--)")
_READ = re.compile(r"(?<![A-Za-z0-9_])(?:S?MEM(?:8|16|32|64|F|D)|MMIO_RD(?:8|16|32))\(")
# Anything that writes memory, calls out, or touches the stack.
_FORBIDDEN = re.compile(
    r"(?<![A-Za-z0-9_])(?:sub_[0-9A-Fa-f]{8}\w*\s*\(|RECOMP_ICALL|ICALL|PUSH\d*\(|POP\d*\(|"
    r"MMIO_WR|RECOMP_ATOMIC|__sync|__atomic|return\b|longjmp|recomp_|fp_)")
_MEM_STORE = re.compile(r"(?<![A-Za-z0-9_])S?MEM(?:8|16|32|64|F|D)\([^;]*\)\s*"
                        r"(?:<<=|>>=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|=(?!=))")
_MAX_BODY = 40


def _paren_arg(s, i):
    """Text of the call whose '(' is at s[i-1]; i is just past it."""
    depth, j = 1, i
    while j < len(s) and depth:
        if s[j] == "(":
            depth += 1
        elif s[j] == ")":
            depth -= 1
        j += 1
    return s[i:j - 1]


def _is_poll(body):
    text = "\n".join(body)
    if _FORBIDDEN.search(text) or _MEM_STORE.search(text):
        return False
    reads = list(_READ.finditer(text))
    if not reads:
        return False                    # a pure register loop: a computation
    changed = set(_REG_ASSIGN.findall(text))
    for m in reads:
        if changed & set(_REG_WORD.findall(_paren_arg(text, m.end()))):
            return False                # walks memory
    # No loop-carried registers: a poll reloads everything it tests on every
    # turn. A register read before this turn writes it (a counter, `edx++`,
    # `eax = eax + 0xC`) makes it a loop that computes, which ends by itself.
    written = set()
    for ln in body:
        code = ln.split("/*", 1)[0]
        a = _REG_ASSIGN.search(code)
        if a:
            rhs = code[:a.start()] + code[a.end():]
            if code[a.end() - 2:a.end()] in ("++", "--") or not code[a.end() - 1] == "=" \
                    or code[a.end() - 2:a.end()] in ("+=", "-=", "|=", "&=", "^=", "*=", "/=", "%="):
                rhs += " " + a.group(1)      # compound: reads its own target
            if (set(_REG_WORD.findall(rhs)) & changed) - written:
                return False
            written.add(a.group(1))
        elif (set(_REG_WORD.findall(code)) & changed) - written:
            return False
    return True


def rewrite(code):
    """Return (code, number of loops marked)."""
    lines = code.split("\n")
    out = list(lines)
    marked = 0
    i = 0
    while i < len(lines):
        m = _LABEL.match(lines[i].strip())
        if not m:
            i += 1
            continue
        label = m.group(1)
        back = "goto " + label + ";"
        j = i + 1
        while j < len(lines) and j - i <= _MAX_BODY:
            ln = lines[j]
            if _ANY_LABEL.match(ln) or ln.startswith("}"):
                break
            if "goto " in ln:
                break
            j += 1
        if j < len(lines) and j - i <= _MAX_BODY:
            last = lines[j]
            s = last.strip()
            if (s.startswith("if (") and re.search(re.escape(back) + r"\s*(?:/\*.*\*/)?$", s)
                    and last.count("goto ") == 1
                    and _is_poll(lines[i + 1:j])):
                k = last.index(back)
                out[j] = last[:k] + "{ RECOMP_SPIN_HINT(); " + back + " }" + last[k + len(back):]
                marked += 1
        i = j if j > i else i + 1
    return "\n".join(out), marked
