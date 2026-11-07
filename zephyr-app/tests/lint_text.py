#!/usr/bin/env python3
"""Check that every text the UI writes fits the 7-segment display and is readable.

Big field = 8 digits, small field = 4 digits. A '.' does not use a digit (it lights the dp of the
digit before it). Letters that look wrong on 7 segments (K Q V W X) must not be used.
Run:  python3 tests/lint_text.py src/ui.c
"""
import re
import sys

READABLE = set("0123456789 -.") | set("ABCDEFGHIJLMNOPSTUYZ") | set("bcdefghnoprtu")
BAD_LETTERS = set("KQVWXkqvwxjzailmsyG")  # readable only as another letter; G/a/i/l/m/s/y kept out of new text
# letters that were already used by the original firmware and are fine
LEGACY_OK = set("aGsyR")  # R: labels SAMR / TARF / SR- come from the original firmware


def shown_len(fmt, sub):
    """Replace printf specs by worst-case text and count the digits."""
    out = ""
    i = 0
    while i < len(fmt):
        if fmt[i] != "%":
            out += fmt[i]
            i += 1
            continue
        m = re.match(r"%(\.?)(\d*)(l|u)*([dusx])", fmt[i:])
        if not m:
            i += 1
            continue
        dot, num, _, kind = m.groups()
        i += m.end()
        n = int(num) if num else 1
        if kind == "s":
            out += sub.get(fmt, "8" * (n if dot else 4))  # serial is hex
        else:
            out += "8" * n
    return out


def digits(text):
    return len(text.replace(".", ""))


def main(path):
    src = open(path, encoding="utf-8").read()
    problems = 0
    checked = 0
    # snprintf(big, ..., "fmt"  /  snprintf(small_, ..., "fmt"  /  menu_pick(k, "LABEL", id)
    pats = [
        (r'snprintf\(big,\s*sizeof\(big\),\s*"((?:[^"\\]|\\.)*)"', 8),
        (r'snprintf\(small_,\s*sizeof\(small_\),\s*"((?:[^"\\]|\\.)*)"', 4),
        (r'menu_pick\(\d,\s*"((?:[^"\\]|\\.)*)"', 4),
        (r'#define ERR_\w+\s+"((?:[^"\\]|\\.)*)"', 8),
        (r'\?\s*"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"', 8),
    ]
    seen = set()
    for pat, width in pats[:4]:
        for m in re.finditer(pat, src):
            seen.add((m.group(1), width))
    # texts chosen with ?: inside snprintf(big, ...)
    for m in re.finditer(r'snprintf\(big,[^;]*?;', src, re.S):
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(0)):
            seen.add((lit, 8))
    for lit, width in sorted(seen):
        if lit in ("%s",):
            continue
        sub = {}
        if "%s" in lit and "SOFT" in lit:
            sub[lit] = "0.1"
        text = shown_len(lit, sub)
        n = digits(text)
        checked += 1
        if n > width:
            print(f"TOO LONG  ({n} > {width}): {lit!r}")
            problems += 1
        for ch in set(text):
            if ch not in READABLE and ch not in LEGACY_OK and ch != "8":
                print(f"NOT READABLE char {ch!r} in {lit!r}")
                problems += 1
    print(f"{checked} texts checked, {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "src/ui.c"))
