#!/usr/bin/env python3
"""
Create a calendar OWNED by the service account, and share it with you.

    python3 tools/make_sa_calendar.py
    python3 tools/make_sa_calendar.py --name "Commute"

WHY
---
Granting a service account access to a calendar you own does not work on a
consumer Google account: the web UI silently drops the entry, and acl.insert
returns 503 backendError indefinitely (issuetracker 148804709). Ten attempts,
both by calendar-id and via 'primary', all failed.

So the direction is reversed. The service account creates a calendar it owns,
then shares that calendar with *you* -- granting a human access to a
service-account-owned calendar is the ordinary direction and works. The device
keeps reading the calendar with the same non-expiring service-account key, and
nothing about the architecture changes.

The cost is that appointments you want the device to act on go in this calendar
rather than your primary one. It shows up in your Google Calendar sidebar and is
selectable per event.

Safe to re-run: it finds an existing calendar of the same name rather than
creating a second one, and re-shares only if the share is missing.

Needs: pip install cryptography
Config: GOOGLE_SA_KEY_PATH and GOOGLE_CALENDAR_ID in .env
"""

import argparse
import json
import pathlib
import sys
import time
import urllib.parse

# Reuse the JWT/token machinery rather than duplicating it -- one implementation
# of the signing flow, exercised by both tools.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_calendar import (DEFAULT_ENV_PATH, SCOPE_RW, load_env,  # noqa: E402
                            mint_access_token, request)

API = "https://www.googleapis.com/calendar/v3"


def api(token, path, payload=None, method=None, timeout=30):
    """Thin JSON wrapper over the shared request() helper."""
    import urllib.error
    import urllib.request
    url = f"{API}{path}"
    headers = {"Authorization": f"Bearer {token}"}
    data = None
    if payload is not None:
        data = json.dumps(payload).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:                                        # noqa: BLE001
        return 0, str(e)


def find_calendar(token, name):
    """Look for a calendar of this name the service account already owns."""
    status, text = api(token, "/users/me/calendarList")
    if status != 200:
        return None, (status, text)
    for c in json.loads(text).get("items", []):
        if c.get("summary") == name and c.get("accessRole") == "owner":
            return c, None
    return None, None


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", default="Commute",
                   help="calendar name as it will appear in your sidebar")
    p.add_argument("--timezone", default="Europe/Berlin")
    args = p.parse_args()

    env = load_env(DEFAULT_ENV_PATH)
    key_path = env.get("GOOGLE_SA_KEY_PATH", "")
    your_email = env.get("GOOGLE_CALENDAR_ID", "")
    if not key_path or not your_email:
        sys.exit("Set GOOGLE_SA_KEY_PATH and GOOGLE_CALENDAR_ID in .env first.")

    key_file = pathlib.Path(key_path).expanduser()
    if not key_file.exists():
        sys.exit(f"No service-account JSON at {key_file}")
    sa = json.loads(key_file.read_text())

    print(f"service account : {sa['client_email']}")
    print(f"sharing with    : {your_email}")
    print(f"calendar name   : {args.name}\n")

    print("1. authenticating as the service account...")
    # Read-write: this tool creates a calendar and writes an ACL. The device
    # never uses this scope -- check_calendar.py and the firmware ask for
    # calendar.readonly, which cannot modify anything.
    token = mint_access_token(sa, SCOPE_RW)
    print(f"   ok — scope {SCOPE_RW}\n")

    print("2. looking for an existing calendar of that name...")
    existing, err = find_calendar(token, args.name)
    if err:
        print(f"   could not list calendars: HTTP {err[0]} {err[1][:200]}")
    if existing:
        cal_id = existing["id"]
        print(f"   found one, reusing it\n   {cal_id}\n")
    else:
        print("   none; creating it...")
        status, text = api(token, "/calendars",
                           {"summary": args.name, "timeZone": args.timezone},
                           method="POST")
        if status not in (200, 201):
            sys.exit(f"   calendars.insert failed: HTTP {status}\n   {text[:400]}")
        cal_id = json.loads(text)["id"]
        print(f"   created\n   {cal_id}\n")

    print(f"3. sharing it with {your_email}...")
    # 'owner' rather than 'writer' so the calendar appears properly in your
    # sidebar and you can edit and delete it. The service account keeps its own
    # owner role regardless, so this does not lock either party out.
    status, text = api(token, f"/calendars/{urllib.parse.quote(cal_id)}/acl"
                              f"?sendNotifications=true",
                       {"role": "owner", "scope": {"type": "user", "value": your_email}},
                       method="POST")
    if status in (200, 201):
        print(f"   ok — HTTP {status}\n")
    elif status in (409,):
        print("   already shared\n")
    else:
        print(f"   acl.insert returned HTTP {status}: {text[:300]}")
        print("   checking whether it landed anyway...")
        time.sleep(2)
        st, tx = api(token, f"/calendars/{urllib.parse.quote(cal_id)}/acl")
        shared = st == 200 and any(
            r.get("scope", {}).get("value", "").lower() == your_email.lower()
            for r in json.loads(tx).get("items", []))
        if shared:
            print("   it did — the error was spurious\n")
        else:
            print("\n   The calendar exists and the service account owns it, but "
                  "sharing it with you failed.\n   You can still add it manually: "
                  "Google Calendar -> Other calendars -> + -> \n   'Subscribe to "
                  f"calendar' -> paste:\n     {cal_id}\n")

    print("=" * 68)
    print("Put this in commute-tv/.env, replacing the current GOOGLE_CALENDAR_ID:\n")
    print(f"    GOOGLE_CALENDAR_ID={cal_id}")
    print(f"    GOOGLE_OWNER_EMAIL={your_email}")
    print("\n(keep your own address as GOOGLE_OWNER_EMAIL — the tools use it to")
    print(" re-share if you ever remove the calendar from your sidebar)")
    print("=" * 68)
    print("\nThen:")
    print("  1. Look for a 'Commute' calendar in your Google Calendar sidebar.")
    print("     If it is not there, use the Subscribe-to-calendar route above.")
    print("  2. Add a test appointment a few hours out, WITH an address in the")
    print("     Location field — that field is the only thing the device acts on.")
    print("  3. python3 tools/check_calendar.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
