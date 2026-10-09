#!/usr/bin/env python3
"""Emit the CJK corpus fixture: one A4 page of Japanese set in an embedded,
subsetted CID-keyed font.

Why this exists
---------------
`documents/Japanese_Government_Notice.pdf` is the only fixture that exercises
the paths Latin files never touch: a Type0 composite font, Identity-H
two-byte encoding, a CIDToGIDMap, and a ToUnicode CMap that text extraction
has to walk to get real codepoints back out. It used to be an opaque binary
with no recipe, so when it was damaged there was no way to rebuild it. Now
there is.

Everything is emitted by hand rather than through a PDF library, for the same
reason `make-sample-pdf.py` is: when PDFium and we disagree about a glyph
box, the input has to be a file whose every byte we chose.

Usage:  python3 tools/make-cjk-pdf.py documents/Japanese_Government_Notice.pdf
"""
import subprocess
import sys
import zlib

from fontTools import subset
from fontTools.ttLib import TTFont

W, H = 595.276, 841.890  # A4, points

# A plausible municipal notice. Mixed kanji / hiragana / katakana, full-width
# punctuation and digits, so the extractor meets more than one Unicode block.
TITLE = "\u5e02\u6c11\u306e\u7686\u69d8\u3078\u306e\u304a\u77e5\u3089\u305b"
SUBTITLE = "\u4ee4\u548c\u516b\u5e74\u56db\u6708\u4e00\u65e5\u3000\u7dcf\u52d9\u90e8\u5e83\u5831\u8ab2"
BODY = [
    "\u5e73\u7d20\u3088\u308a\u5e02\u653f\u306b\u5bfe\u3057\u307e\u3057\u3066\u3054\u7406\u89e3\u3068\u3054\u5354\u529b\u3092\u8cdc\u308a\u3001",
    "\u539a\u304f\u5fa1\u793c\u7533\u3057\u4e0a\u3052\u307e\u3059\u3002\u3055\u3066\u3001\u4e0b\u8a18\u306e\u3068\u304a\u308a",
    "\u7a93\u53e3\u696d\u52d9\u306e\u53d6\u6271\u6642\u9593\u3092\u5909\u66f4\u3044\u305f\u3057\u307e\u3059\u306e\u3067\u3001",
    "\u304a\u77e5\u3089\u305b\u3044\u305f\u3057\u307e\u3059\u3002",
    "",
    "\u4e00\u3001\u5b9f\u65bd\u65e5\u3000\u4ee4\u548c\u516b\u5e74\u4e94\u6708\u5341\u4e94\u65e5\uff08\u6728\u66dc\u65e5\uff09",
    "\u4e8c\u3001\u5bfe\u8c61\u7a93\u53e3\u3000\u5e02\u6c11\u8ab2\u3001\u7a0e\u52d9\u8ab2\u3001\u56fd\u6c11\u5065\u5eb7\u4fdd\u967a\u8ab2",
    "\u4e09\u3001\u65b0\u3057\u3044\u6642\u9593\u3000\u5348\u524d\u516b\u6642\u4e09\u5341\u5206\u304b\u3089\u5348\u5f8c\u4e94\u6642\u307e\u3067",
    "\u56db\u3001\u4f11\u6b62\u65e5\u3000\u571f\u66dc\u65e5\u3001\u65e5\u66dc\u65e5\u53ca\u3073\u795d\u65e5",
    "",
    "\u306a\u304a\u3001\u30de\u30a4\u30ca\u30f3\u30d0\u30fc\u30ab\u30fc\u30c9\u3092\u3054\u5229\u7528\u306e\u65b9\u306f\u3001",
    "\u30b3\u30f3\u30d3\u30cb\u30a8\u30f3\u30b9\u30b9\u30c8\u30a2\u306e\u7aef\u672b\u3067\u3082\u8a3c\u660e\u66f8\u3092",
    "\u53d6\u5f97\u3067\u304d\u307e\u3059\u3002\u8a73\u3057\u304f\u306f\u5e02\u516c\u5f0f\u30b5\u30a4\u30c8\u3092\u3054\u89a7\u304f\u3060\u3055\u3044\u3002",
    "",
    "\u304a\u554f\u5408\u305b\u3000\u96fb\u8a71\uff10\uff13\uff0d\uff15\uff17\uff10\uff19\uff0d\uff12\uff14\uff16\uff18",
    "\u53d7\u4ed8\u6642\u9593\u3000\u5e73\u65e5\u3000\uff18\uff1a\uff13\uff10\u301c\uff11\uff17\uff1a\uff10\uff10",
]
FOOTER = "\u2015\u3000\uff11\u3000\u2015"

SRC = "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc"
PSNAME = "NotoSerifJP-Regular"


def build_subset(chars, dest):
    """Cut the 20 MB pan-CJK collection down to just the glyphs used."""
    font = TTFont(SRC, fontNumber=0)  # face 0 is the JP face
    opts = subset.Options()
    opts.drop_tables += ["BASE", "JSTF", "DSIG", "vhea", "vmtx"]
    opts.layout_features = []
    opts.notdef_outline = True
    opts.recalc_bounds = True
    s = subset.Subsetter(options=opts)
    s.populate(text="".join(chars))
    s.subset(font)
    font.flavor = None
    font.save(dest)
    return TTFont(dest)


def main(out):
    # No .strip() filter here: U+3000 IDEOGRAPHIC SPACE is whitespace to
    # Python but a real, width-carrying glyph in the font. Dropping it from
    # the subset renders it as .notdef - a tofu box in the middle of the text.
    used = sorted({c for line in [TITLE, SUBTITLE, FOOTER] + BODY for c in line})
    sub = build_subset(used, "/tmp/_cjksub.ttf")
    raw = open("/tmp/_cjksub.ttf", "rb").read()

    cmap = sub.getBestCmap()
    order = sub.getGlyphOrder()
    gid_of = {g: i for i, g in enumerate(order)}
    upem = sub["head"].unitsPerEm
    hmtx = sub["hmtx"]

    def gid(ch):
        return gid_of.get(cmap.get(ord(ch)), 0)

    def adv(ch):
        g = cmap.get(ord(ch))
        return int(round(hmtx[g][0] * 1000.0 / upem)) if g else 1000

    def show(text, size, x, y):
        """Identity-H: every glyph is two bytes of GID."""
        hexs = "".join("%04X" % gid(c) for c in text)
        return "BT /F1 %g Tf 1 0 0 1 %.1f %.1f Tm <%s> Tj ET" % (size, x, y, hexs)

    # --- page content -----------------------------------------------------
    ops = ["0 g"]
    ops.append(show(TITLE, 20, 72, H - 96))
    ops.append("0.45 0.45 0.5 rg")
    ops.append(show(SUBTITLE, 9.5, 72, H - 118))
    ops.append("0 g")
    ops.append("0.82 0.82 0.86 RG 0.8 w 72 %.1f m %.1f %.1f l S" % (H - 132, W - 72, H - 132))
    y = H - 168
    for line in BODY:
        if line:
            ops.append(show(line, 11.5, 72, y))
        y -= 26
    ops.append("0.45 0.45 0.5 rg")
    ops.append(show(FOOTER, 9, W / 2 - 22, 56))
    content = zlib.compress("\n".join(ops).encode("latin-1"))

    # --- widths (W array, only non-default advances) ----------------------
    widths = {}
    for ch in used:
        g = gid(ch)
        if g:
            widths[g] = adv(ch)
    wparts, keys = [], sorted(widths)
    i = 0
    while i < len(keys):
        j = i
        while j + 1 < len(keys) and keys[j + 1] == keys[j] + 1:
            j += 1
        wparts.append("%d [%s]" % (keys[i], " ".join(str(widths[k]) for k in keys[i:j + 1])))
        i = j + 1
    warray = " ".join(wparts)

    # --- ToUnicode CMap: without this, extraction returns GIDs, not text --
    pairs = "".join("<%04X> <%04X>\n" % (gid(c), ord(c)) for c in used if gid(c))
    tounicode = (
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n"
        + "".join(
            "%d beginbfchar\n%sendbfchar\n" % (len(ch), "".join(ch))
            for ch in [pairs.splitlines(keepends=True)[k:k + 100]
                       for k in range(0, len(pairs.splitlines()), 100)]
        )
        + "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend"
    ).encode("latin-1")
    tounicode = zlib.compress(tounicode)

    bbox = (sub["head"].xMin, sub["head"].yMin, sub["head"].xMax, sub["head"].yMax)
    sc = 1000.0 / upem
    objs = {}
    objs[1] = b"<< /Type /Catalog /Pages 2 0 R /Lang (ja-JP) >>"
    objs[2] = b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>"
    objs[3] = ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.3f %.3f] "
               "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>" % (W, H)).encode()
    objs[4] = b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(content) + content + b"\nendstream"
    objs[5] = ("<< /Type /Font /Subtype /Type0 /BaseFont /%s /Encoding /Identity-H "
               "/DescendantFonts [6 0 R] /ToUnicode 9 0 R >>" % PSNAME).encode()
    objs[6] = ("<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s "
               "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
               "/FontDescriptor 7 0 R /DW 1000 /W [%s] /CIDToGIDMap /Identity >>"
               % (PSNAME, warray)).encode()
    objs[7] = ("<< /Type /FontDescriptor /FontName /%s /Flags 4 "
               "/FontBBox [%d %d %d %d] /ItalicAngle 0 /Ascent %d /Descent %d "
               "/CapHeight %d /StemV 80 /FontFile2 8 0 R >>"
               % (PSNAME, bbox[0] * sc, bbox[1] * sc, bbox[2] * sc, bbox[3] * sc,
                  sub["hhea"].ascender * sc, sub["hhea"].descender * sc,
                  sub["OS/2"].sCapHeight if hasattr(sub["OS/2"], "sCapHeight") else 700)).encode()
    comp = zlib.compress(raw)
    objs[8] = (b"<< /Length %d /Length1 %d /Filter /FlateDecode >>\nstream\n"
               % (len(comp), len(raw))) + comp + b"\nendstream"
    objs[9] = b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(tounicode) + tounicode + b"\nendstream"

    buf = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offs = {}
    for n in sorted(objs):
        offs[n] = len(buf)
        buf += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
    xref = len(buf)
    buf += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for n in sorted(objs):
        buf += b"%010d 00000 n \n" % offs[n]
    buf += (b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objs) + 1, xref))
    open(out, "wb").write(buf)
    print("%s: %d bytes, %d glyphs embedded" % (out, len(buf), len(used)))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "documents/Japanese_Government_Notice.pdf")
