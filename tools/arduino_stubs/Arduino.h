// Stub. See README.md -- syntax only, no behaviour.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

class String {
public:
    String() {}
    String(const char* s) : s_(s ? s : "") {}
    const char* c_str() const { return s_.c_str(); }
    bool operator==(const char* o) const { return o && s_ == o; }
    bool operator==(const String& o) const { return s_ == o.s_; }
private:
    std::string s_;
};

inline void delay(uint32_t) {}
inline uint32_t millis() { return 0; }

class SerialStub {
public:
    void begin(unsigned long) {}
    size_t print(const char*) { return 0; }
    size_t println() { return 0; }
    size_t println(const char*) { return 0; }
    // The format attribute is the point: it turns a %d/%s mismatch into a
    // compile error here rather than a reboot loop on the desk.
    __attribute__((format(printf, 2, 3)))
    size_t printf(const char*, ...) { return 0; }
    void flush() {}
};
extern SerialStub Serial;

class EspStub {
public:
    uint32_t getFreeHeap() const { return 0; }
    uint32_t getMaxAllocHeap() const { return 0; }
    void restart() {}
};
extern EspStub ESP;

#define SET_LOOP_TASK_STACK_SIZE(sz) extern int loop_task_stack_size_stub

inline void configTzTime(const char*, const char*, const char* = nullptr,
                         const char* = nullptr) {}
