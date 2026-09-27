#ifndef KVM_X86_REGISTRE_SETS_H
#define KVM_X86_REGISTRE_SETS_H
#include <stdint.h>
#include <KvmTool/kvm_vcpu.h>

typedef struct {
    uint64_t rax, rbx, rcx, rdx;
	uint64_t rsi, rdi, rsp, rbp;
	uint64_t r8,  r9,  r10, r11;
	uint64_t r12, r13, r14, r15;
	uint64_t rip, rflags;
} KvmX86RegistreState;

typedef struct {
    uint64_t base;
    uint32_t limit;
    uint16_t selector;
    uint8_t  type;
    uint8_t  present  : 1;
    uint8_t  dpl      : 2;
    uint8_t  db       : 1;
    uint8_t  s        : 1;
    uint8_t  l        : 1;
    uint8_t  g        : 1;
    uint8_t  avl      : 1;
    uint8_t  unusable : 1;
} KvmX86Segment;
typedef struct {
    uint64_t base;
    uint16_t limit;
} KvmX86DTable;
typedef struct {
    KvmX86Segment  cs, ds, es, fs, gs, ss;
    KvmX86Segment  tr, ldt;
    KvmX86DTable   gdt, idt;
    uint64_t cr0, cr2, cr3, cr4, cr8;
    uint64_t efer;
    uint64_t apic_base;
    uint64_t interrupt_bitmap[4];
} KvmX86SRegsState;

KvmRegistreISAHandle* KvmX86RegistreIsaSuport();

KvmRegistresIsa KvmX86TransformRegistreIsa(KvmX86RegistreState* state);
KvmRegistresIsa KvmX86TransformRegistreSregIsa(KvmX86SRegsState* state);

// Helper para extrair estado do objeto opaco (evita cast direto que ignora header `type`)
typedef enum {
    KVM_X86_REGS_T,
    KVM_X86_SREGS_T
} KvmRegistreX86Types;
typedef struct {
    KvmRegistreX86Types type;
    union {
        KvmX86RegistreState registre;
        KvmX86SRegsState sreg;
    } reg;
} KvmThreadTransportReg;

static inline KvmX86RegistreState* KvmX86ExtractRegistreState(KvmRegistresIsa isa) {
    if (!isa) return NULL;
    KvmThreadTransportReg* t = (KvmThreadTransportReg*)isa;
    if (t->type != KVM_X86_REGS_T) return NULL;
    return &t->reg.registre;
}
static inline KvmX86SRegsState* KvmX86ExtractSRegsState(KvmRegistresIsa isa) {
    if (!isa) return NULL;
    KvmThreadTransportReg* t = (KvmThreadTransportReg*)isa;
    if (t->type != KVM_X86_SREGS_T) return NULL;
    return &t->reg.sreg;
}
#endif