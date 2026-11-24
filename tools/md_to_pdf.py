#!/usr/bin/env python3
"""Minimal Markdown -> PDF converter (no third-party PDF library): headings, paragraphs, bullet/numbered lists, tables, code blocks, block quotes,
images (embedded as JPEG), **bold**, `code` and links (shown as their text). Text is real, selectable PDF text in the standard Helvetica/Courier fonts.

  python3 tools/md_to_pdf.py QUICKSTART.md docs/MPC2500-Emulator-Quick-Start.pdf --title "MPC2500 Emulator - Quick Start Guide"

Needs Pillow and the Liberation Sans fonts (metric-compatible with Helvetica) only to measure text width and to convert images."""
import argparse, io, os, re, sys, zlib
from PIL import Image, ImageFont

PAGE_W, PAGE_H, MARGIN = 595.28, 841.89, 54.0
BODY, CODE = 10.0, 8.2
FONT_FILES = {"F1": "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "F2": "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"}
PDF_FONTS = {"F1": "Helvetica", "F2": "Helvetica-Bold", "F3": "Courier", "F4": "Helvetica-Oblique"}
_measure = {k: ImageFont.truetype(v, 100) for k, v in FONT_FILES.items()}
SUBS = {"→": "->", "≥": ">=", "≤": "<=", "≈": "~", "–": "-", "—": "-", "…": "...", "‘": "'", "’": "'", "“": '"', "”": '"',
        " ": " ", "•": "*", "×": "x", "✓": "ok", "✔": "ok"}


def clean(t):
    for a, b in SUBS.items(): t = t.replace(a, b)
    return t.encode("cp1252", "replace").decode("cp1252")


def width(text, font, size):
    if font == "F3": return 0.6 * size * len(text)
    f = "F2" if font == "F2" else "F1"
    return _measure[f].getlength(text) * size / 100.0


def esc(s): return s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def cells_h_next(doc, rows, cw, size, pad, lead, ri):
    if ri + 1 >= len(rows): return 0
    r = rows[ri + 1]
    return max(len(doc.wrap(r[j] if j < len(r) else "", cw[j] - 2 * pad, size)) for j in range(len(cw))) * lead + 2 * pad


class Doc:
    def __init__(self, title, root):
        self.title, self.root = title, root
        self.pages, self.cur, self.y = [], None, 0
        self.images = []                # (name, jpeg bytes, w, h)
        self.new_page()

    def new_page(self):
        if self.cur is not None: self.pages.append(self.cur)
        self.cur = []; self.y = PAGE_H - MARGIN

    def ensure(self, h):
        if self.y - h < MARGIN + 18: self.new_page()

    def emit(self, s): self.cur.append(s)

    def text(self, x, y, font, size, s, color=(0, 0, 0)):
        self.emit("BT %.3f %.3f %.3f rg /%s %.2f Tf %.2f %.2f Td (%s) Tj ET" % (*color, font, size, x, y, esc(clean(s))))

    def rect(self, x, y, w, h, fill=None, stroke=None, lw=0.6):
        c = ""
        if fill: c += "%.3f %.3f %.3f rg " % fill
        if stroke: c += "%.3f %.3f %.3f RG %.2f w " % (*stroke, lw)
        op = "B" if fill and stroke else "f" if fill else "S"
        self.emit("%s%.2f %.2f %.2f %.2f re %s" % (c, x, y, w, h, op))

    def line(self, x1, y1, x2, y2, color=(0.6, 0.6, 0.6), lw=0.6):
        self.emit("%.3f %.3f %.3f RG %.2f w %.2f %.2f m %.2f %.2f l S" % (*color, lw, x1, y1, x2, y2))

    # ---- inline text ----
    def spans(self, text, base="F1"):
        """[(word, font)] with a trailing-space flag, from markdown inline markup"""
        text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text)
        text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)                       # links -> text
        out = []
        for part in re.split(r"(\*\*[^*]+\*\*|`[^`]+`)", text):
            if not part: continue
            if part.startswith("**") and part.endswith("**"): font, body = "F2", part[2:-2]
            elif part.startswith("`") and part.endswith("`"): font, body = "F3", part[1:-1]
            else: font, body = base, part
            for w in re.split(r"(\s+)", body):
                if w: out.append((w, font))
        return out

    def wrap(self, text, avail, size, base="F1"):
        lines, cur, curw = [], [], 0.0
        for w, f in self.spans(text, base):
            ww = width(clean(w), f, size if f != "F3" else size * 0.92)
            if w.isspace():
                if cur: cur.append((" ", f)); curw += ww
                continue
            if curw + ww > avail and cur:
                while cur and cur[-1][0] == " ": cur.pop()
                lines.append(cur); cur, curw = [], 0.0
            cur.append((w, f)); curw += ww
        while cur and cur[-1][0] == " ": cur.pop()
        if cur or not lines: lines.append(cur)
        return lines

    def draw_line(self, x, y, segs, size, color=(0, 0, 0)):
        for w, f in segs:
            fs = size * 0.92 if f == "F3" else size
            if f == "F3" and w.strip(): self.rect(x - 0.8, y - 2.2, width(clean(w), f, fs) + 1.6, fs + 2.6, fill=(0.93, 0.93, 0.93))
            self.text(x, y, f, fs, w, color); x += width(clean(w), f, fs)

    def paragraph(self, text, indent=0.0, size=BODY, base="F1", gap=5.0, bullet=None, color=(0, 0, 0)):
        avail = PAGE_W - 2 * MARGIN - indent
        lines = self.wrap(text, avail - (14 if bullet else 0), size, base); lead = size * 1.38
        first = True
        for ln in lines:
            self.ensure(lead); self.y -= lead
            if bullet and first: self.text(MARGIN + indent, self.y, "F1", size, bullet, color)
            self.draw_line(MARGIN + indent + (14 if bullet else 0), self.y, ln, size, color); first = False
        self.y -= gap

    def heading(self, level, text, need=70.0):
        size = {1: 22, 2: 14.5, 3: 11.5}.get(level, 10.5); lead = size * 1.3
        self.ensure(max(lead + 24, need)); self.y -= (14 if level > 1 else 6)
        self.y -= lead; self.text(MARGIN, self.y, "F2", size, text, (0.07, 0.17, 0.35) if level < 3 else (0.15, 0.15, 0.15))
        if level == 2: self.line(MARGIN, self.y - 4, PAGE_W - MARGIN, self.y - 4, (0.75, 0.78, 0.84), 0.8)
        self.y -= (8 if level == 2 else 4)

    def code(self, lines):
        maxc = int((PAGE_W - 2 * MARGIN - 14) / (0.6 * CODE)); wrapped = []
        for l in lines:
            l = l.rstrip("\n").replace("\t", "    ")
            while len(l) > maxc: wrapped.append(l[:maxc]); l = "      " + l[maxc:]
            wrapped.append(l)
        lead = CODE * 1.32; i = 0
        while i < len(wrapped):
            self.ensure(lead * 3)
            room = int((self.y - MARGIN - 18) / lead); chunk = wrapped[i:i + max(room, 1)]; h = len(chunk) * lead + 8
            self.rect(MARGIN, self.y - h, PAGE_W - 2 * MARGIN, h, fill=(0.955, 0.96, 0.965), stroke=(0.85, 0.86, 0.88), lw=0.5)
            yy = self.y - 5 - CODE
            for l in chunk: self.text(MARGIN + 7, yy, "F3", CODE, l, (0.1, 0.1, 0.12)); yy -= lead
            self.y -= h; i += len(chunk)
        self.y -= 7

    def quote(self, text, danger=False):
        """Block quote. danger=True (a '> [!CAUTION]' quote) draws a red warning box; text may be a list of paragraphs."""
        avail = PAGE_W - 2 * MARGIN - 22; size = 9.4 if not danger else 9.6
        paras = text if isinstance(text, list) else [text]; lines = []
        for k, ptxt in enumerate(paras):
            if k: lines.append(None)
            lines += self.wrap(ptxt, avail, size)
        lead = size * 1.38; i = 0
        fill, bar, col = ((1.0, 0.9, 0.9), (0.8, 0.05, 0.05), (0.55, 0.0, 0.0)) if danger else ((0.985, 0.965, 0.9), (0.85, 0.6, 0.2), (0, 0, 0))
        if danger and len(lines) * lead + 8 < PAGE_H - 2 * MARGIN - 40: self.ensure(len(lines) * lead + 14)       # a warning box is kept on one page
        while i < len(lines):
            self.ensure(lead * 2)
            room = max(int((self.y - MARGIN - 18) / lead), 1); chunk = lines[i:i + room]; h = len(chunk) * lead + 8
            self.rect(MARGIN, self.y - h, PAGE_W - 2 * MARGIN, h, fill=fill, stroke=bar if danger else None, lw=0.8); self.rect(MARGIN, self.y - h, 4 if danger else 3, h, fill=bar)
            yy = self.y - 6 - size + 2
            for ln in chunk:
                if ln is not None: self.draw_line(MARGIN + 12, yy, ln, size, col)
                yy -= lead
            self.y -= h; i += len(chunk)
        self.y -= 7

    def image(self, path, maxw=470):
        full = os.path.join(self.root, path)
        if not os.path.exists(full): self.paragraph("[missing image: %s]" % path, color=(0.7, 0, 0)); return
        im = Image.open(full).convert("RGB"); w0, h0 = im.size
        scale = min(1.0, maxw / (w0 * 0.5)) * 0.5 if w0 > 700 else 0.5          # LCD pictures at half pixel size, the big panel pictures fitted
        wp = min(maxw, w0 * (0.43 if h0 > 500 else 0.5)); hp = wp * h0 / w0
        self.ensure(hp + 12)
        buf = io.BytesIO(); im.save(buf, "JPEG", quality=88); name = "Im%d" % len(self.images); self.images.append((name, buf.getvalue(), w0, h0))
        x = (PAGE_W - wp) / 2; self.y -= hp
        self.emit("q %.2f 0 0 %.2f %.2f %.2f cm /%s Do Q" % (wp, hp, x, self.y, name)); self.y -= 9

    def table(self, rows):
        ncol = max(len(r) for r in rows); avail = PAGE_W - 2 * MARGIN; pad = 4.0; size = 8.6
        nat = [0.0] * ncol
        for r in rows:
            for j, c in enumerate(r):
                words = self.spans(c)
                nat[j] = max(nat[j], min(260.0, sum(width(clean(w), f, size) for w, f in words)) + 2 * pad)
        mins = [max(40.0, min(nat[j], 70.0)) for j in range(ncol)]
        cw = nat[:]
        if sum(cw) > avail:
            extra = avail - sum(mins); span = sum(max(nat[j] - mins[j], 0) for j in range(ncol)) or 1
            cw = [mins[j] + max(nat[j] - mins[j], 0) * extra / span for j in range(ncol)]
        else:
            cw = [c + (avail - sum(cw)) * c / sum(cw) for c in cw]
        lead = size * 1.32
        for ri, r in enumerate(rows):
            cells = [self.wrap(r[j] if j < len(r) else "", cw[j] - 2 * pad, size, "F2" if ri == 0 else "F1") for j in range(ncol)]
            h = max(len(c) for c in cells) * lead + 2 * pad - 1
            if ri <= 1: self.ensure(h + (cells_h_next(self, rows, cw, size, pad, lead, ri) if ri == 0 else 0) + 4)
            else: self.ensure(h + 2)
            top = self.y
            if ri == 0: self.rect(MARGIN, top - h, sum(cw), h, fill=(0.89, 0.92, 0.97))
            elif ri % 2 == 0: self.rect(MARGIN, top - h, sum(cw), h, fill=(0.975, 0.978, 0.985))
            x = MARGIN
            for j in range(ncol):
                yy = top - pad - size + 1.5
                for ln in cells[j]: self.draw_line(x + pad, yy, ln, size); yy -= lead
                x += cw[j]
            self.line(MARGIN, top - h, MARGIN + sum(cw), top - h, (0.8, 0.82, 0.86), 0.5)
            self.y -= h
        self.line(MARGIN, self.y + 0, MARGIN + sum(cw), self.y + 0, (0.8, 0.82, 0.86), 0.5); self.y -= 8

    # ---- output ----
    def save(self, path):
        self.pages.append(self.cur); n = len(self.pages)
        objs = []                                                    # 1-indexed objects as bytes
        def add(b): objs.append(b); return len(objs)
        cat = add(b""); pages_id = add(b"")
        fonts = {k: add(("<< /Type /Font /Subtype /Type1 /BaseFont /%s /Encoding /WinAnsiEncoding >>" % v).encode()) for k, v in PDF_FONTS.items()}
        imgs = {}
        for name, data, w, h in self.images:
            imgs[name] = add(("<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %d >>\nstream\n" % (w, h, len(data))).encode() + data + b"\nendstream")
        page_ids = []
        for i, content in enumerate(self.pages):
            footer = "BT 0.45 0.45 0.45 rg /F1 8 Tf %.2f 30 Td (%s) Tj ET\nBT 0.45 0.45 0.45 rg /F1 8 Tf %.2f 30 Td (%d / %d) Tj ET" % (
                MARGIN, esc(clean(self.title)), PAGE_W - MARGIN - 28, i + 1, n)
            stream = zlib.compress(("\n".join(content) + "\n" + footer).encode("latin-1", "replace"))
            cid = add(("<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(stream)).encode() + stream + b"\nendstream")
            res = "<< /Font << %s >> /XObject << %s >> >>" % (" ".join("/%s %d 0 R" % (k, v) for k, v in fonts.items()), " ".join("/%s %d 0 R" % (k, v) for k, v in imgs.items()))
            page_ids.append(add(("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.2f %.2f] /Contents %d 0 R /Resources %s >>" % (pages_id, PAGE_W, PAGE_H, cid, res)).encode()))
        objs[cat - 1] = ("<< /Type /Catalog /Pages %d 0 R >>" % pages_id).encode()
        objs[pages_id - 1] = ("<< /Type /Pages /Kids [%s] /Count %d >>" % (" ".join("%d 0 R" % p for p in page_ids), n)).encode()
        info = add(("<< /Title (%s) /Creator (md_to_pdf.py) >>" % esc(clean(self.title))).encode())
        out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"); offs = []
        for i, o in enumerate(objs, 1):
            offs.append(len(out)); out += ("%d 0 obj\n" % i).encode() + o + b"\nendobj\n"
        x = len(out); out += ("xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)).encode()
        for o in offs: out += ("%010d 00000 n \n" % o).encode()
        out += ("trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, cat, info, x)).encode()
        open(path, "wb").write(out)


def convert(md_path, pdf_path, title):
    root = os.path.dirname(os.path.abspath(md_path)); doc = Doc(title, root)
    lines = open(md_path, encoding="utf-8").read().split("\n"); i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith("```"):
            j = i + 1; block = []
            while j < len(lines) and not lines[j].startswith("```"): block.append(lines[j]); j += 1
            doc.code(block); i = j + 1; continue
        m = re.match(r"^(#{1,4})\s+(.*)", l)
        if m:
            nxt = next((x for x in lines[i + 1:] if x.strip()), "")
            doc.heading(len(m.group(1)), re.sub(r"[*`]", "", re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", m.group(2))), need=400.0 if nxt.startswith("![") else 90.0); i += 1; continue
        if re.match(r"^\s*---+\s*$", l): doc.line(MARGIN, doc.y - 4, PAGE_W - MARGIN, doc.y - 4); doc.y -= 12; i += 1; continue
        m = re.match(r"^!\[[^\]]*\]\(([^)]+)\)\s*$", l)
        if m: doc.image(m.group(1)); i += 1; continue
        if l.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                if not all(re.match(r"^:?-{2,}:?$", c) for c in cells if c): rows.append(cells)
                i += 1
            if rows: doc.table(rows)
            continue
        if l.startswith(">"):
            buf = []
            while i < len(lines) and lines[i].startswith(">"): buf.append(lines[i][1:].strip()); i += 1
            if buf and buf[0].startswith("[!CAUTION]"):                      # GitHub alert syntax: drawn as a red warning box
                paras, cur = [], []
                for t in buf[1:]:
                    if t: cur.append(t)
                    elif cur: paras.append(" ".join(cur)); cur = []
                if cur: paras.append(" ".join(cur))
                doc.quote(paras, danger=True); continue
            doc.quote(" ".join(buf)); continue
        m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)", l)
        if m:
            indent = 14.0 * (len(m.group(1)) // 2 + 1); item = m.group(3); bullet = "-" if m.group(2) in "-*" else m.group(2); i += 1
            while i < len(lines) and lines[i].strip() and not re.match(r"^(\s*)([-*]|\d+\.)\s+", lines[i]) and not lines[i].startswith(("|", "```", ">", "#", "!")): item += " " + lines[i].strip(); i += 1
            doc.paragraph(item, indent=indent - 14, bullet="•" if bullet == "-" else bullet, gap=2.5); continue
        if not l.strip(): i += 1; continue
        buf = [l.strip()]; i += 1
        while i < len(lines) and lines[i].strip() and not re.match(r"^(#{1,4}\s|```|\||>|!\[|\s*([-*]|\d+\.)\s|---)", lines[i]): buf.append(lines[i].strip()); i += 1
        doc.paragraph(" ".join(buf))
    doc.save(pdf_path)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("md"); ap.add_argument("pdf"); ap.add_argument("--title", default="Document")
    a = ap.parse_args(); convert(a.md, a.pdf, a.title); print("wrote", a.pdf)
