#ifndef KVM_X86_MODES_SETS_H
#define KVM_X86_MODES_SETS_H
#include <KvmTool/kvm_vcpu.h>

KvmStatus KvmX86SetupProtectedMode(KvmVcpu Vcpu, KvmMachineMemory Memory);
KvmStatus KvmX86SetRealMode(KvmVcpu Instance);

KvmStatus KvmX86ConfigureGDT(KvmVcpu Vcpu, KvmMachineMemory Memory);
KvmStatus KvmX86ConfigureIDT(KvmVcpu Vcpu, KvmMachineMemory Memory);

KvmStatus KvmX86SetupLongMode(KvmVcpu Vcpu, KvmMachineMemory Memory);
KvmStatus KvmX86SetupLongModeWithPaging(KvmVcpu Vcpu, KvmMachineMemory Memory, uint64_t pml4_addr);

#endif