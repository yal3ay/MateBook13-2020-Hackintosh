#pragma once
// IOMemoryDescriptor writeBytes requires input-direction memory prepared for I/O.
// prepare/complete must be paired, including a partial-write failure.
namespace MatebookUVC {
template <typename Descriptor>
static inline bool descriptorWritable(Descriptor *descriptor, IOByteCount length) {
    return descriptor && length && descriptor->getLength() >= length &&
        (descriptor->getDirection() & kIODirectionIn);
}

template <typename Descriptor>
static inline IOReturn copyToPreparedDescriptor(Descriptor *descriptor,
    const void *source, IOByteCount length) {
    if (!source || !descriptorWritable(descriptor, length))
        return kIOReturnBadArgument;
    IOReturn status = descriptor->prepare(kIODirectionIn);
    if (status != kIOReturnSuccess) return status;
    IOByteCount copied = descriptor->writeBytes(0, source, length);
    IOReturn completed = descriptor->complete(kIODirectionIn);
    if (copied != length) return kIOReturnUnderrun;
    return completed;
}
}
