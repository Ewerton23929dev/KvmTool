#include <Internal/kvm_device.h>
#include <KvmTool/kvm_capability.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

KvmStatus KvmCheckCapability(KvmDevice Device, KvmCapabilitySuport capability, int* value)
{
    if (!Device || capability == KVM_CAPABILITY_NONE) return KVM_ERRO;
    int suport = ioctl(Device->DeviceFd, KVM_CHECK_EXTENSION, capability);
    if (suport < 0) return KVM_ERRO;

    if (value) *value = suport;
    return KVM_OK;
}