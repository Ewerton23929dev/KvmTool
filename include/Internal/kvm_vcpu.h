#ifndef KVMTOOL_PRIVATE_VCPU_H
#define KVMTOOL_PRIVATE_VCPU_H
#include <KvmTool/kvm_vcpu.h>

#include <semaphore.h>
#include <pthread.h>
#include <linux/kvm.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    KvmRunnerSignal signal;
    KvmSignalHandler handler;
    void* userdata;
} KvmSignalEntry;


typedef void (*KvmThreadTaskFn)(KvmVcpu vcpu, void* data, void* out);
#define MAX_TASKS 128
typedef struct {
    KvmThreadTaskFn func;
    void* data;
    void* out;
} KvmThreadTasks;

// Dispatcher público para executar tarefas na thread da VCPU (usado por kvmx86_modes/reg)
KvmStatus KvmVcpuDispatchTask(KvmVcpu instance, KvmThreadTaskFn func, void* data, void* out);

struct KvmVcpu_T {
    struct {
        pthread_t thread;
        pthread_mutex_t lock;
        pthread_cond_t cond;

        KvmRunnerSignal pending_signal;
        bool has_signal;

        KvmSignalEntry table[KVM_MAX_SIGNAL_HANDLERS];
        uint32_t table_count;

        bool running;
    } kvm_signal;
    struct {
        KvmThreadTasks tasks[MAX_TASKS];
        uint32_t current_tasks;
        sem_t ready_tasks;

        pthread_mutex_t task_lock;
        pthread_cond_t task_cond;
        pthread_cond_t task_done;
        bool has_pending;
    } task_queue;

    KvmRegistreISAHandle isa_handler;

    KvmMachine parent;
    struct kvm_run* run;
    size_t run_size;
    int VcpuFd;
};

#endif