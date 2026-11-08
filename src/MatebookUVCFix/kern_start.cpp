// Targeted adaptation of the UVC1.5 length-recovery approach in UVCProbeFix.
// Upstream: https://github.com/danielnmaldonado/UVCProbeFix
// Copyright (c) 2026 DNMTechLabs. BSD-3-Clause; see included upstream LICENSE.
//
// 0.3.0 (2026-11-07): one kext for macOS 13-26 (Darwin 22-25).
//  - Root cause: camera 13d3:56c6 is UVC 1.5 and always answers VS PROBE/COMMIT GET_CUR
//    with 48 bytes; macOS asks for 26/34 bytes, so the control transfer overruns
//    (0xe00002e8) and video never starts.
//  - Fix: for exactly those requests, ask the device for 48 bytes into a private kernel
//    buffer and hand the caller the first 26/34 bytes.
//  - Replaces the exact-build UUID lock (0.2.x) and exact-minor lock (0.1.x) with:
//    mangled-symbol match (signature) + verified function prologue. Fails closed otherwise.
//  - Caller memory descriptors are written only inside prepare()/complete()
//    (0.2.1 panicked with "IOGMD: not wired for the IODMACommand").
//  - Anything the hook cannot handle safely is passed to Apple's original code untouched.
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <libkern/c++/OSNumber.h>
#include "policy.hpp"
#include "version_gate.hpp"
#include "descriptor_copy.hpp"

namespace {
using RequestFunction = IOReturn (*)(IOService *, IOService *,
    StandardUSB::DeviceRequest &, void *, IOMemoryDescriptor *,
    uint32_t &, IOUSBHostCompletion *, uint32_t);
RequestFunction originalRequest = nullptr;
bool installed = false;
const char *paths[] = {"/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily"};
KernelPatcher::KextInfo hostInfo {
    "com.apple.iokit.IOUSBHostFamily", paths, arrsize(paths), {true}, {},
    KernelPatcher::KextInfo::Unloaded
};
const char symbol[] = "__ZN15IOUSBHostDevice21internalDeviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj";

inline IOReturn passThrough(IOService *device, IOService *client,
    StandardUSB::DeviceRequest &request, void *raw, IOMemoryDescriptor *descriptor,
    uint32_t &transferred, IOUSBHostCompletion *completion, uint32_t timeout) {
    return originalRequest(device, client, request, raw, descriptor, transferred, completion, timeout);
}

IOReturn hook(IOService *device, IOService *client,
    StandardUSB::DeviceRequest &request, void *raw, IOMemoryDescriptor *descriptor,
    uint32_t &transferred, IOUSBHostCompletion *completion, uint32_t timeout) {
    if (!originalRequest) return kIOReturnUnsupported;
    // Cheap field filter first: no property lookups or allocations for unrelated requests.
    if (!device || request.bmRequestType != 0xa1 || request.bRequest != 0x81 ||
        (request.wLength != 26 && request.wLength != 34) || request.wIndex != 1 ||
        (request.wValue != 0x0100 && request.wValue != 0x0200) || completion ||
        ((raw != nullptr) == (descriptor != nullptr)))
        return passThrough(device, client, request, raw, descriptor, transferred, completion, timeout);

    auto vendor = OSDynamicCast(OSNumber, device->getProperty("idVendor"));
    auto product = OSDynamicCast(OSNumber, device->getProperty("idProduct"));
    MatebookUVC::Request view {
        static_cast<uint16_t>(vendor ? vendor->unsigned32BitValue() : 0),
        static_cast<uint16_t>(product ? product->unsigned32BitValue() : 0),
        request.bmRequestType, request.bRequest, request.wValue, request.wIndex,
        request.wLength, completion == nullptr, raw != nullptr, descriptor != nullptr
    };
    if (!MatebookUVC::eligible(view))
        return passThrough(device, client, request, raw, descriptor, transferred, completion, timeout);
    // A descriptor we cannot legally write into is left to Apple's code (it fails safely).
    if (descriptor && !MatebookUVC::descriptorWritable(descriptor, request.wLength))
        return passThrough(device, client, request, raw, descriptor, transferred, completion, timeout);

    auto expanded = static_cast<uint8_t *>(IOMalloc(MatebookUVC::DeviceLength));
    if (!expanded)
        return passThrough(device, client, request, raw, descriptor, transferred, completion, timeout);
    bzero(expanded, MatebookUVC::DeviceLength);
    auto enlarged = request;
    enlarged.wLength = MatebookUVC::DeviceLength;
    uint32_t actual = 0;
    IOReturn result = originalRequest(device, client, enlarged, expanded, nullptr, actual, nullptr, timeout);
    if (MatebookUVC::safeReply(result == kIOReturnSuccess, actual, MatebookUVC::DeviceLength, request.wLength)) {
        IOReturn copied = kIOReturnSuccess;
        if (raw) memcpy(raw, expanded, request.wLength);
        else copied = MatebookUVC::copyToPreparedDescriptor(descriptor, expanded, request.wLength);
        transferred = copied == kIOReturnSuccess ? request.wLength : 0;
        result = copied;
    } else {
        transferred = 0;
        if (result == kIOReturnSuccess) result = kIOReturnUnderrun;
    }
    SYSLOG("mbuvc", "13d3:56c6 GET_CUR %u->48 actual=%u result=0x%x buffer=%s",
        request.wLength, actual, result, raw ? "raw" : "descriptor");
    bzero(expanded, MatebookUVC::DeviceLength);
    IOFree(expanded, MatebookUVC::DeviceLength);
    return result;
}

void loaded(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
    if (installed || !size || !MatebookGate::supportedDarwin(getKernelVersion())) return;
    auto target = patcher.solveSymbol(index, symbol, slide, size, false);
    if (!target) {
        patcher.clearError();
        SYSLOG("mbuvc", "target symbol missing; patch disabled");
        return;
    }
    if (!MatebookGate::prologueMatches(reinterpret_cast<const uint8_t *>(target),
            sizeof(MatebookGate::ExpectedPrologue))) {
        SYSLOG("mbuvc", "target prologue differs from audited 22A380/24H232; patch disabled");
        return;
    }
    KernelPatcher::RouteRequest route {symbol, hook, originalRequest};
    if (!patcher.routeMultiple(index, &route, 1, slide, size, true, false) || !originalRequest) {
        patcher.clearError();
        originalRequest = nullptr;
        SYSLOG("mbuvc", "route failed; patch disabled");
        return;
    }
    installed = true;
    SYSLOG("mbuvc", "13d3:56c6 UVC1.5 recovery 0.3.0 active on Darwin %d", getKernelVersion());
}

void startPlugin() {
    if (!MatebookGate::supportedDarwin(getKernelVersion())) {
        SYSLOG("mbuvc", "unsupported kernel %d; patch disabled", getKernelVersion());
        return;
    }
    auto status = lilu.onKextLoad(&hostInfo, 1, loaded, nullptr);
    if (status != LiluAPI::Error::NoError)
        SYSLOG("mbuvc", "registration failed: %d", static_cast<int>(status));
}
const char *disableArgs[] = {"-mbuvcoff"};
const char *debugArgs[] = {"-mbuvcdbg"};
}

PluginConfiguration ADDPR(config) = {
    xStringify(PRODUCT_NAME), parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal, disableArgs, arrsize(disableArgs), debugArgs, arrsize(debugArgs),
    nullptr, 0, KernelVersion::Ventura, KernelVersion::Tahoe, startPlugin
};
