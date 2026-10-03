#!/usr/bin/env python3
"""prof_report.py PROF.BIN ELF [LOG] [--top N] -- per-thread function tables
from the Switch sampling profiler (RECOMP_NX_PROFILE=1, src/switch_nx.c).

Each record is 32 bytes: u16 slot, u16 time (10 ms since boot), u32 pc, u32 lr, u32 ret[5], all
code addresses as offsets into the NRO (= ELF addresses; 0xFFFFFFFF when
outside it). With the log, slots are named by the thread's entry function
(the "[prof] slots:" line; 0 = the game's main thread).

For every busy thread it prints where the samples are (self), and the same
samples charged to the caller one frame up (ret), which separates "slow
because of glDrawElements" from "slow inside Mesa's state validation".
"""
import bisect
import collections
import re
import struct
import subprocess
import sys

NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"


def load_syms(elf):
    out = subprocess.run([NM, "-n", "-C", elf], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def name_of(addrs, names, a):
    if a == 0xFFFFFFFF:
        return "(outside the NRO: kernel/svc)"
    i = bisect.bisect_right(addrs, a) - 1
    return names[i] if i >= 0 else "?"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 25
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
        args = [a for a in args if a != str(top)]
    prof, elf = args[0], args[1]
    t_from = t_to = None
    if "--time" in sys.argv:     # --time A-B: only samples taken A..B s after boot
        a, b = sys.argv[sys.argv.index("--time") + 1].split("-")
        t_from, t_to = float(a), float(b)
        args = [x for x in args if x != sys.argv[sys.argv.index("--time") + 1]]
    log = args[2] if len(args) > 2 else None
    addrs, names = load_syms(elf)

    slot_name = {}
    if log:
        for line in open(log, errors="replace"):
            if "[prof] slots:" in line:
                for m in re.finditer(r" (\d+)=([0-9a-f]+)", line):
                    e = int(m.group(2), 16)
                    slot_name[int(m.group(1))] = "game main" if e == 0 else name_of(addrs, names, e)

    data = open(prof, "rb").read()
    per = collections.defaultdict(lambda: (collections.Counter(), collections.Counter()))
    total = collections.Counter()
    stacks = collections.defaultdict(collections.Counter)
    wraps, last_when = 0, 0
    for off in range(0, len(data) - 31, 32):
        slot, when, pc, lr, *ret = struct.unpack_from("<HHII5I", data, off)
        # The time is 16 bits of 10 ms: it wraps every 655.36 s. Samples are
        # written in order, so a big step back is a wrap.
        if when + 30000 < last_when:
            wraps += 1
        last_when = when
        when += wraps * 65536
        if t_from is not None and not (t_from <= when / 100.0 <= t_to):
            continue
        fn = name_of(addrs, names, pc)
        caller = name_of(addrs, names, ret[0])
        per[slot][0][fn] += 1
        per[slot][1][caller + "  <-  " + fn] += 1
        chain = [fn] + [name_of(addrs, names, r) for r in ret if r != 0xFFFFFFFF]
        stacks[slot][" < ".join(chain)] += 1
        total[slot] += 1

    for slot, n in total.most_common():
        self_c, call_c = per[slot]
        print(f"\n=== slot {slot} ({slot_name.get(slot, '?')}): {n} samples ===")
        print("-- self --")
        for fn, c in self_c.most_common(top):
            print(f"  {100.0 * c / n:5.1f}%  {fn}")
        print("-- caller <- self --")
        for fn, c in call_c.most_common(top):
            print(f"  {100.0 * c / n:5.1f}%  {fn}")
        print("-- whole stacks (self < callers) --")
        for st, c in stacks[slot].most_common(top):
            print(f"  {100.0 * c / n:5.1f}%  {st}")


if __name__ == "__main__":
    main()
