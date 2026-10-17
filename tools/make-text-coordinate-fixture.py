#!/usr/bin/env python3
"""Recreate the small crop/rotation PDF used by the text-layer regression.

Uses built-in Helvetica so no external fonts or network are needed.
Run from the repository root: python3 tools/make-text-coordinate-fixture.py
"""
from pathlib import Path

out = Path(__file__).resolve().parent.parent / "tests/data/text-coordinate-fixture.pdf"
objects = []

def add(body: bytes) -> int:
    objects.append(body)
    return len(objects)

catalog = add(b"<< /Type /Catalog /Pages 2 0 R >>")
pages = add(b"<< /Type /Pages /Count 3 /Kids [4 0 R 6 0 R 8 0 R] >>")
font = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
for i, (crop, rotation) in enumerate([
    (b"[30 20 230 120]", 0),
    (b"[30 20 230 120]", 90),
    (b"[-20 40 180 140]", 180),
]):
    page_no = add(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] "
                  + b"/CropBox " + crop + b" /Rotate " + str(rotation).encode()
                  + b" /Resources << /Font << /F1 3 0 R >> >> "
                  + f"/Contents {5 + i*2} 0 R >>".encode())
    assert page_no == 4 + 2 * i
    stream = (b"BT /F1 24 Tf 1 0 0 1 50 80 Tm (Folio) Tj ET\n"
              b"BT /F1 18 Tf 1 0 0 1 260 170 Tm (Hidden) Tj ET\n")
    add(b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n"
        + stream + b"endstream")

content = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
offsets = [0]
for i, obj in enumerate(objects, 1):
    offsets.append(len(content))
    content.extend(f"{i} 0 obj\n".encode() + obj + b"\nendobj\n")
xref = len(content)
content.extend(f"xref\n0 {len(objects)+1}\n0000000000 65535 f \n".encode())
for offset in offsets[1:]:
    content.extend(f"{offset:010} 00000 n \n".encode())
content.extend((f"trailer\n<< /Size {len(objects)+1} /Root {catalog} 0 R >>\n"
                f"startxref\n{xref}\n%%EOF\n").encode())
out.write_bytes(content)
print(out, len(content))
