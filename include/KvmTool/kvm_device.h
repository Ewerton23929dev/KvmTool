#ifndef KVMTOOL_PUBLIC_DEVICE_H
#define KVMTOOL_PUBLIC_DEVICE_H
#include "kvm_types.h"

typedef struct Kvm_T* KvmDevice;

KvmStatus KvmCreateDeviceInstance(KvmDevice* InstanceOut);
void KvmDestroyDeviceInstance(KvmDevice InstanceEntry);

int KvmGetDeviceVersion(KvmDevice Instance);

#endif