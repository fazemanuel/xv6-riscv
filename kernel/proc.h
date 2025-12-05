// kernel/proc.h - Estructuras de datos para gestión de procesos
//
// Este archivo define las estructuras fundamentales para:
// 1. Representar procesos (struct proc)
// 2. Representar CPUs (struct cpu)
// 3. Contexto de ejecución (struct context)

// ============================================================================
// CONTEXTO DE EJECUCIÓN
// ============================================================================

// struct context - Registros guardados para context switching
//
// Cuando un proceso cede la CPU (via sched()), estos son los registros
// que se guardan/restauran para cambiar entre contextos.
//
// RISC-V CALLING CONVENTION:
// - Registros "callee-saved" (s0-s11): la función llamada debe preservarlos
// - Registros "caller-saved" (t0-t6, a0-a7): el caller debe guardarlos si los necesita
//
// Solo guardamos callee-saved porque el compilador ya guardó los caller-saved
// antes de llamar a swtch()
struct context {
  uint64 ra;  // Return address - dirección de retorno
  uint64 sp;  // Stack pointer - puntero a la pila
  
  // Callee-saved registers (registros que deben preservarse)
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// ============================================================================
// ESTADOS DE UN PROCESO
// ============================================================================

// Los procesos transicionan entre estos estados según el diagrama:
//
//   UNUSED → USED → RUNNABLE ⇄ RUNNING → ZOMBIE → UNUSED
//                        ↕
//                    SLEEPING
//
// UNUSED:   Slot libre en la tabla de procesos
// USED:     Proceso siendo creado (transitorio)
// RUNNABLE: Proceso listo para ejecutar (en cola del scheduler)
// RUNNING:  Proceso ejecutando actualmente en una CPU
// SLEEPING: Proceso bloqueado esperando un evento (I/O, lock, etc.)
// ZOMBIE:   Proceso terminado, esperando que el padre haga wait()

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// ============================================================================
// ESTRUCTURA DE UN PROCESO
// ============================================================================

// struct proc - Descriptor de proceso (PCB - Process Control Block)
//
// Cada proceso tiene una entrada en el arreglo global proc[NPROC].
// Esta estructura contiene TODA la información necesaria para gestionar
// el proceso: estado, memoria, archivos abiertos, contexto, etc.
struct proc {
  // ──────────────────────────────────────────────────────────────────
  // SINCRONIZACIÓN
  // ──────────────────────────────────────────────────────────────────
  struct spinlock lock;  // Lock para proteger campos de este proceso
                         // DEBE adquirirse antes de leer/modificar estado

  // ──────────────────────────────────────────────────────────────────
  // INFORMACIÓN BÁSICA
  // ──────────────────────────────────────────────────────────────────
  enum procstate state;  // Estado actual del proceso (RUNNABLE, RUNNING, etc.)
  void *chan;            // Canal en el que duerme (si state == SLEEPING)
                         // Usado por sleep/wakeup para sincronización
  int killed;            // Flag: proceso marcado para terminar
  int xstate;            // Exit status (guardado cuando el proceso termina)
  int pid;               // Process ID único

  // ──────────────────────────────────────────────────────────────────
  // RELACIONES ENTRE PROCESOS
  // ──────────────────────────────────────────────────────────────────
  struct proc *parent;   // Puntero al proceso padre
                         // Necesario para wait() y manejo de huérfanos

  // ──────────────────────────────────────────────────────────────────
  // MEMORIA
  // ──────────────────────────────────────────────────────────────────
  uint64 kstack;         // Dirección virtual del kernel stack
                         // Cada proceso tiene su propia pila del kernel (4KB)
  uint64 sz;             // Tamaño de la memoria del proceso (bytes)
  pagetable_t pagetable; // Tabla de páginas de usuario (MMU)
                         // Mapea direcciones virtuales → físicas
  struct trapframe *trapframe;  // Página que guarda registros de usuario
                                // Se usa al entrar/salir del kernel

  // ──────────────────────────────────────────────────────────────────
  // CONTEXTO DE EJECUCIÓN (para context switching)
  // ──────────────────────────────────────────────────────────────────
  struct context context;  // Registros guardados del proceso
                           // swtch() guarda/restaura esto al cambiar procesos
                           // Contiene ra, sp, s0-s11

  // ──────────────────────────────────────────────────────────────────
  // ARCHIVOS Y FILESYSTEM
  // ──────────────────────────────────────────────────────────────────
  struct file *ofile[NOFILE];  // Archivos abiertos (file descriptors)
                               // ofile[0] = stdin, ofile[1] = stdout, etc.
  struct inode *cwd;           // Directorio de trabajo actual (current working directory)

  // ──────────────────────────────────────────────────────────────────
  // INFORMACIÓN DE DEBUGGING
  // ──────────────────────────────────────────────────────────────────
  char name[16];         // Nombre del proceso (para debugging)
};

// ============================================================================
// ESTRUCTURA DE UNA CPU
// ============================================================================

// struct cpu - Descriptor de una CPU física
//
// Cada CPU del sistema tiene su propia entrada en cpus[NCPU].
// Almacena información específica de esa CPU: qué proceso ejecuta,
// su contexto de scheduler, estado de interrupciones, etc.
struct cpu {
  // ──────────────────────────────────────────────────────────────────
  // PROCESO ACTUAL
  // ──────────────────────────────────────────────────────────────────
  struct proc *proc;     // Proceso ejecutando en esta CPU (o NULL)
                         // NULL = CPU ejecutando scheduler
                         // !NULL = CPU ejecutando ese proceso

  // ──────────────────────────────────────────────────────────────────
  // CONTEXTO DEL SCHEDULER
  // ──────────────────────────────────────────────────────────────────
  struct context context;  // Contexto del scheduler de esta CPU
                           // Cuando un proceso llama sched(), se guarda
                           // su contexto y se restaura este para volver
                           // al loop del scheduler()

  // ──────────────────────────────────────────────────────────────────
  // GESTIÓN DE INTERRUPCIONES Y LOCKS
  // ──────────────────────────────────────────────────────────────────
  int noff;              // Profundidad de push_off() anidados
                         // Contador de cuántos locks tiene esta CPU
                         // noff > 0 → interrupciones deshabilitadas
  
  int intena;            // ¿Estaban las interrupciones habilitadas antes
                         // del primer push_off()?
                         // Se preserva al hacer context switch
};

// ============================================================================
// VARIABLES GLOBALES EXTERNAS
// ============================================================================

extern struct cpu cpus[NCPU];      // Arreglo de todas las CPUs
extern struct proc proc[NPROC];    // Tabla de todos los procesos

// ============================================================================
// NOTAS SOBRE EL ALGORITMO DE PLANIFICACIÓN
// ============================================================================

/*
 * ALGORITMO: Round-Robin Simple
 * ═════════════════════════════════════════════════════════════════════
 * 
 * ESTRUCTURAS CLAVE:
 * 
 * 1. proc[NPROC] - Tabla global de procesos
 *    - Arreglo estático de 64 procesos máximo
 *    - El scheduler recorre este arreglo buscando RUNNABLE
 *    - NO hay cola separada de procesos ejecutables
 * 
 * 2. proc->state - Estado del proceso
 *    - UNUSED: slot libre
 *    - RUNNABLE: listo para ejecutar (elegible por scheduler)
 *    - RUNNING: ejecutando actualmente en una CPU
 *    - SLEEPING: bloqueado esperando evento
 *    - ZOMBIE: terminado pero no recogido por el padre
 * 
 * 3. proc->context - Contexto guardado
 *    - Registros ra, sp, s0-s11
 *    - Se guarda cuando el proceso cede la CPU (sched)
 *    - Se restaura cuando el scheduler lo vuelve a ejecutar
 * 
 * 4. cpu->context - Contexto del scheduler
 *    - Cada CPU tiene su propio scheduler context
 *    - Cuando scheduler ejecuta swtch(), guarda aquí su estado
 * 
 * 5. cpu->proc - Proceso actual de la CPU
 *    - NULL cuando la CPU ejecuta scheduler()
 *    - !NULL cuando ejecuta un proceso
 * 
 * FLUJO DE SCHEDULING:
 * ═════════════════════════════════════════════════════════════════════
 * 
 * 1. scheduler() busca proceso RUNNABLE
 *    - Recorre proc[] desde el inicio (O(n))
 *    - Encuentra p con p->state == RUNNABLE
 * 
 * 2. scheduler() selecciona el proceso
 *    - p->state = RUNNING
 *    - c->proc = p
 *    - swtch(&c->context, &p->context)
 * 
 * 3. Proceso ejecuta...
 *    - Timer interrupt después de ~10ms
 *    - usertrap() → yield() → sched()
 * 
 * 4. sched() devuelve control al scheduler
 *    - p->state = RUNNABLE
 *    - swtch(&p->context, &c->context)
 * 
 * 5. scheduler() retorna de swtch()
 *    - c->proc = 0
 *    - Continúa buscando siguiente proceso
 * 
 * CAMPOS IMPORTANTES PARA EL SCHEDULER:
 * ═════════════════════════════════════════════════════════════════════
 * 
 * - proc->state: determina si es elegible
 * - proc->context: para guardar/restaurar estado
 * - proc->lock: sincronización al cambiar estado
 * - cpu->proc: qué proceso ejecuta esta CPU
 * - cpu->context: estado del scheduler
 * 
 * LIMITACIONES DEL DISEÑO ACTUAL:
 * ═════════════════════════════════════════════════════════════════════
 * 
 * ✗ Búsqueda O(n) ineficiente
 * ✗ No hay prioridades
 * ✗ Quantum fijo
 * ✗ No hay colas por prioridad
 * ✗ No distingue procesos I/O-bound vs CPU-bound
 * 
 * POSIBLES MEJORAS (para tu proyecto):
 * ═════════════════════════════════════════════════════════════════════
 * 
 * 1. Agregar campo "priority" a struct proc
 * 2. Agregar campo "vruntime" para CFS
 * 3. Agregar campo "quantum_remaining" para quantum variable
 * 4. Crear lista enlazada de RUNNABLE (eliminar búsqueda O(n))
 * 5. Agregar estadísticas (tiempo CPU, tiempo espera, cambios de contexto)
 */