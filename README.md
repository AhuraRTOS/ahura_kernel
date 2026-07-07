# Ahura Kernel

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
  PendSV context switch, initial stack frames, cycle counter. Shared code is
  organized by architecture (the same split Zephyr and CMSIS-RTX use): one
  v6m implementation, one v7m implementation, thin per-core wrapper folders
  on top.
  - `common/os_arch_port_v7m.c` — ARMv7-M (M3, M4, M7), ARMv8-M mainline
    (M33, M35P) and ARMv8.1-M (M52, M55, M85) implementation. Thumb-2,
    FPU-aware: saves `s16-s31` and a per-task `EXC_RETURN` when built with a
    hard/softfp float ABI. On ARMv8-M mainline it also saves/restores `PSPLIM`
    per task and programs `MSPLIM` for the handler stack (when the linker
    script provides `__StackLimit`), so a stack overflow raises a UsageFault
    instead of silently corrupting memory. There is no separate `v8m` file:
    baseline (M23) executes the same Thumb-1 subset as v6-M and mainline is a
    superset of v7-M, so v8-M support is these conditionals, not a third copy.
  - `common/os_arch_port_v6m.c` — ARMv6-M (M0, M0+) and ARMv8-M baseline
    (M23) implementation. Thumb-1 subset, no FPU; the cycle counter is
    synthesized from SysTick because these cores have no DWT CYCCNT.
    Non-secure v8-M baseline has no `PSPLIM`, so there is no stack-limit
    handling in this file.
  - `cortex_m0/`, `cortex_m0plus/`, `cortex_m23/` — thin wrappers over the
    v6m port.
  - `cortex_m3/`, `cortex_m4/`, `cortex_m7/`, `cortex_m33/`, `cortex_m35p/`,
    `cortex_m52/`, `cortex_m55/`, `cortex_m85/` — thin wrappers over the
    v7m port (M7 additionally relies on the DWT LAR unlock done in
    `os_arch_init`; on the v8.1-M cores Helium/MVE state is covered by the
    existing s16-s31 save plus hardware lazy stacking of s0-s15/FPSCR/VPR).
    Note the folder names follow GCC's `-mcpu` spelling: `cortex_m0plus`
    because the core is the M0"plus", but `cortex_m35p` because that core's
    "P" means physical security, not plus (`-mcpu=cortex-m35p`).
  - The build selects the variant from `-mcpu`, falling back to `-march`
    (`armv8.1-m.main` maps to `cortex_m55` and so on — all folders of one
    profile include the same shared port, so any core of the right
    architecture is equivalent); see `ahura_kernel/CMakeLists.txt`. Override
    with `-DOS_ARCH_VARIANT=cortex_m4`. Note: GCC learned `-mcpu=cortex-m52`
    in GCC 14 — older toolchains build that core with
    `-march=armv8.1-m.main+mve.fp`, which the fallback resolves automatically.
  - `MSPLIM` guard: active when the linker script defines `__StackLimit`
    (CMSIS-template scripts do; this project's STM32CubeMX scripts define it as
    `__StackLimit = _sstack;` — add the equivalent line to other CubeMX
    scripts). Without the symbol the guard is skipped.
  - Not covered yet: TrustZone secure-state switching (build with the Security
    Extension disabled or run the kernel entirely in one security state;
    per-task secure contexts are planned as a dedicated v8-M TZ port) and
    PAC/BTI (`-mbranch-protection` on M85).

## Integration checklist

1. Route `SysTick_Handler` to `os_tick_handler()`. `SVC_Handler` and `PendSV_Handler`
   are provided by the port — do not define them in `stm32*_it.c`.
2. Call `os_init()` after clocks are configured (it reads `SystemCoreClock`).
3. Create and start tasks, then call `os_start()` (never returns).
4. Task stacks: use `OS_TASK_DEFINE(name, stack_bytes)`; the size is in bytes
   (rounded up to an 8-byte multiple by the macro) and must be at least
   `OS_CONFIG_MIN_STACK_SIZE`.

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

- `OS_WAIT_NOTHING` — try once, return `BUSY`/`EMPTY`/`FULL` immediately.
- `1..N` ms — wait up to that long, then return `OS_STATUS_TIMEOUT`.
- `OS_WAIT_FOREVER` — wait until available.

Nonzero timeouts are honored only from task context after `os_start`; from
interrupt context (or before the scheduler starts) the call degrades to a
non-blocking attempt. Waits are currently implemented as one-tick sleep/retry
loops; dedicated wait queues (and mutex priority inheritance) are future work.

## Tickless idle (experimental, not functional yet)

Config: `OS_CONFIG_TICKLESS_ENABLE` (default 0), `OS_CONFIG_TICKLESS_MIN_IDLE`
(shortest idle worth sleeping for), `OS_CONFIG_MAX_SUPPRESSED_TICKS`.

Two weak application callbacks bracket the sleep window (prototypes in
`ahura.h`); override them by defining the functions in application code.
User-overridable callbacks carry the `_cb` suffix by convention:

```c
void os_tickless_pre_sleep_cb(void)   { /* select sleep mode (e.g. SLEEPDEEP), gate clocks */ }
void os_tickless_post_sleep_cb(void)  { /* clear SLEEPDEEP, restore clocks */ }
```

Status: the flow in `tick.c` (`os_tickless_idle_process`) is complete in shape
but is not yet invoked — the idle task still runs a plain `WFI` loop, so
enabling the config flag currently changes nothing. Remaining work before it
can be wired in:

- the idle task must call `os_tickless_idle_process()`;
- the planned idle time only considers software-timer expiries, not blocked
  task delays (`os_delay_ms` sleepers would wake late);
- SysTick keeps running during the sleep window (no tick suppression), which
  would double-count time via both `os_tick_handler` and `os_tick_announce`;
- the elapsed-time measurement uses DWT `CYCCNT`, which halts in sleep mode on
  most implementations — the wake source must provide the duration instead
  (suppressed-SysTick arithmetic, or LPTIM: `OS_CONFIG_LPTIM_CLOCK_HZ` is
  reserved for that but unused so far).

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
