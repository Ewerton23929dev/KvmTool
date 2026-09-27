#include <KvmTool/x86/kvmx86_registre.h>
#include <Internal/kvm_vcpu.h>

#include <sys/ioctl.h>
#include <linux/kvm.h>
#include <string.h>
#include <stdlib.h>

_Static_assert(sizeof(KvmX86RegistreState) == sizeof(struct kvm_regs),"KvmX86RegistreState e kvm_regs divergiram!");
_Static_assert(sizeof(KvmX86SRegsState) == sizeof(struct kvm_sregs),"KvmX86SregsState e kvm_sregs divergiram!");

static KvmRegistresIsa kvmx86_get_registre(KvmVcpu Instance)
{
    if (!Instance) return NULL;

    KvmThreadTransportReg* x86_regs = malloc(sizeof(KvmThreadTransportReg));
    if (!x86_regs) return NULL;
    x86_regs->type = KVM_X86_REGS_T;

    struct kvm_regs regs = {0};
    if (ioctl(Instance->VcpuFd, KVM_GET_REGS, &regs)) {
        free(x86_regs);
        return NULL;
    }
    memcpy(&x86_regs->reg.registre,&regs,sizeof(regs));
    return (KvmRegistresIsa)x86_regs;
}
static KvmStatus kvmx86_set_registre(KvmVcpu Instance, KvmRegistresIsa registre)
{
    if (!Instance || !registre) return KVM_ERRO;
    KvmThreadTransportReg* real_registre = (KvmThreadTransportReg*)registre;
    if (real_registre->type == KVM_X86_SREGS_T) {
        struct kvm_sregs sregs = {0};
        memcpy(&sregs, &real_registre->reg.sreg, sizeof(sregs));
        if (ioctl(Instance->VcpuFd,KVM_SET_SREGS,&sregs)) return KVM_ERRO;
        return KVM_OK;
    }

    struct kvm_regs regs = {0};
    memcpy(&regs,&real_registre->reg.registre,sizeof(regs));
    if (ioctl(Instance->VcpuFd, KVM_SET_REGS, &regs)) return KVM_ERRO;

    return KVM_OK;
}
static void kvmx86_free_object(KvmRegistresIsa registre)
{
    if (!registre) return;
    free(registre);
}
KvmRegistreISAHandle kvm_x86_isa = {
    .get_regs = kvmx86_get_registre,
    .set_regs = kvmx86_set_registre,
    .free_object = kvmx86_free_object
};

KvmRegistreISAHandle* KvmX86RegistreIsaSuport()
{
    return &kvm_x86_isa;
}
KvmRegistresIsa KvmX86TransformRegistreIsa(KvmX86RegistreState* state)
{
    if (!state) return NULL;

    KvmThreadTransportReg* x86_regs = malloc(sizeof(KvmThreadTransportReg));
    if (!x86_regs) return NULL;
    x86_regs->type = KVM_X86_REGS_T;

    memcpy(&x86_regs->reg.registre,state,sizeof(KvmX86RegistreState));
    return (KvmRegistresIsa)x86_regs;
}
KvmRegistresIsa KvmX86TransformRegistreSregIsa(KvmX86SRegsState* state)
{
    if (!state) return NULL;
    KvmThreadTransportReg* x86_regs = malloc(sizeof(KvmThreadTransportReg));
    if (!x86_regs) return NULL;
    x86_regs->type = KVM_X86_SREGS_T;

    memcpy(&x86_regs->reg.sreg,state,sizeof(KvmX86SRegsState));
    return (KvmRegistresIsa)x86_regs;
}