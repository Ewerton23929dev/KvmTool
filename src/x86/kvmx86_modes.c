#include <KvmTool/x86/kvmx86_modes.h>
#include <Internal/kvm_vcpu.h>
#include <Internal/kvm_machine.h>

#include <sys/ioctl.h>
#include <string.h>
#include <linux/kvm.h>
#include <pthread.h>

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) GdtEntry32;
#define GDT_ADDR 0xE000

// --- Helpers para rodar sregs na thread da VCPU (igual a kvmx86_registres.c) ---
static void kvm_modes_get_sregs(KvmVcpu vcpu, void* data, void* out)
{
    (void)data;
    struct kvm_sregs* sregs = (struct kvm_sregs*)out;
    ioctl(vcpu->VcpuFd, KVM_GET_SREGS, sregs);
}
static void kvm_modes_set_sregs(KvmVcpu vcpu, void* data, void* out)
{
    (void)out;
    struct kvm_sregs* sregs = (struct kvm_sregs*)data;
    ioctl(vcpu->VcpuFd, KVM_SET_SREGS, sregs);
}
static KvmStatus kvm_modes_do_get_sregs(KvmVcpu vcpu, struct kvm_sregs* out)
{
    bool running;
    pthread_mutex_lock(&vcpu->kvm_signal.lock);
    running = vcpu->kvm_signal.running;
    pthread_mutex_unlock(&vcpu->kvm_signal.lock);
    if (!running) {
        if (ioctl(vcpu->VcpuFd, KVM_GET_SREGS, out) < 0) return KVM_ERRO;
        return KVM_OK;
    }
    return KvmVcpuDispatchTask(vcpu, kvm_modes_get_sregs, NULL, out);
}
static KvmStatus kvm_modes_do_set_sregs(KvmVcpu vcpu, struct kvm_sregs* sregs)
{
    bool running;
    pthread_mutex_lock(&vcpu->kvm_signal.lock);
    running = vcpu->kvm_signal.running;
    pthread_mutex_unlock(&vcpu->kvm_signal.lock);
    if (!running) {
        if (ioctl(vcpu->VcpuFd, KVM_SET_SREGS, sregs) < 0) return KVM_ERRO;
        return KVM_OK;
    }
    return KvmVcpuDispatchTask(vcpu, kvm_modes_set_sregs, sregs, NULL);
}

KvmStatus KvmX86SetupProtectedMode(KvmVcpu Vcpu, KvmMachineMemory Memory)
{
    if (!Vcpu || !Memory || !Memory->mem) return KVM_ERRO;
    if (Memory->size < (size_t)(GDT_ADDR + 3 * sizeof(GdtEntry32))) return KVM_ERRO;

    struct kvm_sregs sregs = {0};
    if (kvm_modes_do_get_sregs(Vcpu, &sregs) != KVM_OK) return KVM_ERRO;
    sregs.cr0 |= 0x1;

    #define SEG_CODE(seg, sel) do { \
        (seg).base     = 0;          \
        (seg).limit    = 0xFFFFFFFF; \
        (seg).selector = (sel);      \
        (seg).type     = 0xA;        \
        (seg).present  = 1;          \
        (seg).db       = 1;          \
        (seg).g        = 1;          \
        (seg).s        = 1;          \
    } while(0)

    #define SEG_DATA(seg, sel) do { \
        (seg).base     = 0;          \
        (seg).limit    = 0xFFFFFFFF; \
        (seg).selector = (sel);      \
        (seg).type     = 0x2;        \
        (seg).present  = 1;          \
        (seg).db       = 1;          \
        (seg).g        = 1;          \
        (seg).s        = 1;          \
    } while(0)

    SEG_CODE(sregs.cs, 0x8);
    SEG_DATA(sregs.ds, 0x10);
    SEG_DATA(sregs.es, 0x10);
    SEG_DATA(sregs.fs, 0x10);
    SEG_DATA(sregs.gs, 0x10);
    SEG_DATA(sregs.ss, 0x10);

    #undef SEG_CODE
    #undef SEG_DATA

    sregs.gdt.base  = GDT_ADDR;
    sregs.gdt.limit = 3 * sizeof(GdtEntry32) - 1;
    if (kvm_modes_do_set_sregs(Vcpu, &sregs) != KVM_OK) return KVM_ERRO;

    uint8_t* ram = (uint8_t*)Memory->mem;
    GdtEntry32* gdt = (GdtEntry32*)(ram + GDT_ADDR);
    memset(gdt, 0, 3 * sizeof(GdtEntry32));

    // entry 1 — code (selector 0x8):
    gdt[1].limit_low   = 0xFFFF;
    gdt[1].access      = 0x9A;
    gdt[1].granularity = 0xCF;
    // entry 2 — data (selector 0x10):
    gdt[2].limit_low   = 0xFFFF;
    gdt[2].access      = 0x92;
    gdt[2].granularity = 0xCF;
    return KVM_OK;
}


KvmStatus KvmX86SetRealMode(KvmVcpu Instance)
{
    if (!Instance) return KVM_ERRO;
    struct kvm_sregs sregs = {0};
    if (kvm_modes_do_get_sregs(Instance, &sregs) != KVM_OK) return KVM_ERRO;

    sregs.cs.base = 0;
    sregs.cs.selector = 0;

    sregs.ds.base = 0;
    sregs.ds.selector = 0;

    sregs.es.base = 0;
    sregs.es.selector = 0;

    sregs.ss.base = 0;
    sregs.ss.selector = 0;

    if (kvm_modes_do_set_sregs(Instance, &sregs) != KVM_OK) return KVM_ERRO;
    return KVM_OK;
}

typedef struct {
    uint16_t offset_low;   // bits 0-15 do handler
    uint16_t selector;     // 0x8 — code segment da GDT
    uint8_t  zero;         // sempre 0
    uint8_t  type;         // 0x8E — present, ring 0, interrupt gate 32 bits
    uint16_t offset_high;  // bits 16-31 do handler
} __attribute__((packed)) IdtEntry32;
typedef struct {
    uint16_t limit;  // tamanho da IDT - 1
    uint32_t base;   // endereço físico da IDT no guest
} __attribute__((packed)) Idtr32;

#define IDT_ADDR 0xF000
#define STUB_ADDR 0xF800

// Long mode (x86_64) — paginação identity 0..1GB com 2MB pages
#define PML4_ADDR 0x10000
#define PDPT_ADDR 0x11000
#define PD_ADDR   0x12000
#define PML4_SIZE 0x1000
#define PDPT_SIZE 0x1000
#define PD_SIZE   0x1000
// GDT 64-bit (mesmo base do 32-bit, entradas diferentes)
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) GdtEntry64;
KvmStatus KvmX86ConfigureIDT(KvmVcpu Vcpu, KvmMachineMemory Memory)
{
    if (!Vcpu || !Memory || !Memory->mem) return KVM_ERRO;
    if (Memory->size < (size_t)(STUB_ADDR + 3)) return KVM_ERRO;
    if (Memory->size < (size_t)(IDT_ADDR + 256 * sizeof(IdtEntry32))) return KVM_ERRO;

    struct kvm_sregs sregs = {0};
    if (kvm_modes_do_get_sregs(Vcpu, &sregs) != KVM_OK) return KVM_ERRO;
    sregs.idt.base = IDT_ADDR;
    sregs.idt.limit = 256 * sizeof(IdtEntry32)-1;
    if (kvm_modes_do_set_sregs(Vcpu, &sregs) != KVM_OK) return KVM_ERRO;

    uint8_t* ram = (uint8_t*)Memory->mem;
    // Handler para int 0x80: out 0x99, al + iret -> gera KVM_EXIT_IO (port 0x99) para host emular syscall
    // Usa porta 0x99 (não coalescida) para garantir exit ao userspace - porta 0x80 é ignorada em alguns kernels
    ram[STUB_ADDR + 0] = 0xE6; // out 0x99, al
    ram[STUB_ADDR + 1] = 0x99;
    ram[STUB_ADDR + 2] = 0xCF; // iret

    IdtEntry32* idt = (IdtEntry32*)(ram + IDT_ADDR);
    memset(idt, 0, 256 * sizeof(IdtEntry32));
    idt[0x80].offset_low  = STUB_ADDR & 0xFFFF;
    idt[0x80].offset_high = (STUB_ADDR >> 16) & 0xFFFF;
    idt[0x80].selector    = 0x8;
    idt[0x80].zero        = 0;
    idt[0x80].type        = 0x8E; // present, DPL=0, 32-bit interrupt gate (0x8F trap também serve)
    return KVM_OK;
}

KvmStatus KvmX86SetupLongMode(KvmVcpu Vcpu, KvmMachineMemory Memory)
{
    return KvmX86SetupLongModeWithPaging(Vcpu, Memory, PML4_ADDR);
}

KvmStatus KvmX86SetupLongModeWithPaging(KvmVcpu Vcpu, KvmMachineMemory Memory, uint64_t pml4_addr)
{
    if (!Vcpu || !Memory || !Memory->mem) return KVM_ERRO;
    // Precisa de espaço para PML4+PDPT+PD+GDT+IDT
    uint64_t need = PD_ADDR + PD_SIZE;
    if (need > Memory->size) return KVM_ERRO;
    if (pml4_addr + PML4_SIZE > Memory->size) return KVM_ERRO;
    if (Memory->size < (size_t)(GDT_ADDR + 3 * sizeof(GdtEntry64))) return KVM_ERRO;

    // 1) Zera e monta paginação identity 0..2MB (ou 1GB com 2M pages)
    uint8_t* ram = (uint8_t*)Memory->mem;
    uint64_t* pml4 = (uint64_t*)(ram + pml4_addr);
    uint64_t* pdpt = (uint64_t*)(ram + PDPT_ADDR);
    uint64_t* pd   = (uint64_t*)(ram + PD_ADDR);
    memset(pml4, 0, PML4_SIZE);
    memset(pdpt, 0, PDPT_SIZE);
    memset(pd,   0, PD_SIZE);

    // PML4[0] -> PDPT, PDPT[0] -> PD, PD[0..511] -> 2MB identity pages
    // Flags: P=1, RW=1, US=0
    pml4[0] = (PDPT_ADDR | 0x03ULL);
    pdpt[0] = (PD_ADDR   | 0x03ULL);
    // Mapeia primeiro 1GB com 2MB pages (512*2MB = 1GB) — suficiente para RAM de 1MB do exemplo
    // Se quiser só 2MB, basta 1 entrada; mapeamos 1GB para ser genérico
    for (int i = 0; i < 512; i++) {
        pd[i] = ((uint64_t)i * 0x200000ULL) | 0x83ULL; // P|RW|PS
    }

    // 2) Lê sregs via thread (igual ao protected mode) e ativa long mode
    struct kvm_sregs sregs = {0};
    if (kvm_modes_do_get_sregs(Vcpu, &sregs) != KVM_OK) return KVM_ERRO;

    // PAE + PG serão setados, mas ordem importa: PAE antes de LME
    sregs.cr4 |= (1ULL << 5); // CR4.PAE
    sregs.cr3 = pml4_addr;
    sregs.efer |= (1ULL << 8); // EFER.LME
    // sregs.efer |= (1ULL << 10); // EFER.LMA é setado pela CPU após CR0.PG, não precisa setar aqui

    sregs.cr0 |= 0x80000001ULL; // CR0.PE | CR0.PG (PE já vem do protected mode, garante)

    // GDT 64-bit: null, code64 (L=1), data64
    sregs.gdt.base  = GDT_ADDR;
    sregs.gdt.limit = 3 * sizeof(GdtEntry64) - 1;

    // Segmentos long mode: base 0, limit 0, type/flags específicos
    // Code64: P=1, DPL=0, S=1, Exec=1, L=1, G=1
    sregs.cs.base     = 0;
    sregs.cs.limit    = 0xFFFFFFFF;
    sregs.cs.selector = 0x08;
    sregs.cs.type     = 0xA;
    sregs.cs.present  = 1;
    sregs.cs.dpl      = 0;
    sregs.cs.db       = 0; // must be 0 for L=1
    sregs.cs.s        = 1;
    sregs.cs.l        = 1;
    sregs.cs.g        = 1;

    sregs.ds.base     = 0; sregs.ds.limit = 0xFFFFFFFF; sregs.ds.selector = 0x10; sregs.ds.type = 0x2; sregs.ds.present=1; sregs.ds.db=1; sregs.ds.s=1; sregs.ds.g=1;
    sregs.es.base     = 0; sregs.es.limit = 0xFFFFFFFF; sregs.es.selector = 0x10; sregs.es.type = 0x2; sregs.es.present=1; sregs.es.db=1; sregs.es.s=1; sregs.es.g=1;
    sregs.fs.base     = 0; sregs.fs.limit = 0xFFFFFFFF; sregs.fs.selector = 0x10; sregs.fs.type = 0x2; sregs.fs.present=1; sregs.fs.db=1; sregs.fs.s=1; sregs.fs.g=1;
    sregs.gs.base     = 0; sregs.gs.limit = 0xFFFFFFFF; sregs.gs.selector = 0x10; sregs.gs.type = 0x2; sregs.gs.present=1; sregs.gs.db=1; sregs.gs.s=1; sregs.gs.g=1;
    sregs.ss.base     = 0; sregs.ss.limit = 0xFFFFFFFF; sregs.ss.selector = 0x10; sregs.ss.type = 0x2; sregs.ss.present=1; sregs.ss.db=1; sregs.ss.s=1; sregs.ss.g=1;

    if (kvm_modes_do_set_sregs(Vcpu, &sregs) != KVM_OK) return KVM_ERRO;

    // 3) Escreve GDT 64-bit na RAM do guest
    GdtEntry64* gdt = (GdtEntry64*)(ram + GDT_ADDR);
    memset(gdt, 0, 3 * sizeof(GdtEntry64));
    // null já zero
    // code64: limit_low=0, base 0, access 0x9A, granularity 0xAF (G=1,L=1, limit high F)
    gdt[1].limit_low   = 0x0000;
    gdt[1].base_low    = 0x0000;
    gdt[1].base_mid    = 0x00;
    gdt[1].access      = 0x9A;
    gdt[1].granularity = 0xAF; // G=1, D=0, L=1, AVL=0 + limit 0xF
    gdt[1].base_high   = 0x00;
    // data64
    gdt[2].limit_low   = 0x0000;
    gdt[2].base_low    = 0x0000;
    gdt[2].base_mid    = 0x00;
    gdt[2].access      = 0x92;
    gdt[2].granularity = 0x00;
    gdt[2].base_high   = 0x00;

    return KVM_OK;
}