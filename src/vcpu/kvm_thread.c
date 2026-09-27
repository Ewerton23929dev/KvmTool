#include <Internal/kvm_vcpu.h>
#include <Internal/kvm_machine.h>
#include <Internal/kvm_device.h>

#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>

// THREAD FUNCTION
static void kvm_thread_get_regs(KvmVcpu Instance, void* data, void* out)
{
    if (!Instance || !out) return;
    KvmRegistresIsa* registre_out = (KvmRegistresIsa*)out;
    KvmRegistresIsa registre = Instance->isa_handler.get_regs(Instance);
    if (!registre) return;
    *registre_out = registre;
}
static void kvm_thread_set_regs(KvmVcpu Instance, void* data, void* out)
{
    if (!Instance || !data) return;
    KvmRegistresIsa registre = (KvmRegistresIsa)data;
    Instance->isa_handler.set_regs(Instance,registre);
}

void* kvm_thread_func(void* arg)
{
    KvmVcpu vcpu = (KvmVcpu)arg;

    // Executa tarefas enfileiradas antes do RUN (ex: KvmSetRegisterStates pré-run)
    for (uint32_t i = 0; i < vcpu->task_queue.current_tasks; i++) {
        KvmThreadTasks* task = &vcpu->task_queue.tasks[i];
        task->func(vcpu, task->data,task->out);
    }
    // Limpa fila inicial após execução
    pthread_mutex_lock(&vcpu->task_queue.task_lock);
    vcpu->task_queue.current_tasks = 0;
    vcpu->task_queue.has_pending = false;
    pthread_mutex_unlock(&vcpu->task_queue.task_lock);

    // Marca como running antes de liberar semáforo
    pthread_mutex_lock(&vcpu->kvm_signal.lock);
    vcpu->kvm_signal.running = true;
    pthread_mutex_unlock(&vcpu->kvm_signal.lock);
    sem_post(&vcpu->task_queue.ready_tasks);

    while (1) {
        // Se immediate_exit foi setado para parar runner, sai antes de KVM_RUN
        if (vcpu->run->immediate_exit) {
            // Se foi por tarefa pendente, processa; se for shutdown, sai
            pthread_mutex_lock(&vcpu->task_queue.task_lock);
            bool pending = vcpu->task_queue.has_pending;
            pthread_mutex_unlock(&vcpu->task_queue.task_lock);
            if (!pending) break;
            // caso pending, deixa KVM_RUN fazer EINTR e tratar abaixo
        }
        int ret = ioctl(vcpu->VcpuFd, KVM_RUN, 0);

        // Sempre processa tarefas pendentes após retorno de KVM_RUN, inclusive EINTR
        pthread_mutex_lock(&vcpu->task_queue.task_lock);
        if (vcpu->task_queue.has_pending) {
            // Executa todas as tarefas pendentes em ordem FIFO
            uint32_t count = vcpu->task_queue.current_tasks;
            for (uint32_t i = 0; i < count; i++) {
                KvmThreadTasks* task = &vcpu->task_queue.tasks[i];
                task->func(vcpu, task->data, task->out);
            }
            vcpu->task_queue.current_tasks = 0;
            vcpu->task_queue.has_pending = false;
            // Limpa immediate_exit apenas se foi setado para injeção de tarefa
            // Se foi shutdown real, quem chamou KvmStopVcpuRunner mantém =1
            // Detecta: se ainda há shutdown pendente, não limpa
            // Por simplicidade, se tem waiter, limpa e sinaliza
            vcpu->run->immediate_exit = 0;
            pthread_cond_broadcast(&vcpu->task_queue.task_done);
            pthread_mutex_unlock(&vcpu->task_queue.task_lock);
            // Volta ao loop sem dispensar sinal - tarefas já executadas
            continue;
        }
        pthread_mutex_unlock(&vcpu->task_queue.task_lock);

        if (ret < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                // Interrompido por kick/shutdown - reavalia loop
                if (vcpu->run->immediate_exit) break;
                continue;
            }
            // Erro real de KVM_RUN - trata como saída
            // Evita loop infinito: sinaliza erro e sai
            break;
        }

        if (vcpu->run->immediate_exit) break;

        KvmRunnerSignal sig = vcpu->run->exit_reason;
        pthread_mutex_lock(&vcpu->kvm_signal.lock);
        vcpu->kvm_signal.pending_signal = sig;
        vcpu->kvm_signal.has_signal = true;
        pthread_cond_signal(&vcpu->kvm_signal.cond);
        // Aguarda host consumir o sinal antes de próximo KVM_RUN (evita perda de IO->HLT)
        while (vcpu->kvm_signal.has_signal) {
            pthread_cond_wait(&vcpu->kvm_signal.cond, &vcpu->kvm_signal.lock);
        }
        pthread_mutex_unlock(&vcpu->kvm_signal.lock);
    }
    // Runner parado
    pthread_mutex_lock(&vcpu->kvm_signal.lock);
    vcpu->kvm_signal.running = false;
    pthread_mutex_unlock(&vcpu->kvm_signal.lock);
    return NULL;
}



KvmStatus KvmSetupVcpuid(KvmVcpu Instance, const KvmCpuidConfig* config)
{
    if (!Instance || !config) return KVM_ERRO;

    // KVM_SET_CPUID2 deve ser feito com VCPU parado; se estiver rodando, despacha via thread
    bool running = false;
    pthread_mutex_lock(&Instance->kvm_signal.lock);
    running = Instance->kvm_signal.running;
    pthread_mutex_unlock(&Instance->kvm_signal.lock);
    if (running) {
        // Versão thread-safe: executa na thread da VCPU
        struct kvm_cpuid2 *tbl = calloc(1,sizeof(*tbl)+128*sizeof(tbl->entries[0]));
        if (!tbl) return KVM_ERRO;
        tbl->nent = 128;
        if (ioctl(Instance->VcpuFd, KVM_GET_CPUID2, tbl)) { free(tbl); return KVM_ERRO; }
        // aplica config igual abaixo e depois SET via dispatch
        // para evitar duplicação, vamos fazer helper dispatch
        // Simplifica: retorna ERRO se chamado com VCPU rodando — caller deve configurar antes de Run
        free(tbl);
        return KVM_ERRO;
    }

    struct kvm_cpuid2 *tbl = calloc(
        1,sizeof(struct kvm_cpuid2) + 128 * sizeof(tbl->entries[0])
    );
    if (!tbl) return KVM_ERRO;
    tbl->nent = 128;
    if (ioctl(Instance->VcpuFd, KVM_GET_CPUID2, tbl)) {
        free(tbl);
        return KVM_ERRO;
    }

    // Se VCPU ainda não tem CPUID (nent==0 logo após criação), busca o suportado pelo host
    if (tbl->nent == 0) {
        // tenta KVM_GET_SUPPORTED_CPUID via device
        KvmDevice dev = Instance->parent->parent;
        struct kvm_cpuid2 *sup = calloc(1,sizeof(*sup)+128*sizeof(sup->entries[0]));
        if (sup) {
            sup->nent = 128;
            if (ioctl(dev->DeviceFd, KVM_GET_SUPPORTED_CPUID, sup) == 0 && sup->nent > 0) {
                free(tbl);
                tbl = sup;
            } else {
                free(sup);
                // fallback mínimo: cria leaf 0 e 1 manualmente
                tbl->nent = 2;
                memset(tbl->entries, 0, 2*sizeof(tbl->entries[0]));
                tbl->entries[0].function = 0;
                tbl->entries[0].eax = 0x0D; // max leaf arbitrário
                tbl->entries[1].function = 1;
                tbl->entries[1].eax = 0x000506A3; // exemplo
            }
        }
    }

    static const char intel[13] = "GenuineIntel";
    static const char amd[13] = "AuthenticAMD";
    const char* assingnature = NULL;

    switch (config->type_prefix) {
        case KVM_SIGNATURE_INTEL: assingnature = intel; break;
        case KVM_SIGNATURE_AMD: assingnature = amd; break;
        case KVM_SIGNATURE_CUSTOM: assingnature = config->name; break;
        default: assingnature = NULL;
    }

    // Garante que leaf 0 e 1 existam mesmo se nent era 0
    bool has_leaf0 = false, has_leaf1 = false, has_leaf400 = false;
    for (uint32_t i=0;i<tbl->nent;i++) {
        if (tbl->entries[i].function==0) has_leaf0=true;
        if (tbl->entries[i].function==1) has_leaf1=true;
        if (tbl->entries[i].function==0x40000000) has_leaf400=true;
    }
    if (!has_leaf0 && assingnature && tbl->nent < 128) {
        tbl->entries[tbl->nent].function=0;
        tbl->entries[tbl->nent].eax=1;
        tbl->nent++;
    }
    if (!has_leaf1 && tbl->nent < 128) {
        tbl->entries[tbl->nent].function=1;
        tbl->entries[tbl->nent].eax=0;
        tbl->nent++;
    }
    if (!has_leaf400 && tbl->nent < 128) {
        tbl->entries[tbl->nent].function=0x40000000;
        tbl->entries[tbl->nent].eax=0;
        tbl->nent++;
    }

    for (uint32_t i = 0; i < tbl->nent; i++) {
        struct kvm_cpuid_entry2 *e = &tbl->entries[i];
        if (e->function == 0 && assingnature) {
            e->ebx = ((uint32_t)assingnature[0]      ) |
                     ((uint32_t)assingnature[1] <<  8) |
                     ((uint32_t)assingnature[2] << 16) |
                     ((uint32_t)assingnature[3] << 24);
            e->ecx = ((uint32_t)assingnature[4]      ) |
                     ((uint32_t)assingnature[5] <<  8) |
                     ((uint32_t)assingnature[6] << 16) |
                     ((uint32_t)assingnature[7] << 24);
            e->edx = ((uint32_t)assingnature[8]      ) |
                     ((uint32_t)assingnature[9] <<  8) |
                     ((uint32_t)assingnature[10] << 16)|
                     ((uint32_t)assingnature[11] << 24);
        }
        if (e->function == 1) {
            if (config->hypervisor_bit)
                e->ecx |= (1U << 31);
            else
                e->ecx &= ~(1U << 31);
        } 
        if (e->function == 0x40000000 && assingnature) e->eax = e->ebx = e->ecx = e->edx = 0;
    }
    int r = ioctl(Instance->VcpuFd, KVM_SET_CPUID2, tbl);
    free(tbl);
    return (r == 0) ? KVM_OK : KVM_ERRO;
}

static KvmStatus IKvmDispatchTaskForQueue(KvmVcpu instance, KvmThreadTaskFn func, void* data, void* out);
KvmStatus KvmVcpuDispatchTask(KvmVcpu instance, KvmThreadTaskFn func, void* data, void* out)
{
    return IKvmDispatchTaskForQueue(instance, func, data, out);
}
static KvmStatus IKvmDispatchTaskForQueue(KvmVcpu instance, KvmThreadTaskFn func, void* data, void* out)
{
    if (!instance || !func) return KVM_ERRO;

    pthread_mutex_lock(&instance->kvm_signal.lock);
    bool running = instance->kvm_signal.running;
    pthread_mutex_unlock(&instance->kvm_signal.lock);

    if (!running) {
        pthread_mutex_lock(&instance->task_queue.task_lock);
        if (instance->task_queue.current_tasks >= MAX_TASKS) {
            pthread_mutex_unlock(&instance->task_queue.task_lock);
            return KVM_ERRO;
        }
        instance->task_queue.tasks[instance->task_queue.current_tasks++] = (KvmThreadTasks){func,data,out};
        pthread_mutex_unlock(&instance->task_queue.task_lock);
        return KVM_OK;
    }

    pthread_mutex_lock(&instance->task_queue.task_lock);
    if (instance->task_queue.current_tasks >= MAX_TASKS) {
        pthread_mutex_unlock(&instance->task_queue.task_lock);
        return KVM_ERRO;
    }
    instance->task_queue.tasks[instance->task_queue.current_tasks++] = (KvmThreadTasks){func,data,out};
    instance->task_queue.has_pending = true;
    instance->run->immediate_exit = 1;
    pthread_mutex_unlock(&instance->task_queue.task_lock);

    // Kick VCPU thread para interromper KVM_RUN (faz ioctl retornar EINTR)
    pthread_kill(instance->kvm_signal.thread, SIGUSR1);

    // Aguarda execução da tarefa
    pthread_mutex_lock(&instance->task_queue.task_lock);
    while (instance->task_queue.has_pending) {
        pthread_cond_wait(&instance->task_queue.task_done, &instance->task_queue.task_lock);
    }
    pthread_mutex_unlock(&instance->task_queue.task_lock);
    return KVM_OK;
}

KvmStatus KvmGetRegistersStates(KvmVcpu Instance, KvmRegistresIsa* registre)
{
    if (!Instance || !registre) return KVM_ERRO;
    if (!Instance->isa_handler.get_regs) return KVM_ERRO;

    IKvmDispatchTaskForQueue(Instance,kvm_thread_get_regs,NULL,registre);
    return KVM_OK;
}
KvmStatus KvmSetRegisterStates(KvmVcpu Instance, KvmRegistresIsa registre)
{
    if (!Instance || !registre) return KVM_ERRO;
    if (!Instance->isa_handler.set_regs) return KVM_ERRO;
    
    IKvmDispatchTaskForQueue(Instance,kvm_thread_set_regs,registre,NULL);
    return KVM_OK;
}

KvmStatus KvmRunAsync(KvmVcpu instance)
{
    if (!instance) return KVM_ERRO;
    // Evita double-run
    pthread_mutex_lock(&instance->kvm_signal.lock);
    if (instance->kvm_signal.running) {
        pthread_mutex_unlock(&instance->kvm_signal.lock);
        return KVM_ERRO;
    }
    pthread_mutex_unlock(&instance->kvm_signal.lock);

    // Instala handler vazio para SIGUSR1 para garantir EINTR no KVM_RUN
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    // Não usa SA_RESTART para garantir que ioctl seja interrompido
    sigaction(SIGUSR1, &sa, NULL);

    if (pthread_create(&instance->kvm_signal.thread, NULL, kvm_thread_func, instance) != 0)
        return KVM_ERRO;
    sem_wait(&instance->task_queue.ready_tasks);
    // Verifica se thread realmente entrou em running
    pthread_mutex_lock(&instance->kvm_signal.lock);
    bool running = instance->kvm_signal.running;
    pthread_mutex_unlock(&instance->kvm_signal.lock);
    return running ? KVM_OK : KVM_ERRO;
}

static KvmRunnerSignal kvm_wait_thread_signal(KvmVcpu instance)
{
    KvmRunnerSignal sig = KVM_RUNTIMER_SIG_NONE;

    pthread_mutex_lock(&instance->kvm_signal.lock);
    while (!instance->kvm_signal.has_signal) {
        // Se runner parou e não há sinal pendente, não bloqueia para sempre
        if (!instance->kvm_signal.running) {
            pthread_mutex_unlock(&instance->kvm_signal.lock);
            return KVM_RUNTIMER_SIG_NONE;
        }
        pthread_cond_wait(&instance->kvm_signal.cond, &instance->kvm_signal.lock);
    }
        
    sig = instance->kvm_signal.pending_signal;
    instance->kvm_signal.has_signal = false;
    // Acorda thread VCPU que está esperando consumo (evita perda de sinal)
    pthread_cond_signal(&instance->kvm_signal.cond);
    pthread_mutex_unlock(&instance->kvm_signal.lock);
    return sig;
}
KvmStatus KvmWaitSignal(KvmVcpu instance, KvmRunnerSignal* out)
{
    if (!instance || !out) return KVM_ERRO;

    *out = kvm_wait_thread_signal(instance);
    return KVM_OK;
}
KvmStatus KvmWaitSignalDispatch(KvmVcpu instance)
{
    if (!instance) return KVM_ERRO;

    KvmRunnerSignal sig = kvm_wait_thread_signal(instance);
    if (sig == KVM_RUNTIMER_SIG_NONE) {
        // Runner parou ou nenhum sinal - verifica se foi shutdown
        pthread_mutex_lock(&instance->kvm_signal.lock);
        bool running = instance->kvm_signal.running;
        pthread_mutex_unlock(&instance->kvm_signal.lock);
        if (!running || instance->run->immediate_exit) return KVM_ERRO;
    }
    for (uint32_t i = 0; i < instance->kvm_signal.table_count; i++) {
        KvmSignalEntry* handler_entry = &instance->kvm_signal.table[i];
        if (handler_entry->signal == sig) {
            handler_entry->handler(instance,sig,handler_entry->userdata);
            break;
        }
    }
    pthread_mutex_lock(&instance->kvm_signal.lock);
    bool still_running = instance->kvm_signal.running;
    pthread_mutex_unlock(&instance->kvm_signal.lock);
    if (!still_running || instance->run->immediate_exit) return KVM_ERRO;
    return KVM_OK;
}
void KvmStopVcpuRunner(KvmVcpu instance)
{
    if (!instance) return;
    pthread_mutex_lock(&instance->kvm_signal.lock);
    bool was_running = instance->kvm_signal.running;
    pthread_mutex_unlock(&instance->kvm_signal.lock);
    if (!was_running) return;

    instance->run->immediate_exit = 1;
    pthread_kill(instance->kvm_signal.thread, SIGUSR1);
    // Acorda waiter de sinal caso esteja bloqueado
    pthread_mutex_lock(&instance->kvm_signal.lock);
    pthread_cond_broadcast(&instance->kvm_signal.cond);
    pthread_mutex_unlock(&instance->kvm_signal.lock);
    pthread_join(instance->kvm_signal.thread, NULL);
    // Limpa flag para permitir re-uso ou destroy correto
    instance->run->immediate_exit = 0;
}