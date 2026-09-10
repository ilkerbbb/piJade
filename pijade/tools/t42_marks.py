"""BBB-AIRGAP: task 42; count green pixels in the seven marker regions on the Buttons screen.

Usage: python3 t42_marks.py <frame.rgb565> <comma-separated expected names|->
Compare the ENTIRE observed set with the expected one; exit 1 on any difference. Missing
and extra lit markers both count as errors, which gives this measurement its strength.
Regions follow make_io_test_buttons_activity() (main/ui/dashboard.c): outer vsplit 20/56/24,
body hsplit 55/45, pad and button column vsplit 33/34/33, middle pad row hsplit 33/34/33.
Frames are read in layout coordinates (mirroring happens on the panel).
"""
import sys

WIDTH = HEIGHT = 240

# Outer vsplit 20/56/24: body rows.
BODY_TOP = HEIGHT * 20 // 100
BODY_BOTTOM = BODY_TOP + HEIGHT * 56 // 100
BODY_HEIGHT = BODY_BOTTOM - BODY_TOP
# Body hsplit 55/45.
PAD_RIGHT = WIDTH * 55 // 100
# Joystick column vsplit 33/34/33.
TOP_BOTTOM = BODY_TOP + BODY_HEIGHT * 33 // 100
MIDDLE_BOTTOM = TOP_BOTTOM + BODY_HEIGHT * 34 // 100
# Middle row hsplit 33/34/33.
LEFT_RIGHT = PAD_RIGHT * 33 // 100
MIDDLE_RIGHT = LEFT_RIGHT + PAD_RIGHT * 34 // 100

REGIONS = {
    "up": (0, PAD_RIGHT, BODY_TOP, TOP_BOTTOM),
    "left": (0, LEFT_RIGHT, TOP_BOTTOM, MIDDLE_BOTTOM),
    "click": (LEFT_RIGHT, MIDDLE_RIGHT, TOP_BOTTOM, MIDDLE_BOTTOM),
    "right": (MIDDLE_RIGHT, PAD_RIGHT, TOP_BOTTOM, MIDDLE_BOTTOM),
    "down": (0, PAD_RIGHT, MIDDLE_BOTTOM, BODY_BOTTOM),
    "key1": (PAD_RIGHT, WIDTH, BODY_TOP, TOP_BOTTOM),
    "key2": (PAD_RIGHT, WIDTH, TOP_BOTTOM, MIDDLE_BOTTOM),
}

# The bitmap font has no intermediate tones: pixels are green or not. The smallest icon
# (hollow circle) lights 61 pixels; the threshold 20 is well below every complete marker,
# so partial rendering is also detected.
THRESHOLD = 20


def is_green(r, g, b):
    return g > 200 and r < 80 and b < 80


def measure(path):
    raw = open(path, "rb").read()
    expected_size = WIDTH * HEIGHT * 2
    if len(raw) != expected_size:
        raise SystemExit(f"ERROR: 42: frame size {len(raw)}, expected {expected_size}")

    counts = {}
    for name, (x0, x1, y0, y1) in REGIONS.items():
        n = 0
        for y in range(y0, y1):
            row = (y * WIDTH) * 2
            for x in range(x0, x1):
                i = row + x * 2
                v = (raw[i] << 8) | raw[i + 1]
                r = ((v >> 11) & 0x1F) * 255 // 31
                g = ((v >> 5) & 0x3F) * 255 // 63
                b = (v & 0x1F) * 255 // 31
                if is_green(r, g, b):
                    n += 1
        counts[name] = n
    return counts


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: t42_marks.py <frame.rgb565> <expected|->")
    path, expected_raw = sys.argv[1], sys.argv[2]
    expected = set() if expected_raw == "-" else set(expected_raw.split(","))
    unknown = expected - set(REGIONS)
    if unknown:
        raise SystemExit(f"ERROR: 42: unknown marker name: {sorted(unknown)}")

    counts = measure(path)
    observed = {name for name, n in counts.items() if n >= THRESHOLD}
    print("42: counts " + " ".join(f"{name}={counts[name]}" for name in sorted(counts)))
    print(f"42: observed {sorted(observed) or ['-']} / expected {sorted(expected) or ['-']}")

    missing = sorted(expected - observed)
    extra = sorted(observed - expected)
    if missing:
        print(f"ERROR: 42: expected marker did not light: {missing}")
    if extra:
        print(f"ERROR: 42: unexpected marker lit: {extra}")
    if missing or extra:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
