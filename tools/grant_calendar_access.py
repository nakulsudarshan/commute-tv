#!/usr/bin/env python3
"""
Grant the service account read access to your calendar, via the API.

    python3 tools/grant_calendar_access.py

WHY THIS EXISTS
---------------
Adding a service-account address under "Share with specific people or groups" in
the Google Calendar web UI often does not stick: the address vanishes on reload,
with no error. It is a long-standing Google-side problem (issuetracker 148804709,
"Service Accounts don't accept automatically shared calendars") and not
something you can click your way around -- the UI appears to expect the invitee
to accept an invitation, which a service account never does.

The Calendar API's acl.insert writes the same permission directly, and that does
persist. This tool performs it.

WHAT IT DOES TO YOUR CREDENTIALS
--------------------------------
It runs a one-time OAuth authorisation *as you*, in your browser, purely to make
one API call. The access token is used once and discarded -- nothing is written
to disk, and no refresh token is requested. That matters: the 7-day refresh-token
expiry that made user OAuth unattractive for this project never comes into play,
because there is no refresh token. Afterwards the ESP32 still authenticates as
the service account, whose key does not expire.

The scope requested is calendar.acls -- permission to manage sharing, and
nothing else. It cannot read your events.

SETUP (once)
------------
1. In the SAME Google Cloud project as the service account:
   APIs & Services -> Credentials -> Create credentials -> OAuth client ID
   -> Application type: **Desktop app**. Name it anything.
2. Download its JSON. Save it as ~/GeekMagic SmallTV Ultra/oauth-client.json
3. If prompted to configure the consent screen: User type "External", fill in
   the required name/email fields, and add yourself as a Test user. You do not
   need to publish or verify anything -- this is a one-off authorisation.
4. Add to commute-tv/.env:
       GOOGLE_OAUTH_CLIENT_PATH=../oauth-client.json
"""

import base64
import hashlib
import http.server
import json
import pathlib
import secrets
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

PROJECT_DIR = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_ENV_PATH = PROJECT_DIR / ".env"

AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URL = "https://oauth2.googleapis.com/token"
# The narrowest scope that can write an ACL. Notably NOT calendar.readonly:
# this authorisation manages sharing and cannot read a single event.
SCOPE = "https://www.googleapis.com/auth/calendar.acls"


def b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


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


def http_json(url, data=None, headers=None, method=None):
    body = None
    hdrs = dict(headers or {})
    if isinstance(data, dict) and hdrs.get("Content-Type") == "application/json":
        body = json.dumps(data).encode()
    elif data is not None:
        body = urllib.parse.urlencode(data).encode()
    req = urllib.request.Request(url, data=body, headers=hdrs, method=method)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:                                        # noqa: BLE001
        return 0, str(e)


class CodeCatcher(http.server.BaseHTTPRequestHandler):
    """Receives the redirect Google sends back to the loopback address."""
    code = None
    error = None

    def do_GET(self):                                              # noqa: N802
        params = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        CodeCatcher.code = (params.get("code") or [None])[0]
        CodeCatcher.error = (params.get("error") or [None])[0]
        msg = ("Authorised. You can close this tab and return to the terminal."
               if CodeCatcher.code else
               f"Authorisation failed: {CodeCatcher.error}")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(f"<html><body style='font-family:sans-serif;padding:3em'>"
                         f"<h2>{msg}</h2></body></html>".encode())

    def log_message(self, *_args):
        pass                                                       # keep quiet


def authorise(client):
    """Loopback OAuth with PKCE. Returns a one-shot access token."""
    verifier = b64url(secrets.token_bytes(32))
    challenge = b64url(hashlib.sha256(verifier.encode()).digest())

    # Installed-app clients may use any loopback port, so take a free one
    # rather than requiring a particular port to be registered.
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    redirect_uri = f"http://127.0.0.1:{port}/"

    params = {
        "client_id": client["client_id"],
        "redirect_uri": redirect_uri,
        "response_type": "code",
        "scope": SCOPE,
        "code_challenge": challenge,
        "code_challenge_method": "S256",
        # Deliberately no access_type=offline: we do not want a refresh token.
        # One access token, used once, then gone.
        "prompt": "consent",
    }
    url = f"{AUTH_URL}?{urllib.parse.urlencode(params)}"

    server = http.server.HTTPServer(("127.0.0.1", port), CodeCatcher)
    server.timeout = 5          # so handle_request() returns and we can re-check

    print("Opening your browser to authorise. If it does not open, visit:\n")
    print(f"  {url}\n")
    try:
        webbrowser.open(url)
    except Exception:                                              # noqa: BLE001
        pass

    # Serve on the MAIN thread until the redirect arrives. Handling requests in
    # a background thread and then closing the socket -- which is what this did
    # originally -- tears the listener down before the browser ever connects,
    # and shows up as "Unable to connect" on a redirect URL that is perfectly
    # valid. Loop rather than a single handle_request(): browsers cheerfully
    # send a favicon request alongside the one we care about.
    print("waiting for the redirect (Ctrl-C to paste the URL by hand)...")
    deadline = time.monotonic() + 300
    try:
        while (CodeCatcher.code is None and CodeCatcher.error is None
               and time.monotonic() < deadline):
            server.handle_request()
    except KeyboardInterrupt:
        print()
    finally:
        server.server_close()

    if CodeCatcher.error:
        sys.exit(f"authorisation denied: {CodeCatcher.error}")

    if not CodeCatcher.code:
        # Fallback for the cases the loopback cannot cover: a firewall or VPN
        # that swallows 127.0.0.1, or a browser on a different machine. The
        # code is right there in the address bar either way.
        print("\nNo redirect reached this script.")
        print("If your browser DID land on a 127.0.0.1 URL (even an error page),")
        print("copy that whole URL from the address bar and paste it here.")
        print("Otherwise press Enter to give up and re-run.\n")
        try:
            pasted = input("URL: ").strip()
        except (EOFError, KeyboardInterrupt):
            pasted = ""
        if not pasted:
            sys.exit("no authorisation code — re-run the tool and try again.")
        q = urllib.parse.parse_qs(urllib.parse.urlparse(pasted).query)
        CodeCatcher.code = (q.get("code") or [None])[0]
        if not CodeCatcher.code:
            sys.exit("that URL has no ?code= in it.")
        print("   got the code from the pasted URL")

    status, text = http_json(TOKEN_URL, {
        "client_id": client["client_id"],
        "client_secret": client.get("client_secret", ""),
        "code": CodeCatcher.code,
        "code_verifier": verifier,
        "grant_type": "authorization_code",
        "redirect_uri": redirect_uri,
    }, {"Content-Type": "application/x-www-form-urlencoded"})
    if status != 200:
        sys.exit(f"token exchange failed: HTTP {status}\n{text[:400]}")
    return json.loads(text)["access_token"]


def acl_url(cal_id, suffix=""):
    return (f"https://www.googleapis.com/calendar/v3/calendars/"
            f"{urllib.parse.quote(cal_id)}/acl{suffix}")


def acl_has(token, cal_id, sa_email):
    """Is the service account already in this calendar's ACL? Returns
    True/False, or None if the question could not be answered."""
    status, text = http_json(acl_url(cal_id),
                             headers={"Authorization": f"Bearer {token}"})
    if status != 200:
        return None
    for rule in json.loads(text).get("items", []):
        if rule.get("scope", {}).get("value", "").lower() == sa_email.lower():
            return rule.get("role", "?")
    return False


def ensure_acl(token, cal_id, sa_email):
    """Install the reader permission, idempotently.

    acl.insert on Calendar is flaky and returns 503 backendError more often than
    it should. Google's documented remedy for a backend error is exponential
    backoff -- but a failed-looking write can still have landed, so every
    attempt re-reads the ACL first rather than blindly inserting again. Worst
    case that costs one extra GET; the alternative is duplicate rules or giving
    up on a permission that actually exists."""
    body = {"role": "reader", "scope": {"type": "user", "value": sa_email}}
    headers = {"Authorization": f"Bearer {token}",
               "Content-Type": "application/json"}

    # `primary` resolves to the authorising user's own calendar. It is a
    # different server-side path from addressing the same calendar by email,
    # and sometimes succeeds when the email form does not.
    targets = [cal_id, "primary"]

    for attempt in range(5):
        for target in targets:
            existing = acl_has(token, target, sa_email)
            if existing:
                print(f"   already present: role '{existing}'")
                return True

            status, text = http_json(acl_url(target, "?sendNotifications=false"),
                                     body, headers, method="POST")
            if status in (200, 201):
                print(f"   inserted (HTTP {status}"
                      + (f", via '{target}'" if target != cal_id else "") + ")")
                return True

            if status in (500, 503):
                print(f"   HTTP {status} backend error"
                      + (f" on '{target}'" if target != cal_id else "")
                      + " — checking whether it landed anyway...")
                if acl_has(token, target, sa_email):
                    print("   it did. The error was spurious.")
                    return True
            else:
                print(f"   HTTP {status}: {text[:200]}")
                if status in (401, 403):
                    return False              # a permission problem; retrying will not help

        if attempt == 4:
            break                       # no point sleeping before giving up
        delay = 2 ** attempt
        print(f"   retrying in {delay}s (attempt {attempt + 2} of 5)...")
        time.sleep(delay)

    return bool(acl_has(token, cal_id, sa_email))


def main():
    env = load_env(DEFAULT_ENV_PATH)
    cal_id = env.get("GOOGLE_CALENDAR_ID", "")
    sa_path = env.get("GOOGLE_SA_KEY_PATH", "")
    oauth_path = env.get("GOOGLE_OAUTH_CLIENT_PATH", "")
    if not (cal_id and sa_path and oauth_path):
        sys.exit("Set GOOGLE_CALENDAR_ID, GOOGLE_SA_KEY_PATH and "
                 "GOOGLE_OAUTH_CLIENT_PATH in .env first.\n"
                 "See the header of this file for how to create the OAuth client.")

    sa_file = pathlib.Path(sa_path).expanduser()
    oauth_file = pathlib.Path(oauth_path).expanduser()
    for f in (sa_file, oauth_file):
        if not f.exists():
            sys.exit(f"missing file: {f}")

    sa_email = json.loads(sa_file.read_text())["client_email"]
    raw = json.loads(oauth_file.read_text())
    client = raw.get("installed") or raw.get("web")
    if not client:
        sys.exit("that OAuth JSON is neither an 'installed' nor a 'web' client. "
                 "Create one of type 'Desktop app'.")
    if "installed" not in raw:
        print("warning: this looks like a Web client. 'Desktop app' is the type "
              "that works with a loopback redirect.\n")

    print(f"calendar        : {cal_id}")
    print(f"granting reader : {sa_email}\n")

    token = authorise(client)
    print("   ok — authorised\n")

    print("installing the permission...")

    if not ensure_acl(token, cal_id, sa_email):
        print("\nCould not install the permission. Tell me and we will switch to "
              "the fallback plan\n(the calendar's secret iCal URL, or a calendar "
              "the service account owns).")
        return 1

    print("The access token was used once and has been discarded; nothing was "
          "saved to disk.\nNow re-run:  python3 tools/check_calendar.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
