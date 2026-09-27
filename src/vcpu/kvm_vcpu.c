#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>
#include <pthread.h>

#include <stdbool.h>

#include <Internal/kvm_device.h>
#include <Internal/kvm_machine.h>

#include <KvmTool/kvm_vcpu.h>
#include <Internal/kvm_vcpu.h>

static inline bool CreateVcpuRunAccess(struct KvmVcpu_T* Instance)
{
    /* Motivo:
        Herarquia de referencia, Vcpu -> Machine -> device(data)
    */
    KvmDevice device = Instance->parent->parent;
    KvmPreData_t data = device->data;

    struct kvm_run* run = mmap(
        NULL, data.kvm_run_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED, Instance->VcpuFd, 0
    );
    if (run == MAP_FAILED) return false;

    Instance->run = run;
    Instance->run_size = data.kvm_run_size;
    return true;
}
KvmStatus KvmCreateVcpuInstance(KvmMachine InstanceMachine, KvmVcpu* InstanceVcpuOut)
{
    // Variables
    struct KvmVcpu_T* InstanceVcpu = NULL;
    int fd = -1;
    bool sig_lock_init = false;
    bool sig_cond_init = false;
    bool task_lock_init = false;
    bool task_cond_init = false;
    bool task_done_init = false;
    bool sem_init_done = false;

    if (!InstanceMachine || !InstanceVcpuOut) return KVM_ERRO;
    *InstanceVcpuOut = NULL;

    InstanceVcpu = calloc(1, sizeof(struct KvmVcpu_T));
    if (!InstanceVcpu) return KVM_ERRO;

    // Sync primitives must be initialized before any use
    if (pthread_mutex_init(&InstanceVcpu->kvm_signal.lock, NULL) != 0) goto _err;
    sig_lock_init = true;
    if (pthread_cond_init(&InstanceVcpu->kvm_signal.cond, NULL) != 0) goto _err;
    sig_cond_init = true;
    if (pthread_mutex_init(&InstanceVcpu->task_queue.task_lock, NULL) != 0) goto _err;
    task_lock_init = true;
    if (pthread_cond_init(&InstanceVcpu->task_queue.task_cond, NULL) != 0) goto _err;
    task_cond_init = true;
    if (pthread_cond_init(&InstanceVcpu->task_queue.task_done, NULL) != 0) goto _err;
    task_done_init = true;
    if (sem_init(&InstanceVcpu->task_queue.ready_tasks, 0, 0) != 0) goto _err;
    sem_init_done = true;

    fd = ioctl(InstanceMachine->MachineFd, KVM_CREATE_VCPU, 0);
    if (fd < 0) goto _err;

    InstanceVcpu->VcpuFd = fd;
    InstanceVcpu->parent = InstanceMachine;

    if (!CreateVcpuRunAccess(InstanceVcpu)) goto _err;

    *InstanceVcpuOut = InstanceVcpu;
    return KVM_OK;

    _err:
    if (fd >= 0) close(fd);
    if (InstanceVcpu) {
        if (InstanceVcpu->run) munmap(InstanceVcpu->run, InstanceVcpu->run_size);
        if (sem_init_done) sem_destroy(&InstanceVcpu->task_queue.ready_tasks);
        if (task_done_init) pthread_cond_destroy(&InstanceVcpu->task_queue.task_done);
        if (task_cond_init) pthread_cond_destroy(&InstanceVcpu->task_queue.task_cond);
        if (task_lock_init) pthread_mutex_destroy(&InstanceVcpu->task_queue.task_lock);
        if (sig_cond_init) pthread_cond_destroy(&InstanceVcpu->kvm_signal.cond);
        if (sig_lock_init) pthread_mutex_destroy(&InstanceVcpu->kvm_signal.lock);
        free(InstanceVcpu);
    }
    return KVM_ERRO;
}

void KvmDestroyRegisterIsa(KvmVcpu Instance, KvmRegistresIsa registre)
{
    if (!Instance || !registre) return;
    if (!Instance->isa_handler.free_object) return;
    Instance->isa_handler.free_object(registre);
}
KvmStatus KvmAnnounceIsa(KvmVcpu Instance, KvmRegistreISAHandle* handle)
{
    if (!Instance || !handle) return KVM_ERRO;
    if (!handle->set_regs || !handle->get_regs || !handle->free_object) return KVM_ERRO;

    Instance->isa_handler = *handle;
    return KVM_OK;
}

KvmStatus KvmRegisterSignalHandler(
    KvmVcpu instance, KvmRunnerSignal signal,
    KvmSignalHandler handler, void* userdata
)
{
    if (!instance || signal == KVM_RUNTIMER_SIG_NONE || !handler) return KVM_ERRO;
    if (instance->kvm_signal.table_count >= KVM_MAX_SIGNAL_HANDLERS) return KVM_ERRO;

    KvmSignalEntry entry = {
        .handler = handler,
        .signal = signal,
        .userdata = userdata
    };
    instance->kvm_signal.table[instance->kvm_signal.table_count++] = entry;
    return KVM_OK;
}

#include <stdio.h>
KvmStatus KvmGetInternalErroTrace(KvmVcpu Instance)
{
    if (!Instance) return KVM_ERRO;

    printf("Sub-Erro: %u\n", Instance->run->internal.suberror);
    printf("Number os extra data fields: %u\n",Instance->run->internal.ndata);
    for (uint32_t i = 0; i < Instance->run->internal.ndata; i++) {
        fprintf(stderr, "data[%u]: 0x%llx\n", i, Instance->run->internal.data[i]);
    }
    if (Instance->run->internal.suberror == 1) { // KVM_INTERNAL_ERROR_EMULATION
        fprintf(stderr, "Motivo: O KVM tentou emular uma instrução inválida ou desconhecida no endereço atual do EIP/RIP.\n");
    } else if (Instance->run->internal.suberror == 2) { // KVM_INTERNAL_ERROR_SIMUL_EX
        fprintf(stderr, "Motivo: Ocorreu uma exceção simultânea (falha grave de hardware virtual).\n");
    } else if (Instance->run->internal.suberror == 3) { // KVM_INTERNAL_ERROR_DELIVERY_EV
        fprintf(stderr, "Motivo: Falha ao entregar um evento/interrupção para a VCPU.\n");
    }
    return KVM_OK;
}

KvmStatus KvmGetIOperation(KvmVcpu Instance, KvmIOperation* operation)
{
    if (!Instance || !operation) return KVM_ERRO;

    operation->port = Instance->run->io.port;
    operation->direction = Instance->run->io.direction;
    return KVM_OK;
}

void KvmDestroyVcpuInstance(KvmVcpu InstanceVcpuEntry)
{
    if (!InstanceVcpuEntry) return;

    // Caller must have stopped runner via KvmStopVcpuRunner if it was started
    if (InstanceVcpuEntry->run) {
        munmap(InstanceVcpuEntry->run, InstanceVcpuEntry->run_size);
    }
    if (InstanceVcpuEntry->VcpuFd >= 0) close(InstanceVcpuEntry->VcpuFd);
    sem_destroy(&InstanceVcpuEntry->task_queue.ready_tasks);
    pthread_cond_destroy(&InstanceVcpuEntry->task_queue.task_done);
    pthread_cond_destroy(&InstanceVcpuEntry->task_queue.task_cond);
    pthread_mutex_destroy(&InstanceVcpuEntry->task_queue.task_lock);
    pthread_cond_destroy(&InstanceVcpuEntry->kvm_signal.cond);
    pthread_mutex_destroy(&InstanceVcpuEntry->kvm_signal.lock);
    free(InstanceVcpuEntry);
}