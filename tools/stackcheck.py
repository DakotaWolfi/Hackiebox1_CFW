#!/usr/bin/env python3
"""Worst-case stack per task entry of the linked image: the call graph from
the disassembly of the ELF, the frame sizes from the .su files (-fstack-usage)
of the objects and, for the link-time optimised code, of the LTO link
(BUILD_DIR/lto/*.ltrans.su); VLA bytes from a table (Opus at N = 960, C = 2,
16 kHz output). Library functions without .su count 0 and indirect calls are
not followed: add an allowance.

usage: tools/stackcheck.py ELF BUILD_DIR [IGNORED,FUNCS] ENTRY...
  e.g. tools/stackcheck.py build/cfw.elf build x,y player.o:task cloud.o:task
A function is named by its name, or file.o:name where the name is not
unique (static functions, the source file's base name).
SPDX-License-Identifier: GPL-3.0-or-later
"""
import glob, os, re, subprocess, sys
elf, builddir = sys.argv[1], sys.argv[2]
TOOLS = "../hackiebox_cfw_ng_cfw/arm-sdk/10-2020-q4-major/bin/arm-none-eabi-"
REPO = os.getcwd() + "/"

def base(name):
    """the name without the suffixes link-time optimisation adds"""
    return re.sub(r"(\.lto_priv\.\d+)+$", "", name)

def rel(path):
    path = path.split(":")[0]
    return path[len(REPO):] if path.startswith(REPO) else path

# functions of the image: address -> (name, source file)
funcs = {}
for line in subprocess.run([TOOLS + "nm", "-l", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 3)
    if len(p) >= 3 and p[1] in "tTW":
        funcs[int(p[0], 16) & ~1] = (p[2], rel(p[3]) if len(p) > 3 else "")
by_name = {}
for a, (n, f) in funcs.items():
    by_name.setdefault(base(n), []).append(a)

def label(a):
    n, f = funcs[a]
    b = base(n)
    return b if len(by_name[b]) == 1 else f"{os.path.splitext(os.path.basename(f))[0]}.o:{b}"

# frame sizes: (source file, name) -> bytes; the LTO link's files last, so
# that they replace stale ones of earlier builds without LTO
frame = {}
sus = [s for s in glob.glob(builddir + "/**/*.su", recursive=True) if "/lto/" not in s]
for su in sus + sorted(glob.glob(builddir + "/lto/*.su")):
    for line in open(su):
        loc, size, kind = line.rstrip("\n").split("\t")
        parts = loc.split(":")
        frame[(parts[0], parts[-1])] = int(size)
by_su_name = {}
for (f, n), s in frame.items():
    by_su_name.setdefault(n, []).append(s)

def own(a):
    n, f = funcs[a]
    b = base(n)
    if (f, b) in frame:
        return frame[(f, b)]
    cand = by_su_name.get(b, [])
    return max(cand) if cand else 0

vla = {
    "celt_decode_with_ec": 504 + 44 + 3840 + 7680,
    "quant_all_bands": 2496,
    "bands.o:quant_band": 704,
    "bands.o:deinterleave_hadamard": 352, "bands.o:interleave_hadamard": 352,
    "alg_unquant": 704,
    "compute_allocation": 336,
    "clt_mdct_backward": 3840,
    "celt_decoder.o:celt_decode_lost": 7680 + 3840,
    "pitch_search": 664 + 974 + 1240,
    "_celt_autocorr": 2048,
    "celt_fir": 48 + 2096,
    "celt_iir": 48 + 2208,
    "remove_doubling": 4 * 1025,
    "opus_decoder.o:opus_decode_frame": 16,
}

def vla_of(a):
    n, f = funcs[a]
    b = base(n)
    return vla.get(f"{os.path.splitext(os.path.basename(f))[0]}.o:{b}", vla.get(b, 0))

# call graph: direct calls and tail calls to another function's start
calls, indirect, cur = {}, set(), None
dis = subprocess.run([TOOLS + "objdump", "-d", "--no-show-raw-insn", elf], capture_output=True, text=True).stdout
for line in dis.splitlines():
    m = re.match(r"^([0-9a-f]+) <([^>]+)>:", line)
    if m:
        a = int(m.group(1), 16)
        cur = a if a in funcs else None
        if cur is not None:
            calls.setdefault(cur, set())
        continue
    if cur is None:
        continue
    m = re.search(r"\t(bl|blx|b(?:eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?(?:\.[wn])?|cbn?z)\s+(?:r\d+, )?([0-9a-f]+) <([^>+]+)>", line)
    if m:
        t = int(m.group(2), 16)
        if t in funcs and t != cur:
            calls[cur].add(t)
    elif re.search(r"\tblx\s+r\d", line):
        indirect.add(cur)

# celt_decode_lost is called before the VLAs of celt_decode_with_ec exist.
NO_VLA = {("celt_decode_with_ec", "celt_decode_lost")}
IGNORE = set(sys.argv[3].split(",")) if len(sys.argv) > 3 and "," in sys.argv[3] else set()
memo = {}

def depth(f, stack=()):
    if f in stack:
        return 0, [label(f) + "(recursion)"]
    if f in memo:
        return memo[f]
    best, path, bestown = 0, [], 0
    for g in calls.get(f, ()):
        if label(g) in IGNORE:
            continue
        d, p = depth(g, stack + (f,))
        o = own(f) + (0 if (base(funcs[f][0]), base(funcs[g][0])) in NO_VLA else vla_of(f))
        if o + d > bestown + best:
            best, path, bestown = d, p, o
    if not path:
        bestown = own(f) + vla_of(f)
    memo[f] = (bestown + best, [f"{label(f)}:{bestown}"] + path)
    return memo[f]

def find(entry):
    if ":" in entry:
        obj, n = entry.split(":")
        stem = obj[:-2]
        hits = [a for a in by_name.get(n, []) if os.path.splitext(os.path.basename(funcs[a][1]))[0] == stem]
    else:
        hits = by_name.get(entry, [])
    if len(hits) != 1:
        sys.exit(f"stackcheck: {entry}: {len(hits)} functions match")
    return hits[0]

for entry in [a for a in sys.argv[3:] if "," not in a]:
    d, p = depth(find(entry))
    print(f"{entry}: {d} bytes ({(d+3)//4} words)")
    print("   " + " > ".join(p[:40]))

print("functions with indirect calls:", " ".join(sorted(label(a) for a in indirect)))
