"""Sequence alignment of English strings with French ROM units.

Both lists are in the same order (same source files, same compiler), but the
French ROM also holds units that are not strings of this list (other data
between them), and lengths differ. Needleman-Wunsch with:
  match   +score if skeletons are equal (more for distinctive skeletons),
          lengths plausible for a translation
  skip FR unit  small cost (data, padding, strings of another table)
  skip EN unit  larger cost (an English string with no French counterpart)
"""
import re
from textlib import skeleton, pages


def charmap(path="charmap.txt"):
    dec = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^'(.)'\s*=\s*([0-9A-Fa-f]{2})\s*$", line.strip())
        if m:
            dec.setdefault(int(m.group(2), 16), m.group(1))
    return dec


def decode(b, dec):
    out = []
    i = 0
    while i < len(b):
        c = b[i]
        if c == 0xFF:
            break
        if c == 0xFE:
            out.append("\\n")
        elif c in (0xFA, 0xFB):
            out.append("\\l" if c == 0xFA else "\\p")
        elif c in (0xFC, 0xFD) and i + 1 < len(b):
            out.append("{%02X:%02X}" % (c, b[i + 1]))
            i += 1
        else:
            out.append(dec.get(c, "?"))
        i += 1
    return "".join(out)


def match_score(en, fr):
    se, sf = skeleton(en), skeleton(fr)
    if se != sf:
        return None
    le, lf = len(en), len(fr)
    if le <= 2 or lf <= 2:
        return 1.0 if le == lf else (0.5 if abs(le - lf) <= 3 else None)
    r = lf / le
    if r < 0.4 or r > 2.8:
        return None
    # French runs ~15% longer; closeness of length and page count add up
    import math
    closeness = max(0.0, 1.5 - abs(math.log(r / 1.15)) * 2)
    pe, pf = pages(en), pages(fr)
    page_bonus = 1.0 if pe == pf else (0.5 if abs(pe - pf) <= 1 else 0.0)
    return 1.0 + 2.0 * min(len(se), 4) + closeness + page_bonus + min(le, 200) / 100


def align(en, fr, skip_fr=0.3, skip_en=2.0):
    """en: [bytes], fr: [bytes]. Returns list of (i_en, j_fr) pairs."""
    n, m = len(en), len(fr)
    NEG = -1e18
    # band-free DP (n*m up to a few 10^7 is fine in chunks)
    score = [[NEG] * (m + 1) for _ in range(n + 1)]
    back = [[0] * (m + 1) for _ in range(n + 1)]
    for j in range(m + 1):
        score[0][j] = -skip_fr * j
        back[0][j] = 2
    for i in range(1, n + 1):
        score[i][0] = -skip_en * i
        back[i][0] = 1
        row, prev = score[i], score[i - 1]
        e = en[i - 1]
        for j in range(1, m + 1):
            best, how = prev[j] - skip_en, 1           # skip EN
            v = row[j - 1] - skip_fr                   # skip FR
            if v > best:
                best, how = v, 2
            s = match_score(e, fr[j - 1])
            if s is not None and prev[j - 1] + s > best:
                best, how = prev[j - 1] + s, 3
            row[j] = best
            back[i][j] = how
    i, j, pairs = n, max(range(m + 1), key=lambda k: score[n][k]), []
    while i > 0 and j > 0:
        h = back[i][j]
        if h == 3:
            pairs.append((i - 1, j - 1))
            i, j = i - 1, j - 1
        elif h == 1:
            i -= 1
        else:
            j -= 1
    return pairs[::-1]
