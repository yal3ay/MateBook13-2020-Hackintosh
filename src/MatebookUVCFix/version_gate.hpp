#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// 0.3.0: version-independent gating.
// The mangled symbol encodes the full parameter list, so a signature change makes it
// unresolvable. The function prologue below was verified identical on Darwin 22.1.0
// (22A380) and Darwin 24.6.0 (24H232). Any other prologue -> fail closed.
namespace MatebookGate {
constexpr int MinDarwin = 22;   // macOS 13 Ventura
constexpr int MaxDarwin = 25;   // macOS 26 Tahoe (still fails closed if the function differs)

inline bool supportedDarwin(int major) {
    return major >= MinDarwin && major <= MaxDarwin;
}

// push rbp; mov rbp,rsp; push rbx; sub rsp,0x38; mov r10d,[rbp+0x18]; mov r11,[rbp+0x10]
constexpr uint8_t ExpectedPrologue[] = {
    0x55,0x48,0x89,0xe5,0x53,0x48,0x83,0xec,0x38,0x44,0x8b,0x55,0x18,0x4c,0x8b,0x5d,0x10
};

inline bool prologueMatches(const uint8_t *code, size_t available) {
    return code && available >= sizeof(ExpectedPrologue) &&
        memcmp(code, ExpectedPrologue, sizeof(ExpectedPrologue)) == 0;
}
}
