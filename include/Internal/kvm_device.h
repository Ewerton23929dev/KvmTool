#ifndef KVMTOOL_PRIVATE_DEVICE_H
#define KVMTOOL_PRIVATE_DEVICE_H
#include <KvmTool/kvm_device.h>

typedef struct {
    int kvm_run_size;
} KvmPreData_t;
struct Kvm_T {
    int DeviceFd;
    KvmPreData_t data;
};

#endif