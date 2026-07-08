# Ahura Kernel

A small preemptive RTOS kernel for ARM Cortex-M.

## Layout

- `ahura.h` — public umbrella API (the only header applications include).
- `ahura_config_template.h` — template for the application's `ahura_config.h`:
  every build-time option, active at its default value (tick rate, task/timer
  limits, stack sizes, heap size, TrustZone mode, core count, and the
  per-feature switches `OS_CONFIG_<FEATURE>_ENABLE` for mutex, semaphore,
  queue, event, timer, work, memory pool, alloc, stack watermark, CPU
  usage; the intrusive list module has no switch — the scheduler runs on
  it). Never included by the kernel: copy it into the project — see
  "Configuration". Disabling a feature compiles out its code and API;
  disabling timer/work also removes the corresponding kernel service task and
  its stack.
- `ahura_cb_template.c` — template for the application-side callbacks,
  deliberately not compiled into the kernel — see "Application callbacks".
- `core/` — portable kernel modules:
  - `kernel.c` — lifecycle (`os_init`, `os_start`, running flag) and the
    platform clock callback (`os_clock_hz_get_cb`, see "Platform clock").
  - `alloc.c` — kernel heap (`os_alloc`/`os_free`): first-fit allocator with
    coalescing over a static `OS_CONFIG_HEAP_SIZE` heap.
  - `task.c` — static TCB pool with O(1) list-based scheduling: one FIFO
    ready list per priority plus a ready bitmap (highest set bit = next
    priority to run, one `CLZ` on ARMv7-M and up), round-robin by list
    rotation, and a delay list holding only the finite-delay sleepers.
  - `tick.c` — tick counter, tick handler (wakes delays, drives timers, preempts).
  - `delay.c` — blocking millisecond/second delays, DWT-precise microsecond busy-wait.
  - `critical.c` — PRIMASK-based nesting critical sections.
  - `mutex.c`, `semaphore.c`, `queue.c`, `event.c` — sync/IPC primitives with `timeout_ms` waits.
  - `timer.c` — software timers; expiry is detected by the tick, callbacks run on the
    kernel timer task (`tsk_timer`, highest priority).
  - `work.c` — Zephyr-style deferrable work queue; items run on the kernel work task
    (`tsk_work`, highest priority).
  - `memory_pool.c` — fixed-block pool utility.
  - `list.c` — intrusive doubly-linked list; always compiled (the scheduler
    itself runs on it, so it cannot be configured out), also public API.
  - `os_internal.h` — internal cross-module contract (not for applications).
- `arch/arm/` — port layer: SysTick tick source, SVC first-task start,
  PendSV context switch, initial stack frames, cycle counter. Shared code is
  organized by architecture (the same split Zephyr and CMSIS-RTX use): one
  v6m implementation, one v7m implementation, thin per-core wrapper folders
  on top.
  - `common/os_arch_port_v7m.c` — ARMv7-M (M3) and ARMv7E-M (M4, M7)
    implementation. Thumb-2, FPU-aware: saves `s16-s31` and a per-task
    `EXC_RETURN` when built with a hard/softfp float ABI.
  - `common/os_arch_port_v8m.c` — ARMv8-M mainline (M33, M35P) and ARMv8.1-M
    (M52, M55, M85) implementation: superset of the v7m port that always
    saves/restores `PSPLIM` per task and programs `MSPLIM` for the handler
    stack (when the linker script provides the stack-bottom symbol), so a
    stack overflow raises a UsageFault instead of silently corrupting memory.
    TrustZone (all three `OS_CONFIG_TRUSTZONE` modes) lives here.
  - `common/os_arch_port_v6m.c` — ARMv6-M (M0, M0+) and ARMv8-M baseline
    (M23) implementation. Thumb-1 subset, no FPU; the cycle counter is
    synthesized from SysTick because these cores have no DWT CYCCNT.
    Baseline does not belong in the v8m file because it cannot execute the
    mainline Thumb-2 ISA; its TrustZone support is handled here. Non-secure
    v8-M baseline has no `PSPLIM`, so there is no stack-limit handling.
  - Each shared file carries a `#error` guard against being compiled for the
    wrong architecture profile.
  - `cortex_m0/`, `cortex_m0plus/`, `cortex_m23/` — thin wrappers over the
    v6m port.
  - `cortex_m3/`, `cortex_m4/`, `cortex_m7/` — thin wrappers over the v7m
    port (M7 additionally relies on the DWT LAR unlock done in
    `os_arch_init`).
  - `cortex_m33/`, `cortex_m35p/`, `cortex_m52/`, `cortex_m55/`,
    `cortex_m85/` — thin wrappers over the v8m port (on the v8.1-M cores
    Helium/MVE state is covered by the existing s16-s31 save plus hardware
    lazy stacking of s0-s15/FPSCR/VPR).
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
  - `MSPLIM` guard: active when the linker script provides the bottom of the
    main stack as `__StackLimit` (CMSIS-template scripts) or `_sstack` (stock
    STM32CubeMX/CubeIDE scripts) — both are weak references, so either script
    family works unmodified. When neither symbol exists the guard is skipped.
  - TrustZone (ARMv8-M Security Extension): selected with `OS_CONFIG_TRUSTZONE`
    — see the "TrustZone" section below.
  - Not covered yet: PAC/BTI (`-mbranch-protection` on M85).

## Configuration

Projects never edit kernel files, and the kernel ships no editable
configuration of its own — the application owns the one and only config
file (the FreeRTOSConfig.h model):

1. Copy `ahura_kernel/ahura_config_template.h` into the project as
   `ahura_config.h` (any directory — the project layout does not matter).
   Every option is active at its default value; adjust values in place.
2. Make that directory visible to the **kernel library build**, not just
   the application — set `OS_CONFIG_DIR` before
   `add_subdirectory(ahura_kernel)`:

   ```cmake
   set(OS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc)  # wherever the copy lives
   add_subdirectory(ahura_kernel)
   ```

   If only the application saw the file, kernel and application would
   compile with different `OS_CONFIG_` values and structure sizes would
   silently disagree. The kernel CMakeLists warns when `OS_CONFIG_DIR` is
   unset, and the build stops with a clear `#error` when no `ahura_config.h`
   is found or when it is missing options (a missing option would otherwise
   read as 0 in `#if` and silently disable features — so keep all options,
   the template lists exactly what is required).

`ahura_config.h` is the single source of configuration: all options are
plain defines, so do not additionally define `OS_CONFIG_` macros from the
build system (`target_compile_definitions`) — that would redefine them. The
`OS_CONFIG_TRUSTZONE_*` mode values are kernel-owned
(`os_arch_port_common.h`); the config file only selects among them.

## Application callbacks (ahura_cb.c)

All user-overridable hooks are weak `_cb` functions, so overriding is
optional per function. For a clean starting point, copy
`ahura_kernel/ahura_cb_template.c` into the application source tree as
`ahura_cb.c`, add it to the **application** build (never to the kernel — the
template is deliberately absent from the kernel CMakeLists), and adapt:

- `os_clock_hz_get_cb` — CPU clock in Hz (see "Platform clock").
- `os_tickless_pre_sleep_cb` / `os_tickless_post_sleep_cb` — sleep bracket.
- `os_arch_tz_context_save_cb` / `os_arch_tz_context_restore_cb` — TrustZone
  secure-context banking (non-secure kernels only).
- `os_arch_core_id_get_cb` / `os_arch_core_ipi_request_cb` — multi-core SoC
  glue; plus `os_arch_spinlock_acquire_cb`/`_release_cb` on ARMv6-M
  multi-core SoCs (mandatory there).

This project keeps its copy in `Core/Src/ahura_cb.c` with its configuration
in `Core/Inc/ahura_config.h`.

## CPU usage

With `OS_CONFIG_CPU_USAGE_ENABLE` (default 1) the tick interrupt counts how
many ticks interrupted the idle task versus anything else, and

```c
uint32_t percent = os_cpu_usage_get();   /* 0..100 since the previous call */
```

returns the load over the window since the previous call, then restarts the
window. Resolution is one tick, so sample at a period well above the tick
period (e.g. once per second at a 1 kHz tick). Ticks announced after a
tickless sleep count as idle. Cost: two counter updates per tick.

## Architecture profiles at a glance

| Architecture profile | Cortex-M cores | Ahura port | TrustZone support |
|----------------------|----------------|------------|-------------------|
| ARMv6-M | M0, M0+ | `v6m` | No (Security Extension absent) — `OS_CONFIG_TRUSTZONE_DISABLED` only |
| ARMv7-M / ARMv7E-M | M3 / M4, M7 | `v7m` | No — `OS_CONFIG_TRUSTZONE_DISABLED` only |
| ARMv8-M baseline | M23 | `v6m` | Yes, optional per device — all three `OS_CONFIG_TRUSTZONE` modes |
| ARMv8-M mainline | M33, M35P | `v8m` | Yes, optional per device — all three modes |
| ARMv8.1-M | M52, M55, M85 | `v8m` | Yes, optional per device — all three modes |

Notes: M4/M7 are ARMv7E-M (DSP extension) but port-identical to M3 here; M23 is
baseline (Thumb-1 subset), which is why it shares the `v6m` port rather than the
mainline one; Cortex-M1 (ARMv6-M, FPGA) is deliberately not supported. "Optional
per device" means the Security Extension is a silicon-vendor choice and may also
be disabled in option bytes (e.g. `TZEN` on STM32H5) — that case uses
`OS_CONFIG_TRUSTZONE_DISABLED`.

## Integration checklist

1. Route `SysTick_Handler` to `os_tick_handler()`. `SVC_Handler` and `PendSV_Handler`
   are provided by the port — do not define them in `stm32*_it.c`.
2. Call `os_init()` after clocks are configured (it reads the CPU clock
   through `os_clock_hz_get_cb`, see "Platform clock").
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

## Platform clock

The kernel never reads a platform global directly: every place that needs the
CPU frequency (SysTick reload, `os_delay_us` busy-waits, tickless accounting)
calls the weak callback

```c
uint32_t os_clock_hz_get_cb(void);   /* return the CPU clock in Hz, 0 = unknown */
```

The default implementation covers the common cases without any code:

- `OS_CONFIG_CPU_CLOCK_HZ` set (> 0) — returns that fixed value; for
  platforms with a constant clock and no CMSIS.
- `OS_CONFIG_CPU_CLOCK_HZ` = 0 (default) — returns the CMSIS
  `SystemCoreClock` global when the platform defines one (it is a weak
  reference, so linking never fails without it), else 0.

Any other platform convention (Zephyr-style config, a clock-driver query,
dynamic frequency scaling) plugs in by overriding the callback in application
code. When the callback returns 0, tick setup and busy-wait delays refuse to
run (`OS_STATUS_ERROR`) instead of miscounting.

## Kernel heap (os_alloc / os_free)

`OS_CONFIG_ALLOC_ENABLE` (default 1) compiles in a kernel heap of
`OS_CONFIG_HEAP_SIZE` bytes (default 4096, static array — nothing is taken
from the linker heap):

```c
void  *memory = os_alloc(size);          /* 8-byte aligned, NULL when exhausted   */
os_free(memory);                         /* NULL/foreign/double free are ignored  */
size_t now  = os_alloc_free_bytes_get();     /* current free bytes                */
size_t low  = os_alloc_min_free_bytes_get(); /* worst-case watermark since boot   */
```

The allocator is first-fit with an address-ordered free list and coalescing
of adjacent free blocks (comparable to FreeRTOS `heap_4`), so mixed-size
alloc/free patterns do not fragment permanently. Calls are protected by the
kernel critical section: usable from tasks and ISRs, though allocating in an
ISR is discouraged — the walk over the free list runs with interrupts masked.
For hot fixed-size objects prefer `os_memory_pool_*` (O(1), no fragmentation).

## Timeout semantics

Blocking APIs (`os_mutex_lock`, `os_semaphore_take`, `os_queue_send/receive`,
`os_event_group_wait_bits`) take a `timeout_ms` argument:

- `OS_WAIT_NOTHING` — try once, return `BUSY`/`EMPTY`/`FULL` immediately.
- `1..N` ms — wait up to that long, then return `OS_STATUS_TIMEOUT`.
- `OS_WAIT_FOREVER` — wait until available.

Nonzero timeouts are honored only from task context after `os_start`; from
interrupt context (or before the scheduler starts) the call degrades to a
non-blocking attempt.

Waits are exact: every object carries its own waiter list (queues carry two —
senders and receivers). A blocked task consumes zero CPU until the object
signals it (unlock/give/send/receive/set_bits wake the **highest-priority**
waiter, FIFO among equals; event groups wake all waiters so each re-evaluates
its bit condition) or its timeout expires (the tick removes it from both the
delay list and the waiter list). Wakeups re-check the condition, so a faster
third task taking the object in between is handled by re-waiting with the
remaining timeout. Mutex priority inheritance is still future work.

## TrustZone (ARMv8-M security states)

`OS_CONFIG_TRUSTZONE` selects which security state the kernel runs in on
ARMv8-M cores (M23, M33, M35P, M52, M55, M85); the build fails with a clear
`#error` on cores without the Security Extension or when the compile flags do
not match the chosen mode.

- `OS_CONFIG_TRUSTZONE_DISABLED` (default) — the kernel ignores TrustZone.
  Use on devices without the Security Extension or with TrustZone disabled
  (e.g. `TZEN` cleared on STM32H5/L5/U5).
- `OS_CONFIG_TRUSTZONE_SECURE` — the kernel and every task run in the secure
  state. Compile the kernel (and application) with `-mcmse`. The context
  switch itself needs nothing extra: the secure `EXC_RETURN` encoding equals
  the TrustZone-less one, and PSPLIM/MSPLIM guards stay active.
- `OS_CONFIG_TRUSTZONE_NON_SECURE` — the kernel and tasks run non-secure
  beside separate secure firmware. Initial task frames use the non-secure
  `EXC_RETURN` (`0xFFFFFFBC`), and the context switch calls two weak
  callbacks (override them in the application, following the `_cb`
  convention) so the secure-side glue can bank per-task secure contexts
  (secure stack / `PSP_S`), in the same way FreeRTOS's ARM_CM33 secure
  context management works:

  ```c
  void os_arch_tz_context_save_cb(uint32_t task_id);     /* before the switch, outgoing task */
  void os_arch_tz_context_restore_cb(uint32_t task_id);  /* after selection, incoming task   */
  ```

  `task_id` 0 is the idle task (never owns a secure context). Tasks that
  never call secure functions need no handling — the weak defaults do
  nothing.

## Multi-core (experimental, untested on hardware)

`OS_CONFIG_CORE_COUNT` (default 1, max 31) declares how many cores schedule
tasks. Every scheduling core runs its own PendSV/SVC and its own idle task
and pulls work from the shared ready lists; **core affinity** selects where
each task may run:

- `os_task_config_t.core_affinity` — bitmask of allowed cores (bit n =
  core n); `OS_TASK_CORE_ANY` (0, the default) means any core. Change it at
  runtime with `os_task_core_affinity_set(task, mask)`; a task executing on
  a core the new mask excludes is asked to reschedule (locally or by IPI).
- When a task becomes ready (wake, start), the kernel preempts locally when
  the task's affinity allows this core, otherwise it nudges the first core
  in the mask through `os_arch_core_ipi_request_cb` (weak default: none —
  that core then picks the task up at its own next tick).
- Core 0 boots the kernel as usual (`os_init`, `os_start`). Each secondary
  core is booted by the SoC layer (vector table with the kernel's SVC/
  PendSV/SysTick handlers), then calls `os_core_start()` — it configures
  the banked SHPR/SysTick/DWT/MSPLIM for that core and enters the
  scheduler; it never returns.
- Core 0 owns the time base: delays, timers, work queues and `os_tick_get`
  advance only from core 0's tick. Ticks on other cores drive that core's
  preemption and round-robin. CPU usage (`os_cpu_usage_get`) samples core 0.
- The kernel service tasks are placed with `OS_CONFIG_WORK_CORE_AFFINITY`
  and `OS_CONFIG_TIMER_CORE_AFFINITY` (core-affinity bitmasks, 0 = any
  core), so work handlers and timer callbacks run where the config says.
- Critical sections are PRIMASK + a global kernel spinlock with per-core
  nesting; the spinlock uses `LDREX/STREX` on ARMv7-M/v8-M, while ARMv6-M
  multi-core SoCs (e.g. RP2040) must provide
  `os_arch_spinlock_acquire_cb`/`_release_cb` backed by hardware spinlocks —
  a missing implementation fails at link time by design.
- The SoC layer supplies `os_arch_core_id_get_cb()` (weak default: 0), since
  Cortex-M has no architectural core-id register.

Constraints: a task currently executing on another core cannot be paused or
deleted from this one (`OS_STATUS_BUSY`) — suspend it from its own core
first. The SMP paths compile in the CI matrix but have not run on real
multi-core silicon yet; treat them as experimental.

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
