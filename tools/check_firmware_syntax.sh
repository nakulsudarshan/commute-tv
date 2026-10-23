#!/bin/sh
# Parse firmware/src/ on the laptop, against stub Arduino headers.
#
# This is a SYNTAX GATE, not a build. It catches the mistakes that otherwise
# cost a full PlatformIO cycle and a reflash to discover: a misspelled member,
# a const method calling a non-const one, a printf format that does not match
# its arguments. It cannot catch anything about how the ESP32 behaves --
# see tools/arduino_stubs/README.md.
#
# `pio run` is still the build that decides.
set -e
cd "$(dirname "$0")/.."
${CXX:-g++} -std=gnu++17 -Wall -Wextra -Wshadow -Werror -fsyntax-only \
    -I tools/arduino_stubs \
    -I firmware/include \
    -I firmware/src \
    -I src \
    firmware/src/main.cpp firmware/src/net_client_probe.cpp
echo "ok: firmware/src parses (syntax only -- pio run is the real build)"
