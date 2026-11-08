#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// MatebookGVAFix 0.3.0 - HEVC hardware encode on the UHD 620 (Intel IGPU only) hackintosh.
//
// Root cause (AppleGVAHEVCEncoder, macOS 15.8.2 24H232):
//   VideoToolbox creates the encoder, then calls its SetProperty("MetalRegistryID", id).
//   The encoder stores the id and sets an "explicit GPU" flag (encoder+0x4cc8). AVDDriver
//   then takes the explicit-GPU path, which hits assert(0) "[AppleGVAHEVC] Error: metalId
//   for IG" for any Intel IGPU. (Real Intel-only Macs have T2 and never get here.)
//   Without the property the encoder uses its default GPU selection, which supports the IGPU.
//
// 0.1.x/0.2.0 patched the encoder binary itself. That page lives in the file cache, so later
// reads of the file returned patched bytes, its code signature failed, and VideoToolbox
// dropped "HEVC (HW)" from the encoder list after the first use (-12908 until reboot).
//
// 0.3.0 patches VideoToolbox inside the dyld shared cache instead (same technique as
// RestrictEvents / FeatureUnlock; the shared cache is not signature-checked at run time):
// in the encoder-creation function, the "if (class->setProperty) setProperty(MetalRegistryID)"
// branch becomes unconditional, so the property is never forwarded to any encoder.
// The decoder path (VTDecoderMetalRegistryID) is a different function and is untouched.
// The id is still returned to VideoToolbox's caller as before.
//
// macOS 15.8.2 24H232 dyld_shared_cache_x86_64h (sha256 0c509cf90c6d633c...),
// VideoToolbox VA 0x7ff812cec9d1, file offset 0x12cec9d1 (in-page 0x9d1):
//   84 db                test bl, bl
//   75 36                jne  skip
//   48 8b 45 98          mov  rax, [rbp-0x68]
//   48 8b 38             mov  rdi, [rax]
//   e8 9e 71 ae ff       call <encoder object>
//   48 89 c3             mov  rbx, rax
//   48 89 c7             mov  rdi, rax
//   e8 6e 5c 0b 00       call CMBaseObjectGetVTable
//   48 8b 40 08          mov  rax, [rax+8]
//   48 8b 40 34          mov  rax, [rax+0x34]        ; class->setProperty
//   48 85 c0             test rax, rax
//   74 12                je   skip                   <- patched to  eb 12  (jmp skip)
//   48 8d 0d 00 b2 92 2e lea  rcx, [kMetalRegistryID]
//   48 8b 31             mov  rsi, [rcx]
//   48 89 df             mov  rdi, rbx
//   4c 89 fa             mov  rdx, r15
//   ff d0                call rax                    ; setProperty(enc, MetalRegistryID, id)
// The pattern contains rel32 displacements, so it only matches this exact cache build
// (fails closed on any other build, e.g. macOS 26 needs its own audited pattern).
namespace MatebookGVA {
constexpr char SharedCachePath[] =
    "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h";

constexpr size_t MaxPattern = 64;
constexpr size_t PatchSize = 1;

struct Pattern {
    const char *name;
    size_t size;
    uint8_t find[MaxPattern];
    size_t patchOffset;
    uint8_t original[PatchSize];
    uint8_t replace[PatchSize];
};

constexpr Pattern Patterns[] = {
    {"24H232-VideoToolbox", 58, {
        0x84,0xdb, 0x75,0x36, 0x48,0x8b,0x45,0x98, 0x48,0x8b,0x38,
        0xe8,0x9e,0x71,0xae,0xff, 0x48,0x89,0xc3, 0x48,0x89,0xc7,
        0xe8,0x6e,0x5c,0x0b,0x00, 0x48,0x8b,0x40,0x08, 0x48,0x8b,0x40,0x34,
        0x48,0x85,0xc0, 0x74,0x12, 0x48,0x8d,0x0d,0x00,0xb2,0x92,0x2e,
        0x48,0x8b,0x31, 0x48,0x89,0xdf, 0x4c,0x89,0xfa, 0xff,0xd0},
     38, {0x74}, {0xeb}},
};
constexpr size_t PatternCount = sizeof(Patterns) / sizeof(Patterns[0]);

struct Hit { size_t offset; size_t pattern; };
constexpr Hit NoHit {SIZE_MAX, SIZE_MAX};

// Finds the single occurrence of any known pattern in buf. Returns NoHit if none, or if
// there is more than one occurrence in total (ambiguous: refuse to patch).
inline Hit locate(const uint8_t *buf, size_t size) {
    if (!buf) return NoHit;
    Hit found = NoHit;
    for (size_t p = 0; p < PatternCount; p++) {
        const Pattern &pt = Patterns[p];
        if (size < pt.size) continue;
        for (size_t i = 0; i + pt.size <= size; i++) {
            if (buf[i] != pt.find[0] || memcmp(buf + i, pt.find, pt.size) != 0) continue;
            if (found.offset != SIZE_MAX) return NoHit;
            found = {i, p};
        }
    }
    return found;
}

// Patch in place. Returns the pattern index patched, or SIZE_MAX if nothing was changed.
inline size_t apply(uint8_t *buf, size_t size) {
    Hit hit = locate(buf, size);
    if (hit.offset == SIZE_MAX) return SIZE_MAX;
    const Pattern &pt = Patterns[hit.pattern];
    memcpy(buf + hit.offset + pt.patchOffset, pt.replace, PatchSize);
    return hit.pattern;
}
}
