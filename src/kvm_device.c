#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include <stdio.h>

#include <stdbool.h>

#include <Internal/kvm_device.h>

static inline bool GetDeviceRunMmapSize(int fd, KvmPreData_t* data)
{
    int mmap_size = ioctl(fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        return false;
    }

    data->kvm_run_size = mmap_size;
    return true;
}
KvmStatus KvmCreateDeviceInstance(KvmDevice* InstanceOut)
{
    // Variables
    struct Kvm_T* instance = NULL;
    int fd = -1;

    if (!InstanceOut) return KVM_ERRO;
    *InstanceOut = NULL;

    instance = calloc(1, sizeof(struct Kvm_T));
    if (!instance) return KVM_ERRO;

    fd = open("/dev/kvm",O_RDWR | O_CLOEXEC);
    if (fd < 0) goto _err;

    if (!GetDeviceRunMmapSize(fd,&instance->data)) goto _err;

    instance->DeviceFd = fd;
    *InstanceOut = instance;

    return KVM_OK;

    _err:
    if (fd >= 0) close(fd);
    free(instance);
    return KVM_ERRO;
}
void KvmDestroyDeviceInstance(KvmDevice InstanceEntry)
{
    if (!InstanceEntry) return;

    if (InstanceEntry->DeviceFd >= 0) close(InstanceEntry->DeviceFd);
    free(InstanceEntry);
}

int KvmGetDeviceVersion(KvmDevice Instance)
{
    if (!Instance) return -1;
    return ioctl(Instance->DeviceFd,KVM_GET_API_VERSION, 0);
}