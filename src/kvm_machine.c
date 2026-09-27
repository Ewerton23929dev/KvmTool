#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

#include <stdint.h>

#include <Internal/kvm_device.h>
#include <KvmTool/kvm_machine.h>
#include <Internal/kvm_machine.h>

KvmStatus KvmCreateMachineInstance(KvmDevice InstanceDevice, KvmMachine* InstanceMachineOut)
{
    // Varibles
    struct KvmMachine_T* InstanceMachine = NULL;
    int fd = -1;

    if (!InstanceDevice) return KVM_ERRO;
    *InstanceMachineOut = NULL;

    InstanceMachine = calloc(1, sizeof(struct KvmMachine_T));
    if (!InstanceMachine) return KVM_ERRO;

    fd = ioctl(InstanceDevice->DeviceFd, KVM_CREATE_VM, 0);
    if (fd < 0) goto _err;

    InstanceMachine->MachineFd = fd;
    InstanceMachine->parent = InstanceDevice;
    *InstanceMachineOut = InstanceMachine;

    return KVM_OK;

    _err:
    free(InstanceMachine);
    return KVM_ERRO;
}
void KvmDestroyMachineInstance(KvmMachine InstanceMachineEntry)
{
    if (!InstanceMachineEntry) return;

    if (InstanceMachineEntry->MachineFd >= 0) close(InstanceMachineEntry->MachineFd);
    free(InstanceMachineEntry);
}

KvmStatus KvmCreateMemory(KvmMachineMemoryRequest* MemoryRequest, KvmMachineMemory* MemoryOut)
{
    if (!MemoryRequest) return KVM_ERRO;
    *MemoryOut = NULL;

    // Variables: 
    struct KvmMachineMemory_T* Memory = NULL;
    void* mem = NULL;

    if (MemoryRequest->size == 0) return KVM_ERRO;

    Memory = calloc(1, sizeof(struct KvmMachineMemory_T));
    if (!Memory) return KVM_ERRO;

    mem = mmap(
        NULL,MemoryRequest->size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    );
    if (mem == MAP_FAILED) goto _err;

    Memory->mem = mem;
    Memory->addr = MemoryRequest->phys_addr;
    Memory->size = MemoryRequest->size;
    Memory->slot = MemoryRequest->slot;
    Memory->flag = MemoryRequest->flags;
    *MemoryOut = Memory;
    return KVM_OK;

    _err:
    free(Memory);
    return KVM_ERRO;
}
void KvmDestroyMemoryMachine(KvmMachineMemory MemoryEntry)
{
    if (!MemoryEntry) return;

    if (MemoryEntry->mem) munmap(MemoryEntry->mem,MemoryEntry->size);
    free(MemoryEntry);
}

KvmStatus KvmGetRawMemoryManipulator(KvmMachineMemory MemomryEntry, void** ptr)
{
    if (!MemomryEntry || !ptr) return KVM_ERRO;
    if (!MemomryEntry->mem) return KVM_ERRO;

    *ptr = MemomryEntry->mem;

    return KVM_OK;
}

KvmStatus KvmRegisterMachineMemory(KvmMachine Instance, KvmMachineMemory Memory)
{
    if (!Instance || !Memory) return KVM_ERRO;
    struct kvm_userspace_memory_region2 mem = {
        .slot = Memory->slot,
        .flags = Memory->flag,
        .guest_phys_addr = Memory->addr,
        .memory_size = Memory->size,
        .userspace_addr = (uint64_t)Memory->mem,
        .guest_memfd = 0,
        .guest_memfd_offset = 0
    };

    if (ioctl(Instance->MachineFd,KVM_SET_USER_MEMORY_REGION2, &mem) < 0) return KVM_ERRO;
    return KVM_OK;
}