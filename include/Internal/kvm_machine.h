#ifndef KVMTOOL_PRIVATE_MACHINE_H
#define KVMTOOL_PRIVATE_MACHINE_H
#include <KvmTool/kvm_machine.h>

struct KvmMachine_T {
    KvmDevice parent;
    int MachineFd;
};
struct KvmMachineMemory_T {
    void* mem;
    int slot;
    int flag;

    size_t size;
    size_t addr;
};

#endif