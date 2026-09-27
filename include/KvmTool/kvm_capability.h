#ifndef KVM_CAPABILITY_H
#define KVM_CAPABILITY_H
#include <KvmTool/kvm_device.h>

typedef enum {
    KVM_CAPABILITY_NONE = -1,

    KVM_CAPABILITY_HLT = 1,
    KVM_CAPABILITY_USER_MEMORY = 3,
    KVM_CAPABILITY_MEMSLOTS = 10,
    KVM_CAPABILITY_MAX_VCPUS = 66,
    KVM_CAPABILITY_IMMEDIATE_EXIT = 136
} KvmCapabilitySuport;

KvmStatus KvmCheckCapability(KvmDevice Device, KvmCapabilitySuport capability, int* value);
#endif