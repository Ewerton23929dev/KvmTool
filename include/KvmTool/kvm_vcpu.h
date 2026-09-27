#ifndef KVMTOOL_PUBLIC_VCPU_H
#define KVMTOOL_PUBLIC_VCPU_H
#include "kvm_types.h"
#include "kvm_machine.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct KvmVcpu_T* KvmVcpu;

#define KVM_MAX_SIGNAL_HANDLERS 16
typedef enum {
    KVM_RUNTIMER_SIG_NONE = -1,
    KVM_RUNTIMER_SIG_IO = 2,
    KVM_RUNTIMER_SIG_HLT = 5,
    KVM_RUNTIMER_SIG_MMIO = 6,
    KVM_RUNTIMER_SIG_INTERNAL_ERRO = 17,
    KVM_RUNTIMER_SIG_FAIL_ENTRY = 9,
} KvmRunnerSignal;

typedef enum {
    KVM_SIGNATURE_KVM,
    KVM_SIGNATURE_INTEL,
    KVM_SIGNATURE_AMD,
    KVM_SIGNATURE_CUSTOM
} KvmSignatureType;
typedef struct {
    KvmSignatureType type_prefix;
    char name[12];
    bool hypervisor_bit;
} KvmCpuidConfig;

typedef void (*KvmSignalHandler)(KvmVcpu vcpu, KvmRunnerSignal signal, void* userdata);

typedef void* KvmRegistresIsa;
typedef struct {
    KvmRegistresIsa (*get_regs)(KvmVcpu vcpu);
    KvmStatus (*set_regs)(KvmVcpu vcpu, KvmRegistresIsa);
    void (*free_object)(KvmRegistresIsa);
} KvmRegistreISAHandle;

KvmStatus KvmCreateVcpuInstance(KvmMachine InstanceMachine, KvmVcpu* InstanceVcpuOut);
void KvmDestroyVcpuInstance(KvmVcpu InstanceVcpuEntry);

KvmStatus KvmSetupVcpuid(KvmVcpu Instance, const KvmCpuidConfig* config);
KvmStatus KvmAnnounceIsa(KvmVcpu Instance, KvmRegistreISAHandle* handle);

KvmStatus KvmGetRegistersStates(KvmVcpu Instance, KvmRegistresIsa* registre);
KvmStatus KvmSetRegisterStates(KvmVcpu Instance, KvmRegistresIsa registre);
void KvmDestroyRegisterIsa(KvmVcpu Instance, KvmRegistresIsa registre);

KvmStatus KvmGetInternalErroTrace(KvmVcpu Instance);

typedef struct {
    uint32_t port;
    uint8_t direction;
} KvmIOperation;
KvmStatus KvmGetIOperation(KvmVcpu Instance, KvmIOperation* operation);

// RUNNER
KvmStatus KvmRunAsync(KvmVcpu instance);
KvmStatus KvmWaitSignal(KvmVcpu instance, KvmRunnerSignal* out);
KvmStatus KvmWaitSignalDispatch(KvmVcpu instance);

KvmStatus KvmRegisterSignalHandler(
    KvmVcpu instance, KvmRunnerSignal signal,
    KvmSignalHandler handler, void* userdata
);

void KvmStopVcpuRunner(KvmVcpu instance);

#endif