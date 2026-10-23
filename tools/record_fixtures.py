#!/usr/bin/env python3
"""
Record real AVV HAFAS responses as offline fixtures.

Why this exists: AVV state that the demo system is unavailable or degraded
outside German office hours. Without captured responses, development stalls
every evening. Run this once during office hours and the rest of the build can
proceed against real data at any time.

The access ID is redacted from everything written to disk, so the fixtures are
safe to keep in the repo.

Usage:
    python tools/record_fixtures.py --access-id YOUR_ID \
        --home 50.7753,6.0839 --dest 50.7660,6.0450

    # against the production system once the app is ready
    python tools/record_fixtures.py --access-id YOUR_ID --env prod ...
"""

import argparse
import datetime as dt
import json
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE = {
    "demo": "https://avv.demo.hafas.de/restproxy/",
    "prod": "https://auskunft.avv.de/restproxy/",
}

FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures"
REDACTED = "REDACTED-ACCESS-ID"


def fetch(base, service, params, access_id, timeout=30):
    """GET one service. Returns (url_redacted, status, parsed_or_text)."""
    q = dict(params)
    q["accessId"] = access_id
    q["format"] = "json"
    url = base + service + "?" + urllib.parse.urlencode(q)
    safe_url = url.replace(access_id, REDACTED)

    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
            status = r.status
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        status = e.code
    except Exception as e:                                   # noqa: BLE001
        return safe_url, 0, {"_transport_error": str(e)}

    raw = raw.replace(access_id, REDACTED)
    try:
        return safe_url, status, json.loads(raw)
    except json.JSONDecodeError:
        # A non-JSON body is itself worth capturing -- it usually means the
        # format parameter was ignored, or the demo server returned an HTML
        # error page, and the parser needs to survive both.
        return safe_url, status, {"_non_json_body": raw[:4000]}


def save(name, url, status, body):
    FIXTURES.mkdir(exist_ok=True)
    path = FIXTURES / f"{name}.json"
    doc = {
        "_recorded_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "_request_url": url,
        "_http_status": status,
        "body": body,
    }
    path.write_text(json.dumps(doc, indent=2, ensure_ascii=False), encoding="utf-8")

    size = path.stat().st_size
    note = ""
    if isinstance(body, dict):
        if "Trip" in body:
            note = f"{len(body['Trip'])} trips"
        elif "stopLocationOrCoordLocation" in body:
            note = f"{len(body['stopLocationOrCoordLocation'])} locations"
        elif "errorCode" in body:
            note = f"error {body.get('errorCode')}: {body.get('errorText', '')[:60]}"
    print(f"  {'ok ' if 200 <= status < 300 else 'HTTP ' + str(status)}"
          f"  {path.name:<34} {size:>7,d} B  {note}")


def coords(s):
    lat, lon = s.split(",")
    return float(lat.strip()), float(lon.strip())


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--access-id", required=True)
    p.add_argument("--env", choices=("demo", "prod"), default="demo")
    p.add_argument("--home", type=coords, required=True, help="lat,lon of your home")
    p.add_argument("--dest", type=coords, required=True, help="lat,lon of a typical destination")
    p.add_argument("--arrive-in", type=int, default=90,
                   help="record a trip arriving N minutes from now (default 90)")
    args = p.parse_args()

    base = BASE[args.env]
    home_lat, home_lon = args.home
    dest_lat, dest_lon = args.dest

    target = dt.datetime.now() + dt.timedelta(minutes=args.arrive_in)
    date_s = target.strftime("%Y-%m-%d")
    time_s = target.strftime("%H:%M")

    print(f"Recording from {base} (arrive by {date_s} {time_s})\n")

    trip_common = {
        "originCoordLat": f"{home_lat:.6f}",
        "originCoordLong": f"{home_lon:.6f}",
        "destCoordLat": f"{dest_lat:.6f}",
        "destCoordLong": f"{dest_lon:.6f}",
        "date": date_s,
        "time": time_s,
        "originWalk": "1",
        "destWalk": "1",
        "numF": "3",
        "poly": "0",
        "passlist": "0",
        # Must match src/hafas/hafas_request.cpp exactly -- a fixture that does
        # not is a fixture that tests the wrong request. withFreq defaults to 1
        # server-side and Leg.Freq measured 52% of a real response, so leaving
        # it out is not the same as setting it to 0.
        "withFreq": "0",
        "tariff": "0",
        "trafficMessages": "0",
    }

    cases = [
        # The primary call the firmware makes.
        ("trip_arrival_realtime", "trip",
         {**trip_common, "searchForArrival": "1", "rtMode": "REALTIME"}),
        # Same query on planned data only: lets tests compare rt vs planned
        # fields and confirm the delay maths against a known baseline.
        ("trip_arrival_planned", "trip",
         {**trip_common, "searchForArrival": "1", "rtMode": "OFF"}),
        # Forward search, for comparison.
        ("trip_departure_realtime", "trip",
         {**trip_common, "searchForArrival": "0", "rtMode": "REALTIME"}),
        # Location resolution: a stop, a landmark, and a street address.
        ("location_stop", "location.name",
         {"input": "Aachen Ponttor", "maxNo": "5", "attributes": "0", "infotexts": "0"}),
        ("location_poi", "location.name",
         {"input": "Uniklinik Aachen", "maxNo": "5", "attributes": "0", "infotexts": "0"}),
        ("location_address", "location.name",
         {"input": "Templergraben 55, Aachen", "maxNo": "5",
          "attributes": "0", "infotexts": "0"}),
        # Nonsense input: the parser must handle an empty result set.
        ("location_nonsense", "location.name",
         {"input": "zzzqqq not a real place", "maxNo": "5"}),
        # Disruption messages.
        ("him_messages", "himsearch", {}),
        # Server metadata, useful for detecting a version bump later.
        ("datainfo", "datainfo", {}),
    ]

    for name, service, params in cases:
        url, status, body = fetch(base, service, params, args.access_id)
        save(name, url, status, body)

    # An intentionally bad key, so the error-handling path is built against a
    # real error document rather than a guess at its shape.
    url, status, body = fetch(base, "trip",
                              {**trip_common, "searchForArrival": "1"},
                              "00000000-0000-0000-0000-000000000000")
    save("error_bad_accessid", url, status, body)

    print(f"\nWrote fixtures to {FIXTURES}")
    print("Access ID is redacted in every file; these are safe to commit.")


if __name__ == "__main__":
    sys.exit(main())
