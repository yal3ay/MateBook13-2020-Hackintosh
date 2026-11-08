#pragma once
#include <stdint.h>
#include <stddef.h>

namespace MatebookUVC {
constexpr uint16_t Vendor = 0x13d3;
constexpr uint16_t Product = 0x56c6;
constexpr uint16_t DeviceLength = 48;
struct Request {
    uint16_t vendor, product;
    uint8_t type, request;
    uint16_t value, index, length;
    bool synchronous, raw, descriptor;
};
inline bool eligible(const Request &r) {
    return r.vendor == Vendor && r.product == Product &&
        r.type == 0xa1 && r.request == 0x81 &&
        (r.value == 0x0100 || r.value == 0x0200) && r.index == 1 &&
        (r.length == 26 || r.length == 34) && r.synchronous &&
        (r.raw != r.descriptor);
}
inline bool safeReply(bool success, uint32_t actual, size_t capacity, size_t requested) {
    return success && actual == DeviceLength && capacity >= DeviceLength &&
        (requested == 26 || requested == 34);
}
}
