#ifndef KVMTOOL_PUBLIC_MACHINE_H
#define KVMTOOL_PUBLIC_MACHINE_H
#include "kvm_types.h"
#include "kvm_device.h"

#include <stddef.h>
#include <stdint.h>

typedef struct KvmMachine_T* KvmMachine;

typedef struct KvmMachineMemory_T* KvmMachineMemory;
typedef enum : uint32_t {
    KVM_MEMORY_FLAG_NONE            = 0,
    KVM_MEMORY_FLAG_READONLY        = (1UL << 1),
    KVM_MEMORY_FLAG_LOG_DIRTY_PAGES = (1UL << 0),
    KVM_MEMORY_FLAG_GUEST_MEMFD     = (1UL << 2)
} KvmMachineMemoryFlags;
typedef int KvmMemorySlotId;
typedef unsigned char KvmMemoryAccess;

typedef struct {
    uint64_t size;
    uint64_t phys_addr;

    KvmMemorySlotId slot;
    KvmMachineMemoryFlags flags;
} KvmMachineMemoryRequest;

KvmStatus KvmCreateMachineInstance(KvmDevice InstanceDevice, KvmMachine* InstanceMachineOut);
void KvmDestroyMachineInstance(KvmMachine InstanceMachineEntry);

KvmStatus KvmCreateMemory(KvmMachineMemoryRequest* MemoryRequest, KvmMachineMemory* MemoryOut);
void KvmDestroyMemoryMachine(KvmMachineMemory MemoryEntry);
KvmStatus KvmGetRawMemoryManipulator(KvmMachineMemory MemomryEntry, void** ptr);

KvmStatus KvmRegisterMachineMemory(KvmMachine Instance, KvmMachineMemory SlotMemmory);
#endif