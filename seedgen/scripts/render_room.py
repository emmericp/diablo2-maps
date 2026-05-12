"""Render a single room from per-seed JSON dumps as PNG.

Defaults: room 114 across Tower Cellar levels 21..24, scanning the first 1000
seed_*.json files in seedgen/out. Colors mirror renderer/src/colors.ts so the
output reads the same as the web viewer.
"""

import argparse
import base64
import json
import zlib
from pathlib import Path

from PIL import Image, ImageDraw


# Collision colors (mirror renderer/src/colors.ts).
COL_BG        = (60, 60, 60)
COL_FLOOR     = (235, 235, 235)
COL_ALTERNATE = (200, 200, 200)
COL_WALL      = (15, 15, 15)
COL_WATER     = (45, 45, 45)

# Marker colors.
COL_EXIT       = (30, 80, 255)
COL_NPC        = (230, 50, 50)
COL_WAYPOINT   = (0, 220, 230)
COL_SHRINE     = (220, 60, 220)
COL_WELL       = (255, 140, 0)
COL_SUPERCHEST = (255, 230, 60)
COL_CHEST      = (180, 140, 30)
COL_QUEST      = (255, 255, 255)
COL_DOOR       = (100, 90, 60)
COL_GENERIC    = (160, 160, 60)
COL_STAIRS     = (30, 80, 255)

# Collision flag bits (mirror seedgen/src/mapdata.h via renderer/src/types.ts).
COLL_BLOCKWALK     = 0x0001
COLL_WALL          = 0x0004
COLL_ALTERNATETILE = 0x0010
NO_DATA = 0xFFFF


def classify(v: int):
    if v == NO_DATA:
        return None
    if (v & COLL_BLOCKWALK) == 0:
        if (v & COLL_ALTERNATETILE) != 0:
            return COL_ALTERNATE
        return COL_FLOOR
    if (v & COLL_WALL) != 0:
        return COL_WALL
    return COL_WATER


def marker_for(p):
    """Return (color, radius_in_tiles, priority) matching renderer/src/colors.ts."""
    t = p.get("type")
    if t == "exit":
        return COL_EXIT, 5, 30
    if t == "npc":
        return COL_NPC, 2, 10
    kind = p.get("kind", "Generic")
    table = {
        "Waypoint":   (COL_WAYPOINT,   5, 40),
        "Shrine":     (COL_SHRINE,     4, 40),
        "Well":       (COL_WELL,       4, 40),
        "SuperChest": (COL_SUPERCHEST, 4, 40),
        "Quest":      (COL_QUEST,      4, 40),
        "Chest":      (COL_CHEST,      2, 20),
        "Door":       (COL_DOOR,       1, 20),
        "Stairs":     (COL_STAIRS,     5, 30),
    }
    if kind in table:
        return table[kind]
    return (COL_GENERIC, 2, 5)


def decode_collision(b64: str, w: int, h: int):
    raw = zlib.decompress(base64.b64decode(b64))
    n = w * h
    out = [NO_DATA] * n
    limit = min(n, len(raw) // 2)
    for i in range(limit):
        out[i] = raw[2 * i] | (raw[2 * i + 1] << 8)
    return out


def render_room(level, room, out_path: Path, scale: int) -> None:
    """Render the room using the same iso projection as renderer/src/texture.ts.

    Native iso canvas: (rw + rh) * 2 wide, (rw + rh) tall. Each tile becomes a
    2x2 stamp at (iso_x * 2, iso_y), where iso_x = (lx - ly) + (rh - 1) and
    iso_y = lx + ly. Then we NEAREST-upscale by `scale` and draw preset
    markers on top so circles stay smooth.
    """
    rx, ry = room["x"], room["y"]
    rw, rh = room["sizeX"], room["sizeY"]
    cw = level.get("collisionWidth") or level["size"][0]
    ch = level.get("collisionHeight") or level["size"][1]
    b64 = level.get("collisionDeflateB64")
    if not b64:
        return
    coll = decode_collision(b64, cw, ch)

    nat_w = (rw + rh) * 2
    nat_h = rw + rh
    img = Image.new("RGB", (nat_w, nat_h), COL_BG)
    px = img.load()
    for ly in range(rh):
        yy = ry + ly
        if yy < 0 or yy >= ch:
            continue
        row = yy * cw
        for lx in range(rw):
            xx = rx + lx
            if xx < 0 or xx >= cw:
                continue
            c = classify(coll[row + xx])
            if c is None:
                continue
            iso_x = (lx - ly) + (rh - 1)
            iso_y = lx + ly
            bx = iso_x * 2
            for dy in range(2):
                yyy = iso_y + dy
                if yyy < 0 or yyy >= nat_h:
                    continue
                for dx in range(2):
                    xxx = bx + dx
                    if xxx < 0 or xxx >= nat_w:
                        continue
                    px[xxx, yyy] = c

    if scale != 1:
        img = img.resize((nat_w * scale, nat_h * scale), Image.Resampling.NEAREST)

    # Overlay preset markers that fall inside the room. Sorted ascending by
    # priority so important kinds land on top. Marker world coords land on
    # the top-left of each tile's 2x2 stamp — matches the renderer exactly.
    items = []
    for p in level.get("presets", []):
        if not (rx <= p["x"] < rx + rw and ry <= p["y"] < ry + rh):
            continue
        items.append((p, marker_for(p)))
    items.sort(key=lambda it: it[1][2])

    if items:
        draw = ImageDraw.Draw(img)
        for p, (color, radius, _prio) in items:
            lx = p["x"] - rx
            ly = p["y"] - ry
            iso_x = (lx - ly) + (rh - 1)
            iso_y = lx + ly
            cx = iso_x * 2 * scale
            cy = iso_y * scale
            r = radius * 2 * scale  # iso doubles marker radius — see colors.ts
            draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)

    img.save(out_path)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in-dir", default="seedgen/out",
                    help="directory of seed_*.json files (default: seedgen/out)")
    ap.add_argument("--out-dir", default="seedgen/scripts/room_renders",
                    help="output directory for PNGs")
    ap.add_argument("--limit", type=int, default=1000,
                    help="scan at most this many JSON files (default: 1000)")
    ap.add_argument("--scale", type=int, default=8,
                    help="pixels per tile (default: 8)")
    ap.add_argument("--room-no", type=int, default=114,
                    help="roomNo to match (default: 114)")
    ap.add_argument("--levels", default="21,22,23,24",
                    help="comma-separated levelNos to scan (default: 21,22,23,24)")
    args = ap.parse_args()

    levels = {int(x) for x in args.levels.split(",")}
    in_dir = Path(args.in_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    files = sorted(in_dir.glob("seed_*.json"))[:args.limit]
    print(f"Scanning {len(files)} files for roomNo={args.room_no} on levels {sorted(levels)}...")

    total = 0
    for fp in files:
        try:
            with open(fp) as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            print(f"  skip {fp.name}: {e}")
            continue
        seed = data["seed"]
        for lvl in data["levels"]:
            if lvl["levelNo"] not in levels:
                continue
            matches = [r for r in lvl["rooms"] if r["roomNo"] == args.room_no]
            for i, room in enumerate(matches):
                suffix = "" if len(matches) == 1 else f"-{i}"
                out_path = out_dir / f"room-{args.room_no}-{seed}-{lvl['levelNo']}{suffix}.png"
                render_room(lvl, room, out_path, scale=args.scale)
                total += 1

    print(f"Rendered {total} room images to {out_dir}")


if __name__ == "__main__":
    main()
