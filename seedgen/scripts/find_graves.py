"""Ad-hoc: scan room 114 in tower cellar levels (21-24) across the first
N seeds and locate every patch of BlockWalk|AlternateTile tiles (open graves).

For each room instance we report:
  seed, level, list of (min_lx, min_ly, w, h) bboxes of connected patches,
  and a summary of patch sizes / counts.

We compute positions in level-local tile coords AND in iso-screen coords
(iso_x = lx - ly, iso_y = lx + ly) so we can sort the four graves by
"south / above-south / top-right-of-above / very right".
"""

import argparse
import base64
import collections
import json
import zlib
from pathlib import Path

COLL_BLOCKWALK     = 0x0001
COLL_ALTERNATETILE = 0x0010
NO_DATA = 0xFFFF


def decode_collision(b64, w, h):
    raw = zlib.decompress(base64.b64decode(b64))
    n = w * h
    out = [NO_DATA] * n
    limit = min(n, len(raw) // 2)
    for i in range(limit):
        out[i] = raw[2 * i] | (raw[2 * i + 1] << 8)
    return out


def is_grave_tile(v):
    """A grave-flagged tile has both BlockWalk and AlternateTile set."""
    if v == NO_DATA:
        return False
    return (v & COLL_BLOCKWALK) != 0 and (v & COLL_ALTERNATETILE) != 0


def find_patches(coll, cw, ch, rx, ry, rw, rh):
    """Find 4-connected patches of grave tiles inside the room. Returns
    list of dicts with bbox, size, and tile coordinates (level-local)."""
    in_grave = [[False] * rw for _ in range(rh)]
    for ly in range(rh):
        for lx in range(rw):
            v = coll[(ry + ly) * cw + (rx + lx)]
            if is_grave_tile(v):
                in_grave[ly][lx] = True

    visited = [[False] * rw for _ in range(rh)]
    patches = []
    for sy in range(rh):
        for sx in range(rw):
            if not in_grave[sy][sx] or visited[sy][sx]:
                continue
            # BFS
            stack = [(sx, sy)]
            visited[sy][sx] = True
            tiles = []
            while stack:
                x, y = stack.pop()
                tiles.append((x, y))
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < rw and 0 <= ny < rh and in_grave[ny][nx] and not visited[ny][nx]:
                        visited[ny][nx] = True
                        stack.append((nx, ny))
            xs = [t[0] for t in tiles]
            ys = [t[1] for t in tiles]
            minx, maxx = min(xs), max(xs)
            miny, maxy = min(ys), max(ys)
            patches.append({
                "minx": minx, "miny": miny,
                "maxx": maxx, "maxy": maxy,
                "w": maxx - minx + 1,
                "h": maxy - miny + 1,
                "count": len(tiles),
                "tiles": tiles,
            })
    return patches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-dir", default="seedgen/out")
    ap.add_argument("--limit", type=int, default=100)
    ap.add_argument("--room-no", type=int, default=114)
    ap.add_argument("--levels", default="21,22,23,24")
    args = ap.parse_args()

    levels = {int(x) for x in args.levels.split(",")}
    in_dir = Path(args.in_dir)
    files = sorted(in_dir.glob("seed_*.json"))[:args.limit]

    # Per-room-instance records.
    records = []
    size_hist = collections.Counter()
    count_hist = collections.Counter()

    for fp in files:
        with open(fp) as f:
            data = json.load(f)
        seed = data["seed"]
        for lvl in data["levels"]:
            if lvl["levelNo"] not in levels:
                continue
            b64 = lvl.get("collisionDeflateB64")
            if not b64:
                continue
            cw = lvl.get("collisionWidth") or lvl["size"][0]
            ch = lvl.get("collisionHeight") or lvl["size"][1]
            coll = None
            for room in lvl["rooms"]:
                if room["roomNo"] != args.room_no:
                    continue
                if coll is None:
                    coll = decode_collision(b64, cw, ch)
                patches = find_patches(coll, cw, ch,
                                       room["x"], room["y"],
                                       room["sizeX"], room["sizeY"])
                count_hist[len(patches)] += 1
                for p in patches:
                    size_hist[(p["w"], p["h"], p["count"])] += 1
                records.append({
                    "seed": seed,
                    "level": lvl["levelNo"],
                    "room_xy": (room["x"], room["y"]),
                    "room_size": (room["sizeX"], room["sizeY"]),
                    "patches": patches,
                })

    print(f"Scanned {len(files)} seeds — {len(records)} room {args.room_no} instances.")
    print()
    print("Patch count per room (number of grave patches found in one room):")
    for k in sorted(count_hist):
        print(f"  {k} patches: {count_hist[k]} rooms")
    print()
    print("Patch (w, h, tile-count) histogram:")
    for k, v in sorted(size_hist.items(), key=lambda kv: -kv[1]):
        print(f"  {k}: {v}")
    print()

    # Position clustering. The 4 grave positions should appear at consistent
    # (lx, ly) centers across all room instances (modulo small jitter).
    # Collect every patch's center and bucket by (round to int).
    centers = collections.Counter()
    for r in records:
        for p in r["patches"]:
            cx = (p["minx"] + p["maxx"]) / 2.0
            cy = (p["miny"] + p["maxy"]) / 2.0
            centers[(round(cx), round(cy))] += 1
    print("Patch-center (lx, ly) histogram — top 12:")
    for (cx, cy), n in centers.most_common(12):
        iso_x = cx - cy
        iso_y = cx + cy
        print(f"  lx={cx:>3} ly={cy:>3}  iso_x={iso_x:>4} iso_y={iso_y:>4}  count={n}")

    # Also show min-corner clustering for the most-tightly-defined position.
    corners = collections.Counter()
    for r in records:
        for p in r["patches"]:
            corners[(p["minx"], p["miny"])] += 1
    print()
    print("Patch min-corner (lx, ly) histogram — top 12:")
    for (cx, cy), n in corners.most_common(12):
        iso_x = cx - cy
        iso_y = cx + cy
        print(f"  lx={cx:>3} ly={cy:>3}  iso_x={iso_x:>4} iso_y={iso_y:>4}  count={n}")


if __name__ == "__main__":
    main()
