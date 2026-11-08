#include "gva_patch.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
using namespace MatebookGVA;

// Scan a file page by page, exactly like the kernel hook sees it. Returns number of hits.
static int scan(const char *path, size_t *at) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> page(4096);
    int hits = 0; size_t off = 0;
    while (f) {
        f.read(reinterpret_cast<char *>(page.data()), 4096);
        size_t n = f.gcount(); if (!n) break;
        Hit h = locate(page.data(), n);
        if (h.offset != SIZE_MAX) {
            hits++; *at = off + h.offset;
            std::vector<uint8_t> copy(page.begin(), page.begin() + n);
            assert(apply(copy.data(), n) == h.pattern);
            const Pattern &p = Patterns[h.pattern];
            for (size_t i = 0; i < n; i++) {
                if (i == h.offset + p.patchOffset) assert(copy[i] == p.replace[0] && page[i] == p.original[0]);
                else assert(copy[i] == page[i]);
            }
        }
        off += n;
    }
    return hits;
}

int main(int argc, char **argv) {
    const Pattern &p = Patterns[0];
    assert(p.find[p.patchOffset] == p.original[0]);
    assert(p.patchOffset < p.size);
    uint8_t page[4096]; memset(page, 0x90, sizeof(page));
    memcpy(page + 0x9d1, p.find, p.size);
    Hit h = locate(page, sizeof(page)); assert(h.offset == 0x9d1 && h.pattern == 0);
    assert(apply(page, sizeof(page)) == 0);
    assert(page[0x9d1 + p.patchOffset] == 0xeb);
    assert(locate(page, sizeof(page)).offset == SIZE_MAX);          // idempotent
    uint8_t two[4096]; memset(two, 0, sizeof(two));
    memcpy(two + 10, p.find, p.size); memcpy(two + 2000, p.find, p.size);
    assert(apply(two, sizeof(two)) == SIZE_MAX);                     // ambiguous refused
    uint8_t edge[4096]; memset(edge, 0, sizeof(edge));
    memcpy(edge + sizeof(edge) - p.size, p.find, p.size);
    assert(locate(edge, sizeof(edge)).offset == sizeof(edge) - p.size);
    assert(locate(edge, sizeof(edge) - 1).offset == SIZE_MAX);
    assert(locate(nullptr, 4096).offset == SIZE_MAX);
    for (int i = 1; i < argc; i++) {
        size_t at = 0; int hits = scan(argv[i], &at);
        printf("%s: hits=%d", argv[i], hits);
        if (hits) printf(" at file offset 0x%zx (patch byte 0x%zx)", at, at + p.patchOffset);
        printf("\n");
    }
    puts("PASS");
}
