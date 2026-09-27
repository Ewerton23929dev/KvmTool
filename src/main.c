#include <KvmTool/kvm.h>
#include <KvmTool/x86/kvmx86_modes.h>
#include <KvmTool/x86/kvmx86_registre.h>
#include <stdio.h>
#include <unistd.h>

static void SignalHlt(KvmVcpu vcpu, KvmRunnerSignal signal, void* userdata)
{
    (void)vcpu;
    (void)signal;
    (void)userdata;
    printf("bateu no HLT!!\n");
    KvmStopVcpuRunner(vcpu);
}
static void SignalInternalErro(KvmVcpu vcpu, KvmRunnerSignal signal, void* userdata)
{
    (void)signal;
    (void)userdata;
    printf("Erro Interno!\n");
    KvmGetInternalErroTrace(vcpu);
    KvmStopVcpuRunner(vcpu);
    return;
}
static void SignalFailEntry(KvmVcpu vcpu, KvmRunnerSignal signal, void* userdata)
{
    (void)signal;
    (void)userdata;
    printf("Erro de FAIL ENTRY\n");
    KvmGetInternalErroTrace(vcpu);
    KvmStopVcpuRunner(vcpu);
}
static void SignalIO(KvmVcpu vcpu, KvmRunnerSignal signal, void* userdata)
{
    (void)signal;
    (void)userdata;
    KvmRegistresIsa registre = NULL;
    KvmGetRegistersStates(vcpu,&registre);
    KvmX86RegistreState* r = KvmX86ExtractRegistreState(registre);
    if (!r) {
        KvmDestroyRegisterIsa(vcpu, registre);
        return;
    }
    printf("Syscall %d: Arg1: %d, Arg2: %d\n", (uint32_t)r->rax,(uint32_t)r->rbx, (uint32_t)r->rcx);
    KvmDestroyRegisterIsa(vcpu,registre);
}

int main()
{
    KvmDevice Device = NULL;
    KvmMachine Machine = NULL;
    KvmVcpu Vcpu = NULL;
    KvmMachineMemory Memory = NULL;

    if (!KvmCreateDeviceInstance(&Device)) {
        printf("Erro em criar Device!\n");
        return 1;
    }
    printf("Version: %d\n",KvmGetDeviceVersion(Device));
    if (!KvmCreateMachineInstance(Device,&Machine)) {
        printf("Erro em criar Machine!\n");
        KvmDestroyDeviceInstance(Device);
        return 1;
    }
    if (!KvmCreateVcpuInstance(Machine,&Vcpu)) {
        printf("Erro em criar Vcpu!\n");
        KvmDestroyMachineInstance(Machine);
        KvmDestroyDeviceInstance(Device);
        return 1;
    }

    KvmMachineMemoryRequest slot_exemple = {
        .slot = 0,
        .flags = KVM_MEMORY_FLAG_NONE,
        .phys_addr = 0x0,
        .size = (1 * 1024 * 1024),
    };
    void* raw_ptr = NULL;

    KvmCreateMemory(&slot_exemple,&Memory);
    KvmRegisterMachineMemory(Machine,Memory);
    KvmGetRawMemoryManipulator(Memory,&raw_ptr);
    uint8_t* ram = (uint8_t*)raw_ptr;

    // sys_write(1, 0x1000, 1) — escreve 'A' no stdout
// coloca o 'A' em 0x1000
ram[0x1000] = 'A';

// mov eax, 4  (sys_write)
ram[0] = 0xB8;
ram[1] = 0x04;
ram[2] = 0x00;
ram[3] = 0x00;
ram[4] = 0x00;

// mov ebx, 1  (fd = stdout)
ram[5] = 0xBB;
ram[6] = 0x01;
ram[7] = 0x00;
ram[8] = 0x00;
ram[9] = 0x00;

// mov ecx, 0x1000  (buffer)
ram[10] = 0xB9;
ram[11] = 0x00;
ram[12] = 0x10;
ram[13] = 0x00;
ram[14] = 0x00;

// mov edx, 1  (tamanho)
ram[15] = 0xBA;
ram[16] = 0x01;
ram[17] = 0x00;
ram[18] = 0x00;
ram[19] = 0x00;

// int 0x80
ram[20] = 0xCD;
ram[21] = 0x80;

// hlt
ram[22] = 0xF4;

    KvmX86SetupProtectedMode(Vcpu, Memory);
    KvmX86SetupLongMode(Vcpu,Memory);
    KvmX86ConfigureIDT(Vcpu,Memory);
    KvmCpuidConfig vcpu_config = {
        .hypervisor_bit = false,
        .type_prefix = KVM_SIGNATURE_INTEL
    };
    KvmSetupVcpuid(Vcpu,&vcpu_config);

    KvmAnnounceIsa(Vcpu,KvmX86RegistreIsaSuport());
    KvmX86RegistreState registre_x86 = {0};
    registre_x86.rip = 0x0;
    registre_x86.rflags = 0x2;
    registre_x86.rsp = 0x7000;
    KvmRegistresIsa registre = KvmX86TransformRegistreIsa(&registre_x86);
    KvmSetRegisterStates(Vcpu,registre);
    KvmDestroyRegisterIsa(Vcpu,registre);

    KvmRegisterSignalHandler(Vcpu,KVM_RUNTIMER_SIG_HLT,SignalHlt,NULL);
    KvmRegisterSignalHandler(Vcpu,KVM_RUNTIMER_SIG_IO, SignalIO,NULL);
    KvmRegisterSignalHandler(Vcpu,KVM_RUNTIMER_SIG_INTERNAL_ERRO, SignalInternalErro, NULL);
    KvmRegisterSignalHandler(Vcpu,KVM_RUNTIMER_SIG_FAIL_ENTRY,SignalFailEntry,NULL);

    KvmRunAsync(Vcpu);
    while (KvmWaitSignalDispatch(Vcpu) == KVM_OK);

    KvmDestroyMemoryMachine(Memory);
    KvmDestroyVcpuInstance(Vcpu);
    KvmDestroyMachineInstance(Machine);
    KvmDestroyDeviceInstance(Device);
    return 0;
}