#!/bin/sh
# Compile the shared headers the way the FIRMWARE includes them -- by module
# path against -I src -- rather than the way the tests do, with relative paths.
#
# This catches the class of mistake that only shows up on the first flash: a
# header that resolves fine from test/ but not from firmware/src/. It cannot
# check the Arduino-facing code (this machine has no ESP32 toolchain), but it
# does check everything that is ours.
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp /tmp/commute-tv-incl-XXXXXX.cpp)
trap 'rm -f "$tmp"' EXIT
{
  for h in core/textutil.h core/timeutil.h core/types.h core/places.h \
           core/ratelimit.h core/alert.h core/comfort.h core/trip_select.h \
           json/json_stream.h net/http.h net/json_sink.h \
           hafas/hafas_client.h hafas/hafas_parse.h hafas/hafas_request.h \
           google/calendar.h google/jwt.h ors/ors_client.h \
           weather/meteo_client.h smalltv/smalltv_client.h \
           oled/health.h oled/mono_canvas.h oled/ssd1306.h \
           app/orchestrator.h app/live_sources.h; do
    echo "#include \"$h\""
  done
  echo "int main() { return 0; }"
} > "$tmp"
${CXX:-g++} -std=gnu++17 -Wall -Wextra -Isrc -fsyntax-only "$tmp"
echo "ok: every shared header resolves by module path"
