// MatebookGVAFix 0.4.1 (2026-11-08)
// HEVC hardware encode on this MateBook UHD 620 hackintosh. See gva_patch.hpp for the root cause.
// 0.4.1: the locator thread touches no VFS until IOResources has IOConsoleUsers (login window up,
//        all volumes mounted). 0.4.0 called vnode_lookup in a loop from early boot (before the
//        root volume was mounted) and macOS 15 hung at "console".
// 0.4.0: self-locating. A kernel thread waits until the dyld shared cache is readable, then finds
//        the VideoToolbox "setProperty(encoder, MetalRegistryID)" site by structure (dsc_locate.hpp),
//        so OS updates that move the code keep working. The static 24H232 pattern stays as an
//        immediate fallback for pages validated before the thread finishes. Exactly one site or
//        nothing is patched. Status: IOResources property "MatebookGVAFix".
// 0.3.0: patch VideoToolbox in the dyld shared cache (do not forward MetalRegistryID to encoders)
//        instead of patching AppleGVAHEVCEncoder; patching the encoder's file-cache page broke
//        its code signature for later readers and VideoToolbox then hid the HW encoder.
// 0.2.0: route with routeMultipleLong. 0.1.0 used routeMultiple (5-byte jump); WhateverGreen and
//        FeatureUnlock already long-route cs_validate_page, so the short wrapper copied half of
//        their 14-byte jump and macOS 15 hung at boot.
// Boot arg -mbgvaoff disables it.
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_file.hpp>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <sys/vnode.h>
#include <sys/syslimits.h>
#include "gva_patch.hpp"
#include "dsc_locate.hpp"

namespace {
using CsValidatePage = void (*)(vnode *, memory_object_t, memory_object_offset_t,
    const void *, int *, int *, int *);
mach_vm_address_t orgCsValidatePage = 0;
volatile uint32_t patchedPages = 0;

DscLocate::Result dyn {};
char dynPath[PATH_MAX] {};
volatile uint32_t dynReady = 0;

void publish(const char *msg) {
    SYSLOG("mbgva", "%s", msg);
    if (auto res = IOService::getResourceService()) res->setProperty("MatebookGVAFix", msg);
}

void csValidatePage(vnode *vp, memory_object_t pager, memory_object_offset_t offset,
    const void *data, int *validated, int *tainted, int *nx) {
    FunctionCast(csValidatePage, orgCsValidatePage)(vp, pager, offset, data, validated, tainted, nx);
    if (!vp || !data) return;
    auto page = static_cast<uint8_t *>(const_cast<void *>(data));
    char path[PATH_MAX];
    int length = PATH_MAX;
    // Located site (any OS build): cheap offset + exact window check first.
    if (__atomic_load_n(&dynReady, __ATOMIC_ACQUIRE)) {
        uint64_t pageOff = dyn.windowFileOffset & ~0xFFFULL;
        if (offset == pageOff &&
            memcmp(page + (dyn.windowFileOffset - pageOff), dyn.window, dyn.windowSize) == 0 &&
            vn_getpath(vp, path, &length) == 0 && strcmp(path, dynPath) == 0 &&
            DscLocate::applyToPage(dyn, pageOff, page, PAGE_SIZE)) {
            patchedPages++;
            SYSLOG("mbgva", "VideoToolbox: MetalRegistryID no longer forwarded (located, page 0x%llx, count %u)",
                static_cast<unsigned long long>(offset), patchedPages);
            return;
        }
    }
    // Static fallback for 24H232.
    if (MatebookGVA::locate(page, PAGE_SIZE).offset == SIZE_MAX) return;
    length = PATH_MAX;
    if (vn_getpath(vp, path, &length) != 0 || strcmp(path, MatebookGVA::SharedCachePath) != 0) return;
    size_t which = MatebookGVA::apply(page, PAGE_SIZE);
    if (which != SIZE_MAX) {
        patchedPages++;
        SYSLOG("mbgva", "VideoToolbox: MetalRegistryID no longer forwarded to encoders (%s, page 0x%llx, count %u)",
            MatebookGVA::Patterns[which].name, static_cast<unsigned long long>(offset), patchedPages);
    }
}

// ---- locator thread ----
struct Files { vnode_t vp[DscLocate::MaxSubcaches + 1]; vfs_context_t ctx; };

bool ioRead(void *c, uint32_t f, uint64_t off, void *buf, size_t size) {
    auto fs = static_cast<Files *>(c);
    if (f > DscLocate::MaxSubcaches || !fs->vp[f]) return false;
    return FileIO::readFileData(buf, static_cast<off_t>(off), size, fs->vp[f], fs->ctx) == 0;
}
void *ioAlloc(void *, size_t size) { return IOMalloc(size); }
void ioFree(void *, void *p, size_t size) { IOFree(p, size); }

void locatorThread(void *, wait_result_t) {
    char msg[160];
    vfs_context_t ctx = nullptr;
    Files fs {};
    int tries = 0;
    // Wait for the login window (IOConsoleUsers is published by loginwindow/WindowServer), so the
    // root, Preboot and cryptex volumes are certainly mounted before any VFS call.
    for (;;) {
        auto res = IOService::getResourceService();
        if (res && res->getProperty("IOConsoleUsers")) break;
        if (++tries > 1800) { publish("locator: no console after 1h, static pattern only"); thread_terminate(current_thread()); return; }
        IOSleep(2000);
    }
    ctx = vfs_context_create(nullptr);
    fs.ctx = ctx;
    tries = 0;
    while (vnode_lookup(MatebookGVA::SharedCachePath, 0, &fs.vp[0], ctx) != 0) {
        if (++tries > 600) { publish("locator: shared cache not found, static pattern only"); goto out; }
        IOSleep(1000);
    }
    {
        uint8_t h[0x200];
        uint32_t so = 0, sc = 0;
        if (FileIO::readFileData(h, 0, sizeof(h), fs.vp[0], ctx) == 0) {
            memcpy(&so, h + 0x188, 4); memcpy(&sc, h + 0x18c, 4);
        }
        if (sc > DscLocate::MaxSubcaches) sc = 0;
        for (uint32_t i = 0; i < sc; i++) {
            uint8_t e[56]; char p[PATH_MAX];
            if (FileIO::readFileData(e, so + i * 56, sizeof(e), fs.vp[0], ctx) != 0) break;
            char sfx[32]; memcpy(sfx, e + 24, 31); sfx[31] = 0;
            snprintf(p, sizeof(p), "%s%s", MatebookGVA::SharedCachePath, sfx);
            if (vnode_lookup(p, 0, &fs.vp[i + 1], ctx) != 0) fs.vp[i + 1] = nullptr;
        }
        DscLocate::Io io {ioRead, ioAlloc, ioFree, &fs};
        int st = DscLocate::locate(io, dyn);
        if (st == DscLocate::Ok) {
            snprintf(dynPath, sizeof(dynPath), "%s%s", MatebookGVA::SharedCachePath, dyn.subcacheSuffix);
            __atomic_store_n(&dynReady, 1, __ATOMIC_RELEASE);
            snprintf(msg, sizeof(msg), "located: file '%s' offset 0x%llx kind %u (shape candidates %u) after %ds",
                dyn.subcacheSuffix, static_cast<unsigned long long>(dyn.patchFileOffset), dyn.kind, dyn.candidates, tries);
        } else {
            snprintf(msg, sizeof(msg), "locator failed: %s (candidates %u), static pattern only",
                DscLocate::statusName(st), dyn.candidates);
        }
        publish(msg);
    }
out:
    for (auto &v : fs.vp) if (v) vnode_put(v);
    vfs_context_rele(ctx);
    thread_terminate(current_thread());
}

void patcherLoaded(void *, KernelPatcher &patcher) {
    KernelPatcher::RouteRequest route {"_cs_validate_page", csValidatePage, orgCsValidatePage};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1)) {
        SYSLOG("mbgva", "failed to route cs_validate_page: %d", patcher.getError());
        patcher.clearError();
        return;
    }
    SYSLOG("mbgva", "MatebookGVAFix 0.4.1 active on Darwin %d", getKernelVersion());
    thread_t th = nullptr;
    if (kernel_thread_start(locatorThread, nullptr, &th) == KERN_SUCCESS) thread_deallocate(th);
    else SYSLOG("mbgva", "locator thread failed to start, static pattern only");
}

void startPlugin() {
    auto status = lilu.onPatcherLoad(patcherLoaded, nullptr);
    if (status != LiluAPI::Error::NoError)
        SYSLOG("mbgva", "registration failed: %d", static_cast<int>(status));
}
const char *disableArgs[] = {"-mbgvaoff"};
const char *debugArgs[] = {"-mbgvadbg"};
}

PluginConfiguration ADDPR(config) = {
    xStringify(PRODUCT_NAME), parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal, disableArgs, arrsize(disableArgs), debugArgs, arrsize(debugArgs),
    nullptr, 0, KernelVersion::Sequoia, KernelVersion::Sequoia, startPlugin
};
