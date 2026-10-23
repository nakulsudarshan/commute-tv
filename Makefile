# Host build. No Arduino, no libraries, no network, no hardware.
#
#   make test      run the unit tests
#   make preview   render the 240x240 screen layouts to preview/out/
#   make oled      render the 128x64 health panels to preview/out/
#   make firmware  parse firmware/src/ against stub Arduino headers
#   make font      check src/oled/font_pixel.h still matches its art
#   make ca        check firmware/include/root_ca.h exists
#   make secrets   check firmware/include/secrets_generated.h exists
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion

CORE    := $(wildcard src/core/*.cpp) $(wildcard src/hafas/*.cpp) $(wildcard src/json/*.cpp) $(wildcard src/net/*.cpp) $(wildcard src/google/*.cpp) $(wildcard src/ors/*.cpp) $(wildcard src/weather/*.cpp) $(wildcard src/app/*.cpp)
RENDER  := $(wildcard src/render/*.cpp) $(wildcard src/oled/*.cpp)
JPEG    := $(wildcard src/jpeg/*.cpp)
DEVICE  := $(wildcard src/smalltv/*.cpp)
TEST    := $(wildcard test/*.cpp)

.PHONY: test preview oled firmware-includes firmware font ca secrets all clean
all: font test preview oled firmware-includes firmware

test: build/run_tests
	@./build/run_tests

build/run_tests: $(CORE) $(RENDER) $(JPEG) $(DEVICE) $(TEST)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(CORE) $(RENDER) $(JPEG) $(DEVICE) $(TEST) -o $@

preview: build/preview
	@mkdir -p preview/out
	@./build/preview

build/preview: $(RENDER) $(JPEG) src/core/textutil.cpp preview/main.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(RENDER) $(JPEG) src/core/textutil.cpp preview/main.cpp -o $@

oled: build/oled_preview
	@mkdir -p preview/out
	@./build/oled_preview

build/oled_preview: $(wildcard src/oled/*.cpp) src/core/textutil.cpp preview/oled_main.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(wildcard src/oled/*.cpp) src/core/textutil.cpp preview/oled_main.cpp -o $@

# The firmware includes the shared headers by module path (-I src), not by the
# relative paths the tests use. This catches a header that resolves from test/
# but not from firmware/src/ -- on the laptop, rather than on the first flash.
firmware-includes:
	@./tools/check_firmware_includes.sh

# And this one parses the firmware's OWN sources against stub Arduino headers,
# so a typo or a printf format mismatch costs a second here instead of a build
# and a reflash. It says nothing about how the ESP32 behaves -- `pio run` is
# still the build that decides.
firmware:
	@./tools/check_firmware_syntax.sh

# src/oled/font_pixel.h is generated from ASCII art in tools/gen_pixelfont.py.
# The header is committed so the build needs no Python -- which means the two
# can drift. This says so out loud rather than letting the art become fiction.
font:
	@python3 tools/gen_pixelfont.py --check

# The firmware REFUSES unverified TLS, so a missing root_ca.h is a build that
# compiles and then declines every request. Say so here instead.
ca:
	@python3 tools/fetch_root_ca.py --check

# .env and the service-account JSON are the source of truth; this header is
# derived from them. Nothing here is ever retyped by hand -- see the tool.
secrets:
	@python3 tools/gen_secrets.py --check

clean:
	@rm -rf build preview/out
