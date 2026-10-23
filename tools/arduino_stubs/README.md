# Arduino stubs -- a syntax gate, not an emulator

These headers exist for one job: let `g++ -fsyntax-only` parse
`firmware/src/` on the laptop, so that a typo, a wrong member name, a
const-correctness slip or a printf format mismatch is caught in a second
instead of after a two-minute PlatformIO build and a reflash.

**They prove syntax and nothing else.** They do not emulate the ESP32, they
do not implement any behaviour, and a program that passes here can still be
wrong on the hardware -- the `tm_gmtoff` mistake that cost a build cycle was
exactly the kind of thing these catch, and the WPA2-enterprise init ordering
is exactly the kind they cannot. `pio run` is still the build that counts.

Signatures are copied from Arduino-ESP32 2.0.17. If a stub disagrees with the
real core, the stub is the one that is wrong -- fix it here rather than
working around it in the firmware.
