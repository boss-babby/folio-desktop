#!/usr/bin/env python3
"""Emit a real, multi-page PDF for Folio to open.

Deliberately hand-rolled rather than pulled from a library: the point is to
have a file whose bytes we control, so that when PDFium disagrees with us
about a glyph position we know the input exactly. It uses only base-14
fonts, so there is nothing to embed and the file stays a few kilobytes.
"""
import sys, zlib

W, H = 595.276, 841.890  # A4 in points


def esc(s):
    return s.replace("\\", r"\\").replace("(", r"\(").replace(")", r"\)")


def page_stream(n, total):
    """Content stream for page n: heading, body copy, a rule, a bar chart."""
    out = []
    # heading
    out.append("BT /F2 22 Tf 1 0 0 1 64 %.1f Tm (%s) Tj ET" % (H - 92, esc(TITLES[(n - 1) % len(TITLES)])))
    out.append("BT /F1 9 Tf 0.45 0.45 0.5 rg 1 0 0 1 64 %.1f Tm (Folio sample document  \\267  page %d of %d) Tj ET"
               % (H - 112, n, total))
    out.append("0 g")
    # rule
    out.append("0.82 0.82 0.86 RG 0.8 w 64 %.1f m %.1f %.1f l S" % (H - 124, W - 64, H - 124))

    y = H - 158
    for para in BODY:
        for line in para:
            out.append("BT /F1 10.5 Tf 1 0 0 1 64 %.1f Tm (%s) Tj ET" % (y, esc(line)))
            y -= 15.5
        y -= 10

    # bar chart
    y -= 6
    out.append("BT /F2 11 Tf 1 0 0 1 64 %.1f Tm (Revenue by quarter) Tj ET" % y)
    y -= 16
    base = y - 96
    vals = [0.42, 0.68, 0.55, 0.88, 0.73, 0.95]
    for i, v in enumerate(vals):
        x = 64 + i * 46
        h = 96 * v * (0.6 + 0.4 * ((n % 5) / 4.0))
        out.append("0.31 0.27 0.90 rg %.1f %.1f %.1f %.1f re f" % (x, base, 30, h))
        out.append("BT /F1 8 Tf 0.45 0.45 0.5 rg 1 0 0 1 %.1f %.1f Tm (Q%d) Tj ET"
                   % (x + 8, base - 13, i + 1))
    out.append("0 g")

    # footnote
    out.append("BT /F3 9.5 Tf 0.35 0.35 0.42 rg 1 0 0 1 64 86 Tm "
               "(Rendered by PDFium inside Folio. Text is real: select it, search it, zoom it.) Tj ET")
    out.append("BT /F1 9 Tf 0.55 0.55 0.6 rg 1 0 0 1 %.1f 86 Tm (%d) Tj ET" % (W - 72, n))
    return "\n".join(out).encode("latin-1")


TITLES = [
    "Annual Report 2026", "Executive Summary", "Quarterly Overview",
    "Highlights", "Revenue by Region", "Regional Trends",
    "Customer Growth", "Cost Structure", "Operating Margin",
    "Outlook and Risks", "Appendix A", "Appendix B",
]

BODY = [
    ["Revenue grew 12% over the previous quarter, led by enterprise",
     "subscriptions and a stronger renewal rate across all regions. The",
     "pipeline entering the next quarter is the largest on record."],
    ["Operating costs held steady while the team expanded, and the margin",
     "improved by two points compared with the same period last year.",
     "Headcount growth was concentrated in engineering and support."],
    ["The board approved an additional allocation for infrastructure,",
     "which is expected to reduce unit costs materially by year end."],
]


def build(path, pages=24):
    objs = {}
    font_ids = {"F1": 5, "F2": 6, "F3": 7}
    first_page_obj = 8

    objs[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    kids = " ".join("%d 0 R" % (first_page_obj + 2 * i) for i in range(pages))
    objs[2] = ("<< /Type /Pages /Count %d /Kids [%s] >>" % (pages, kids)).encode()
    objs[3] = ("<< /Title (Annual Report 2026) /Author (Folio) "
               "/Creator (folio tools/make-sample-pdf.py) >>").encode()
    objs[4] = b"<< >>"
    objs[5] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
    objs[6] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>"
    objs[7] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Times-Italic /Encoding /WinAnsiEncoding >>"

    for i in range(pages):
        pobj = first_page_obj + 2 * i
        cobj = pobj + 1
        res = "<< /Font << %s >> >>" % " ".join(
            "/%s %d 0 R" % (k, v) for k, v in font_ids.items())
        objs[pobj] = ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.3f %.3f] "
                      "/Resources %s /Contents %d 0 R >>" % (W, H, res, cobj)).encode()
        raw = page_stream(i + 1, pages)
        comp = zlib.compress(raw, 9)
        objs[cobj] = (b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(comp)
                      + comp + b"\nendstream")

    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    for num in sorted(objs):
        offsets[num] = len(out)
        out += b"%d 0 obj\n" % num + objs[num] + b"\nendobj\n"

    xref_at = len(out)
    maxobj = max(objs) + 1
    out += b"xref\n0 %d\n" % maxobj
    out += b"0000000000 65535 f \n"
    for num in range(1, maxobj):
        out += b"%010d 00000 n \n" % offsets.get(num, 0)
    out += (b"trailer\n<< /Size %d /Root 1 0 R /Info 3 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (maxobj, xref_at))

    with open(path, "wb") as f:
        f.write(out)
    return len(out)


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "sample.pdf"
    pages = int(sys.argv[2]) if len(sys.argv) > 2 else 24
    n = build(path, pages)
    print("%s: %d pages, %d bytes" % (path, pages, n))
