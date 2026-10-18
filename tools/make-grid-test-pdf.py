#!/usr/bin/env python3
"""Deterministic 36-page mixed-aspect PDF used by grid worker priority tests."""
from pathlib import Path


def build(count=36):
    objects = [b"", b""]
    content = b"0.15 0.2 0.8 rg\n12 12 35 35 re f\n"
    objects.append(b"<< /Length %d >>\nstream\n%sendstream" % (len(content), content))
    pages = []
    for i in range(count):
        w, h = ((792, 450) if i % 7 == 0 else (612, 792))
        pages.append(len(objects) + 1)
        objects.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Contents 3 0 R >>" % (w, h))
    objects[1] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (
        b" ".join(b"%d 0 R" % i for i in pages), count)
    objects[0] = b"<< /Type /Catalog /Pages 2 0 R >>"
    data = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, obj in enumerate(objects, 1):
        offsets.append(len(data))
        data.extend(b"%d 0 obj\n%s\nendobj\n" % (i, obj))
    xref = len(data)
    data.extend(b"xref\n0 %d\n0000000000 65535 f \n" % (len(offsets)))
    for offset in offsets[1:]:
        data.extend(b"%010d 00000 n \n" % offset)
    data.extend(b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(offsets), xref))
    return data


if __name__ == "__main__":
    path = Path(__file__).resolve().parent.parent / "tests/data/folio-grid.pdf"
    path.write_bytes(build())
    print(path, path.stat().st_size)
