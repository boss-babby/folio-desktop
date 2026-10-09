#!/usr/bin/env python3
"""tools/make-test-pdf.py  --  generates tests/data/folio-test.pdf

A hand-written PDF with geometry chosen so that a rendering bug is visible as
an assertion failure rather than as "the picture looks a bit off".

Why not a real-world PDF? Because a real one cannot answer the question "is
the 90-degree rotation matrix transposed?". This one can: page 1 is four
solid quadrants in four unmistakable colours, so a test can sample one pixel
per quadrant and know exactly which way is up. Sign errors, swapped axes and
off-by-one tile origins all fail loudly.

  page 1  200 x 100 pt, four quadrants
            PDF user space (origin bottom-left):
              red    top-left      green  top-right
              blue   bottom-left   yellow bottom-right
            Device space is top-down, so an unrotated render puts red at
            device (0,0).
  page 2  612 x 792 pt (US Letter), text in Helvetica plus a black border.
            Exercises the text path and non-embedded font substitution.
  page 3  200 x 100 pt with /Rotate 90, identical content to page 1.
            Separates "the PDF says rotate" from "the user asked to rotate",
            which are different inputs that must compose correctly.

No compression, no object streams, no xref streams: when a test fails, the
file should be readable in a text editor.
"""

import pathlib
import sys

QUADRANTS = b"""\
1 0 0 rg
0 50 100 50 re f
0 1 0 rg
100 50 100 50 re f
0 0 1 rg
0 0 100 50 re f
1 1 0 rg
100 0 100 50 re f
0 0 0 RG
2 w
1 1 198 98 re S
"""

LETTER = b"""\
0 0 0 RG
3 w
36 36 540 720 re S
BT
/F1 36 Tf
72 680 Td
(Folio render test) Tj
ET
BT
/F1 14 Tf
72 640 Td
(The quick brown fox jumps over the lazy dog. 0123456789) Tj
ET
0.4 0.1 0.8 rg
72 400 200 120 re f
0 0.6 0.5 rg
320 400 200 120 re f
"""


def build() -> bytes:
    objects: list[bytes] = []

    def add(body: bytes) -> int:
        objects.append(body)
        return len(objects)          # 1-based object number

    def stream(data: bytes) -> bytes:
        return b"<< /Length %d >>\nstream\n%s\nendstream" % (len(data), data)

    # Reserve 1 = Catalog, 2 = Pages; page objects follow.
    objects.append(b"")              # placeholder 1
    objects.append(b"")              # placeholder 2

    font = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
               b"/Encoding /WinAnsiEncoding >>")

    c1 = add(stream(QUADRANTS))
    p1 = add(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
             b"/Contents %d 0 R /Resources << >> >>" % c1)

    c2 = add(stream(LETTER))
    p2 = add(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             b"/Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> >> >>" % (c2, font))

    c3 = add(stream(QUADRANTS))
    p3 = add(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Rotate 90 "
             b"/Contents %d 0 R /Resources << >> >>" % c3)

    kids = b" ".join(b"%d 0 R" % n for n in (p1, p2, p3))
    objects[1] = b"<< /Type /Pages /Kids [%s] /Count 3 >>" % kids
    objects[0] = b"<< /Type /Catalog /Pages 2 0 R >>"

    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0] * (len(objects) + 1)
    for i, body in enumerate(objects, start=1):
        offsets[i] = len(out)
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"

    xref_at = len(out)
    out += b"xref\n0 %d\n" % (len(objects) + 1)
    out += b"0000000000 65535 f \n"
    for i in range(1, len(objects) + 1):
        out += b"%010d 00000 n \n" % offsets[i]
    out += (b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objects) + 1, xref_at))
    return bytes(out)


def main() -> int:
    here = pathlib.Path(__file__).resolve().parent.parent
    dest = here / "tests" / "data" / "folio-test.pdf"
    dest.parent.mkdir(parents=True, exist_ok=True)
    data = build()
    dest.write_bytes(data)
    print(f"wrote {dest} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
