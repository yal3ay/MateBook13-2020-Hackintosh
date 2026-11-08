#include "dsc_locate.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>
struct F { int fd[DscLocate::MaxSubcaches + 1]; };
static bool rdf(void *c, uint32_t f, uint64_t o, void *b, size_t s) {
    auto *x = (F *)c; if (f > DscLocate::MaxSubcaches || x->fd[f] < 0) return false;
    return pread(x->fd[f], b, s, o) == (ssize_t)s;
}
static void *al(void *, size_t s) { return malloc(s); }
static void fr(void *, void *p, size_t) { free(p); }
int main(int argc, char **argv) {
    std::string base = argv[1]; F x; for (auto &d : x.fd) d = -1;
    x.fd[0] = open(base.c_str(), O_RDONLY);
    const char *sfx[] = {"", ".01", ".02", ".03", ".04", ".05", ".06"};
    for (int i = 1; i <= 6; i++) x.fd[i] = open((base + sfx[i]).c_str(), O_RDONLY);
    DscLocate::Io io {rdf, al, fr, &x}; DscLocate::Result r {};
    int st = DscLocate::locate(io, r);
    printf("status=%s candidates=%u\n", DscLocate::statusName(st), r.candidates);
    if (st) return 1;
    printf("file=%u suffix='%s' patch=0x%llx kind=%u siteVA=0x%llx window=0x%llx+%u\n", r.file, r.subcacheSuffix,
        (unsigned long long)r.patchFileOffset, r.kind, (unsigned long long)r.siteVA,
        (unsigned long long)r.windowFileOffset, r.windowSize);
    uint8_t page[4096]; uint64_t po = r.windowFileOffset & ~0xFFFULL;
    rdf(&x, r.file, po, page, 4096);
    bool ok = DscLocate::applyToPageByContent(r, page, 4096) && page[r.patchFileOffset - po] == 0xeb;
    bool again = DscLocate::applyToPageByContent(r, page, 4096);
    printf("apply=%d reapply=%d\n", ok, again);
    return (r.patchFileOffset == 0x12cec9f7 && ok && !again) ? 0 : 2;
}
