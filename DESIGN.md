# commute-tv

Firmware for an ESP32 that watches your Google Calendar, works out when you need
to leave to catch the bus, and pushes the answer to a GeekMagic SmallTV Ultra
running **unmodified stock firmware**.

See [`../IMPLEMENTATION_PLAN.md`](../IMPLEMENTATION_PLAN.md) for the full design.

---

## Status

Built and tested so far — the logic that is worth getting right before any
hardware exists:

| Module | What it does | Tests |
|---|---|---|
| `src/core/timeutil` | HAFAS date/time parsing, minute arithmetic | ✅ |
| `src/core/types.h` | Itinerary / leg / weather / route structs | — |
| `src/core/urlencode` | RFC 3986 escaping, shared by HAFAS and the SmallTV | ✅ |
| `src/core/textutil` | UTF-8 → ASCII folding for the baked font | ✅ |
| `src/core/ratelimit` | Request budget: interval, caps, backoff | ✅ |
| `src/core/places` | Calendar location text → a known coordinate | ✅ |
| `src/core/comfort` | Cycling comfort score | ✅ |
| `src/core/trip_select` | Choosing which itinerary to act on | ✅ |
| `src/core/alert` | Leave time, alert window, state machine, quiet hours | ✅ |
| `src/json/json_stream` | Streaming JSON reader with path matching | ✅ |
| `src/hafas/hafas_request` | AVV HAFAS URL construction | ✅ |
| `src/hafas/hafas_parse` | Trip response → itineraries, streaming | ✅ + real fixtures |
| `src/hafas/hafas_client` | trip() and resolve() over the network | ✅ |
| `src/net/http` | Streaming HTTP client interface | — |
| `src/ors/ors_client` | Cycling route and elevation | ✅ |
| `src/weather/meteo_client` | Forecast for the departure hour | ✅ |
| `src/oled/mono_canvas` | 1bpp buffer in SSD1306 page order | ✅ |
| `src/oled/ssd1306` | Init sequence and flush over I²C | ✅ |
| `src/oled/health` | The health panel layout | ✅ |
| `src/google/jwt` | base64url, RS256 service-account assertion | ✅ |
| `src/google/calendar` | Token exchange, events.list → appointments | ✅ |
| `src/render/canvas` | RGB565 framebuffer, text, clipping, BMP writer | ✅ |
| `src/render/screen` | The 240×240 layout | ✅ |
| `src/render/band_renderer` | Draws the screen one strip at a time | ✅ |
| `src/jpeg/jpeg_encoder` | Baseline JPEG, RGB565 → JFIF | ✅ + decoder check |
| `src/smalltv/smalltv_request` | Stock HTTP API: URLs, multipart, parsing | ✅ |
| `src/smalltv/smalltv_client` | Take/release the screen, upload, frame gate | ✅ |
| `tools/probe_smalltv.py` | Phase 0 device probe | — |
| `tools/record_fixtures.py` | Captures real AVV responses for offline work | — |
| `tools/verify_jpeg.py` | Checks our JPEGs against an independent decoder | — |
| `tools/check_env.py` | Validates `.env`; `--live` proves both API keys work | — |

**1655 assertions, all passing, zero warnings** at `-Wall -Wextra -Wpedantic
-Wshadow -Wconversion`, and clean under `-fsanitize=address,undefined`.

Not yet built: web config UI, orchestrator, OTA.

`firmware/` is a PlatformIO project for the ESP32. At present it is a bring-up
stage: OLED, WiFi (PSK and 802.1X), NTP, and the health panel showing real
state. The shared code under `src/` is compiled straight into it rather than
copied, so `make test` here and `pio run` there build the same files.

---

## Running the tests

No libraries, no toolchain, no hardware. Just a C++17 compiler:

```
cd commute-tv
make test                 # 1655 assertions
make firmware-includes    # the headers resolve the way the firmware includes them
```

```
----------------------------------------
1655 passed, 0 failed
```

Every module here is deliberately free of Arduino, networking and third-party
library dependencies, so it compiles on your laptop in about a second. The parts
most likely to be subtly wrong — time arithmetic across midnight, delay
handling, band boundaries, the alert window, JPEG byte stuffing, a JSON token
split across two socket reads — are exactly the parts you can test without
plugging anything in.

If `fixtures/` holds recorded AVV responses, the parser tests additionally run
against those real files; without them they fall back to an embedded excerpt and
still cover every case. Nothing fails when the fixtures are absent.

## Previewing and verifying the screen

```
make preview                      # renders 12 scenarios to BMP *and* JPEG
python3 tools/verify_jpeg.py      # decodes them with Pillow and compares
```

`make preview` prints the encoded size of every scenario against the upload
budget. `verify_jpeg.py` pushes the bytes through a decoder we did not write
and reports PSNR per scenario, plus the worst 64 px tile — because a mean
figure says nothing about whether 12 px amber text survived. It also writes
`preview/out/jpeg_compare.png`, the worst region magnified, canvas on the left
and decoded JPEG on the right.

```
All 12 scenarios pass: structure valid, PSNR >= 35.0 dB, worst tile >= 31.0 dB.
```

Needs `pillow` and `numpy`; nothing else in this repo does.

---

## Previewing the health panel

```
make oled                         # writes preview/out/oled_*.pgm
make oled ARGS=--ascii            # or print them in the terminal
```

Four states: armed and working, idle with nothing fetched yet, the panel doing
its actual job, and the boot splash. Reviewing a 128x64 layout is much faster
on a laptop than on a desk with a soldering iron.

---

## Checking your configuration

```
python3 tools/check_env.py           # shape and sanity, no network
python3 tools/check_env.py --live    # one call to each API, proves the keys
```

Every secret is masked, so the output is safe to share. Beyond presence checks
it detects transposed home coordinates — openrouteservice takes
**longitude, latitude** while Google Maps shows **latitude, longitude**, and
that transposition otherwise surfaces as a routing failure rather than a config
error.

---

## Recording AVV fixtures

AVV state their demo system is unavailable outside German office hours. Run this
once during the working day and development is no longer hostage to it:

```
python3 tools/record_fixtures.py --access-id YOUR_ID \
    --home 50.7753,6.0839 --dest 50.7660,6.0450
```

Your access ID is redacted from every file written.

---

## Design notes worth knowing

**Times are local throughout.** HAFAS returns local times, calendar events are
local, and the ESP32's clock is set to local time via `configTzTime`. No UTC
conversion happens anywhere, because introducing one would add a class of bug
for no benefit.

**Hours ≥ 24 are valid.** HAFAS emits `25:10:00` for a journey crossing
midnight. `parseDateTime` rolls the date forward accordingly — this is tested.

**Realtime falls back to planned in one place only.** `Leg::depTime()` and
`arrTime()` decide it. No caller picks a field itself. That single rule is what
prevents the "showed the scheduled time during a delay" class of bug.

**The latest departure wins.** Given several itineraries that all arrive in
time, `selectTrip` picks the one leaving last. There is no virtue in leaving
early, and an alert that fires sooner than necessary gets ignored.

**When you cannot make it, say so.** If no itinerary arrives before the event,
`selectTrip` returns the least-late option and flags `willBeLate` with the
number of minutes, rather than showing nothing.

**State is derived from the clock, never from timers.** `evaluate()` takes
`now` and recomputes from scratch. A reboot mid-alert lands back in the right
state within one tick instead of losing the window.

**Comfort band boundaries hold exactly.** The ±1 °C smoothing sits *outside*
each band, so 10 °C scores exactly 0.5, and 15 °C and 28 °C score exactly 1.0 —
the values in the specification. Only past a boundary does the score slide.
This is asserted directly in the tests.

**Missing data is never silently treated as good data.** No weather assumes a
neutral 20 °C rather than a perfect score; a missing cycling route yields no
score at all rather than a misleading zero.

**Every threshold is configuration, not a constant.** `ComfortConfig` and
`AlertConfig` carry all of them, so tuning the scoring from the web UI never
requires a reflash.

**Text collisions are prevented structurally**, not by careful wording.
`rowLR()` measures the right-hand string and clips the left. The bike row
*drops* the ascent rather than truncating it, because "+8…" still reads as a
number and misleads.

### Reading the calendar

**The RSA is not the risky part.** A service account authenticates by signing a
JWT, and the signature itself is mbedTLS's job on the device. What this project
can get wrong is *what gets signed* — base64url with the wrong alphabet or a
padding character, a claim set with a stray `sub`, a clock an hour out. So the
signature sits behind an interface, the tests use a stub that records its input
byte for byte, and every one of those failure modes is asserted directly.

**One place uses UTC, and it has to.** The assertion's `iat` and `exp` are
checked against Google's clock. Everything else in this firmware is local time
on purpose, and the calendar request asks for `timeZone=Europe/Berlin` so that
`start.dateTime` comes back as local wall time and is read exactly like a HAFAS
one. Nothing converts anything.

**An event without a location is not a commute.** That rule is applied where the
response is parsed rather than downstream, because there are six slots and a day
full of reminders that name no place would push out the appointment that
mattered. Google returns `""` for a cleared location; that counts as absent.

### The panel that admits things

The SmallTV shows a commute for thirty-five minutes a day. The rest of the time
the useful question is whether the thing is still working, and the little OLED
is the only place the firmware ever says otherwise. So every line follows one
rule: say what is true, including when what is true is "we don't know".

"Never fetched" reads `no data`, not a small number that looks fresh. An
unsynced clock reads `no clock`, not `--:--` that a tired reader takes for a
time. A signal strength of zero is the absence of a reading, so the number is
left off rather than shown as perfect. Those are the assertions in the tests —
not that the pixels are in the right place, but that the panel cannot say
something reassuring it has not earned.

Its framebuffer is stored in the SSD1306's own page layout — one byte is eight
pixels *vertically* — so a flush is a memcpy rather than transposing a kilobyte
on a device with better things to do. And the I²C write is the only part behind
an interface, because everything else is byte generation and byte generation can
be asserted: including the two initialisation bytes whose wrong values are
correct for a different module, and which turn a working panel into a dark one
with nothing to say about why.

### Asking the right question about the weather

Not "what is it like now" — an alert opens half an hour before departure, and
the question the comfort score is really asking is what it will be like when
someone is actually on the bike. Open-Meteo's `start_hour`/`end_hour` narrow the
request to that one hour, so the response is a few hundred bytes with one value
per variable and no index-matching across parallel arrays.

**A null is not a zero.** Open-Meteo returns `null` for a variable its model
does not carry. Reading that as zero would turn "we don't know whether it will
rain" into "it will not rain", and put someone on a bike in a storm. Nulls leave
the field at its neutral default; a missing temperature fails the fetch outright,
because the score would otherwise be guessing.

**openrouteservice is here for elevation and nothing else.** Distance and
duration could come from anywhere. A well-formed route that arrives without an
ascent is therefore rejected rather than accepted — a climb scored as flat is
worse than no score. And its coordinates are `[longitude, latitude]`, the
opposite of the Google Maps order every coordinate here was copied from, so the
request body is asserted against literal JSON in both hemispheres.

### Naming places the way people actually name them

A destination label is compared against what someone types into a calendar
event, and against nothing else — not Google Maps, not the nearest stop's
official name. So the match is forgiving: case, spacing and punctuation are
ignored, a trailing address is fine, and the German digraphs are collapsed so
`Hauptgebäude`, `Hauptgebaeude` and `Hauptgebaude` are one place. That last one
is not decoration — the same event gets typed on a phone with a German keyboard
and on a laptop without one.

Matching is whole-word, though. `Supercomputer Centre` does not match `SuperC`,
because a miss costs one lookup while a wrong match costs a wasted journey.

### Housekeeping on a 4 MB device

The SmallTV keeps every image it is sent, permanently. At ~20 KB a frame and one
frame a minute through a 35-minute alert, an alert that left its frames behind
would cost ~700 KB — and a few days of commuting would fill the flash of a
device this project has promised never to reflash.

So the client deletes its image when it hands the screen back, after restoring
the theme. A refused delete does not fail the release — the screen is already
back — but it is counted and surfaced, because a device filling up silently is
the failure you find out about far too late.

**The device's own web page is the API documentation.** Saving the console's
five pages and reading their inline JavaScript settled more than the firmware
binary ever did: the exact delete URL, a free-space endpoint, the theme numbers,
and two settings that would have quietly ruined every alert — photo-album
autoplay, which cycles stored images on a timer, and automatic theme rotation.
The client suppresses the first for the duration of an alert and restores it
afterwards; the second it deliberately leaves alone, because that setting cannot
be read back on this firmware and rewriting something you cannot restore is
worse than the problem.

**Nothing the delete endpoint says is evidence.** It answers HTTP 200 with the
body `Fail` whether or not it removed the file — measured both ways, minutes
apart. Status, body and outcome are three independent facts, so the client
ignores the first two and checks whether the name is still in the file list. And because the working path spelling is known only from
the device's own web page — its delete button passes `/image//name.jpg`, with
the slash doubled — the client tries that first and falls back to the other
spellings, remembering whichever actually removes the file. The mock models all
of it, including the lying 200, so the tests fail if the client ever goes back
to trusting a status code.

### Staying inside the usage agreement

The AVV agreement sets no rate limit, but clause 3.a permits dynamic data only
as live queries and clause 6.a asks for technical measures against mass
retrieval. `core::RequestBudget` is those measures: a minimum interval, rolling
hour and day caps, exponential backoff, and `Retry-After` support.

It is a **fuse, not a throttle**. Ordinary use is a few hundred requests a day
and never comes close to the caps. What it stops is a spinning loop: one call a
second for two hours gets through about 120 times, and the same loop against a
server that is down makes twelve attempts rather than 3,600. Both are asserted.

The windows roll rather than resetting at a boundary, and they run off a
monotonic clock — a wall-clock limiter is defeated by an NTP step, and this
device boots without a clock at all.

Clause 3.b requires the AVV mark wherever results are displayed, so the footer
carries it on every result screen, unconditionally, with tests that fail if a
layout change drops it.

### The response pipeline

**The JSON document is never held whole.** A trip reply is ~36 KB compact; the
board's largest contiguous allocation is 110,580 bytes. Buffering the body would
make it the single biggest allocation in the firmware, competing with TLS and
the JPEG buffer for the one usable DRAM region. So bytes go into the parser as
they come off the socket, and only the fields the screen shows come out —
**8,760 bytes of fixed state**, the same for three itineraries or three hundred.

**The parser knows where it is.** Scanning for `"time":` and taking what follows
is wrong on this data and quietly so: a leg carries `Origin.time`,
`Destination.time`, *and* a whole `JourneyDetail.Stops.Stop[].time` list. The
reader tracks the path, so the handler asks for
`Trip[*].LegList.Leg[*].Origin.time` and gets exactly that. Unknown fields match
nothing, which is what makes this survive the schema drift we already know about
— the XSD we were given is 2.49.0 and the server reports 2.51.0.

**Any chunking gives the same answer.** The transport decides where the reads
split, including inside a `\uXXXX` escape, so that property is asserted rather
than assumed: every chunk size from one byte upward produces identical events.

**A truncated body fails.** Two complete itineraries followed by a cut third
would otherwise look like a successful reply that happens to be missing the best
option.

**An itinerary with too many legs is dropped whole**, never trimmed. The arrival
time comes from the last leg, so keeping the first six would put a too-early
arrival on the screen — worse than showing one connection fewer, and the count
is reported.

**Names are folded to ASCII where they are parsed.** The baked font covers
U+0020..U+007E, so "Aachen, Hörn Brücke" would render as "Aachen, Hrn Brcke" —
which reads as a broken display rather than a missing glyph. Folding at the
parse boundary means no rendering code has to remember, and the German digraphs
cost no extra bytes.

### The image pipeline

**No chroma subsampling.** The screen is not a photograph: it is 12 px coloured
text on a near-black panel, which is the content 4:2:0 damages most. Measured
across the twelve scenarios, 4:2:0 plateaus around 30 dB *however much quality
you spend* — the loss is structural, not a quantiser setting — while 4:4:4 at
quality 90 reaches ~36 dB for about 20 KB a frame. 4:4:4 is therefore the
default, and the extra bytes are not a real cost at one frame per minute.

**The frame is never held whole.** The ESP32 cannot allocate one: measured on
the board, `malloc(115200)` fails while the largest contiguous block is 110,580
bytes. So the scene is re-rendered into 16-row strips and fed to the encoder as
it goes — 7,680 bytes instead of 115,200. `Canvas` keeps reporting the full
screen size while writing only the current strip, so no layout code knows or
cares, and the tests assert the banded output is byte-identical to whole-frame
output. Re-drawing ~20 primitives fifteen times costs far less than the DCT.

**Nothing is copied to upload it.** The multipart header and trailer bracket the
JPEG buffer as separate scatter-gather slices, so a 20 KB image costs 20 KB and
not 40 KB. The encoder likewise reads blocks straight out of the canvas and
never builds an intermediate image, so its working set is a few hundred bytes of
stack whatever the resolution.

**A frame that will not fit degrades instead of failing.**
`encodeRgb565WithBudget` steps quality down until the result fits the upload
buffer, and reports which quality it used. A slightly softer screen always beats
no screen.

**The multipart boundary is generated, not fixed.** JPEG entropy data is
effectively random bytes, so a constant boundary would eventually appear inside
an image, truncate it server-side, and be very hard to diagnose from the
symptom. The client re-rolls until the boundary provably does not occur in the
payload.

**Byte-identical to the probe.** `tools/probe_smalltv.py` and
`smalltv_request.cpp` produce the same multipart bytes and the same URL
escaping — including leaving `/` unescaped in `?img=`, which is what Python's
`quote()` does by default. The probe's answer about the upload field name is
only transferable if the request around it is the same, so that equality is
asserted in the tests rather than assumed.

**The upload field name is discovered, not guessed.** The firmware binary
confirms `/doUpload` exists but not which field it wants, so the client tries
the candidate list on first use and remembers what worked — and re-discovers if
a previously working name starts returning 4xx. This is why the device probe is
no longer on the critical path: running it is still worth doing, but the
firmware now works without its answer.

**Taking the screen is always reversible.** The device's current theme is
recorded once, on the transition into holding the screen, and never re-read
while armed — re-reading mid-alert would read back our own picture theme and
destroy the only record of what to restore. `releaseScreen()` additionally
refuses to restore the picture theme itself, so the SmallTV can never be left
displaying a stale commute frame.

**Uploads are suppressed when the pixels are identical.** An armed alert
re-renders every 60 s for 35 minutes; the countdown only actually changes once a
minute, and most refreshes change nothing at all. The gate hashes the canvas
rather than the encoded JPEG, so an unchanged frame skips the encode as well as
the flash write.

---

## Layout

```
commute-tv/
├── Makefile              host build, `make test`, `make preview`
├── src/core/             pure C++, no dependencies
│   ├── timeutil.{h,cpp}
│   ├── types.h
│   ├── urlencode.{h,cpp}
│   ├── textutil.{h,cpp}
│   ├── ratelimit.{h,cpp}
│   ├── places.{h,cpp}
│   ├── comfort.{h,cpp}
│   ├── trip_select.{h,cpp}
│   └── alert.{h,cpp}
├── src/json/
│   └── json_stream.{h,cpp}
├── src/net/
│   ├── http.{h,cpp}
│   └── json_sink.h
├── src/google/
│   ├── jwt.{h,cpp}
│   └── calendar.{h,cpp}
├── src/ors/
│   └── ors_client.{h,cpp}
├── src/weather/
│   └── meteo_client.{h,cpp}
├── src/hafas/
│   ├── hafas_request.{h,cpp}
│   ├── hafas_parse.{h,cpp}
│   └── hafas_client.{h,cpp}
├── src/render/
│   ├── font.h, font_data.h
│   ├── canvas.{h,cpp}
│   └── screen.{h,cpp}
├── src/oled/
│   ├── font_tiny.h
│   ├── mono_canvas.{h,cpp}
│   ├── ssd1306.{h,cpp}
│   └── health.{h,cpp}
├── src/jpeg/
│   └── jpeg_encoder.{h,cpp}
├── src/smalltv/
│   ├── smalltv_request.{h,cpp}
│   └── smalltv_client.{h,cpp}
├── firmware/             PlatformIO project for the ESP32
│   ├── platformio.ini
│   └── src/platform/     Wire, WiFi and clock adapters
├── preview/main.cpp      renders + encodes 12 scenarios
├── test/                 zero-dependency harness
└── tools/
    ├── probe_smalltv.py
    ├── resolve_places.py
    ├── record_fixtures.py
    ├── gen_font.py
    └── verify_jpeg.py
```
