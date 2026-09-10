"""In the container: pad /probe/<tag>_qrNN.rgb565 dumps with white borders and decode with quirc;
write unique UR parts. /tmp/screen_qr_decode is compiled from pijade/tools/screen_qr_decode.c.
Usage: python3 m3_qrparts.py <tag>"""
import glob, os, struct, subprocess, sys
tag = sys.argv[1]; PAD = 24
parts, seen = [], set()
for f in sorted(glob.glob(f"/probe/{tag}_qr*.rgb565")):
    raw = open(f, "rb").read(); w = h = 240
    W = w + 2 * PAD; white = b"\xff\xff"
    rows = [white * W] * PAD
    for y in range(h):
        rows.append(white * PAD + raw[2 * y * w:2 * (y + 1) * w] + white * PAD)
    rows += [white * W] * PAD
    padded = f.replace(".rgb565", "_pad.rgb565")
    open(padded, "wb").write(b"".join(rows))
    out = subprocess.run(["/tmp/screen_qr_decode", padded, str(W), str(W)], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith("content"):
            txt = line.split(":", 1)[1].strip()
            if txt not in seen:
                seen.add(txt); parts.append(txt)
    os.remove(padded)
open(f"/probe/{tag}_parts.txt", "w").write("\n".join(parts) + "\n")
print(f"{len(parts)} unique parts; first: {parts[0][:60] if parts else '-'}")
