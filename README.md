# Ahura Kernel

Not ready to use. 
A small preemptive RTOS kernel for ARM Cortex-M.

## Layout

- `ahura.h` — public umbrella API (the only header applications include).
- `ahura_config.h` — build-time configuration (tick rate, task/timer limits, stack
  sizes) and per-feature switches (`OS_CONFIG_<FEATURE>_ENABLE` for mutex, semaphore,
  queue, event, timer, work, memory pool, list, stack watermark). Every option is a
  default overridable from the build system, e.g.
  `target_compile_definitions(ahura_kernel PUBLIC OS_CONFIG_TIMER_ENABLE=0U)`.
  Disabling a feature compiles out its code and API; disabling timer/work also
  removes the corresponding kernel service task and its stack.
- `core/` — portable kernel modules:
  - `kernel.c` — lifecycle (`os_init`, `os_start`, running flag).
  - `task.c` — TCB pool, priority + round-robin scheduling, blocking, idle task.
  - `tick.c` — tick counter, tick handler (wakes delays, drives timers, preempts).
  - `delay.c` — blocking millisecond/second delays, DWT-precise microsecond busy-wait.
  - `critical.c` — PRIMASK-based nesting critical sections.
  - `mutex.c`, `semaphore.c`, `queue.c`, `event.c` — sync/IPC primitives with `timeout_ms` waits.
  - `timer.c` — software timers; expiry is detected by the tick, callbacks run on the
    kernel timer task (`tsk_timer`, highest priority).
  - `work.c` — Zephyr-style deferrable work queue; items run on the kernel work task
    (`tsk_work`, highest priority).
  - `memory_pool.c`, `list.c` — fixed-block pool and intrusive list utilities.
  - `os_internal.h` — internal cross-module contract (not for applications).
- `arch/arm/` — port layer: SysTick tick source, SVC first-task start,
  PendSV context switch, initial stack frames, cycle counter.
  - `common/os_arch_port_v7m.c` — shared ARMv7-M / ARMv8-M mainline implementation
    (Thumb-2, FPU-aware: saves `s16-s31` and a per-task `EXC_RETURN` when built
    with a hard/softfp float ABI). On ARMv8-M mainline (M33/M55/M85) it also
    saves/restores `PSPLIM` per task, so a stack overflow raises a UsageFault
    instead of silently corrupting memory. There is no separate `v8m` file:
    baseline (M23) executes the v6m Thumb-1 subset and mainline is a superset
    of v7-M, so v8-M support is these conditionals, not a third copy.
  - `common/os_arch_port_v6m.c` — shared ARMv6-M / ARMv8-M baseline implementation
    (Thumb-1 subset, no FPU; the cycle counter is synthesized from SysTick because
    these cores have no DWT CYCCNT).
  - `cortex_m0/`, `cortex_m0plus/`, `cortex_m1/`, `cortex_m23/` — thin wrappers over
    the v6m port.
  - `cortex_m3/`, `cortex_m4/`, `cortex_m7/`, `cortex_m33/`, `cortex_m35p/`,
    `cortex_m52/`, `cortex_m55/`, `cortex_m85/` — thin wrappers over the v7m port
    (M7 additionally relies on the DWT LAR unlock done in `os_arch_init`; on the
    v8.1-M cores Helium/MVE state is covered by the existing s16-s31 save plus
    hardware lazy stacking of s0-s15/FPSCR/VPR).
  - The build selects the variant from `-mcpu` (see `ahura_kernel/CMakeLists.txt`);
    override with `-DOS_ARCH_VARIANT=cortex_m4`. Note: GCC learned
    `-mcpu=cortex-m52` in GCC 14 — with older toolchains build that core with
    `-march=armv8.1-m.main+mve.fp` and set `OS_ARCH_VARIANT` manually.
  - Not covered: PAC/BTI (`-mbranch-protection` on M85) and TrustZone secure-state
    switching — build without those features for now.

## Integration checklist

1. Route `SysTick_Handler` to `os_tick_handler()`. `SVC_Handler` and `PendSV_Handler`
   are provided by the port — do not define them in `stm32*_it.c`.
2. Call `os_init()` after clocks are configured (it reads `SystemCoreClock`).
3. Create and start tasks, then call `os_start()` (never returns).
4. Task stacks: use `OS_TASK_DEFINE(name, stack_units)`; units are 32-bit words and
   the total must be at least `OS_CONFIG_MIN_STACK_SIZE` bytes.

## Task priorities

- `0` — idle task (kernel).
- `OS_CONFIG_MAX_PRIORITY` — kernel service tasks (`tsk_work`, `tsk_timer`), created
  automatically by `os_init()`; they occupy two `OS_CONFIG_MAX_TASKS` slots.
- `OS_TASK_PRIORITY_USER_MIN .. OS_TASK_PRIORITY_USER_MAX` (1 .. MAX-1) — user tasks;
  `os_task_create` rejects anything outside this range.

## Work queue

Defer a function to run later on the highest-priority kernel task (ISR-safe):

```c
static void my_handler(void *context) { /* runs on tsk_work */ }
static os_work_t my_work;

os_work_init(&my_work, my_handler, &my_data);  /* handler + user-data pointer */
os_work_submit(&my_work, 100U);                /* run after 100 ms (0 = as soon as possible) */
os_work_cancel(&my_work);                      /* drop it if it has not run yet */
```

Re-submitting a pending item reschedules it. Handlers and timer callbacks run in
task context (they may use kernel APIs), but they execute at the highest priority:
keep them short and do not block in them, or everything else is starved.

## Timeout semantics

Blocking APIs (`os_mutex_lock`, `os_semaphore_take`, `os_queue_send/receive`,
`os_event_group_wait_bits`) take a `timeout_ms` argument:

- `OS_NO_WAIT` — try once, return `BUSY`/`EMPTY`/`FULL` immediately.
- `1..N` ms — wait up to that long, then return `OS_STATUS_TIMEOUT`.
- `OS_WAIT_FOREVER` — wait until available.

Nonzero timeouts are honored only from task context after `os_start`; from
interrupt context (or before the scheduler starts) the call degrades to a
non-blocking attempt. Waits are currently implemented as one-tick sleep/retry
loops; dedicated wait queues (and mutex priority inheritance) are future work.

## Notes and constraints

- Do not block (delay, lock with timeout) inside a critical section or an ISR.
- Work handlers and timer callbacks run on the highest-priority kernel tasks:
  keep them short and non-blocking so user tasks are not starved.
- Timers run in two modes: `OS_TIMER_MODE_ONE_SHOT` (fires once, then stops) and
  `OS_TIMER_MODE_PERIODIC` (reloads every period), selected in `os_timer_init`.
- With `OS_CONFIG_STACK_WATERMARK_ENABLE`, stacks are pattern-filled at creation
  and `os_task_stack_watermark_get(task, &min_free)` reports the worst-case
  remaining stack in bytes (NULL task = calling task).
- The project builds with the hard-float ABI; the port saves/restores the FPU
  context (`s16-s31` + per-task `EXC_RETURN`) automatically.
