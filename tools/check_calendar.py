#!/usr/bin/env python3
"""
Prove the Google service account can actually read your calendar.

    python3 tools/check_calendar.py
    python3 tools/check_calendar.py --hours 48 --show-locations

This does exactly what the ESP32 firmware will do, in the same order:

    1. build a JWT asserting the service account's identity
    2. sign it RS256 with the private key from the JSON file
    3. exchange it at oauth2.googleapis.com for a 1-hour access token
    4. call calendar/v3 events.list with the same query and field mask

Writing it in Python first is deliberate. If the sharing is wrong, or a scope is
missing, or the clock is skewed, this says so in seconds -- rather than after a
few hundred lines of C++ have been written against an assumption.

PRIVACY: by default this prints NO event titles and NO addresses. It reports how
many events there are and how many carry a location, because the location field
is the only thing the project acts on. Pass --show-locations if you want to see
them yourself; the output is then no longer safe to paste anywhere.

Needs: pip install cryptography
Config: GOOGLE_SA_KEY_PATH and GOOGLE_CALENDAR_ID in .env
"""

import argparse
import base64
import datetime as dt
import json
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request

PROJECT_DIR = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_ENV_PATH = PROJECT_DIR / ".env"

TOKEN_URL = "https://oauth2.googleapis.com/token"

# What the DEVICE uses, and all it ever needs: read events, change nothing.
SCOPE = "https://www.googleapis.com/auth/calendar.readonly"
# Read-write. Used only by tools/make_sa_calendar.py, which has to create a
# calendar and write an ACL. Deliberately not the default: the firmware should
# never hold a token that can modify a calendar.
SCOPE_RW = "https://www.googleapis.com/auth/calendar"

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
except ImportError:
    sys.exit("needs the cryptography package:  pip install cryptography")


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def load_env(path):
    if not path.exists():
        sys.exit(f"No .env at {path}")
    env = {}
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            v = v[1:-1]
        elif " #" in v:
            v = v.split(" #", 1)[0].strip()
        env[k.strip()] = v
    return env


def request(url, data=None, headers=None, timeout=30):
    body = urllib.parse.urlencode(data).encode() if data else None
    req = urllib.request.Request(url, data=body, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:                                        # noqa: BLE001
        return 0, str(e).encode()


def mint_access_token(sa, scope=SCOPE):
    """Sign a JWT with the service-account key and swap it for a token. This is
    the flow the ESP32 repeats every hour; RS256 is hardware-accelerated there
    and takes about 100 ms.

    `scope` is a parameter because the setup tools need write access while the
    device needs none. A token minted here can do exactly what its scope says
    and nothing more -- asking for a scope you do not need is how a read-only
    device ends up able to delete a calendar."""
    now = int(dt.datetime.now(dt.timezone.utc).timestamp())
    header = {"alg": "RS256", "typ": "JWT", "kid": sa.get("private_key_id")}
    claims = {
        "iss": sa["client_email"],
        "scope": scope,
        "aud": TOKEN_URL,
        "iat": now,
        "exp": now + 3600,
    }
    signing_input = f"{b64url(json.dumps(header).encode())}." \
                    f"{b64url(json.dumps(claims).encode())}".encode()

    key = serialization.load_pem_private_key(sa["private_key"].encode(), password=None)
    signature = key.sign(signing_input, padding.PKCS1v15(), hashes.SHA256())
    assertion = f"{signing_input.decode()}.{b64url(signature)}"

    status, body = request(
        TOKEN_URL,
        {"grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer",
         "assertion": assertion},
        {"Content-Type": "application/x-www-form-urlencoded"},
    )
    text = body.decode("utf-8", "replace")
    if status != 200:
        # The two failures worth telling apart, because the fixes are opposite.
        hint = ""
        if "invalid_grant" in text:
            hint = ("\n  'invalid_grant' almost always means the system clock is "
                    "off, or the key was revoked. Check `timedatectl`.")
        elif "invalid_client" in text or "unauthorized_client" in text:
            hint = ("\n  The key is not valid for this service account — "
                    "re-download the JSON.")
        elif "invalid_scope" in text:
            hint = f"\n  The scope was rejected: {scope}"
        sys.exit(f"token exchange failed: HTTP {status}\n  {text[:300]}{hint}")
    return json.loads(text)["access_token"]


def diagnose_unshared(sa, cal_id):
    """Ordered by how often each one is actually the cause."""
    print("   Work through these in order:\n")
    print(f"   1. Open Google Calendar -> hover {cal_id} in the left sidebar")
    print("      -> the three dots -> 'Settings and sharing'.")
    print("   2. Scroll to 'Share with specific people or groups'. This is NOT")
    print("      the same section as 'Access permissions for events' — adding")
    print("      the account under the wrong heading is the usual mistake.")
    print("   3. Click 'Add people and groups' and paste EXACTLY:")
    print(f"         {sa['client_email']}")
    print("      Permission: 'See all event details'. Then click Send.")
    print("   4. Reload that settings page and confirm the address is still")
    print("      listed. The dialog sometimes drops an address if you press")
    print("      Enter without picking it from the dropdown, and it looks like")
    print("      it worked.")
    print("   5. Give it a minute and re-run this tool. Propagation is usually")
    print("      instant but is not guaranteed to be.\n")
    print("   Note: you do NOT need to make the calendar public. Public sharing")
    print("   is a different setting and grants this account nothing.")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--hours", type=int, default=24,
                   help="how far ahead to look (default 24)")
    p.add_argument("--show-locations", action="store_true",
                   help="print the actual location strings (not safe to paste)")
    p.add_argument("--env", type=pathlib.Path, default=DEFAULT_ENV_PATH)
    args = p.parse_args()

    env = load_env(args.env)
    key_path = env.get("GOOGLE_SA_KEY_PATH", "")
    cal_id = env.get("GOOGLE_CALENDAR_ID", "")
    if not key_path or not cal_id:
        sys.exit("Set GOOGLE_SA_KEY_PATH and GOOGLE_CALENDAR_ID in .env first.")

    key_file = pathlib.Path(key_path).expanduser()
    if not key_file.exists():
        sys.exit(f"No service-account JSON at {key_file}")

    sa = json.loads(key_file.read_text())
    print(f"service account : {sa['client_email']}")
    print(f"project         : {sa.get('project_id')}")
    print(f"calendar        : {cal_id}\n")

    print("1. signing JWT and exchanging it for an access token...")
    token = mint_access_token(sa)
    print(f"   ok — token acquired ({len(token)} chars, valid 1 hour)\n")

    # The exact query the firmware makes: only the four fields we use, expanded
    # recurrences, ordered, bounded to the near future. Keeping the field mask
    # tight is what holds the response to a few hundred bytes on the device.
    now = dt.datetime.now(dt.timezone.utc)
    params = {
        "timeMin": now.isoformat().replace("+00:00", "Z"),
        "timeMax": (now + dt.timedelta(hours=args.hours)).isoformat().replace("+00:00", "Z"),
        "singleEvents": "true",
        "orderBy": "startTime",
        "maxResults": "20",
        "fields": "items(summary,location,start,end)",
    }
    url = (f"https://www.googleapis.com/calendar/v3/calendars/"
           f"{urllib.parse.quote(cal_id)}/events?{urllib.parse.urlencode(params)}")

    print(f"2. events.list for the next {args.hours} h...")
    status, body = request(url, headers={"Authorization": f"Bearer {token}"})
    text = body.decode("utf-8", "replace")

    if status == 404:
        # Google returns 404 rather than 403 for a calendar the caller cannot
        # access, so it does not distinguish "wrong ID" from "not shared".
        # calendarList.list does: it returns everything this service account can
        # see, which is the difference between those two cases.
        print("   HTTP 404 — this service account cannot see that calendar.")
        print("   (A clean 404 from the Calendar service also means the API IS")
        print("    enabled on the project — otherwise this would be a 403.)\n")
        print("   asking what it CAN see...")
        st2, b2 = request("https://www.googleapis.com/calendar/v3/users/me/calendarList",
                          headers={"Authorization": f"Bearer {token}"})
        if st2 == 200:
            cals = json.loads(b2.decode("utf-8", "replace")).get("items", [])
            if not cals:
                print("   -> nothing at all is shared with it. The share never "
                      "took effect.\n")
                diagnose_unshared(sa, cal_id)
            else:
                print(f"   -> it can see {len(cals)} calendar(s):")
                for c in cals:
                    print(f"        {c.get('id')}   (access: {c.get('accessRole')})")
                print(f"\n   None of these is {cal_id!r}, so the share landed on a "
                      f"different\n   calendar, or GOOGLE_CALENDAR_ID is wrong. Set "
                      f"GOOGLE_CALENDAR_ID to one\n   of the IDs above, or re-share "
                      f"the right calendar.")
        else:
            print(f"   calendarList also failed: HTTP {st2} "
                  f"{b2.decode('utf-8','replace')[:200]}")
            diagnose_unshared(sa, cal_id)
        return 1
    if status == 403:
        sys.exit(f"   HTTP 403 — access denied. Usually the Calendar API is not "
                 f"enabled\n   on project {sa.get('project_id')!r}.\n   {text[:200]}")
    if status != 200:
        sys.exit(f"   HTTP {status}: {text[:300]}")

    items = json.loads(text).get("items", [])
    print(f"   ok — HTTP 200, {len(items)} event(s) in the window\n")

    # This is the project's core rule: an event is acted on only if it has a
    # non-empty location. Counting them here shows whether the idea will even
    # fire in practice on a real calendar.
    with_loc = [e for e in items if (e.get("location") or "").strip()]
    all_day = [e for e in items if "date" in e.get("start", {})]

    print("3. what the firmware would do with them")
    print(f"   {len(items):3} total")
    print(f"   {len(all_day):3} all-day        -> ignored by design")
    print(f"   {len(with_loc):3} with a location -> ACTIONABLE")
    print(f"   {len(items) - len(with_loc) - len(all_day):3} timed, no location -> ignored by design")

    if items:
        print("\n   timeline (no titles shown):")
        for e in items:
            start = e.get("start", {})
            when = start.get("dateTime") or start.get("date") or "?"
            has = "location" if (e.get("location") or "").strip() else "—"
            line = f"     {when:26} {has}"
            if args.show_locations and (e.get("location") or "").strip():
                line += f"   {e['location']}"
            print(line)

    print()
    if not with_loc:
        print("No actionable events in this window. That is not a failure — it "
              "just means\nnothing here has a location set. To test the whole "
              "chain end to end, add a\ncalendar entry a few hours out with a "
              "real address in the Location field.")
    else:
        print(f"{len(with_loc)} actionable event(s). The service account reads "
              f"your calendar correctly;\nPhase 4 is unblocked.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
