#pragma once
// Self-locating finder for the VideoToolbox "forward MetalRegistryID to encoder" call site in the
// x86_64 dyld shared cache. Portable: used by the kext (reads through vnode I/O) and by the
// userspace tests (reads through pread). No allocation besides the caller-supplied allocator.
//
// What it looks for (structure, not exact bytes, so it survives OS updates):
//   1. VideoToolbox image in the cache (by install path).
//   2. C strings "MetalRegistryID" and "VTEncoderMetalRegistryID" in VideoToolbox __TEXT,__cstring.
//   3. CFString objects in VideoToolbox data segments whose string pointer targets them.
//   4. Pointer slots (GOT/const) in VideoToolbox data segments that point at those CFStrings.
//   5. In VideoToolbox __TEXT,__text, a site of the exact shape
//          test rX, rX ; je SKIP ; lea/mov rY, [rip -> MetalRegistryID slot/CFString] ;
//          ... (<= 16 bytes) ... ; call r ; SKIP:
//      i.e. "if (setProperty) setProperty(obj, kMetalRegistryID, value)" where je skips exactly
//      the call.
//   6. Encoder (not decoder) site: some direct call within 0x90 bytes before it goes to a function
//      whose first 0x60 bytes reference the "VTEncoderMetalRegistryID" key.
//   7. Exactly one such site, otherwise fail.
// Result: file index + file offset of the je, its kind, and an exact byte window around it read
// from the file (the kernel hook only patches a page whose bytes equal this window).
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace DscLocate {

struct Io {
    // Reads size bytes at offset of file `file` (0 = main cache, n = n-th subcache in header order).
    // Returns true only on a full read.
    bool (*read)(void *ctx, uint32_t file, uint64_t offset, void *buf, size_t size);
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *ptr, size_t size);
    void *ctx;
};

enum Status : int {
    Ok = 0, ErrHeader, ErrNoImage, ErrImageFile, ErrMachO, ErrSections, ErrStrings, ErrCFString,
    ErrSlots, ErrNoSite, ErrAmbiguous, ErrWindow, ErrAlloc, ErrSubcache
};

inline const char *statusName(int s) {
    static const char *n[] = {"ok", "header", "no-image", "image-file", "macho", "sections", "strings",
        "cfstring", "slots", "no-site", "ambiguous", "window", "alloc", "subcache"};
    return (s >= 0 && s <= ErrSubcache) ? n[s] : "?";
}

constexpr size_t MaxSubcaches = 16;
constexpr size_t WindowMax = 48;

struct Result {
    uint32_t file;            // 0 main, n = subcache n (suffix in subcacheSuffix)
    char subcacheSuffix[32];
    uint64_t patchFileOffset; // offset of the first byte to change
    uint8_t kind;             // 1 = je rel8 (74 -> eb), 2 = je rel32 (0f 84 -> 90 e9)
    uint64_t windowFileOffset;
    uint32_t windowSize;      // window lies entirely inside one 4 KiB page
    uint8_t window[WindowMax];
    uint64_t siteVA;
    uint32_t candidates;      // shape matches before the encoder/decoder discriminator
};

namespace detail {

inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
inline int32_t rds32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

struct Mapping { uint64_t addr, size, fileOff; uint32_t file; };

struct Ctx {
    Io io;
    Mapping maps[8 * (MaxSubcaches + 1)];
    uint32_t mapCount = 0;
    uint64_t base = 0, cacheEnd = 0;
    char suffix[MaxSubcaches + 1][32] {};
};

inline bool rd(Ctx &c, uint32_t f, uint64_t off, void *b, size_t s) { return c.io.read(c.io.ctx, f, off, b, s); }

inline bool va2file(Ctx &c, uint64_t va, uint32_t &file, uint64_t &off, uint64_t *avail = nullptr) {
    for (uint32_t i = 0; i < c.mapCount; i++) {
        auto &m = c.maps[i];
        if (va >= m.addr && va < m.addr + m.size) {
            file = m.file; off = m.fileOff + (va - m.addr);
            if (avail) *avail = m.addr + m.size - va;
            return true;
        }
    }
    return false;
}

inline bool readVA(Ctx &c, uint64_t va, void *buf, size_t size) {
    uint32_t f; uint64_t off, avail;
    if (!va2file(c, va, f, off, &avail) || avail < size) return false;
    return rd(c, f, off, buf, size);
}

// Shared cache stores rebase-able pointers as (high flag bits | offset from cache base).
inline uint64_t ptrTarget(const Ctx &c, uint64_t raw) {
    if (raw >= c.base && raw < c.cacheEnd) return raw;           // already a plain VA
    uint64_t off = raw & 0xFFFFFFFFFULL;                           // low 36 bits = offset
    return off ? c.base + off : 0;
}

inline bool loadMappings(Ctx &c, uint32_t file) {
    uint8_t h[0x200];
    if (!rd(c, file, 0, h, sizeof(h)) || memcmp(h, "dyld_v1", 7) != 0) return false;
    uint32_t mo = rd32(h + 0x10), mc = rd32(h + 0x14);
    if (mc == 0 || mc > 8 || mo < 0x20 || mo > 0x1000) return false;
    uint8_t mm[8 * 32];
    if (!rd(c, file, mo, mm, mc * 32)) return false;
    for (uint32_t i = 0; i < mc; i++) {
        if (c.mapCount >= sizeof(c.maps) / sizeof(c.maps[0])) return false;
        c.maps[c.mapCount++] = {rd64(mm + i * 32), rd64(mm + i * 32 + 8), rd64(mm + i * 32 + 16), file};
    }
    return true;
}

struct Range { uint64_t va = 0, size = 0; };

inline bool findCString(const uint8_t *buf, size_t size, uint64_t va, const char *s, uint64_t &out) {
    size_t n = strlen(s);
    for (size_t i = 0; i + n < size; i++) {
        if (buf[i] == (uint8_t)s[0] && (i == 0 || buf[i - 1] == 0) && memcmp(buf + i, s, n + 1) == 0) {
            out = va + i; return true;
        }
    }
    return false;
}

// rip-relative lea/mov r64 (REX.W 8d/8b modrm with mod=00 rm=101) at p; returns target or 0.
inline uint64_t ripRef(const uint8_t *text, size_t size, size_t p, uint64_t textVA) {
    if (p + 7 > size) return 0;
    uint8_t rex = text[p], op = text[p + 1], modrm = text[p + 2];
    if ((rex != 0x48 && rex != 0x4c) || (op != 0x8d && op != 0x8b) || (modrm & 0xC7) != 0x05) return 0;
    return textVA + p + 7 + (int64_t)rds32(text + p + 3);
}

} // namespace detail

// Finds the site. `result` is filled only when Ok is returned.
inline int locate(const Io &io, Result &result) {
    using namespace detail;
    static const char VTPath[] = "/System/Library/Frameworks/VideoToolbox.framework/Versions/A/VideoToolbox";
    Ctx *cp = static_cast<Ctx *>(io.alloc(io.ctx, sizeof(Ctx)));
    if (!cp) return ErrAlloc;
    memset(cp, 0, sizeof(Ctx));
    Ctx &c = *cp;
    c.io = io;
    int status = Ok;
    uint8_t *lc = nullptr, *cstr = nullptr, *data = nullptr, *text = nullptr;
    size_t lcSize = 0, cstrSize = 0, dataSize = 0, textSize = 0;
    uint64_t imgVA = 0;

    do {
        uint8_t h[0x200];
        if (!rd(c, 0, 0, h, sizeof(h)) || memcmp(h, "dyld_v1", 7) != 0) { status = ErrHeader; break; }
        if (!loadMappings(c, 0)) { status = ErrHeader; break; }
        c.base = c.maps[0].addr;
        // Subcaches (header 0x188 offset / 0x18c count, entries of 56 bytes: uuid, vmOffset, suffix[32]).
        uint32_t so = rd32(h + 0x188), sc = rd32(h + 0x18c);
        if (sc > MaxSubcaches) { status = ErrSubcache; break; }
        for (uint32_t i = 0; i < sc; i++) {
            uint8_t e[56];
            if (!rd(c, 0, so + i * 56, e, sizeof(e))) { status = ErrSubcache; break; }
            memcpy(c.suffix[i + 1], e + 24, 31); c.suffix[i + 1][31] = 0;
            if (!loadMappings(c, i + 1)) { status = ErrSubcache; break; }
        }
        if (status != Ok) break;
        for (uint32_t i = 0; i < c.mapCount; i++)
            if (c.maps[i].addr + c.maps[i].size > c.cacheEnd) c.cacheEnd = c.maps[i].addr + c.maps[i].size;

        // Image list (new fields at 0x1c0/0x1c4, old at 0x18/0x1c).
        uint32_t io2 = rd32(h + 0x1c0), ic = rd32(h + 0x1c4);
        if (io2 == 0 || ic == 0) { io2 = rd32(h + 0x18); ic = rd32(h + 0x1c); }
        if (io2 == 0 || ic == 0 || ic > 20000) { status = ErrHeader; break; }
        for (uint32_t i = 0; i < ic && !imgVA; i++) {
            uint8_t e[32]; char path[sizeof(VTPath) + 1];
            if (!rd(c, 0, io2 + (uint64_t)i * 32, e, sizeof(e))) { status = ErrHeader; break; }
            if (!rd(c, 0, rd32(e + 24), path, sizeof(path))) continue;
            if (memcmp(path, VTPath, sizeof(VTPath)) == 0) imgVA = rd64(e);
        }
        if (status != Ok) break;
        if (!imgVA) { status = ErrNoImage; break; }

        // Mach-O header + load commands.
        uint8_t mh[32];
        if (!readVA(c, imgVA, mh, sizeof(mh)) || rd32(mh) != 0xfeedfacf) { status = ErrMachO; break; }
        uint32_t ncmds = rd32(mh + 16), sizeofcmds = rd32(mh + 20);
        if (sizeofcmds == 0 || sizeofcmds > 0x20000 || ncmds > 512) { status = ErrMachO; break; }
        lcSize = sizeofcmds;
        lc = static_cast<uint8_t *>(io.alloc(io.ctx, lcSize));
        if (!lc) { status = ErrAlloc; break; }
        if (!readVA(c, imgVA + 32, lc, lcSize)) { status = ErrMachO; break; }

        Range textSect, cstrSect, dataSegs[6]; uint32_t dataSegCount = 0;
        size_t p = 0;
        for (uint32_t i = 0; i < ncmds && p + 8 <= lcSize; i++) {
            uint32_t cmd = rd32(lc + p), csz = rd32(lc + p + 4);
            if (csz < 8 || p + csz > lcSize) { status = ErrMachO; break; }
            if (cmd == 0x19 && csz >= 72) {  // LC_SEGMENT_64
                const char *seg = reinterpret_cast<const char *>(lc + p + 8);
                uint64_t vmaddr = rd64(lc + p + 24), vmsize = rd64(lc + p + 32);
                uint32_t nsects = rd32(lc + p + 64);
                if (!strncmp(seg, "__DATA", 6) || !strncmp(seg, "__AUTH", 6)) {
                    if (dataSegCount < 6) dataSegs[dataSegCount++] = {vmaddr, vmsize};
                }
                for (uint32_t s = 0; s < nsects && 72 + (s + 1) * 80 <= csz; s++) {
                    const uint8_t *sp = lc + p + 72 + s * 80;
                    const char *sect = reinterpret_cast<const char *>(sp);
                    if (!strncmp(seg, "__TEXT", 16) && !strncmp(sect, "__text", 16)) textSect = {rd64(sp + 32), rd64(sp + 40)};
                    if (!strncmp(seg, "__TEXT", 16) && !strncmp(sect, "__cstring", 16)) cstrSect = {rd64(sp + 32), rd64(sp + 40)};
                }
            }
            p += csz;
        }
        if (status != Ok) break;
        if (!textSect.size || !cstrSect.size || !dataSegCount ||
            textSect.size > 0x4000000 || cstrSect.size > 0x1000000) { status = ErrSections; break; }

        // 2. strings
        cstrSize = cstrSect.size;
        cstr = static_cast<uint8_t *>(io.alloc(io.ctx, cstrSize));
        if (!cstr) { status = ErrAlloc; break; }
        if (!readVA(c, cstrSect.va, cstr, cstrSize)) { status = ErrSections; break; }
        uint64_t sKey = 0, sEnc = 0;
        if (!findCString(cstr, cstrSize, cstrSect.va, "MetalRegistryID", sKey) ||
            !findCString(cstr, cstrSize, cstrSect.va, "VTEncoderMetalRegistryID", sEnc)) { status = ErrStrings; break; }

        // 3./4. CFStrings and slots, over all data segments.
        uint64_t cfKey[4] {}, cfEnc[4] {}; uint32_t nKey = 0, nEnc = 0;
        uint64_t slotKey[8] {}, slotEnc[8] {}; uint32_t nsKey = 0, nsEnc = 0;
        for (int pass = 0; pass < 2 && status == Ok; pass++) {
            for (uint32_t d = 0; d < dataSegCount; d++) {
                if (dataSegs[d].size > 0x2000000) { status = ErrSections; break; }
                dataSize = dataSegs[d].size;
                data = static_cast<uint8_t *>(io.alloc(io.ctx, dataSize));
                if (!data) { status = ErrAlloc; break; }
                if (!readVA(c, dataSegs[d].va, data, dataSize)) { io.free(io.ctx, data, dataSize); data = nullptr; continue; }
                for (size_t q = 0; q + 8 <= dataSize; q += 8) {
                    uint64_t t = ptrTarget(c, rd64(data + q));
                    uint64_t at = dataSegs[d].va + q;
                    if (pass == 0) {
                        // CFString: {isa, flags, ptr, length}; ptr at +16
                        if (q >= 16 && q + 16 <= dataSize) {
                            uint64_t len = rd64(data + q + 8);
                            if (t == sKey && len == 15 && nKey < 4) cfKey[nKey++] = at - 16;
                            if (t == sEnc && len == 24 && nEnc < 4) cfEnc[nEnc++] = at - 16;
                        }
                    } else {
                        for (uint32_t k = 0; k < nKey; k++) if (t == cfKey[k] && nsKey < 8) slotKey[nsKey++] = at;
                        for (uint32_t k = 0; k < nEnc; k++) if (t == cfEnc[k] && nsEnc < 8) slotEnc[nsEnc++] = at;
                    }
                }
                io.free(io.ctx, data, dataSize); data = nullptr;
            }
            if (pass == 0 && status == Ok && (!nKey || !nEnc)) status = ErrCFString;
        }
        if (status != Ok) break;
        if (!nsKey || !nsEnc) { status = ErrSlots; break; }

        auto isKeyRef = [&](uint64_t t) {
            for (uint32_t k = 0; k < nKey; k++) if (t == cfKey[k]) return true;
            for (uint32_t k = 0; k < nsKey; k++) if (t == slotKey[k]) return true;
            return false;
        };
        auto isEncRef = [&](uint64_t t) {
            for (uint32_t k = 0; k < nEnc; k++) if (t == cfEnc[k]) return true;
            for (uint32_t k = 0; k < nsEnc; k++) if (t == slotEnc[k]) return true;
            return false;
        };

        // 5. scan text
        textSize = textSect.size;
        text = static_cast<uint8_t *>(io.alloc(io.ctx, textSize));
        if (!text) { status = ErrAlloc; break; }
        if (!readVA(c, textSect.va, text, textSize)) { status = ErrSections; break; }
        uint32_t found = 0, shape = 0;
        uint64_t siteJe = 0; uint8_t kind = 0;
        for (size_t q = 0x100; q + 32 < textSize; q++) {
            uint64_t t = ripRef(text, textSize, q, textSect.va);
            if (!t || !isKeyRef(t)) continue;
            // je immediately before, test reg,reg immediately before that
            size_t je = 0; uint8_t k = 0; uint64_t jeTarget = 0;
            if (text[q - 2] == 0x74) { je = q - 2; k = 1; jeTarget = textSect.va + q + (int8_t)text[q - 1]; }
            else if (text[q - 6] == 0x0f && text[q - 5] == 0x84) { je = q - 6; k = 2; jeTarget = textSect.va + q + rds32(text + q - 4); }
            if (!k) continue;
            const uint8_t *tst = text + je - 3;
            if ((tst[0] != 0x48 && tst[0] != 0x4d) || tst[1] != 0x85 || (tst[2] & 0xC0) != 0xC0 ||
                ((tst[2] >> 3) & 7) != (tst[2] & 7)) continue;
            // indirect call ending exactly at the je target, within 16 bytes after the lea
            bool callOk = false;
            for (size_t r = q + 7; r < q + 7 + 16 && r + 2 <= textSize; r++) {
                bool c1 = text[r] == 0xff && (text[r + 1] & 0xF8) == 0xD0 && textSect.va + r + 2 == jeTarget;
                bool c2 = r + 3 <= textSize && text[r] == 0x41 && text[r + 1] == 0xff && (text[r + 2] & 0xF8) == 0xD0 &&
                          textSect.va + r + 3 == jeTarget;
                if (c1 || c2) { callOk = true; break; }
            }
            if (!callOk) continue;
            shape++;
            // 6. encoder discriminator
            bool enc = false;
            size_t from = je > 3 + 0x90 ? je - 3 - 0x90 : 0;
            for (size_t r = from; r + 5 <= je - 3 && !enc; r++) {
                if (text[r] != 0xe8) continue;
                uint64_t ct = textSect.va + r + 5 + (int64_t)rds32(text + r + 1);
                if (ct < textSect.va || ct + 0x60 > textSect.va + textSize) continue;
                size_t co = ct - textSect.va;
                for (size_t u = co; u < co + 0x60 - 7; u++) {
                    uint64_t tt = ripRef(text, textSize, u, textSect.va);
                    if (tt && isEncRef(tt)) { enc = true; break; }
                }
            }
            if (!enc) continue;
            found++;
            siteJe = textSect.va + je; kind = k;
        }
        result.candidates = shape;
        if (found == 0) { status = ErrNoSite; break; }
        if (found > 1) { status = ErrAmbiguous; break; }

        // 7. window from the file, inside one page
        uint32_t f; uint64_t off;
        if (!va2file(c, siteJe, f, off)) { status = ErrWindow; break; }
        uint64_t page = off & ~0xFFFULL;
        uint64_t ws = off >= 24 ? off - 24 : 0, we = off + 24;
        if (ws < page) ws = page;
        if (we > page + 0x1000) we = page + 0x1000;
        if (we - ws < 16 || we - ws > WindowMax || off + 2 > we) { status = ErrWindow; break; }
        result.file = f;
        memcpy(result.subcacheSuffix, c.suffix[f], sizeof(result.subcacheSuffix));
        result.patchFileOffset = off;
        result.kind = kind;
        result.windowFileOffset = ws;
        result.windowSize = (uint32_t)(we - ws);
        result.siteVA = siteJe;
        if (!rd(c, f, ws, result.window, result.windowSize)) { status = ErrWindow; break; }
        size_t rel = off - ws;
        if ((kind == 1 && result.window[rel] != 0x74) ||
            (kind == 2 && (result.window[rel] != 0x0f || result.window[rel + 1] != 0x84))) { status = ErrWindow; break; }
    } while (false);

    if (text) io.free(io.ctx, text, textSize);
    if (data) io.free(io.ctx, data, dataSize);
    if (cstr) io.free(io.ctx, cstr, cstrSize);
    if (lc) io.free(io.ctx, lc, lcSize);
    io.free(io.ctx, cp, sizeof(Ctx));
    return status;
}

// Applies the located patch to a 4 KiB page whose file offset is pageOffset, only if the page holds
// exactly the expected window bytes. Returns true if patched.
inline bool applyToPage(const Result &r, uint64_t pageOffset, uint8_t *page, size_t pageSize) {
    if (r.windowFileOffset < pageOffset || r.windowFileOffset + r.windowSize > pageOffset + pageSize) return false;
    size_t w = r.windowFileOffset - pageOffset;
    if (memcmp(page + w, r.window, r.windowSize) != 0) return false;
    size_t p = r.patchFileOffset - pageOffset;
    if (r.kind == 1) { page[p] = 0xeb; return true; }
    if (r.kind == 2) { page[p] = 0x90; page[p + 1] = 0xe9; return true; }
    return false;
}

// Same, without knowing the page's file offset: the window must match at its in-page offset.
inline bool applyToPageByContent(const Result &r, uint8_t *page, size_t pageSize) {
    uint64_t pageOffset = r.windowFileOffset & ~0xFFFULL;
    return applyToPage(r, pageOffset, page, pageSize);
}

} // namespace DscLocate
