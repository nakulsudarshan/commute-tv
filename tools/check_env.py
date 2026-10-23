#!/usr/bin/env python3
"""
Check .env before anything depends on it.

    python3 tools/check_env.py           # offline checks only
    python3 tools/check_env.py --live    # also call AVV and openrouteservice once

Offline it confirms every value is present and the right shape, and that your
home coordinates are where you think they are. With --live it makes exactly one
request to each API to prove the keys work.

Secrets are never printed in full and never leave your machine: everything shown
is masked, so the output is safe to paste into a chat.

Why bother: a wrong key or a swapped coordinate pair fails in a way that looks
like a bug in the firmware rather than a typo in a config file, and you would
find out during fixture recording -- which has a narrow weekday window.

Standard library only.
"""

import argparse
import json
import math
import pathlib
import sys
import urllib.error
import urllib.request

# Anchored to this file, not to the shell's working directory: tools/ lives
# inside commute-tv/, so .env is always one level up from here. Resolving it
# relative to cwd meant the script only worked from one directory, which is a
# silly thing to have to remember.
PROJECT_DIR = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_ENV_PATH = PROJECT_DIR / ".env"

# Aachen, roughly the Markt. Only used to sanity-check the home coordinates.
AACHEN = (50.7753, 6.0839)

ok_count = 0
problems = []
notes = []


def ok(msg):
    global ok_count
    ok_count += 1
    print(f"  ok    {msg}")


def bad(msg):
    problems.append(msg)
    print(f"  FAIL  {msg}")


def warn(msg):
    notes.append(msg)
    print(f"  warn  {msg}")


def mask(value, keep=4):
    """Show enough to tell two keys apart, not enough to use one."""
    if not value:
        return "(empty)"
    if len(value) <= keep * 2:
        return "*" * len(value)
    return f"{value[:keep]}...{value[-keep:]}  ({len(value)} chars)"


def load_env(path):
    """Minimal .env reader. Tolerates quotes, inline comments and blank lines,
    because every one of those is a normal thing to end up with by hand."""
    if not path.exists():
        sys.exit(f"No .env at {path}\n"
                 f"Create it with:\n"
                 f"    cp {PROJECT_DIR / '.env.example'} {path}\n"
                 f"then fill in AVV_ACCESS_ID, ORS_API_KEY, HOME_LAT and HOME_LON.")
    env = {}
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            warn(f".env line {lineno} has no '=' and was ignored: {line[:40]!r}")
            continue
        key, _, val = line.partition("=")
        val = val.strip()
        # Strip matching quotes, then a trailing comment outside them.
        if len(val) >= 2 and val[0] == val[-1] and val[0] in "\"'":
            val = val[1:-1]
        elif " #" in val:
            val = val.split(" #", 1)[0].strip()
        env[key.strip()] = val
    return env


def looks_like_uuid(s):
    parts = s.split("-")
    return (len(parts) == 5
            and [len(p) for p in parts] == [8, 4, 4, 4, 12]
            and all(all(c in "0123456789abcdefABCDEF" for c in p) for p in parts))


def haversine_km(a, b):
    lat1, lon1 = math.radians(a[0]), math.radians(a[1])
    lat2, lon2 = math.radians(b[0]), math.radians(b[1])
    h = (math.sin((lat2 - lat1) / 2) ** 2
         + math.cos(lat1) * math.cos(lat2) * math.sin((lon2 - lon1) / 2) ** 2)
    return 2 * 6371.0 * math.asin(math.sqrt(h))


def get(url, headers=None, timeout=20):
    req = urllib.request.Request(url, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)
    except Exception as e:                                        # noqa: BLE001
        return 0, str(e).encode(), {}


def post_json(url, payload, headers=None, timeout=30):
    body = json.dumps(payload).encode()
    hdrs = {"Content-Type": "application/json"}
    hdrs.update(headers or {})
    req = urllib.request.Request(url, data=body, headers=hdrs, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)
    except Exception as e:                                        # noqa: BLE001
        return 0, str(e).encode(), {}


# ---------------------------------------------------------------------------

def check_avv(env):
    print("\nAVV HAFAS")
    key = env.get("AVV_ACCESS_ID", "")
    if not key:
        bad("AVV_ACCESS_ID is empty — it is in Dr. von der Ruhren's email")
        return None
    print(f"        AVV_ACCESS_ID = {mask(key)}")
    if not looks_like_uuid(key):
        bad("AVV_ACCESS_ID is not UUID-shaped (8-4-4-4-12 hex). Check for a "
            "stray space or a truncated paste.")
    else:
        ok("AVV_ACCESS_ID is the right shape")

    envname = env.get("AVV_ENV", "demo").lower()
    if envname not in ("demo", "prod"):
        bad(f"AVV_ENV is {envname!r}; expected 'demo' or 'prod'")
    else:
        ok(f"AVV_ENV = {envname}"
           + ("  (AVV asked that development happen here first)" if envname == "demo" else ""))
    return key


def check_ors(env):
    print("\nopenrouteservice")
    key = env.get("ORS_API_KEY", "")
    if not key:
        bad("ORS_API_KEY is empty — free key from https://account.heigit.org")
        return None
    print(f"        ORS_API_KEY   = {mask(key)}")
    if len(key) < 20:
        warn("ORS_API_KEY looks short; make sure the whole token was copied")
    else:
        ok("ORS_API_KEY is present and plausibly complete")
    return key


def check_home(env):
    print("\nHome location")
    raw_lat, raw_lon = env.get("HOME_LAT", ""), env.get("HOME_LON", "")
    if not raw_lat or not raw_lon:
        bad("HOME_LAT / HOME_LON are empty")
        return None
    try:
        lat, lon = float(raw_lat), float(raw_lon)
    except ValueError:
        bad(f"HOME_LAT/HOME_LON are not numbers: {raw_lat!r}, {raw_lon!r}")
        return None

    print(f"        {lat}, {lon}")
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        bad("coordinates are out of range")
        return None

    here = haversine_km((lat, lon), AACHEN)
    swapped = haversine_km((lon, lat), AACHEN)

    # The classic error: Google Maps shows "lat, lon" and openrouteservice wants
    # "lon, lat", so the two get transposed somewhere. It is worth detecting
    # rather than debugging a route that starts in the wrong country.
    if swapped < here:
        bad(f"these look SWAPPED — as given you are {here:,.0f} km from Aachen, "
            f"but transposed you would be {swapped:,.0f} km away. "
            f"Google Maps gives latitude first: HOME_LAT={lon}, HOME_LON={lat}")
    elif here > 100:
        warn(f"{here:,.0f} km from Aachen — fine if deliberate, but outside the "
             f"AVV area, so transit routing will not find anything")
    else:
        ok(f"{here:.1f} km from Aachen centre — inside the AVV area")
    return (lat, lon)


def live_avv(env, key):
    # location.name, NOT datainfo. On the AVV proxy `datainfo` returns an HTML
    # index of WADL links rather than the DataInfo document the spec describes,
    # so it can neither confirm the key nor report a version. location.name is
    # cheap, returns real JSON, and carries the CommonResponseType envelope.
    print("\nLive check: AVV location.name")
    base = ("https://auskunft.avv.de/restproxy/" if env.get("AVV_ENV", "demo").lower() == "prod"
            else "https://avv.demo.hafas.de/restproxy/")
    status, body, _ = get(f"{base}location.name?accessId={key}"
                          f"&format=json&input=Aachen&maxNo=1")
    # Parse the WHOLE body; truncate only for display. Truncating before
    # json.loads was silently turning every response into a parse failure and
    # reporting "no serverVersion" on a perfectly good reply.
    full = body.decode("utf-8", "replace")
    text = full[:400].replace("\n", " ")

    if status == 0:
        warn(f"could not reach {base} — {text[:120]}")
        warn("On the demo system this is expected outside German office hours. "
             "Retry on a weekday morning before concluding anything.")
    elif status in (401, 403):
        bad(f"HTTP {status} — the AccessId was rejected. {text[:160]}")
    elif 200 <= status < 300:
        try:
            data = json.loads(full) if full.strip().startswith("{") else {}
        except json.JSONDecodeError:
            data = {}

        # Every HAFAS response extends CommonResponseType, whose attributes are
        # serverVersion / dialectVersion / version / planRtTs / errorCode /
        # errorText (docs/rest-2.49.0.xsd, line 48). Reading them here is the
        # same code path the parser will need.
        ver = data.get("serverVersion")
        dialect = data.get("dialectVersion")
        ok(f"HTTP {status} — key accepted."
           + (f" serverVersion {ver}" if ver else "")
           + (f", dialect {dialect}" if dialect else ""))

        if data.get("errorCode"):
            bad(f"the response carries errorCode={data['errorCode']!r} "
                f"errorText={data.get('errorText', '')!r}")
        if not ver:
            warn("no serverVersion in the response — harmless, but tell me "
                 "the top-level keys so the parser expects the right shape: "
                 + ", ".join(sorted(data)[:12]))
        elif ver.startswith("2.5") and not ver.startswith("2.49"):
            print(f"        (our XSD is 2.49.0 against a {ver} server, so "
                  f"parsing must tolerate unknown fields — already the plan)")
    else:
        warn(f"HTTP {status} — {text[:160]}")
        warn("5xx on the demo system usually means it is asleep, not that "
             "anything is wrong with your key.")


def live_ors(key, home):
    print("\nLive check: openrouteservice directions + elevation")
    if home is None:
        warn("skipped: no usable home coordinates")
        return
    lat, lon = home
    # A short hop from home. openrouteservice takes [longitude, latitude] --
    # the opposite order to Google Maps, which is exactly why this tool exists.
    dest = [lon + 0.02, lat + 0.02]
    status, body, headers = post_json(
        "https://api.openrouteservice.org/v2/directions/cycling-regular/json",
        {"coordinates": [[lon, lat], dest], "elevation": True},
        {"Authorization": key},
    )
    text = body.decode("utf-8", "replace")

    if status == 0:
        warn(f"could not reach openrouteservice — {text[:140]}")
        return
    if status in (401, 403):
        bad(f"HTTP {status} — the ORS key was rejected. {text[:160]}")
        return
    if status == 429:
        warn("HTTP 429 — rate limited. The key works; you are just over quota.")
    elif not (200 <= status < 300):
        warn(f"HTTP {status} — {text[:200]}")
        return
    else:
        try:
            route = json.loads(text)["routes"][0]
            summ = route.get("summary", {})
            asc = route.get("ascent", summ.get("ascent"))
            desc = route.get("descent", summ.get("descent"))
            ok(f"HTTP {status} — key accepted. "
               f"{summ.get('distance', 0) / 1000:.2f} km, "
               f"{summ.get('duration', 0) / 60:.0f} min")
            if asc is None:
                bad("no ascent/descent in the response — elevation is the ONLY "
                    "reason we use openrouteservice at all")
            else:
                ok(f"elevation present: ascent {asc:.0f} m, descent {desc:.0f} m")
        except (KeyError, IndexError, ValueError) as e:
            warn(f"could not parse the route response ({e}): {text[:200]}")

    # These answer the quota question without anyone reading a dashboard.
    limits = {k: v for k, v in headers.items() if "ratelimit" in k.lower()}
    if limits:
        print("        quota headers:")
        for k in sorted(limits):
            print(f"          {k}: {limits[k]}")
    else:
        notes.append("openrouteservice returned no rate-limit headers; check "
                     "the quota on your dashboard at account.heigit.org")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--live", action="store_true",
                   help="make one real request to each API")
    p.add_argument("--env", type=pathlib.Path, default=DEFAULT_ENV_PATH,
                   help="path to the .env file (default: commute-tv/.env)")
    args = p.parse_args()

    print(f"Reading {args.env}\n")
    env = load_env(args.env)

    avv = check_avv(env)
    ors = check_ors(env)
    home = check_home(env)

    smalltv = env.get("SMALLTV_HOST", "")
    print("\nSmallTV")
    if smalltv:
        ok(f"SMALLTV_HOST = {smalltv}"
           + ("  (its own GIFTV hotspot)" if smalltv == "192.168.4.1" else ""))
    else:
        warn("SMALLTV_HOST is empty; 192.168.4.1 over the GIFTV hotspot")

    if args.live:
        if avv:
            live_avv(env, avv)
        if ors:
            live_ors(ors, home)
    else:
        print("\n(no network calls made — re-run with --live to test the keys)")

    print("\n" + "-" * 60)
    if problems:
        print(f"{len(problems)} problem(s) to fix:")
        for m in problems:
            print(f"  - {m}")
    if notes:
        print(f"{len(notes)} thing(s) worth knowing:")
        for m in notes:
            print(f"  - {m}")
    if not problems:
        print(f"{ok_count} checks passed, nothing blocking.")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
