// Stub. See ../README.md -- syntax only. IP_NAPT is deliberately left UNDEFINED
// so the gate compiles the "this core cannot do it" branch: that is the branch
// a laptop can be sure about. Whether the real core defines it is a question
// only the Xtensa toolchain can answer, which is why the firmware asks at
// compile time and says so at run time either way.
#pragma once
