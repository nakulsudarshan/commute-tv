#!/usr/bin/env python3
"""
Probe a GeekMagic SmallTV Ultra's stock HTTP API.

This is Phase 0. It confirms, against your actual unit, the endpoints recovered
from the firmware binary -- and in particular discovers the multipart field name
that /doUpload expects, which the binary does not reveal.

It works over the device's OWN hotspot, so no home network or dorm approval is
needed:

    1. Connect this computer to the WiFi network "GIFTV"
    2. python3 tools/probe_smalltv.py --host 192.168.4.1

Read-only by default. Add --test-display to actually upload a test image and
switch the screen to it; the original theme is restored afterwards.

    python3 tools/probe_smalltv.py --host 192.168.4.1 --test-display
"""

import argparse
import base64
import json
import pathlib
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# A 240x240 JPEG, embedded so the script has no dependencies at all.
ROOT = pathlib.Path(__file__).resolve().parent.parent

TEST_JPEG_B64 = (
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAYEBQYFBAYGBQYHBwYIChAKCgkJChQODwwQFxQYGBcU"
    "FhYaHSUfGhsjHBYWICwgIyYnKSopGR8tMC0oMCUoKSj/2wBDAQcHBwoIChMKChMoGhYaKCgoKCgo"
    "KCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCj/wAARCADwAPADASIA"
    "AhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQA"
    "AAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3"
    "ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWm"
    "p6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEA"
    "AwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSEx"
    "BhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElK"
    "U1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3"
    "uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDhvHvj"
    "HUPD2sQ2llDavG8AlJlViclmHZhxwK5r/hZ2s/8APrp3/ft//i6PjF/yM1r/ANea/wDob1wtEYpo"
    "+ayrKsHWwdOpUppto7r/AIWdrP8Az66d/wB+3/8Ai6P+Fnaz/wA+unf9+3/+LrhaKrkR6H9i4D/n"
    "0juv+Fnaz/z66d/37f8A+Lo/4WdrP/Prp3/ft/8A4uuFoo5EH9i4D/n0juv+Fnaz/wA+unf9+3/+"
    "Lo/4WdrP/Prp3/ft/wD4uuFoo5EH9i4D/n0juv8AhZ2s/wDPrp3/AH7f/wCLo/4WdrP/AD66d/37"
    "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"
)

READ_ENDPOINTS = [
    ("/v.json",                 "firmware / model identity"),
    ("/app.json",               "current theme"),
    ("/theme_list.json",        "available themes"),
    ("/filelist?dir=/image/",   "stored images"),
    ("/brt.json",               "brightness"),
    ("/tz.json",                "timezone"),
    ("/city.json",              "weather city"),
    ("/rotation.json",          "screen rotation"),
    ("/hour12.json",            "12/24h format"),
    ("/unit.json",              "temperature unit"),
]

UPLOAD_FIELD_CANDIDATES = ["file", "upload", "data", "image", "filename", "doUpload"]

# Confirmed against firmware Ultra-V9.0.40: theme 3 is picture mode.
PICTURE_THEME = 3
# Anything that is not picture mode, used when the previous theme WAS picture
# mode and so cannot be meaningfully restored.
FALLBACK_THEME = 0


class Client:
    def __init__(self, host, user=None, password=None, timeout=10):
        self.base = f"http://{host}"
        self.timeout = timeout
        self.auth = None
        if user:
            token = base64.b64encode(f"{user}:{password or ''}".encode()).decode()
            self.auth = f"Basic {token}"

    def _req(self, req):
        if self.auth:
            req.add_header("Authorization", self.auth)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()
        except Exception as e:                                    # noqa: BLE001
            return 0, str(e).encode()

    def get(self, path):
        return self._req(urllib.request.Request(self.base + path))

    def post_multipart(self, path, field, filename, payload, content_type="image/jpeg"):
        boundary = "----commutetv" + str(int(time.time()))
        body = b"".join([
            f"--{boundary}\r\n".encode(),
            f'Content-Disposition: form-data; name="{field}"; filename="{filename}"\r\n'.encode(),
            f"Content-Type: {content_type}\r\n\r\n".encode(),
            payload,
            f"\r\n--{boundary}--\r\n".encode(),
        ])
        req = urllib.request.Request(self.base + path, data=body, method="POST")
        req.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
        req.add_header("Content-Length", str(len(body)))
        return self._req(req)


def text(body):
    """Response bodies are bytes. Everything below wants str."""
    if isinstance(body, bytes):
        return body.decode("utf-8", "replace")
    return body if isinstance(body, str) else ""


def parse_filelist(body):
    """
    Pull (name, href) out of the device's /filelist HTML.

    It is a table, not JSON:
      <tr><td>1</td><td><a href='/image//foo.jpg'>foo.jpg</a></td><td>21</td>...

    Note the DOUBLED slash in the href. The device builds its paths as
    dir + "/" + name with dir already ending in "/", and its own delete button
    passes that same doubled form -- which is very likely why a delete built the
    tidy way comes back "Fail".
    """
    html = text(body)
    return re.findall(r"href='([^']*)'>([^<]*)</a>", html)


def show(status, body, limit=160):
    if status == 0:
        return f"unreachable ({body.decode('utf-8', 'replace')[:80]})"
    t = text(body).strip().replace("\n", " ")
    if len(t) > limit:
        t = t[:limit] + "..."
    return f"HTTP {status}  {t}"


# Recovered from the device's own image.html (25 Aug 2026):
#
#     function deletef(h) {
#         var fnstring = String("/delete?file=") + encodeURIComponent(h);
#     }
#     ... onclick="deletef('/image//spaceman.gif')"
#
# encodeURIComponent escapes '/' as %2F, so the UI sends BOTH a doubled slash
# and full encoding. That exact form goes first; the rest are here because a
# correct server decodes %2F and would see the first two as identical, while a
# naive one would not.
# PROVEN on the device, 25 Aug 2026: the doubled slash is what matters and the
# encoding does not. /delete?file=/image//probe.jpg removed the file;
# /delete?file=/image/probe.jpg did not. Both answered "Fail".
DELETE_FORMS = [
    ("dir//name",      lambda d, n: d.rstrip("/") + "//" + n, "path"),
    ("ui exact",       lambda d, n: d.rstrip("/") + "//" + n, "full"),
    ("dir/name enc",   lambda d, n: d.rstrip("/") + "/" + n,  "full"),
    ("dir/name",       lambda d, n: d.rstrip("/") + "/" + n,  "path"),
    ("name only",      lambda d, n: n,                        "full"),
]


def free_space(client):
    """GET /space.json -- the number that says whether housekeeping works."""
    st, bd = client.get("/space.json")
    if not 200 <= st < 300:
        return None
    m = re.search(r'"free"\s*:\s*"?(\d+)', text(bd))
    return int(m.group(1)) if m else None


def file_present(client, name):
    """Ask the device, rather than trusting a status code."""
    st, bd = client.get("/filelist?dir=/image/")
    if not 200 <= st < 300:
        return None, bd                     # cannot tell
    entries = parse_filelist(bd)
    return any(n == name or n.endswith("/" + name) for _, n in entries), bd


def try_delete(client, name, verbose=False):
    """
    Delete `name` from /image/, working out which URL form this firmware wants.

    Returns (form_label, response_text) on success, or (None, notes) if none of
    them worked. Success means THE FILE IS GONE FROM THE LISTING.

    Neither the status nor the body is consulted: V9.0.40 answers "Fail" whether
    or not it deleted the file. Both of these were observed minutes apart --

        /delete?file=/image/probe.jpg    HTTP 200  Fail   -> still there
        /delete?file=/image//probe.jpg   HTTP 200  Fail   -> gone

    -- so the listing is the only evidence there is.
    """
    notes = []
    present, _ = file_present(client, name)
    if present is False:
        return "not present", "nothing to delete"

    for label, build, enc in DELETE_FORMS:
        raw = build("/image/", name)
        # safe="" reproduces encodeURIComponent; safe="/" is what a person and
        # our own C++ path encoder would write.
        quoted = urllib.parse.quote(raw, safe="" if enc == "full" else "/")
        path = "/delete?file=" + quoted
        st, bd = client.get(path)
        body = text(bd).strip()
        if verbose:
            print(f"    {path}  ->  HTTP {st}  {body[:60]}")
        gone, _ = file_present(client, name)
        if gone is False:
            note = f"HTTP {st} {body[:40] or '(empty)'}"
            if "fail" in body.lower():
                note += "  <- said Fail, deleted it anyway"
            return label, note
        notes.append(f"{label}: HTTP {st} {body[:40] or '(empty)'}")

    return None, "; ".join(notes)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="192.168.4.1",
                   help="SmallTV address (192.168.4.1 over its own GIFTV hotspot)")
    p.add_argument("--user", help="web console username, if you set one")
    p.add_argument("--password")
    p.add_argument("--test-display", action="store_true",
                   help="upload a test image and show it (restores the theme afterwards)")
    p.add_argument("--leave-file", action="store_true",
                   help="do not delete probe.jpg afterwards (it stays on the "
                        "device's flash for ever if you use this)")
    p.add_argument("--cleanup", action="store_true",
                   help="delete probe.jpg and exit, for a probe that was "
                        "interrupted before it could tidy up")
    p.add_argument("--keep", action="store_true",
                   help="with --test-display, leave the test image on screen")
    args = p.parse_args()

    c = Client(args.host, args.user, args.password)
    print(f"Probing SmallTV at http://{args.host}\n")

    # --- reachability -------------------------------------------------------
    status, body = c.get("/v.json")
    if status == 0:
        print(f"  Cannot reach the device: {body.decode('utf-8', 'replace')[:120]}")
        print("\n  Checklist:")
        print("   - is this computer on the SmallTV's network (GIFTV, or your WiFi)?")
        print("   - if on a phone hotspot, client isolation may be blocking you")
        print("   - try opening http://%s in a browser first" % args.host)
        return 1
    if status == 401:
        print("  HTTP 401: the web console has a password. Re-run with --user/--password.")
        return 1

    # --- read-only endpoints ------------------------------------------------
    print("READ-ONLY ENDPOINTS")
    results = {}
    for path, desc in READ_ENDPOINTS:
        st, bd = c.get(path)
        results[path] = (st, bd)
        flag = "ok " if 200 <= st < 300 else "!! "
        print(f"  {flag}{path:<26} {desc:<24} {show(st, bd, 90)}")

    # --- current theme, so we can restore it --------------------------------
    original_theme = None
    st, bd = results.get("/app.json", (0, b""))
    if 200 <= st < 300:
        try:
            original_theme = json.loads(bd.decode("utf-8", "replace")).get("theme")
        except json.JSONDecodeError:
            pass
    print(f"\n  Current theme: {original_theme if original_theme is not None else 'unknown'}")

    # Theme 3 is picture mode. If the device is ALREADY on it, restoring the
    # theme afterwards is a no-op -- the screen stays on whatever image we set,
    # because /set?img= is a separate, sticky setting and /app.json does not
    # report which image is displayed. There is no way to put the previous
    # photo back, so say so before touching anything.
    if original_theme == PICTURE_THEME and args.test_display:
        print()
        print("  ⚠️  The device is already on the picture theme, so this probe")
        print("      CANNOT put the current photo back afterwards -- the API does")
        print("      not report which image is showing. It will be left on a")
        print(f"      clock theme instead ({FALLBACK_THEME}); reselect your photo in the")
        print("      web console if you want it back.")

    if args.cleanup:
        print("\nCLEANUP ONLY")
        form, detail = try_delete(c, "probe.jpg", verbose=True)
        if form:
            print(f"  probe.jpg deleted (URL form: {form})")
        else:
            print(f"  could not delete probe.jpg -- {detail}")
            print("  Delete it from the web console instead.")
        _, bd = file_present(c, "probe.jpg")
        print(f"  files now: {[n for _, n in parse_filelist(bd)]}")
        return 0

    if not args.test_display:
        print("\nRead-only probe complete. Re-run with --test-display to test the screen.")
        return 0

    # --- upload -------------------------------------------------------------
    # The binary confirms /doUpload exists but not the multipart field name it
    # expects, so try the plausible candidates and report which one works.
    free_before = free_space(c)
    if free_before is not None:
        print(f"\n  Free image space before we start: {free_before // 1024} KB")

    print("\nUPLOAD TEST")
    # Prefer a frame the firmware's own encoder produced. The embedded fallback
    # is a 6.5 KB JPEG -- about a third of the bytes a real frame uses -- so its
    # ringing around small text says nothing about what the device will actually
    # show. `make preview` writes the real thing.
    real_frame = ROOT / "preview" / "out" / "01_normal.jpg"
    if real_frame.exists():
        payload = real_frame.read_bytes()
        print(f"  test image: {len(payload)} bytes, a REAL rendered frame "
              f"({real_frame.relative_to(ROOT)})")
    else:
        payload = base64.b64decode(TEST_JPEG_B64)
        print(f"  test image: {len(payload)} bytes, the built-in placeholder")
        print("  (run `make preview` first to test with a real frame instead)")

    working_field = None
    for field in UPLOAD_FIELD_CANDIDATES:
        st, bd = c.post_multipart("/doUpload?dir=/image/", field, "probe.jpg", payload)
        ok = 200 <= st < 300
        print(f"    field name '{field}':  {show(st, bd, 70)}")
        if ok:
            working_field = field
            break

    if not working_field:
        print("\n  No field name accepted. Capture the real request from the web UI:")
        print("   browser -> F12 -> Network -> upload an image -> copy the request.")
        return 1

    print(f"\n  Upload accepted with field name: '{working_field}'")

    st, listing_after_first = c.get("/filelist?dir=/image/")
    files_before = [n for _, n in parse_filelist(listing_after_first)]
    print(f"  files now: {files_before}")

    # --- does a second upload of the same name REPLACE the first? -----------
    #
    # This decides how the firmware has to behave. The device keeps uploads in
    # LittleFS permanently and has 4 MB of flash; an alert sends a fresh frame
    # every minute for 35 minutes. If the same filename overwrites, steady state
    # is one file. If it appends, one alert leaves 35 behind and a few days of
    # commuting fills the partition.
    print("\nOVERWRITE TEST")
    print("  Uploading the same filename a second time...")
    st2, _ = c.post_multipart("/doUpload?dir=/image/", working_field, "probe.jpg", payload)
    st3, listing_after_second = c.get("/filelist?dir=/image/")
    files_after = [n for _, n in parse_filelist(listing_after_second)]

    before = sum(1 for n in files_before if "probe" in n)
    after = sum(1 for n in files_after if "probe" in n)
    print(f"  second upload: HTTP {st2}")
    print(f"  files now: {files_after}")
    print(f"  copies of probe.jpg: {before} -> {after}")
    if after > before or after > 1:
        print("  >>> APPENDS. The firmware must delete before every upload:")
        print("      set Config::deleteBeforeUpload = true in smalltv_client.h.")
    elif after == 1:
        print("  >>> OVERWRITES. Deleting once, on release, is enough.")
    else:
        print("  >>> INCONCLUSIVE -- probe.jpg is not in the listing at all.")
        print(f"      Raw listing: {show(st3, listing_after_second, 300)}")

    # --- display it ---------------------------------------------------------
    print("\nDISPLAY TEST")
    st, bd = c.get(f"/set?theme={PICTURE_THEME}")
    print(f"  /set?theme={PICTURE_THEME}            {show(st, bd, 60)}")
    time.sleep(1)
    st, bd = c.get("/set?img=" + urllib.parse.quote("/image/probe.jpg"))
    print(f"  /set?img=/image/probe.jpg  {show(st, bd, 60)}")

    print("\n  >>> Look at the SmallTV now. You should see a dark screen with a")
    print("      green 'COMMUTE-TV' bar and '240x240 OK' in orange.")

    if args.keep:
        print("\n  Leaving it on screen (--keep).")
    else:
        input("\n  Press Enter to restore the previous theme...")
        # Never restore the picture theme. Doing so hands the screen back still
        # showing OUR test image, which looks exactly like the probe broke the
        # device. src/smalltv/smalltv_client.cpp has the same guard, and this is
        # the case that proved it was needed.
        target = original_theme
        if target is None:
            target = FALLBACK_THEME
            print(f"  Original theme unknown; falling back to theme {target}.")
        elif target == PICTURE_THEME:
            target = FALLBACK_THEME
            print(f"  Previous theme was picture mode, which would leave the test")
            print(f"  image up. Switching to theme {target} instead.")
        st, bd = c.get(f"/set?theme={target}")
        print(f"  restored theme {target}: {show(st, bd, 60)}")
        if original_theme == PICTURE_THEME:
            print("  Reselect your own photo in the web console if you want it back.")

    # --- tidy up ------------------------------------------------------------
    #
    # The first version of this probe did not, and probe.jpg was still sitting
    # on the device days later. Uploads are permanent; a tool that leaves
    # litter on a 4 MB filesystem is a tool with a bug.
    if args.leave_file:
        print("\n  Leaving /image/probe.jpg on the device (--leave-file).")
    else:
        print("\nCLEANUP")
        # The status code is worthless here: a failed delete on V9.0.40 returns
        # HTTP 200 with the body "Fail". The only trustworthy check is whether
        # the file is still in the listing afterwards.
        form, detail = try_delete(c, "probe.jpg", verbose=True)
        if form == "not present":
            print("  probe.jpg was not in the listing.")
        elif form:
            print(f"  probe.jpg deleted. Working URL form: {form}  ({detail})")
            if form != "dir//name":
                print("  >>> Tell me this form -- the firmware is built around it.")
        else:
            print(f"  >>> COULD NOT DELETE probe.jpg. Tried: {detail}")
            print("      Delete it from the web console, and send me this output:")
            print("      the firmware depends on this endpoint to keep the")
            print("      device's image partition from filling up.")

    free_after = free_space(c)
    if free_after is not None:
        print(f"\n  Free image space now: {free_after // 1024} KB")
        if free_before is not None and free_after < free_before:
            print(f"  >>> {(free_before - free_after) // 1024} KB LESS than before "
                  f"-- something of ours is still on the device.")

    # --- settings that would quietly ruin an alert --------------------------
    print("\nSETTINGS THAT MATTER")
    st, bd = c.get("/album.json")
    body = text(bd)
    print(f"  /album.json  {show(st, bd, 80)}")
    m = re.search(r'"autoplay"\s*:\s*"?(\d+)', body)
    if m and m.group(1) != "0":
        print("  >>> AUTOPLAY IS ON. The device cycles stored images on a timer,")
        print("      so a commute frame would be replaced by the next picture")
        print("      seconds after it appeared. The firmware turns this off while")
        print("      it holds the screen and puts it back afterwards -- but it is")
        print("      worth knowing it is on.")
    elif m:
        print("  autoplay is off -- nothing to work around.")

    print("\n  Check in the web console -> Settings: 'Auto Switch Themes'")
    print("  must be OFF. If it is on the device rotates away from picture mode")
    print("  mid-alert. The firmware re-asserts the theme every minute, so the")
    print("  screen would come back, but it would flicker.")

    print("\nProbe complete. Send me this whole output.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
