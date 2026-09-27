# KvmTool

Uma biblioteca fina, em C, sobre a API `/dev/kvm` do Linux, com uma camada de
alto nível (objetos opacos + handlers de sinal assíncronos) e helpers de
inicialização x86 (protected mode, long mode/paginação, GDT/IDT) para escrever
hipervisores de usuário sem depender de QEMU/KVM-tool.

O objetivo é simples: dar a você os quatro ioctls essenciais do KVM
(`/dev/kvm` → VM → VCPU → `KVM_RUN`) com ergonomia de biblioteca C, mais o
runner em thread que já resolve o problema de ioctls concorrentes na mesma
vCPU.

---

## Sumário

- [Estado atual](#estado-atual)
- [Requisitos](#requisitos)
- [Build](#build)
- [Execução](#execução)
- [Arquitetura](#arquitetura)
- [Modelo de objetos](#modelo-de-objetos)
- [API pública](#api-pública)
  - [Status](#status)
  - [Device](#device)
  - [Machine e memória](#machine-e-memória)
  - [Vcpu](#vcpu)
  - [Registers (ISA)](#registers-isa)
  - [CPUID](#cpuid)
  - [Capabilities](#capabilities)
  - [Helpers x86](#helpers-x86)
- [O runner assíncrono](#o-runner-assíncrono)
- [Exemplo: um `sys_write` em código de máquina](#exemplo-um-sys_write-em-código-de-máquina)
- [Layout de memória do guest](#layout-de-memória-do-guest)
- [Decisões de projeto](#decisões-de-projeto)
- [Limitações conhecidas](#limitações-conhecidas)
- [Roteiro de evolução](#roteiro-de-evolução)

---

## Estado atual

Funcional e verificado ponta a ponta (monkey test de uso + inspeção de
comportamento de sinais):

- criação de device / machine / vcpu, com *cleanup* em todos os caminhos de erro;
- mapeamento de memória via `mmap` anônimo + `KVM_SET_USER_MEMORY_REGION2`;
- setup de GDT, IDT e paginação identity;
- runner em thread dedicada, com despacho de tarefas para a thread da vCPU
  (usado por `KVM_GET/SET_REGS` e `KVM_GET/SET_SREGS`);
- CPUID com assinatura configurável (`GenuineIntel` / `AuthenticAMD` / custom)
  e bit de hypervisor;
- handlers de sinal para `HLT`, `IO`, `INTERNAL_ERROR` e `FAIL_ENTRY`;
- trace legível de erros internos do KVM.

Não implementado ainda: vCPUs múltiplas, MMIO, emulação de `sys_read`/portas de
E/S no host, `KVM_SET_TSS_ADDR`, timers, SMP.

## Requisitos

- Linux com `/dev/kvm` habilitado (KVM embutido ou módulo `kvm`/`kvm_intel`/`kvm_amd`);
- headers do kernel (`linux/kvm.h`) — em Arch/Fedora: `kernel-headers`; o código
  usa os headers de userspace do kernel, não `linux/` da árvore do kernel;
- GCC com suporte a C23 (o código usa `typedef enum : uint32_t` e
  `enum ... : uint32_t` em headers);
- `pthreads` (já é padrão no glibc moderno).

O binário precisa ser executado com permissão de leitura/escrita em `/dev/kvm`
(grupo `kvm` ou root).

## Build

```sh
make          # gera ./main
make clean    # remove ./main
```

O `Makefile` é propositalmente simples: um único comando `gcc` com todos os
`.c` da árvore. Flags atuais: `-g -O1 -Iinclude -Wall -Wextra -pthread`.

Se você usa Clang/Clangd, o `compile_commands.json` da raiz é gerado para
`clangd`; note que ele é local e está no `.gitignore`.

Se seu GCC for antigo e reclamar de enums com tipo definido, compilar com
`-std=c2x` (ou trocar o `typedef enum : uint32_t` por um typedef simples).

## Execução

```sh
./main
```

Saída esperada do exemplo embutido:

```
Version: 12
Syscall 4: Arg1: 1, Arg2: 1
A
```

O `A` vem do buffer em `0x1000` escrito pela syscall `sys_write` que o próprio
exemplo monta na RAM do guest.

## Arquitetura

```
                    userspace
  +--------------------------------------------------+
  |  include/KvmTool/        API pública (opaca)    |
  |    kvm.h  (agregador)                            |
  |    kvm_device.h  kvm_machine.h                    |
  |    kvm_vcpu.h    kvm_capability.h                 |
  |    x86/kvmx86_modes.h  x86/kvmx86_registre.h      |
  |                                                  |
  |  include/Internal/        structs reais           |
  |    Kvm_T  KvmMachine_T  KvmMachineMemory_T       |
  |    KvmVcpu_T  (runner, fila, tabela de sinais)   |
  +--------------------------------------------------+
  |  src/                                            |
  |    kvm_device.c     open("/dev/kvm"), API version |
  |    kvm_capability.c  KVM_CHECK_EXTENSION         |
  |    kvm_machine.c    CREATE_VM, mmap, SET_USER... |
  |    vcpu/kvm_vcpu.c  CREATE_VCPU, mmap kvm_run,    |
  |                    tabela de handlers, trace     |
  |    vcpu/kvm_thread.c  runner assíncrono +         |
  |                    dispatcher de tarefas + CPUID |
  |    x86/kvmx86_modes.c   GDT/IDT/paginação/long    |
  |    x86/kvmx86_registres.c  get/set regs + sregs   |
  |    main.c   exemplo executável                    |
  +--------------------------------------------------+
  |  linux/kvm.h  →  ioctl() sobre /dev/kvm          |
  +--------------------------------------------------+
```

A separação `KvmTool/` vs `Internal/` é deliberada: `KvmTool/` é o contrato
estável (só ponteiros opacos), `Internal/` expõe a struct real e só é incluído
dentro de `src/`. Isso permite trocar a representação interna sem quebrar
quem usa a biblioteca.

## Modelo de objetos

Quatro níveis, na ordem clássica do KVM, com hierarquia de posse explícita:

```
KvmDevice  ──fd──▶  /dev/kvm
   │
   └─ KvmMachine  ──fd──▶  KVM_CREATE_VCPU / KVM_SET_USER_MEMORY_REGION2
        │
        └─ KvmVcpu  ──mmap──▶  struct kvm_run  (região compartilhada)
```

- `KvmDevice` guarda o fd de `/dev/kvm` e pré-dados (`kvm_run_size`, obtido de
  `KVM_GET_VCPU_MMAP_SIZE`);
- `KvmMachine` guarda o fd da VM e a memória criada para ela;
- `KvmVcpu` guarda o fd da vCPU, o mapeamento de `kvm_run`, a fila de tarefas e a
  tabela de handlers de sinal;
- cada objeto tem um `KvmDestroy*` correspondente, e os `Create*` já liberam tudo
  o que alocaram em caso de falha (`goto _err` com flags de inicialização).

`KvmDestroy*` recebe `NULL` e retorna sem efeito, então é seguro encadear
destruições.

## API pública

### Status

```c
typedef enum { KVM_ERRO = 0, KVM_OK = 1 } KvmStatus;
```

Toda função que pode falhar devolve `KvmStatus`. As `KvmDestroy*` e
`KvmGetInternalErroTrace` são `void`.

### Device

```c
KvmStatus KvmCreateDeviceInstance(KvmDevice* out);
void      KvmDestroyDeviceInstance(KvmDevice dev);
int       KvmGetDeviceVersion(KvmDevice dev);
```

`KvmCreateDeviceInstance` abre `/dev/kvm` com `O_RDWR | O_CLOEXEC` e já registra
o tamanho de `kvm_run` para uso posterior. `KvmGetDeviceVersion` devolve o
resultado de `KVM_GET_API_VERSION` (12 na API atual).

### Machine e memória

```c
typedef struct {
    uint64_t                size;       // bytes
    uint64_t                phys_addr;  // GPA inicial
    KvmMemorySlotId         slot;
    KvmMachineMemoryFlags   flags;      // READONLY, LOG_DIRTY_PAGES, GUEST_MEMFD
} KvmMachineMemoryRequest;

KvmStatus KvmCreateMemory(const KvmMachineMemoryRequest* req, KvmMachineMemory* out);
void      KvmDestroyMemoryMachine(KvmMachineMemory mem);
KvmStatus KvmGetRawMemoryManipulator(KvmMachineMemory mem, void** ptr);
KvmStatus KvmRegisterMachineMemory(KvmMachine vm, KvmMachineMemory mem);
```

A memória é um `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` comum do processo. Você
escreve nela por um ponteiro normal, e o mesmo mapeamento é registrado como
região de usuário da VM — o host e o guest compartilham a mesma página, então
alterações feitas pelo host aparecem imediatamente para o guest (e vice-versa),
sem `KVM_SET_USER_MEMORY_REGION` a cada mudança.

A ordem importa: `KvmCreateMemory` → escreva na RAM → `KvmRegisterMachineMemory`
→ `KvmGetRawMemoryManipulator`. Não é preciso re-registrar após escribir.

### Vcpu

```c
KvmStatus KvmCreateVcpuInstance(KvmMachine vm, KvmVcpu* out);
void      KvmDestroyVcpuInstance(KvmVcpu vcpu);

KvmStatus KvmRegisterSignalHandler(KvmVcpu vcpu, KvmRunnerSignal sig,
                                   KvmSignalHandler handler, void* userdata);
KvmStatus KvmRunAsync(KvmVcpu vcpu);
KvmStatus KvmWaitSignal(KvmVcpu vcpu, KvmRunnerSignal* out);
KvmStatus KvmWaitSignalDispatch(KvmVcpu vcpu);
void      KvmStopVcpuRunner(KvmVcpu vcpu);

KvmStatus KvmGetInternalErroTrace(KvmVcpu vcpu);
KvmStatus KvmGetIOperation(KvmVcpu vcpu, KvmIOperation* op);
```

Ao criar a vCPU, a biblioteca faz `KVM_CREATE_VCPU` e o `mmap` de
`struct kvm_run` (usando o `kvm_run_size` que o device já registrou). É esse
mapeamento que o runner lê depois de cada `ioctl(KVM_RUN)` para saber o
`exit_reason`.

Sinais mapeados de `exit_reason` para a enum da biblioteca:

| `KvmRunnerSignal`        | `KVM_EXIT_*`            | Quando acontece                          |
|--------------------------|-------------------------|------------------------------------------|
| `KVM_RUNTIMER_SIG_IO`     | `KVM_EXIT_IO` (2)      | guest fez `in`/`out`                     |
| `KVM_RUNTIMER_SIG_HLT`    | `KVM_EXIT_HLT` (5)     | guest executou `hlt`                     |
| `KVM_RUNTIMER_SIG_MMIO`   | `KVM_EXIT_MMIO` (6)    | acesso a MMIO (não tratado ainda)       |
| `KVM_RUNTIMER_SIG_FAIL_ENTRY` | `KVM_EXIT_FAIL_ENTRY` (9) | falha ao entrar no modo guest      |
| `KVM_RUNTIMER_SIG_INTERNAL_ERRO` | `KVM_EXIT_INTERNAL_ERROR` (17) | erro interno do KVM      |

Até 16 handlers (`KVM_MAX_SIGNAL_HANDLERS`) por vCPU, registrados por
`KvmRegisterSignalHandler`; o primeiro handler cujo `signal` bate é chamado.
`KvmWaitSignalDispatch` é o loop de uso normal:

```c
KvmRunAsync(vcpu);
while (KvmWaitSignalDispatch(vcpu) == KVM_OK) { /* ... */ }
```

Ele bloqueia até o próximo sinal, despacha o handler, e devolve `KVM_ERRO` quando
o runner foi parado — daí o `while` no exemplo, que faz o programa sair
sozinho quando o guest dá `hlt`.

`KvmStopVcpuRunner` pode ser chamado de dentro de um handler: ele seta
`immediate_exit`, interrompe a thread com `SIGUSR1` e faz `pthread_join`.

### Registers (ISA)

A biblioteca não fixa uma arquitetura no núcleo. O vCPU carrega um
`KvmRegistreISAHandle` — três ponteiros de função (`get_regs`, `set_regs`,
`free_object`) — que a camada x86 preenche:

```c
KvmStatus KvmAnnounceIsa(KvmVcpu vcpu, KvmRegistreISAHandle* handle);
KvmStatus KvmGetRegistersStates(KvmVcpu vcpu, KvmRegistresIsa* out);
KvmStatus KvmSetRegisterStates(KvmVcpu vcpu, KvmRegistresIsa regs);
void      KvmDestroyRegisterIsa(KvmVcpu vcpu, KvmRegistresIsa regs);
```

O objeto devolvido é opaco, então o lado x86 usa um header de tipo para
distinguir `kvm_regs` de `kvm_sregs` e oferecer helpers que verificam antes de
dar cast:

```c
KvmRegistreISAHandle* KvmX86RegistreIsaSuport();
KvmX86RegistreState*  KvmX86ExtractRegistreState(KvmRegistresIsa isa);
KvmX86SRegsState*     KvmX86ExtractSRegsState(KvmRegistresIsa isa);
```

`KvmX86RegistreState` cobre `rax..r15`, `rip` e `rflags`;
`KvmX86SRegsState` cobre os segmentos (`cs/ds/es/fs/gs/ss/tr/ldt`), as tabelas
`gdt`/`idt` e os controles `cr0..cr4`, `cr8`, `efer`, `apic_base` e o bitmap de
interrupções. Um `_Static_assert` garante que ambas as structs continuam com o
mesmo tamanho de `kvm_regs`/`kvm_sregs` — se o kernel mudar, o build quebra em
vez de corromper memória.

Uso típico:

```c
KvmX86RegistreState r = { .rip = 0, .rflags = 0x2, .rsp = 0x7000 };
KvmRegistresIsa isa = KvmX86TransformRegistreIsa(&r);
KvmSetRegisterStates(vcpu, isa);
KvmDestroyRegisterIsa(vcpu, isa);
```

### CPUID

```c
KvmStatus KvmSetupVcpuid(KvmVcpu vcpu, const KvmCpuidConfig* cfg);

typedef struct {
    KvmSignatureType type_prefix;  // KVM_SIGNATURE_INTEL / AMD / CUSTOM
    char             name[12];
    bool             hypervisor_bit;
} KvmCpuidConfig;
```

Lê o `CPUID` atual (ou o suportado pelo host, via `KVM_GET_SUPPORTED_CPUID`),
reescreve a assinatura no leaf 0 e ajusta o bit 31 do leaf 1 conforme
`hypervisor_bit`, e o que `40000000` para não vazar identificadores de
hypervisor. Deve ser chamada **antes** de `KvmRunAsync` — com a vCPU rodando a
função devolve `KVM_ERRO`, porque `KVM_SET_CPUID2` não pode ser concorrente com
`KVM_RUN`.

### Capabilities

```c
KvmStatus KvmCheckCapability(KvmDevice dev, KvmCapabilitySuport cap, int* value);
```

Wrapper sobre `KVM_CHECK_EXTENSION`, com os caps que interessam a este projeto
já nomeados: `HLT`, `USER_MEMORY`, `MEMSLOTS`, `MAX_VCPUS`, `IMMEDIATE_EXIT`.
`value` pode ser `NULL` se você só quer saber se suporta.

### Helpers x86

```c
KvmStatus KvmX86SetRealMode(KvmVcpu vcpu);
KvmStatus KvmX86SetupProtectedMode(KvmVcpu vcpu, KvmMachineMemory mem);
KvmStatus KvmX86SetupLongMode(KvmVcpu vcpu, KvmMachineMemory mem);
KvmStatus KvmX86SetupLongModeWithPaging(KvmVcpu vcpu, KvmMachineMemory mem,
                                        uint64_t pml4_addr);
KvmStatus KvmX86ConfigureGDT(KvmVcpu vcpu, KvmMachineMemory mem);
KvmStatus KvmX86ConfigureIDT(KvmVcpu vcpu, KvmMachineMemory mem);
```

`SetupProtectedMode` liga `CR0.PE`, monta uma GDT flat de 3 entradas (null,
código `0x9A`, dados `0x92`) e popula `cs/ds/es/fs/gs/ss` com base 0 e limite
`0xFFFFFFFF`. `SetupLongMode` liga `CR4.PAE`, `CR3`, `EFER.LME` e `CR0.PG`, e
reescreve os segmentos para `cs` com `L=1`/`DB=0`. `ConfigureIDT` instala 256
entradas zeradas e um único handler em `int 0x80`.

Os helpers de modo consultam e escrevem `sregs` pela fila de tarefas, então
funcionam com a vCPU parada ou rodando.

## O runner assíncrono

O ponto mais delicado do projeto: **ioctl em KVM não é thread-safe por vCPU**.
Se o host chama `KVM_GET_REGS` enquanto a thread do runner está em
`ioctl(KVM_RUN)`, o comportamento é indefinido.

A solução aqui é uma thread dedicada por vCPU que é a **única** a falar com o
fd, e um despachante para o resto do código:

- `KvmRunAsync` cria a thread e espera (via semáforo) ela declarar-se *running*;
- a thread executa tarefas enfileiradas antes do `RUN` (é assim que
  `KvmSetRegisterStates` funciona antes de `KvmRunAsync`), entra no laço de
  `ioctl(KVM_RUN)` e, a cada retorno, publica o `exit_reason` e **espera o host
  consumir o sinal** antes do próximo `RUN` — é isso que impede um
  `IO → HLT` de ser engolido;
- `KvmVcpuDispatchTask` enfileira uma tarefa e, se a vCPU está rodando, seta
  `run->immediate_exit` e manda `SIGUSR1` para a thread. O `ioctl` volta com
  `EINTR`, a thread executa as tarefas FIFO e volta ao laço. Se não há tarefa
  pendente, é um shutdown e a thread sai.

Os sinais são entregues com `mutex` + `condvar`, com handshake nos dois lados
para que nenhum evento se perca. Como consequência, o host e o guest rodam em
exclusão mútua: o guest não avança enquanto o host está tratando um sinal. É o
trade-off correto para um tool de estudo.

## Exemplo: um `sys_write` em código de máquina

O `main.c` monta à mão um programa guest mínimo que faz `sys_write(1, "A", 1)`
e depois `hlt`:

```asm
mov eax, 4      ; B8 04 00 00 00   sys_write
mov ebx, 1      ; BB 01 00 00 00   fd = stdout
mov ecx, 0x1000 ; B9 00 10 00 00   buffer
mov edx, 1      ; BA 01 00 00 00   tamanho
int 0x80        ; CD 80
hlt             ; F4
```

O `'A'` é escrito em `ram[0x1000]` antes de o código ser montado. O handler de
`int 0x80` é um stub em `0xF800` que faz `out 0x99, al` seguido de `iret` — a
porta `0x99` é escolhida de propósito: o host não a coalesce, então o KVM sempre
gera `KVM_EXIT_IO` e o host consegue ver os argumentos da chamada.

O handler de `SIG_IO` no host lê `rax/rbx/rcx` e imprime a "syscall", o que
demonstra os dois caminhos critiques ao mesmo tempo: interceptar E/S de porta e
ler registros enquanto a vCPU está rodando (via fila de tarefas).

## Layout de memória do guest

Endereços físicos usados pelos helpers — todos dentro do slot 0 de 1 MiB do
exemplo:

| Endereço    | Conteúdo                                        |
|-------------|-------------------------------------------------|
| `0x0000`    | código do guest (início em `rip = 0`)            |
| `0x1000`    | buffer de dados do guest                         |
| `0x7000`    | pilha (`rsp`)                                    |
| `0xE000`    | GDT (3 entradas de 8 bytes)                      |
| `0xF000`    | IDT (256 entradas × 8 bytes)                     |
| `0xF800`    | stub do handler de `int 0x80`                    |
| `0x10000`   | PML4                                            |
| `0x11000`   | PDPT                                           |
| `0x12000`   | Page Directory (512 entradas de 2 MiB)           |

A paginação é identity: 512 entradas de 2 MiB mapeiam os primeiros 1 GiB com
`P|RW|PS`, o que cobre com folga a RAM do exemplo. Para usar menos memória,
passe o seu próprio endereço de PML4 para `KvmX86SetupLongModeWithPaging` e
espaça as páginas de tabela de acordo com a sua convenção.

## Decisões de projeto

- **C23 de verdade.** Enums com tipo (`enum : uint32_t`) e `bool` explícito; é C moderno, não C com exeções.
- **Handles opacos com typedef de ponteiro.** `KvmDevice` é
  `struct Kvm_T*`, então o compilador impede o usuário de fazer marshal por acidente e
  o `Internal/` pode mudar sem quebrar a API.
- **Status em vez de errno.** Consistente com o estilo de `KVM_OK`/`KVM_ERRO`
  semânticos do próprio kernel.
- **Abstração de ISA, não de x86.** O core não conhece `kvm_regs`; quem sabe é
  `src/x86/`. Trocar ou acrescentar uma arquitetura é registrar outro
  `KvmRegistreISAHandle`.
- **Runner em thread desde o começo.** Modelos baseados em "chame `KVM_RUN` e faça
  polling" quebram assim que o host quiser ler registros. Fazer certo
  primeiro evitou uma reescrita.
- **Porta `0x99` para o trap de syscall.** Em vez de `0x80`, para garantir que o
  `KVM_EXIT_IO` não seja coalescido e o argumento chegue ao host.
- **Wrappers próprios de struct de segmento/página.** `GdtEntry32`/`GdtEntry64`/
  `IdtEntry32` são declarados no projeto, em vez de usar `<asm/segment.h>`, para
  manter a árvore de headers mínima.

## Limitações conhecidas

- **vCPU única por máquina.** A biblioteca já tem a abstração certa, mas
  `main.c` cria uma; nada impede várias, mas não há便利 estática para o mapa
  vCPU→thread (um `KvmVcpu` carrega sua própria thread, então é direto).
- **`KvmSetupVcpuid` falha se a vCPU já está rodando.** Deliberado, mas vale
  registrar: configure CPUID antes de `KvmRunAsync`.
- **`KVM_EXIT_MMIO` está declarado mas não tratado.** O sinal chega; não há
  handler de emulação.
- **Sem emulação de entrada/saída no host.** O exemplo só *observa* as
  operações de porta; falta implementar o lado de escrita (`in`/`out` de fato
  alterando estado),o que exige um dispositivo emulado.
- **Sem `KVM_SET_TSS_ADDR`, sem `TSS`, sem_swaparea.** O guest não faz troca de
  contexto.
- **Sem tratamento de `KVM_EXIT_DEBUG`, `SHUTDOWN`, `SYSTEM_EVENT`.** Sinais
  desconhecidos simplesmente não têm handler e o `KvmWaitSignalDispatch` segue
  para o próximo ciclo.
- **`KvmDestroyVcpuInstance` exige que o runner já tenha parado** via
  `KvmStopVcpuRunner`; caso contrário, a thread em `KVM_RUN` continua viva
  enquanto a memória é liberada.
- **Detecção de `SIGUSR1` com `SIG_IGN`.** O `pthread_kill` gera `EINTR` de
  forma confiável, mas o handler vazio é um truque conhecido por ser frágil se
  o programa host instalar tratadores próprios de `SIGUSR1`.

## Roteiro de evolução

1. Emulação de `sys_read`/`sys_write` de verdade no host, com escrita real na
   porta (usando o `KvmIOperation` exposto).
2. Várias vCPUs por máquina, com `KVM_RUN` concorrente (cada vCPU já tem sua
   própria thread; falta um escalonador e o suporte a SMP/interrupts de IPI).
3. `KVM_SET_TSS_ADDR` + swap de páginas, para um guest com pilha por thread.
4. `KVM_EXIT_MMIO`: handler genérico de memória de dispositivo, que é o que
   falta para devices virtuais (virtio).
5. Relógio (`KVM_SET_TICKS_PER_SECOND` + timer) para o guest não poder fazer
   busy-loop infinito.
6. Testes automatizados: um harness que sobe a vCPU, injeta bytes e compara
   com o esperado, mais `ASAN`/`UBSAN` no CI.
7. Isolar `x86/` atrás de um registry de ISA para que o core não dependa de
   nenhuma arquitetura.

## Convenções

- Prefixos `Kvm`/`KVM` na API pública, `kvm_` nos helpers internos, `I` para
  helpers estáticos internos (ex.: `IKvmDispatchTaskForQueue`).
- Sufixos de arquivo por área: `kvm_<area>.<c>`; helpers x86 em `kvmx86_<area>.<c>`.
- Comentários explicam **por que**, não **o que**: cada decisão não óbvia
  (porta `0x99`, `SIG_IGN`, ordem dos bits de EFER, handshake dos sinais) tem um
  comentário explicando a razão.
- Erros são logados com `perror`/`fprintf` no ponto do erro; a camada pública
  só devolve `KVM_ERRO`.

## Licença

Projeto de estudo, sem licença declarada explicitamente. Adicione uma se for
compartilhar.
