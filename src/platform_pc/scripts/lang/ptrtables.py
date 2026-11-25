"""Exact French addresses of strings listed in C tables of pointers.

A table initializer of the English sources lists strings in order, e.g.
    { .text = gEasyChatWord_Play, ... }, { .text = gEasyChatWord_Love, ... }
    sPotionDesc, sAntidoteDesc, ...           (const u8 *const [])
The French ROM has the same table with French pointers at base + k*stride.
Strings whose French address is already known (most of them) vote for
(base, stride): their pointers are searched in the ROM. Then every entry of
the French table gives the exact French address of its string - which also
corrects strings that were aligned at the wrong place (lists sorted in
another order in French, e.g. the Easy Chat words).
"""
import collections
import re
import struct

TOP_INIT = re.compile(rb"^[^\n;{}]*\b(\w+)\s*(?:\[[^\]]*\])+\s*=\s*\{", re.M)
REF = re.compile(rb"(?:\.(\w+)\s*=\s*)?\b([A-Za-z_]\w*)\b")


def table_lists(pp_text):
    """[(table, field, [symbols])] of the top-level initializers of a file."""
    out = []
    for m in TOP_INIT.finditer(pp_text):
        start = m.end()
        depth, i = 1, start
        while i < len(pp_text) and depth:
            c = pp_text[i]
            if c == 0x7B:
                depth += 1
            elif c == 0x7D:
                depth -= 1
            i += 1
        body = pp_text[start:i - 1]
        groups = collections.OrderedDict()
        for r in REF.finditer(body):
            field = r.group(1).decode() if r.group(1) else ""
            groups.setdefault(field, []).append(r.group(2).decode())
        for field, syms in groups.items():
            out.append((m.group(1).decode(), field, syms))
    return out


def pointer_positions(rom, value, cache):
    if value in cache:
        return cache[value]
    pat = struct.pack("<I", value + 0x08000000)
    res, p = [], rom.find(pat)
    while p >= 0:
        if p % 4 == 0:
            res.append(p)
        p = rom.find(pat, p + 1)
    cache[value] = res
    return res


class TextPointers:
    """Aligned ROM words that point to a text unit, for structural search."""

    def __init__(self, rom, looks_like_text):
        import array
        words = array.array("I")
        words.frombytes(rom[:len(rom) // 4 * 4])
        self.words = words
        self.rom = rom
        is_text = {}
        flags = bytearray(len(words))
        for k, w in enumerate(words):
            if 0x08000000 <= w < 0x08000000 + len(rom):
                off = w - 0x08000000
                t = is_text.get(off)
                if t is None:
                    j = rom.find(b"\xff", off, off + 1200)
                    t = j >= 0 and looks_like_text(rom[off:j + 1])
                    is_text[off] = t
                if t:
                    flags[k] = 1
        self.flags = bytes(flags)

    def find_table(self, en_texts, match_score):
        """French table of len(en_texts) pointers whose texts match best."""
        n = len(en_texts)
        run = b"\x01" * n
        best, best_score = None, 0
        p = self.flags.find(run)
        while p >= 0:
            score = 0
            for k in range(n):
                off = self.words[p + k] - 0x08000000
                j = self.rom.find(b"\xff", off, off + 1200)
                if match_score(en_texts[k], self.rom[off:j + 1]) is not None:
                    score += 1
            if score > best_score:
                best, best_score = p, score
            p = self.flags.find(run, p + 1)
        if best is None or best_score < 0.7 * n:
            return {}
        return {k: self.words[best + k] - 0x08000000 for k in range(n)}


def resolve(rom, syms, known, cache):
    """syms: ordered symbols (only those with a known French address vote).
    Returns {index: french_offset} read from the French table, or {}."""
    pts = [(k, known[s]) for k, s in enumerate(syms) if s in known]
    if len(pts) < 3:
        return {}
    votes = collections.Counter()
    occ = {k: pointer_positions(rom, off, cache) for k, off in pts}
    ks = [k for k, _ in pts]
    for a, b in zip(ks, ks[1:]):
        for pa in occ[a][:8]:
            for pb in occ[b][:8]:
                d = pb - pa
                if d > 0 and d % (b - a) == 0:
                    stride = d // (b - a)
                    if stride % 4 == 0 and stride <= 256:
                        votes[(pa - a * stride, stride)] += 1
    if not votes:
        return {}
    (base, stride), v = votes.most_common(1)[0]
    if v < max(2, len(pts) // 4):
        return {}
    out = {}
    for k in range(len(syms)):
        p = base + k * stride
        if p < 0 or p + 4 > len(rom):
            break
        w = struct.unpack_from("<I", rom, p)[0]
        if 0x08000000 <= w < 0x08000000 + len(rom):
            out[k] = w - 0x08000000
    return out
