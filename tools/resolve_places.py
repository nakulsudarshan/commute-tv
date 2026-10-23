#!/usr/bin/env python3
"""
Check that the places in .env are usable, and show what serves them.

The labels are YOURS. They are matched against what you type into a calendar
event's location field, not against what Google Maps or AVV call somewhere. This
tool does not rename anything -- it answers two questions:

  1. Can AVV actually plan a journey from home to this coordinate?
     That is the only thing that has to be true, and it is checked with a real
     trip query -- the same call the firmware makes.

  2. Which stops are near it?
     Informational. Useful for sanity-checking a coordinate you pasted.

Runs on your machine so neither the access ID nor your home address leaves it.

    python3 tools/resolve_places.py
    python3 tools/resolve_places.py --coords 50.7708,6.1187
    python3 tools/resolve_places.py --raw          # dump responses when puzzled
"""

import argparse
import datetime as dt
import json
import math
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
ENV = ROOT / ".env"

BASE = {
    "demo": "https://avv.demo.hafas.de/restproxy/",
    "prod": "https://auskunft.avv.de/restproxy/",
}

# location.nearbystops documents a maximum radius of 1000 m (spec 2.4). Asking
# for more is not an error, it is just silently not honoured -- which looks
# exactly like "there is nothing there".
MAX_RADIUS_M = 1000


def load_env(path):
    values = {}
    if not path.exists():
        return values
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        values[k.strip()] = v.strip()
    return values


def haversine_m(lat1, lon1, lat2, lon2):
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def get(base, service, params, access_id, timeout=25):
    q = dict(params)
    q["accessId"] = access_id
    q["format"] = "json"
    url = base + service + "?" + urllib.parse.urlencode(q)
    safe = url.replace(access_id, "REDACTED")
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
            status = r.status
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        status = e.code
    except Exception as e:                                       # noqa: BLE001
        return safe, 0, {"_transport_error": str(e)}, ""
    try:
        return safe, status, json.loads(raw), raw
    except json.JSONDecodeError:
        return safe, status, {"_non_json_body": raw[:400]}, raw


def extract_stops(body, lat, lon):
    """
    Pull StopLocation entries out of a LocationList, whatever shape it arrived
    in. Returns (stops, note). `note` is set when the body was understood but
    held no stops, or not understood at all -- the two must not look alike.
    """
    if not isinstance(body, dict):
        return [], f"response was {type(body).__name__}, not an object"

    if "errorCode" in body:
        return [], f"{body.get('errorCode')}: {body.get('errorText', '')[:80]}"
    if "_transport_error" in body:
        return [], body["_transport_error"]
    if "_non_json_body" in body:
        return [], "response was not JSON (first 100 chars: " \
                   + body["_non_json_body"][:100].replace("\n", " ") + ")"

    # HAFAS serialises a LocationList in more than one way depending on service
    # and dialect. Accept every shape rather than reporting an empty result for
    # one we did not anticipate -- that is what made this look like Aachen
    # Hauptbahnhof had no stops near it.
    containers = []
    for key in ("stopLocationOrCoordLocation", "StopLocation", "CoordLocation",
                "stopLocation", "locations"):
        v = body.get(key)
        if isinstance(v, list):
            containers.append(v)
    inner = body.get("LocationList")
    if isinstance(inner, dict):
        for key in ("stopLocationOrCoordLocation", "StopLocation", "CoordLocation"):
            v = inner.get(key)
            if isinstance(v, list):
                containers.append(v)

    if not containers:
        keys = ", ".join(sorted(k for k in body.keys() if not k.startswith("_")))
        return [], f"no location list in the response; top-level keys were: {keys}"

    out = []
    for entries in containers:
        for entry in entries:
            stop = entry.get("StopLocation") if isinstance(entry, dict) else None
            if stop is None and isinstance(entry, dict) and "name" in entry:
                stop = entry                       # already a StopLocation
            if not isinstance(stop, dict):
                continue
            products = sorted({
                p.get("catOut", "").strip()
                for p in stop.get("productAtStop", [])
                if p.get("catOut")
            })
            dist = stop.get("dist")
            if dist is None:
                dist = haversine_m(lat, lon, stop.get("lat", lat), stop.get("lon", lon))
            out.append((stop.get("name", "?"), stop.get("extId", ""), int(dist), products))

    out.sort(key=lambda s: s[2])
    if not out:
        return [], "the response was understood and held no stops"
    return out, None


def iso_minutes(hhmmss):
    """"15:33:00" -> minutes past midnight, or None."""
    try:
        h, m = int(hhmmss[0:2]), int(hhmmss[3:5])
    except (ValueError, IndexError, TypeError):
        return None
    return h * 60 + m


def duration_minutes(iso):
    """"PT1H22M" -> 82."""
    if not isinstance(iso, str) or not iso.startswith("P"):
        return None
    total, acc, in_time = 0, 0, False
    for ch in iso[1:]:
        if ch == "T":
            in_time, acc = True, 0
        elif ch.isdigit():
            acc = acc * 10 + int(ch)
        elif ch == "D":
            total, acc = total + acc * 1440, 0
        elif ch == "H" and in_time:
            total, acc = total + acc * 60, 0
        elif ch == "M" and in_time:
            total, acc = total + acc, 0
        elif ch == "S" and in_time:
            acc = 0
        else:
            return None
    return total


def summarise_trip(body):
    """One line per itinerary: what the firmware would have to work with."""
    if not isinstance(body, dict):
        return None, "response was not an object"
    if "errorCode" in body:
        return None, f"{body.get('errorCode')}: {body.get('errorText', '')[:80]}"
    if "_transport_error" in body:
        return None, body["_transport_error"]
    trips = body.get("Trip")
    if not isinstance(trips, list):
        keys = ", ".join(sorted(k for k in body.keys() if not k.startswith("_")))
        return None, f"no Trip list; top-level keys were: {keys}"
    if not trips:
        return None, "AVV planned no journey to this coordinate"

    lines = []
    for t in trips:
        legs = t.get("LegList", {}).get("Leg", [])
        names = []
        for leg in legs:
            if leg.get("type") != "JNY":
                continue
            prod = leg.get("Product")
            p0 = prod[0] if isinstance(prod, list) and prod else (prod or {})
            names.append(p0.get("line") or p0.get("displayNumber") or leg.get("name", "?"))

        dep_raw = legs[0].get("Origin", {}).get("time") if legs else None
        arr_raw = legs[-1].get("Destination", {}).get("time") if legs else None
        dep = (dep_raw or "?")[:5]
        arr = (arr_raw or "?")[:5]
        stated = duration_minutes(t.get("duration"))
        lines.append(f"{dep}-{arr}  {t.get('duration', '?'):>7}  "
                     f"{t.get('transferCount', 0)} chg  via {' + '.join(names) or 'walk only'}")

        # The times and the stated duration have to agree. When they do not,
        # one of them is being read wrong -- and the firmware computes arrival
        # from these same fields, so an unexplained disagreement here is an
        # unexplained arrival time on the screen. Print the legs rather than
        # letting it slide.
        d0, a0 = iso_minutes(dep_raw), iso_minutes(arr_raw)
        if d0 is not None and a0 is not None and stated is not None:
            implied = a0 - d0
            if implied < 0:
                implied += 1440                     # crossed midnight
            if abs(implied - stated) > 1:
                lines.append(f"    ⚠ times say {implied} min, duration says {stated} min "
                             f"-- leg breakdown:")
                for leg in legs:
                    o = leg.get("Origin", {})
                    d = leg.get("Destination", {})
                    lines.append(
                        f"      {leg.get('type', '?'):<4} "
                        f"{(o.get('time') or '?')[:5]}->{(d.get('time') or '?')[:5]} "
                        f"rt {(o.get('rtTime') or '-')[:5]}->{(d.get('rtTime') or '-')[:5]}  "
                        f"{(o.get('name') or '?')[:28]} -> {(d.get('name') or '?')[:28]}")
    return lines, None


def check_place(base, access_id, label, lat, lon, home, radius, maxno, raw):
    print(f"\n{label or '(unnamed)'}   {lat:.6f}, {lon:.6f}")
    print("  " + "-" * 68)

    # --- the part that actually matters: can a journey be planned there? ---
    if home is None:
        print("  no HOME_LAT/HOME_LON in .env -- skipping the journey check")
    else:
        target = dt.datetime.now() + dt.timedelta(minutes=60)
        url, status, body, text = get(base, "trip", {
            "originCoordLat": f"{home[0]:.6f}",
            "originCoordLong": f"{home[1]:.6f}",
            "destCoordLat": f"{lat:.6f}",
            "destCoordLong": f"{lon:.6f}",
            "date": target.strftime("%Y-%m-%d"),
            "time": target.strftime("%H:%M"),
            "numF": "2",
            "originWalk": "1", "destWalk": "1",
            "poly": "0", "passlist": "0",
            "withFreq": "0", "tariff": "0", "trafficMessages": "0",
        }, access_id)
        if raw:
            print(f"  [trip] HTTP {status} {url}\n  {text[:600]}\n")
        trips, err = summarise_trip(body)
        if err:
            print(f"  JOURNEY: FAILED -- {err}")
        else:
            print("  JOURNEY: ok")
            for line in trips:
                print(f"    {line}")

    # --- informational: what stops are nearby ---
    url, status, body, text = get(base, "location.nearbystops", {
        "originCoordLat": f"{lat:.6f}",
        "originCoordLong": f"{lon:.6f}",
        "r": str(radius),
        "maxNo": str(maxno),
    }, access_id)
    if raw:
        print(f"  [nearbystops] HTTP {status} {url}\n  {text[:600]}\n")

    stops, note = extract_stops(body, lat, lon)
    if note:
        # Deliberately not "no stops found": an earlier version said that when
        # it meant "I did not recognise the response", which sent us looking in
        # entirely the wrong place.
        #
        # On the AVV demo proxy this reliably comes back as a valid but EMPTY
        # envelope -- serverVersion and requestId, no location list. The service
        # is simply not populated there. It is context only: the firmware
        # resolves calendar text through location.name and plans with trip, and
        # uses nearbystops nowhere.
        if "no location list" in note:
            print("  nearby stops: not available on this proxy (context only, "
                  "nothing depends on it)")
        else:
            print(f"  nearby stops: {note}")
            if status != 200:
                print(f"                (HTTP {status} -- rerun with --raw for the body)")
    else:
        print(f"  nearby stops within {radius} m:")
        for name, ext_id, dist, products in stops:
            kinds = ", ".join(products) if products else "-"
            print(f"    {dist:>4d} m  {name:<40} {kinds}  [{ext_id}]")


def coords(s):
    lat, lon = s.split(",")
    return float(lat.strip()), float(lon.strip())


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--env", choices=("demo", "prod"), default=None)
    p.add_argument("--access-id", default=None)
    p.add_argument("--radius", type=int, default=500,
                   help=f"metres, capped at {MAX_RADIUS_M} by AVV (default 500)")
    p.add_argument("--max", type=int, default=6, dest="maxno")
    p.add_argument("--coords", action="append", default=[],
                   help="lat,lon to check instead of reading .env; repeatable")
    p.add_argument("--raw", action="store_true",
                   help="print each response, for when the summary is not enough")
    args = p.parse_args()

    env = load_env(ENV)
    access_id = args.access_id or env.get("AVV_ACCESS_ID", "")
    which = args.env or env.get("AVV_ENV", "demo")
    if not access_id:
        print("No AVV_ACCESS_ID. Put it in .env or pass --access-id.", file=sys.stderr)
        return 2
    base = BASE.get(which, BASE["demo"])

    radius = args.radius
    if radius > MAX_RADIUS_M:
        print(f"note: AVV caps the search radius at {MAX_RADIUS_M} m "
              f"(spec 2.4), so --radius {radius} is being sent as {MAX_RADIUS_M}.")
        radius = MAX_RADIUS_M

    home = None
    if env.get("HOME_LAT") and env.get("HOME_LON"):
        home = (float(env["HOME_LAT"]), float(env["HOME_LON"]))

    places = []
    if args.coords:
        for i, c in enumerate(args.coords, 1):
            lat, lon = coords(c)
            places.append((f"--coords {i}", lat, lon))
    else:
        for i in range(1, 10):
            entry = env.get(f"DEST_{i}")
            if not entry:
                continue
            parts = entry.split("|")
            if len(parts) != 3:
                print(f"DEST_{i} is not LABEL|LAT|LON -- skipping", file=sys.stderr)
                continue
            places.append((parts[0].strip(), float(parts[1]), float(parts[2])))

    if not places:
        print("Nothing to check. Add DEST_n entries to .env or pass --coords.")
        return 1

    print(f"Checking {len(places)} place(s) against {base}")
    if home:
        print(f"Home: {home[0]:.6f}, {home[1]:.6f}")

    for label, lat, lon in places:
        check_place(base, access_id, label, lat, lon, home, radius, args.maxno, args.raw)

    print("\nThe JOURNEY line is the one that matters: if AVV can plan a trip to")
    print("a coordinate, the firmware can use it. Nearby stops are only context.")
    print("\nYour labels are matched against what you type in a calendar event --")
    print("not against Google Maps. Case, spacing, punctuation and umlauts are")
    print("all ignored, and a trailing address is fine:")
    print('  "SuperC", "superc", "Super C", "SuperC, Templergraben 57" all match.')
    return 0


if __name__ == "__main__":
    sys.exit(main())
