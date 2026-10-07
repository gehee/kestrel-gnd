#!/usr/bin/env python3
"""Make zones_data.cpp: every time zone, the country it is in, and its POSIX
TZ string.

SYSTEM > Time Zone is one setting that answers two questions: what the clock
shows, and which country's rules the channel page uses (fpv_rules.cpp, kept by
hand). A named zone does both; a POSIX TZ string alone does not (Shanghai and
Taipei are both "CST-8", Toronto and New York both "EST5EDT,...").

From tzdata: zone.tab (one country per zone), iso3166.tab (country names),
the zone files' footers (the POSIX TZ string), and the link lines in
tzdata.zi (old names a phone may still report, like Asia/Calcutta). Nothing
of it is needed on the goggle: the output is a C++ table, kept in the
repository. Run again to pick up a newer tzdata:

  tools/gen_zones.py --tzdir /usr/share/zoneinfo > zones_data.cpp
"""
import argparse
import math
import os
import re
import sys

# ITU Radio Regulations No. 5.2-5.9: the three regions' boundary lines, as
# (latitude, longitude east) points joined by great circle arcs, north to south.
LINE_A = [(90, 40), (40, 40), (23.4364, 60), (-90, 60)]
LINE_B = [(90, -10), (72, -10), (40, -50), (-10, -20), (-90, -20)]
# Bering Strait boundary at 65 deg 30 min N: 168 deg 58 min W.
LINE_C_NORTH = [(90, -168.97), (65.5, -168.97), (50, 165), (10, -170)]   # then along 10 N to 120 W
LINE_C_SOUTH_LON = -120
# No. 5.3: wholly in Region 1 whatever the lines say; Iran is wholly in Region 3.
REGION1_COUNTRIES = {"AM", "AZ", "RU", "GE", "KZ", "MN", "UZ", "KG", "TJ", "TM", "TR", "UA"}
REGION3_COUNTRIES = {"IR"}

AREAS = ["Africa", "America", "Antarctica", "Arctic", "Asia", "Atlantic",
         "Australia", "Europe", "Indian", "Pacific"]


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def to_vec(lat, lon):
    la, lo = math.radians(lat), math.radians(lon)
    return (math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la))


def to_latlon(v):
    x, y, z = v
    return math.degrees(math.atan2(z, math.hypot(x, y))), math.degrees(math.atan2(y, x))


def slerp(a, b, t):
    dot = max(-1.0, min(1.0, sum(p * q for p, q in zip(a, b))))
    om = math.acos(dot)
    if om < 1e-9:
        return a
    s1, s2 = math.sin((1 - t) * om) / math.sin(om), math.sin(t * om) / math.sin(om)
    return tuple(s1 * p + s2 * q for p, q in zip(a, b))


def line_lon(points, lat):
    """The longitude where a north-to-south line of great circle arcs crosses lat."""
    for (la1, lo1), (la2, lo2) in zip(points, points[1:]):
        if not (la2 <= lat <= la1):
            continue
        if la1 >= 89.999 or lo1 == lo2:      # a meridian
            return lo2
        a, b = to_vec(la1, lo1), to_vec(la2, lo2)
        lo_t, hi_t = 0.0, 1.0
        for _ in range(60):                  # latitude falls along these arcs
            mid = (lo_t + hi_t) / 2
            if to_latlon(slerp(a, b, mid))[0] > lat:
                lo_t = mid
            else:
                hi_t = mid
        return to_latlon(slerp(a, b, lo_t))[1]
    return points[-1][1]


def itu_region(lat, lon):
    la = line_lon(LINE_A, lat)
    lb = line_lon(LINE_B, lat)
    lc = line_lon(LINE_C_NORTH, lat) if lat > 10 else LINE_C_SOUTH_LON
    east = lambda frm, to: (to - frm) % 360.0      # degrees east from frm to to
    if east(lb, lon) < east(lb, la):
        return 1                                   # east of B, west of A
    if east(la, lon) < east(la, lc):
        return 3                                   # east of A, west of C
    return 2                                       # east of C, west of B


def parse_coord(c):
    """zone.tab's +DDMM+DDDMM or +DDMMSS+DDDMMSS."""
    m = re.match(r"([+-])(\d{2})(\d{2})(\d{2})?([+-])(\d{3})(\d{2})(\d{2})?$", c)
    sg = lambda x: -1 if x == "-" else 1
    lat = sg(m.group(1)) * (int(m.group(2)) + int(m.group(3)) / 60 + int(m.group(4) or 0) / 3600)
    lon = sg(m.group(5)) * (int(m.group(6)) + int(m.group(7)) / 60 + int(m.group(8) or 0) / 3600)
    return lat, lon


def read_tz(tzdir):
    countries = {}
    for line in open(os.path.join(tzdir, "iso3166.tab"), encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        code, name = line.rstrip("\n").split("\t")[:2]
        countries[code] = name
    zones = []
    for line in open(os.path.join(tzdir, "zone.tab"), encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        code, coord, name = f[0], f[1], f[2]
        area = name.split("/")[0]
        if area not in AREAS:
            continue
        data = open(os.path.join(tzdir, name), "rb").read()
        posix = data.rstrip(b"\n").rsplit(b"\n", 1)[-1].decode()
        zones.append({"name": name, "code": code, "area": area, "posix": posix,
                      "coord": parse_coord(coord)})
    version, links = "", {}
    zi = os.path.join(tzdir, "tzdata.zi")
    if os.path.exists(zi):
        for line in open(zi, encoding="utf-8"):
            if line.startswith("# version"):
                version = line.split()[2]
            elif line.startswith("L "):
                _, target, alias = line.split()
                links[alias] = target
    return countries, zones, version, links


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tzdir", default="/usr/share/zoneinfo")
    a = ap.parse_args()

    names, zones, tz_version, links = read_tz(a.tzdir)

    codes = sorted({z["code"] for z in zones})
    # A country's ITU region: the named ones of No. 5.3, Antarctica none, the
    # rest where zone.tab puts its first (main) zone.
    region = {}
    for z in zones:                     # still in zone.tab's order here
        c = z["code"]
        if c in region:
            continue
        if c in REGION1_COUNTRIES:
            region[c] = 1
        elif c in REGION3_COUNTRIES:
            region[c] = 3
        elif c == "AQ":
            region[c] = 0
        else:
            region[c] = itu_region(*z["coord"])
    cidx = {c: i for i, c in enumerate(codes)}
    # Sorted the way the menu walks them: by area, then country name, then city.
    for z in zones:
        parts = z["name"].split("/")
        z["city"] = parts[-1].replace("_", " ")
        z["sub"] = parts[1].replace("_", " ") if len(parts) == 3 else ""
    # A zone in a state or province says which ("Center (North Dakota)"), unless
    # that is the country's own name (America/Argentina/...).
    for z in zones:
        if z["sub"] and z["sub"] != names.get(z["code"], ""):
            z["city"] = "%s (%s)" % (z["city"], z["sub"])
    zones.sort(key=lambda z: (AREAS.index(z["area"]), names.get(z["code"], z["code"]), z["city"]))
    zidx = {z["name"]: i for i, z in enumerate(zones)}

    out = []
    w = out.append
    w("// Generated by tools/gen_zones.py - do not edit.")
    w("//")
    w("// tzdata %s: zone.tab, iso3166.tab and the zone files' POSIX TZ footers (public domain)." % (tz_version or "?"))
    w("// Each country's ITU region: ITU Radio Regulations No. 5.2-5.9 (lines A, B and C, and the")
    w("// countries No. 5.3 names), applied to the coordinates of the country's first zone in zone.tab.")
    w("")
    w('#include "zones.hpp"')
    w("")
    w("namespace zones {")
    w("namespace data {")
    w("")
    w("const char* const kTzdataVersion = %s;" % c_str(tz_version))
    w("")
    w("const char* const kAreas[] = { %s };" % ", ".join(c_str(x) for x in AREAS))
    w("const int kAreaCount = %d;" % len(AREAS))
    w("")
    w("const Country kCountries[] = {")
    for c in codes:
        w("    { %s, %s, %d }," % (c_str(c), c_str(names.get(c, c)), region[c]))
    w("};")
    w("const int kCountryCount = %d;" % len(codes))
    w("")
    w("const Zone kZones[] = {")
    for z in zones:
        w("    { %s, %s, %d, %d, %s }," % (c_str(z["name"]), c_str(z["city"]), AREAS.index(z["area"]),
                                         cidx[z["code"]], c_str(z["posix"])))
    w("};")
    w("const int kZoneCount = %d;" % len(zones))
    w("")
    w("// Old and alternative names (tzdata's links) for zones in the list.")
    w("const Alias kAliases[] = {")
    na = 0
    for alias in sorted(links):
        t = links[alias]
        while t in links and t not in zidx:
            t = links[t]
        if t in zidx and alias not in zidx:
            w("    { %s, %d }," % (c_str(alias), zidx[t]))
            na += 1
    if na == 0:
        w("    { \"\", 0 },")
    w("};")
    w("const int kAliasCount = %d;" % na)
    w("")
    w("}  // namespace data")
    w("}  // namespace zones")
    sys.stdout.write("\n".join(out) + "\n")
    print("%d zones, %d countries, %d aliases" % (len(zones), len(codes), na), file=sys.stderr)


if __name__ == "__main__":
    main()
