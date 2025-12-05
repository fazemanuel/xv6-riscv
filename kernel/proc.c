/*
 * kernel/proc.c - Gestión de Procesos y Planificador de CPU
 * 
 * FUNCIONES PRINCIPALES:
 * - Creación/destrucción de procesos (allocproc, freeproc, fork, exit)
 * - Planificador Round-Robin (scheduler)
 * - Context switching (sched, yield)
 * - Sincronización (sleep, wakeup)
 */

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// Arreglo global de todas las CPUs
struct cpu cpus[NCPU];

// Tabla de procesos (máximo 64 procesos)
struct proc proc[NPROC];

// Puntero al proceso init (primer proceso de usuario)
struct proc *initproc;

// Contador para asignar PIDs únicos
int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// Lock para sincronización entre padres e hijos en wait/exit
struct spinlock wait_lock;

// proc_mapstacks() - Mapea las pilas del kernel para cada proceso
// Cada proceso necesita su propia pila cuando ejecuta en modo kernel
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();  // Asignar página física de 4KB
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));  // Dirección virtual del stack
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// procinit() - Inicializa la tabla de procesos al arrancar
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;  // Todos los slots inicialmente libres
      p->kstack = KSTACK((int) (p - proc));
  }
}

// cpuid() - Retorna el ID de la CPU actual
// PRECONDICIÓN: interrupciones deshabilitadas
int
cpuid()
{
  int id = r_tp();
  return id;
}

// mycpu() - Retorna el descriptor de la CPU actual
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// myproc() - Retorna el proceso ejecutando en esta CPU
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

// allocpid() - Asigna un PID único
int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// allocproc() - Busca un slot UNUSED y prepara un nuevo proceso
// RETORNA: puntero al proceso con p->lock held, o NULL si falla
static struct proc*
allocproc(void)
{
  struct proc *p;

  // Buscar slot libre
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Alocar trapframe (guarda registros de usuario)
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Crear tabla de páginas vacía
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Configurar contexto inicial para que empiece en forkret()
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// freeproc() - Libera todos los recursos de un proceso
// PRECONDICIÓN: p->lock held
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// proc_pagetable() - Crea tabla de páginas con trampoline y trapframe
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // Mapear trampoline (código para transiciones user↔kernel)
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // Mapear trapframe (página para guardar registros)
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// proc_freepagetable() - Libera tabla de páginas de usuario
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// userinit() - Crea el primer proceso de usuario (init)
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");
  p->state = RUNNABLE;

  release(&p->lock);
}

// growproc() - Aumenta o reduce memoria de usuario
// n > 0: crecer | n < 0: reducir
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// kfork() - Crea proceso hijo (copia del padre)
// RETORNA: PID del hijo en el padre, 0 en el hijo
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Alocar nuevo proceso
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copiar memoria de usuario del padre al hijo
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // Copiar registros guardados
  *(np->trapframe) = *(p->trapframe);

  // Hacer que fork() retorne 0 en el hijo
  np->trapframe->a0 = 0;

  // Duplicar file descriptors
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  // Establecer relación padre-hijo
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  // Marcar como ejecutable
  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// reparent() - Reasigna hijos huérfanos a init
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}
// kexit() - Termina el proceso actual (NUNCA RETORNA)
// El proceso queda en estado ZOMBIE hasta que el padre llame wait()
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Cerrar todos los archivos abiertos
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Reasignar hijos a init
  reparent(p);

  // Despertar al padre que podría estar en wait()
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;  // Marcar como zombie

  release(&wait_lock);

  // Saltar al scheduler y nunca retornar
  sched();
  panic("zombie exit");
}

// kwait() - Espera a que un hijo termine
// RETORNA: PID del hijo, o -1 si no hay hijos
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Buscar hijos
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Hijo terminado encontrado
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No hay hijos
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Dormir esperando que un hijo termine
    sleep(p, &wait_lock);
  }
}

// ============================================================================
// SCHEDULER - PLANIFICADOR ROUND-ROBIN
// ============================================================================

// scheduler() - Función principal del planificador
// 
// ALGORITMO: Round-Robin simple
// - Recorre el arreglo proc[] buscando procesos RUNNABLE
// - Ejecuta cada uno por un quantum de tiempo
// - Si no hay procesos, ejecuta WFI para ahorrar energía
//
// CADA CPU ejecuta su propio scheduler() en loop infinito
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // Habilitar interrupciones brevemente para evitar deadlock
    // (dispositivos I/O necesitan despertar procesos)
    intr_on();
    intr_off();

    int found = 0;
    
    // Buscar proceso RUNNABLE (búsqueda O(n))
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      
      if(p->state == RUNNABLE) {
        // ¡Proceso ejecutable encontrado!
        
        // Cambiar estado: RUNNABLE → RUNNING
        p->state = RUNNING;
        c->proc = p;
        
        // CONTEXT SWITCH: saltar al proceso
        // swtch() guarda contexto del scheduler y restaura el del proceso
        // NO retorna inmediatamente, retorna cuando el proceso ceda la CPU
        swtch(&c->context, &p->context);

        // El proceso terminó su quantum o se bloqueó
        // Ya cambió su estado antes de volver aquí
        c->proc = 0;
        found = 1;
      }
      
      release(&p->lock);
    }
    
    // Si no hay procesos RUNNABLE, dormir CPU con WFI
    // (Wait For Interrupt - ahorra energía)
    if(found == 0) {
      asm volatile("wfi");
    }
  }
}

// ============================================================================
// FUNCIONES DE CONTEXT SWITCHING
// ============================================================================

// sched() - Cede control al scheduler
//
// PRECONDICIONES:
// - Debe tener p->lock held
// - Solo UN lock held (noff == 1)
// - Estado YA debe haber cambiado (no RUNNING)
// - Interrupciones deshabilitadas
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  // Verificaciones de seguridad
  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  // Preservar estado de interrupciones
  intena = mycpu()->intena;
  
  // CONTEXT SWITCH al scheduler
  swtch(&p->context, &mycpu()->context);
  
  // Cuando retornemos (nos re-scheduleen), restaurar intena
  mycpu()->intena = intena;
}

// yield() - Cede voluntariamente la CPU
//
// Usado por:
// - Timer interrupt (quantum expirado)
// - Procesos cooperativos
// - Spinlocks (mientras espera)
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;  // Cambiar estado: RUNNING → RUNNABLE
  sched();              // Ir al scheduler
  release(&p->lock);
}

// forkret() - Primera ejecución de un proceso hijo después de fork
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  release(&p->lock);

  if (first) {
    // El primer proceso inicializa el filesystem
    fsinit(ROOTDEV);

    first = 0;
    __sync_synchronize();

    // Ejecutar /init
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // Retornar a user space
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// ============================================================================
// SINCRONIZACIÓN: SLEEP/WAKEUP
// ============================================================================

// sleep() - Bloquea el proceso esperando un evento
//
// PARÁMETROS:
// - chan: "canal" que identifica el evento (cualquier puntero)
// - lk: lock de condición que debe liberarse al dormir
//
// PROTOCOLO:
// 1. Caller adquiere lk
// 2. Verifica condición
// 3. Si no se cumple, llama sleep(chan, lk)
// 4. sleep() libera lk y duerme
// 5. Cuando wakeup(chan) se llama, sleep() retorna
// 6. sleep() re-adquiere lk antes de retornar
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Adquirir p->lock antes de liberar lk
  // Esto previene perder un wakeup
  acquire(&p->lock);
  release(lk);

  // Dormir en el canal
  p->chan = chan;
  p->state = SLEEPING;

  sched();  // Ir al scheduler

  // Cuando despertemos, limpiar
  p->chan = 0;

  // Re-adquirir locks
  release(&p->lock);
  acquire(lk);
}

// wakeup() - Despierta TODOS los procesos durmiendo en chan
//
// PARÁMETRO:
// - chan: canal donde están durmiendo los procesos
//
// Busca en toda la tabla de procesos y cambia estado:
// SLEEPING (con p->chan == chan) → RUNNABLE
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;  // Despertar proceso
      }
      release(&p->lock);
    }
  }
}

// ============================================================================
// OTRAS FUNCIONES
// ============================================================================

// kkill() - Mata un proceso por PID
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        p->state = RUNNABLE;  // Despertar para que vea la señal
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// either_copyout() - Copia a dirección de usuario o kernel
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// either_copyin() - Copia desde dirección de usuario o kernel
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// procdump() - Imprime lista de procesos (debugging)
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}