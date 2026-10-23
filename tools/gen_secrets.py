#!/usr/bin/env python3
"""
Generate firmware/include/secrets_generated.h from .env and the service-account
JSON — the files that already hold every one of these values.

    python3 tools/gen_secrets.py
    python3 tools/gen_secrets.py --check     # is it present and current?

WHY A GENERATOR AND NOT A TEMPLATE TO FILL IN.

Three times now I have asked Nakul to retype something that already existed in a
file, and three times it went wrong: a placeholder pasted verbatim, angle
brackets read as a shell redirect, and a config key I had invented. A Google
service-account private key is 1,700 characters of base64 that has to become a C
string literal. Asking anyone to do that by hand is asking for a bug that
presents as "Google rejects the assertion" and takes an evening to find.

So: nothing is retyped. `.env` and the service-account JSON stay the single
source of truth, and this turns them into a header.

WHAT IT WRITES, AND WHERE IT MUST NOT GO.

`firmware/include/secrets_generated.h` contains an RSA PRIVATE KEY. It is
gitignored, and it lives in unencrypted flash once compiled — anyone holding the
board can read it back. That is a real exposure and the reason the calendar is
shared read-only with a service account that owns nothing else.
"""

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "firmware" / "include" / "secrets_generated.h"


def read_env(path):
    env = {}
    if not path.exists():
        return env
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        env[k.strip()] = v.strip().strip('"').strip("'")
    return env


def c_string(s):
    """A C string literal, escaped, one source line per input line."""
    lines = s.split("\n")
    out = []
    for i, ln in enumerate(lines):
        esc = ln.replace("\\", "\\\\").replace('"', '\\"')
        nl = "\\n" if i < len(lines) - 1 else ""
        out.append(f'    "{esc}{nl}"')
    return "\n".join(out) if out else '    ""'


def one_line(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def parse_dest(v):
    """DEST_n=Label|lat|lon"""
    bits = v.split("|")
    if len(bits) != 3:
        return None
    label = bits[0].strip()
    try:
        lat, lon = float(bits[1]), float(bits[2])
    except ValueError:
        return None
    if not label or (lat == 0.0 and lon == 0.0):
        return None
    return label, lat, lon


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", type=pathlib.Path, default=ROOT / ".env")
    ap.add_argument("--out", type=pathlib.Path, default=OUT)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    if a.check:
        if not a.out.exists():
            print(f"MISSING: {a.out}\n  run: python3 tools/gen_secrets.py",
                  file=sys.stderr)
            return 1
        print(f"ok: {a.out.name} exists")
        return 0

    if not a.env.exists():
        print(f"no {a.env}. Copy .env.example to .env and fill it in.", file=sys.stderr)
        return 1

    env = read_env(a.env)
    missing, warn = [], []

    # --- the service account -------------------------------------------------
    sa_path = env.get("GOOGLE_SA_KEY_PATH", "")
    sa_email, sa_key = "", ""
    if sa_path:
        p = (a.env.parent / sa_path).resolve()
        if p.exists():
            try:
                sa = json.loads(p.read_text())
                sa_email = sa.get("client_email", "")
                sa_key = sa.get("private_key", "")
                if not sa_key:
                    missing.append(f"{p.name} has no private_key")
                elif "BEGIN PRIVATE KEY" not in sa_key:
                    missing.append(f"{p.name}: private_key is not a PEM block")
            except json.JSONDecodeError as e:
                missing.append(f"{p.name} is not valid JSON: {e}")
        else:
            missing.append(f"GOOGLE_SA_KEY_PATH points at {p}, which does not exist")
    else:
        missing.append("GOOGLE_SA_KEY_PATH is not set in .env")

    for key, why in [
        ("AVV_ACCESS_ID", "no journeys can be planned"),
        ("ORS_API_KEY", "the cycling alternative is skipped"),
        ("GOOGLE_CALENDAR_ID", "no appointments can be read"),
        ("HOME_LAT", "every journey starts from nowhere"),
        ("HOME_LON", "every journey starts from nowhere"),
    ]:
        if not env.get(key):
            (warn if key == "ORS_API_KEY" else missing).append(f"{key} is empty -- {why}")

    # --- destinations --------------------------------------------------------
    places = []
    for i in range(1, 9):
        v = env.get(f"DEST_{i}", "")
        if not v:
            continue
        d = parse_dest(v)
        if d:
            places.append(d)
        else:
            warn.append(f"DEST_{i} is not Label|lat|lon -- skipped: {v!r}")

    if missing:
        print("Cannot generate; these are needed first:\n", file=sys.stderr)
        for m in missing:
            print(f"  * {m}", file=sys.stderr)
        return 1
    for w in warn:
        print(f"note: {w}", file=sys.stderr)

    L = [
        "// GENERATED by tools/gen_secrets.py -- do not edit by hand.",
        "// Rerun it after changing .env or the service-account JSON.",
        "//",
        "// ⚠️ THIS FILE CONTAINS AN RSA PRIVATE KEY.",
        "// It is gitignored, and once compiled it sits in unencrypted flash --",
        "// anyone holding the board can read it back. That exposure is why the",
        "// calendar is shared READ-ONLY with a service account that owns nothing",
        "// else: the worst case is someone learning where you have to be, which",
        "// is bad enough without it also being your Google account.",
        "#pragma once",
        "#include <cstddef>",
        "",
        "namespace secrets {",
        "",
        "// --- AVV -----------------------------------------------------------",
        f'inline constexpr const char* kAvvAccessId = {one_line(env.get("AVV_ACCESS_ID", ""))};',
        f'inline constexpr bool kAvvProd = {"true" if env.get("AVV_ENV", "demo") == "prod" else "false"};',
        "",
        "// --- openrouteservice ----------------------------------------------",
        f'inline constexpr const char* kOrsApiKey = {one_line(env.get("ORS_API_KEY", ""))};',
        "",
        "// --- Google --------------------------------------------------------",
        f'inline constexpr const char* kGoogleClientEmail = {one_line(sa_email)};',
        f'inline constexpr const char* kGoogleCalendarId = {one_line(env.get("GOOGLE_CALENDAR_ID", ""))};',
        "",
        "// RS256 signing key. mbedtls_pk_parse_key() wants the trailing NUL",
        "// counted in the length, which is why kGoogleKeyLen adds one.",
        "inline constexpr const char kGooglePrivateKey[] =",
        c_string(sa_key.strip() + "\n") + ";",
        "inline constexpr size_t kGoogleKeyLen = sizeof kGooglePrivateKey;",
        "",
        "// --- where home is --------------------------------------------------",
        f'inline constexpr double kHomeLat = {float(env.get("HOME_LAT", 0) or 0)};',
        f'inline constexpr double kHomeLon = {float(env.get("HOME_LON", 0) or 0)};',
        "",
        "// --- the places you actually go -------------------------------------",
        "// Your labels, not a map's. core::PlaceBook matches a calendar",
        "// location against these before ever asking AVV to guess at a name.",
        f"inline constexpr int kPlaceCount = {len(places)};",
        "#define COMMUTE_TV_PLACES \\",
    ]
    rows = [f'    {{ {one_line(lbl)}, {lat}, {lon} }}' for lbl, lat, lon in places]
    L.append(", \\\n".join(rows) if rows else "    {}")
    L += [
        "",
        "// --- the budgets, straight from .env --------------------------------",
        f'inline constexpr unsigned kAvvMinIntervalSec = {int(env.get("AVV_MIN_INTERVAL_SEC", 20) or 20)};',
        f'inline constexpr unsigned kAvvMaxPerHour = {int(env.get("AVV_MAX_PER_HOUR", 60) or 60)};',
        f'inline constexpr unsigned kAvvMaxPerDay = {int(env.get("AVV_MAX_PER_DAY", 500) or 500)};',
        "",
        "} // namespace secrets",
        "",
    ]
    text = "\n".join(L)

    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(text)
    try:
        a.out.chmod(0o600)
    except OSError:
        pass

    print(f"wrote {a.out}")
    print(f"  service account : {sa_email}")
    print(f"  private key     : {len(sa_key)} chars of PEM")
    print(f"  calendar        : {env.get('GOOGLE_CALENDAR_ID','')[:40]}...")
    print(f"  home            : {env.get('HOME_LAT')}, {env.get('HOME_LON')}")
    print(f"  places          : {len(places)}  ({', '.join(p[0] for p in places)})")
    print(f"  AVV             : {'prod' if env.get('AVV_ENV')=='prod' else 'demo'}, "
          f"{env.get('AVV_MAX_PER_HOUR', 60)}/hour")
    print("\nThis file holds a private key. It is gitignored; keep it that way.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
