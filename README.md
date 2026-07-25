# Ahura Kernel

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Standard: C11](https://img.shields.io/badge/standard-C11-blue.svg)
![Platform: Cortex-M](https://img.shields.io/badge/platform-Cortex--M-informational.svg)

A small, preemptive, priority-based RTOS kernel for ARM Cortex-M, covering
everything from the M0 to the M85. It is TrustZone-aware and has no mandatory
HAL or CMSIS dependency.

- **Preemptive priority scheduler.** 31 priority levels, O(1) list-based ready
  queues, and round-robin among tasks of equal priority.
- **Full sync/IPC set.** Mutexes (always with single-level priority
  inheritance), counting semaphores, queues, event groups, and lightweight
  per-task notifications, all with millisecond timeouts.
- **Software timers and a deferrable work queue.** One-shot and periodic timers
  plus a work queue in the style of Zephyr, each on its own kernel service task.
- **Optional kernel heap.** A first-fit allocator with coalescing, comparable to
  FreeRTOS `heap_4`, compiled out entirely when unused.
- **Built-in diagnostics.** Stack watermarking and CPU usage sampling, both
  opt-in and close to free at runtime.
- **TrustZone-aware.** Secure, non-secure, or disabled, selectable per build on
  ARMv8-M.
- **Every feature is a compile-time switch.** `OS_CONFIG_<FEATURE>_ENABLE`
  removes unused code, RAM, and API surface entirely, not just at runtime.
- **Self-testing.** A built-in suite exercises every enabled feature on real
  hardware with no board or HAL dependencies, and ends with a cycle-accurate
  benchmark table.
- **Broad Cortex-M coverage.** M0/M0+/M23, M3/M4/M7, and M33/M35P/M52/M55/M85
  all share just three portable port implementations.
- **Experimental:** multi-core (SMP) scheduling and tickless idle.

---

## Contents

**[Getting started](#getting-started)**
[Quick start](#quick-start) ·
[Configuration](#configuration) ·
[Integration checklist](#integration-checklist)

**[Using the kernel](#using-the-kernel)**
[API at a glance](#api-at-a-glance) ·
[Default application task](#default-application-task) ·
[Task priorities](#task-priorities) ·
[Timeout semantics](#timeout-semantics) ·
[Mutexes and priority inheritance](#mutexes-and-priority-inheritance) ·
[Task notifications](#task-notifications) ·
[Work queue](#work-queue) ·
[Kernel heap](#kernel-heap) ·
[Diagnostics](#diagnostics)

**[Platform support](#platform-support)**
[Supported cores](#supported-cores) ·
[Application callbacks](#application-callbacks) ·
[Platform clock](#platform-clock) ·
[TrustZone](#trustzone) ·
[Multi-core (experimental)](#multi-core-experimental) ·
[Tickless idle (experimental)](#tickless-idle-experimental)

**[Testing and examples](#testing-and-examples)**
[Self-test suite](#self-test-suite) ·
[Examples](#examples)

**[Internals](#internals)**
[Source layout](#source-layout) ·
[Notes and constraints](#notes-and-constraints)

---

## Getting started

### Quick start

1. **Configure.** Copy [`os_config_template.h`](os_config_template.h) into the
   project as `os_config.h`. Any directory works, the layout does not matter.
   Every option starts at its default value.
2. **Point the kernel build at it**, before `add_subdirectory`, so the kernel
   library and the application compile against the exact same config:

   ```cmake
   set(OS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc)  # wherever the copy lives
   add_subdirectory(ahura_kernel)
   ```

3. **Boot the kernel** from `main()`, after clocks are configured:

   ```c
   os_init();
   os_start();   /* never returns */
   ```

   `os_init()` has already created and started a default application task, so
   there is nothing else to create just to get moving.
4. **Write the application** in `os_main()`. Copy
   [`os_main_template.c`](os_main_template.c) into the project as `os_main.c`
   and replace the body, or spawn further tasks from it with `OS_TASK_DEFINE`
   and `os_task_create`.

New to a specific feature? [`ahura_examples/kernel/`](../ahura_examples/kernel/)
has a minimal, standalone example per feature. See [Examples](#examples).

### Configuration

Projects never edit kernel files, and the kernel ships no editable configuration
of its own. The application owns the one and only config file, following the
same model as `FreeRTOSConfig.h`:

1. Copy `ahura_kernel/os_config_template.h` into the project as `os_config.h`.
   Any directory works. Every option is active at its default value, so adjust
   values in place.
2. Make that directory visible to the **kernel library build**, not just the
   application, by setting `OS_CONFIG_DIR` before
   `add_subdirectory(ahura_kernel)`:

   ```cmake
   set(OS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc)  # wherever the copy lives
   add_subdirectory(ahura_kernel)
   ```

   If only the application saw the file, the kernel and the application would
   compile with different `OS_CONFIG_` values and their structure sizes would
   silently disagree. The kernel CMakeLists warns when `OS_CONFIG_DIR` is unset,
   and the build stops with a clear `#error` when no `os_config.h` is found or
   when it is missing options. A missing option would otherwise read as 0 in
   `#if` and silently disable features, so keep all of them. The template lists
   exactly what is required.

`os_config.h` is the single source of configuration. All options are plain
defines, so do not additionally define `OS_CONFIG_` macros from the build system
with `target_compile_definitions`, since that would redefine them. The
`OS_CONFIG_TRUSTZONE_*` mode values are kernel-owned (`os_arch_port_common.h`)
and the config file only selects among them. `OS_TASK_PRIO_MAX` is kernel-owned
too, fixed at `31` in `ahura.h` because that is the most a 32-bit ready bitmap
can support, so it is not one of the options `os_config.h` defines.

### Integration checklist

1. Route `SysTick_Handler` to `os_tick_handler()`. `SVC_Handler` and
   `PendSV_Handler` are provided by the port, so do not define them in
   `stm32*_it.c`.
2. Call `os_init()` after clocks are configured. It reads the CPU clock through
   `os_clock_hz_get_cb`, see [Platform clock](#platform-clock).
3. Create and start any tasks the application needs before the scheduler runs.
   `os_init()` already created the default task (see [Default application
   task](#default-application-task)) unless this is a self-test build. Then call
   `os_start()`, which never returns.
4. For task stacks, use `OS_TASK_DEFINE(name, stack_bytes)`. The size is in
   bytes, rounded up to an 8-byte multiple by the macro, and must be at least
   `OS_CONFIG_MIN_STACK_SIZE`.

---

## Using the kernel

### API at a glance

Everything below is declared in the single public header, `ahura.h`. Each group
compiles away entirely when its `OS_CONFIG_<FEATURE>_ENABLE` is 0.

| Group | Functions |
|---|---|
| **Lifecycle** | `os_init` · `os_start` · `os_kernel_is_running` · `os_core_start` |
| **Tasks** | `os_task_create` · `os_task_start` · `os_task_pause` · `os_task_delete` · `os_task_yield` · `os_task_state_get` · `os_task_core_affinity_set` |
| **Delays and time** | `os_delay_ms` · `os_delay_us` · `os_delay_s` · `os_tick_get` |
| **Critical sections** | `os_critical_enter` · `os_critical_exit` |
| **Mutex** | `os_mutex_init` · `os_mutex_lock` · `os_mutex_try_lock` · `os_mutex_unlock` |
| **Semaphore** | `os_semaphore_init` · `os_semaphore_give` · `os_semaphore_take` |
| **Queue** | `os_queue_init` · `os_queue_send` · `os_queue_receive` · `os_queue_count_get` |
| **Event group** | `os_event_group_init` · `os_event_group_set_bits` · `os_event_group_clear_bits` · `os_event_group_wait_bits` |
| **Task notifications** | `os_task_notify_give` · `os_task_notify_wait` |
| **Software timers** | `os_timer_init` · `os_timer_start` · `os_timer_stop` |
| **Work queue** | `os_work_init` · `os_work_submit` · `os_work_cancel` · `os_work_is_pending` |
| **Kernel heap** | `os_mem_alloc` · `os_mem_free` · `os_mem_free_get` · `os_mem_watermark_get` |
| **Diagnostics** | `os_task_stack_watermark_get` · `os_cpu_usage_get` |
| **Intrusive list** | `os_list_init` · `os_list_is_empty` · `os_list_push_back` · `os_list_pop_front` · `os_list_remove` · `os_list_insert_before` |
| **Tickless idle** | `os_tickless_idle_process` · `os_tickless_expected_idle_ticks_get` · `os_tickless_max_suppressed_ticks_get` |
| **Application-provided** | `os_main` · `os_test` · `os_clock_hz_get_cb` · `os_tickless_pre_sleep_cb` · `os_tickless_post_sleep_cb` · `os_arch_tz_context_save_cb` · `os_arch_tz_context_restore_cb` · `os_arch_core_id_get_cb` · `os_arch_core_ipi_request_cb` |

Helper macros: `OS_TASK_DEFINE` (stack and handle), `OS_TASK_CONFIG` and
`OS_TASK_CONFIG_CORE` (task parameters), `OS_TICKS_FROM_MS`, `OS_WAIT_NOTHING`,
and `OS_WAIT_FOREVER`.

### Default application task

Most RTOS applications create every task by hand in `main()` before calling
`os_start()`. Ahura instead gives every application one default task for free.
`os_init()` unconditionally creates and starts it (see `os_kernel.c`'s
`os_main_system_init()`), except in self-test builds, so `main()` needs nothing
beyond the usual:

```c
os_init();
os_start();
```

The task's body is the weak function `os_main()`, whose prototype is in
`ahura.h` and whose weak default in `os_kernel.c` simply idles forever. It is
deliberately **not** a `_cb` function: this is where the application's own code
runs, not a kernel query for platform behavior, even though it is wired up the
same way through a weak default and a strong override.

Override it with its own template, separate from `os_cb_template.c`. Copy
`ahura_kernel/os_main_template.c` into the project as `os_main.c`, add it to the
**application** build (never to the kernel, where it is deliberately absent from
the CMakeLists, just like `os_cb_template.c`), and replace `os_main()`'s body
with the application's own code. That can be a plain `while (1)` loop, or it can
spawn further tasks. Two config options size the task:

```c
#define OS_CONFIG_MAIN_TASK_STACK_SIZE  1024U  /* bytes                            */
#define OS_CONFIG_MAIN_TASK_PRIORITY    1U     /* OS_TASK_PRIO_USER_MIN..USER_MAX  */
```

There is no switch to compile the default task out. It always exists unless the
build is a self-test build. Tasks that must exist before the scheduler starts,
which is rare, still belong in `main()`, created the usual way.

When `OS_CONFIG_TEST_ENABLE` is `1`, `os_init()` does not create `tsk_main` at
all, so the self-test suite runs alone instead of racing the application's own
task. See [Self-test suite](#self-test-suite). `os_main()` itself still compiles,
so an application's `os_main.c` links unchanged either way. It is simply never
called in that build.

### Task priorities

- `0` is the idle task, owned by the kernel.
- `OS_TASK_PRIO_MAX` is reserved for the kernel service tasks `tsk_work` and
  `tsk_timer`, which `os_init()` creates automatically. They occupy two
  `OS_CONFIG_MAX_TASKS` slots.
- `OS_TASK_PRIO_USER_MIN` through `OS_TASK_PRIO_USER_MAX` (1 to MAX-1) are user
  tasks. `os_task_create` rejects anything outside this range.

The default application task (`tsk_main`) and the self-test task (`tsk_test`)
live in the user range too, at `OS_CONFIG_MAIN_TASK_PRIORITY` and
`OS_CONFIG_TEST_PRIORITY`. Unlike `tsk_work` and `tsk_timer` they are not
priority-reserved, so pick values that fit alongside the application's own tasks.

Because mutexes always do priority inheritance, a task's effective priority can
be temporarily boosted above its configured value while it holds a contended
mutex. See [Mutexes and priority
inheritance](#mutexes-and-priority-inheritance).

### Timeout semantics

Blocking APIs (`os_mutex_lock`, `os_semaphore_take`, `os_queue_send`,
`os_queue_receive`, `os_event_group_wait_bits`, `os_task_notify_wait`) take a
`timeout_ms` argument:

| Value | Behavior |
|---|---|
| `OS_WAIT_NOTHING` | Try once, return `BUSY`, `EMPTY`, or `FULL` immediately. |
| `1..N` ms | Wait up to that long, then return `OS_STATUS_TIMEOUT`. |
| `OS_WAIT_FOREVER` | Wait until available. |

Nonzero timeouts are honored only from task context after `os_start`. From
interrupt context, or before the scheduler starts, the call degrades to a
non-blocking attempt.

Waits are exact. Every object carries its own waiter list, and queues carry two,
one for senders and one for receivers. A blocked task consumes zero CPU until
the object signals it or its timeout expires. Unlock, give, send, receive, and
set_bits all wake the **highest-priority** waiter, FIFO among equals, while
event groups wake all waiters so each one re-evaluates its bit condition. On a
timeout the tick removes the task from both the delay list and the waiter list.
Wakeups re-check the condition, so a faster third task taking the object in
between is handled by re-waiting with the remaining timeout.

### Mutexes and priority inheritance

Mutexes always do priority inheritance, the way FreeRTOS and Zephyr do it, with
no switch to opt out while still calling it a mutex. `os_mutex_lock` boosts a
lower-priority owner to the blocking waiter's effective priority for as long as
it holds the mutex, and `os_mutex_unlock` restores it. This stays correct even
when the task holds several mutexes at once, because the restore recomputes
against every mutex it still holds rather than only the one just released.

Two limitations are accepted rather than implemented:

- **Single-level only.** An owner that is itself blocked on a second mutex held
  by a third, lower-priority task does not propagate the boost through that
  chain.
- **Lazy recompute.** A boost already in effect is not eagerly repositioned
  within some other object's wait queue, nor eagerly lowered when a waiter times
  out early. Both recompute at the owner's next `os_mutex_lock` or
  `os_mutex_unlock`.

A mutex is also an ownership object, which makes it task-only: calls from an ISR
are rejected, because an ISR has no identity of its own. It is not recursive
either, so locking a mutex the caller already holds fails with `OS_STATUS_BUSY`
rather than deadlocking.

### Task notifications

A lightweight, single-value mailbox built directly into every task's own control
block, enabled by `OS_CONFIG_TASK_NOTIFY_ENABLE`. It lets you signal one
specific task without allocating a separate semaphore or queue object:

```c
os_task_notify_give(&some_task, 42U);         /* ISR-safe; overwrite: last write wins */
os_task_notify_wait(OS_WAIT_FOREVER, &value); /* called by that task about itself     */
```

`os_task_notify_give` latches the value and, if the target is currently blocked
in `os_task_notify_wait`, wakes it immediately. A task blocked for any other
reason, such as a delay or a mutex, queue, semaphore, or event wait, is left
running as normal. The value simply waits to be picked up on its next
`os_task_notify_wait` call, so nothing is lost. `os_task_notify_wait` is
task-only, like `os_mutex_lock`, because an ISR has no task identity to wait as,
and it follows the `timeout_ms` convention above.

### Work queue

Defer a function to run later on the highest-priority kernel task. This is
ISR-safe:

```c
static void my_handler(void *context) { /* runs on tsk_work */ }
static os_work_t my_work;

os_work_init(&my_work, my_handler, &my_data);  /* handler + user-data pointer */
os_work_submit(&my_work, 100U);                /* run after 100 ms (0 = as soon as possible) */
os_work_cancel(&my_work);                      /* drop it if it has not run yet */
```

Re-submitting a pending item reschedules it. Handlers and timer callbacks run in
task context, so they may use kernel APIs, but they execute at the highest
priority. Keep them short and do not block in them, or everything else starves.

### Kernel heap

`OS_CONFIG_ALLOC_ENABLE` (default 1) compiles in a kernel heap of
`OS_CONFIG_HEAP_SIZE` bytes (default 4096). It is a static array, so nothing is
taken from the linker heap:

```c
void  *memory = os_mem_alloc(size);   /* 8-byte aligned, NULL when exhausted   */
os_mem_free(memory);                  /* NULL/foreign/double free are ignored  */
size_t now  = os_mem_free_get();      /* current free bytes                    */
size_t low  = os_mem_watermark_get(); /* worst-case watermark since boot       */
```

The allocator is first-fit with an address-ordered free list and coalescing of
adjacent free blocks, comparable to FreeRTOS `heap_4`, so mixed-size alloc and
free patterns do not fragment permanently. Calls are protected by the kernel
critical section, which makes them usable from tasks and ISRs, although
allocating in an ISR is discouraged because the walk over the free list runs
with interrupts masked.

### Diagnostics

**Stack watermark.** With `OS_CONFIG_STACK_WATERMARK_ENABLE` (default 1), task
stacks are pattern-filled at creation and

```c
size_t min_free;
os_task_stack_watermark_get(task, &min_free);   /* NULL task = calling task */
```

reports the worst-case remaining stack in bytes since that task was created.

**CPU usage.** With `OS_CONFIG_CPU_USAGE_ENABLE` (default 1) the tick interrupt
counts how many ticks interrupted the idle task versus anything else, and

```c
uint32_t percent = os_cpu_usage_get();   /* 0..100 since the previous call */
```

returns the load over the window since the previous call, then restarts the
window. Resolution is one tick, so sample at a period well above the tick period,
for example once per second at a 1 kHz tick. Ticks announced after a tickless
sleep count as idle. The cost is two counter updates per tick.

---

## Platform support

### Supported cores

| Architecture profile | Cortex-M cores | Ahura port | TrustZone support |
|---|---|---|---|
| ARMv6-M | M0, M0+ | `v6m` | No, the Security Extension is absent. `OS_CONFIG_TRUSTZONE_DISABLED` only |
| ARMv7-M / ARMv7E-M | M3 / M4, M7 | `v7m` | No. `OS_CONFIG_TRUSTZONE_DISABLED` only |
| ARMv8-M baseline | M23 | `v6m` | Yes, optional per device. All three `OS_CONFIG_TRUSTZONE` modes |
| ARMv8-M mainline | M33, M35P | `v8m` | Yes, optional per device. All three modes |
| ARMv8.1-M | M52, M55, M85 | `v8m` | Yes, optional per device. All three modes |

A few notes on the table. M4 and M7 are ARMv7E-M with the DSP extension, but
they are port-identical to the M3 here. The M23 is baseline, running a Thumb-1
subset, which is why it shares the `v6m` port rather than the mainline one. The
Cortex-M1 (ARMv6-M, FPGA) is deliberately not supported. "Optional per device"
means the Security Extension is a silicon-vendor choice and may also be disabled
in option bytes, in which case use `OS_CONFIG_TRUSTZONE_DISABLED`.

### Application callbacks

All user-overridable hooks are weak `_cb` functions, so overriding is optional
per function. For a clean starting point, copy `ahura_kernel/os_cb_template.c`
into the application source tree as `os_cb.c`, add it to the **application**
build (never to the kernel, where the template is deliberately absent from the
CMakeLists), and adapt:

- `os_clock_hz_get_cb` returns the CPU clock in Hz. See [Platform
  clock](#platform-clock).
- `os_tickless_pre_sleep_cb` and `os_tickless_post_sleep_cb` bracket the sleep.
- `os_arch_tz_context_save_cb` and `os_arch_tz_context_restore_cb` handle
  TrustZone secure-context banking, for non-secure kernels only.
- `os_arch_core_id_get_cb` and `os_arch_core_ipi_request_cb` are multi-core SoC
  glue, along with `os_arch_spinlock_acquire_cb` and `os_arch_spinlock_release_cb`
  on ARMv6-M multi-core SoCs, where they are mandatory.

### Platform clock

The kernel never reads a platform global directly. Every place that needs the
CPU frequency, such as the SysTick reload, `os_delay_us` busy-waits, and
tickless accounting, calls the weak callback:

```c
uint32_t os_clock_hz_get_cb(void);   /* return the CPU clock in Hz, 0 = unknown */
```

The default implementation covers the common cases without any code:

- With `OS_CONFIG_CPU_CLOCK_HZ` set above 0, it returns that fixed value. This
  suits platforms with a constant clock and no CMSIS.
- With `OS_CONFIG_CPU_CLOCK_HZ` at 0, the default, it returns the CMSIS
  `SystemCoreClock` global when the platform defines one. That is a weak
  reference, so linking never fails without it, and the callback returns 0
  instead.

Any other platform convention, such as a Zephyr-style config, a clock-driver
query, or dynamic frequency scaling, plugs in by overriding the callback in
application code. When the callback returns 0, tick setup and busy-wait delays
refuse to run and return `OS_STATUS_ERROR` rather than miscounting.

### TrustZone

`OS_CONFIG_TRUSTZONE` selects which security state the kernel runs in on ARMv8-M
cores (M23, M33, M35P, M52, M55, M85). The build fails with a clear `#error` on
cores without the Security Extension, or when the compile flags do not match the
chosen mode.

- `OS_CONFIG_TRUSTZONE_DISABLED` is the default, and the kernel ignores
  TrustZone. Use it on devices without the Security Extension, or with TrustZone
  disabled in option bytes.
- `OS_CONFIG_TRUSTZONE_SECURE` runs the kernel and every task in the secure
  state. Compile the kernel and the application with `-mcmse`. The context
  switch itself needs nothing extra, because the secure `EXC_RETURN` encoding
  equals the TrustZone-less one and the PSPLIM/MSPLIM guards stay active.
- `OS_CONFIG_TRUSTZONE_NON_SECURE` runs the kernel and its tasks non-secure,
  beside separate secure firmware. Initial task frames use the non-secure
  `EXC_RETURN` (`0xFFFFFFBC`), and the context switch calls two weak callbacks,
  which the application overrides following the `_cb` convention, so the
  secure-side glue can bank per-task secure contexts such as the secure stack
  and `PSP_S`. This mirrors how FreeRTOS's ARM_CM33 secure context management
  works:

  ```c
  void os_arch_tz_context_save_cb(uint32_t task_id);     /* before the switch, outgoing task */
  void os_arch_tz_context_restore_cb(uint32_t task_id);  /* after selection, incoming task   */
  ```

  A `task_id` of 0 is the idle task, which never owns a secure context. Tasks
  that never call secure functions need no handling at all, since the weak
  defaults do nothing.

### Multi-core (experimental)

`OS_CONFIG_CORE_COUNT` (default 1, max 31) declares how many cores schedule
tasks. Every scheduling core runs its own PendSV and SVC and its own idle task,
and pulls work from the shared ready lists. **Core affinity** selects where each
task may run:

- `os_task_config_t.core_affinity` is a bitmask of allowed cores, where bit n
  means core n. `OS_TASK_CORE_ANY` (0, the default) means any core. Change it at
  runtime with `os_task_core_affinity_set(task, mask)`. A task executing on a
  core the new mask excludes is asked to reschedule, either locally or by IPI.
- When a task becomes ready, through a wake or a start, the kernel preempts
  locally if the task's affinity allows this core. Otherwise it nudges the first
  core in the mask through `os_arch_core_ipi_request_cb`, whose weak default
  does nothing, in which case that core picks the task up at its own next tick.
- Core 0 boots the kernel as usual with `os_init` and `os_start`. Each secondary
  core is booted by the SoC layer, with a vector table pointing at the kernel's
  SVC, PendSV, and SysTick handlers, then calls `os_core_start()`. That
  configures the banked SHPR, SysTick, DWT, and MSPLIM for that core and enters
  the scheduler. It never returns.
- Core 0 owns the time base. Delays, timers, work queues, and `os_tick_get`
  advance only from core 0's tick, while ticks on other cores drive that core's
  preemption and round-robin. CPU usage through `os_cpu_usage_get` samples core 0.
- The kernel service tasks are placed with `OS_CONFIG_WORK_CORE_AFFINITY` and
  `OS_CONFIG_TIMER_CORE_AFFINITY`, both core-affinity bitmasks where 0 means any
  core, so work handlers and timer callbacks run where the config says.
- Critical sections are PRIMASK plus a global kernel spinlock with per-core
  nesting. The spinlock uses `LDREX/STREX` on ARMv7-M and ARMv8-M, while ARMv6-M
  multi-core SoCs such as the RP2040 must provide `os_arch_spinlock_acquire_cb`
  and `os_arch_spinlock_release_cb` backed by hardware spinlocks. A missing
  implementation fails at link time by design.
- The SoC layer supplies `os_arch_core_id_get_cb()`, whose weak default returns
  0, since Cortex-M has no architectural core-id register.

There is one constraint worth knowing: a task currently executing on another
core cannot be paused or deleted from this one, and the call returns
`OS_STATUS_BUSY`. Suspend it from its own core first. The SMP paths compile in
the CI matrix but have not run on real multi-core silicon yet, so treat them as
experimental.

### Tickless idle (experimental)

Config options: `OS_CONFIG_TICKLESS_ENABLE` (default 0),
`OS_CONFIG_TICKLESS_MIN_IDLE` (the shortest idle worth sleeping for), and
`OS_CONFIG_MAX_SUPPRESSED_TICKS`.

Two weak application callbacks bracket the sleep window, with prototypes in
`ahura.h`. Override them by defining the functions in application code.
User-overridable callbacks carry the `_cb` suffix by convention:

```c
void os_tickless_pre_sleep_cb(void)   { /* select sleep mode (e.g. SLEEPDEEP), gate clocks */ }
void os_tickless_post_sleep_cb(void)  { /* clear SLEEPDEEP, restore clocks */ }
```

**Suspend every other periodic interrupt source here too, not just SysTick.**
WFI wakes on any pending interrupt regardless of masking, so anything else
firing more often than the planned sleep will cut every suppressed sleep short
at its own period, no matter how long SysTick itself was reprogrammed for. A
common example is a HAL library's own tick redirected to a spare timer,
precisely so the RTOS can have SysTick to itself. On STM32, `HAL_SuspendTick()`
and `HAL_ResumeTick()` are the standard hook for this. A periodic ADC or comms
timer has the same effect. `os_tickless_pre_sleep_cb` and
`os_tickless_post_sleep_cb` are exactly where to pause and resume those sources.

**Status.** `os_tickless_expected_idle_ticks_get()` already bounds the planned
sleep by the earliest of the next software-timer expiry, the next ready work
item, and the next finite-delay task sleeper, so `os_delay_ms` waiters are
covered and not just timers.

On the ARMv8-M mainline port (`os_arch_port_v8m.c`, covering the M33, M35P, M52,
M55, and M85), `os_arch_sleep_prepare` and `os_arch_elapsed_ticks_get` now
really suppress SysTick for the sleep window. They reprogram its reload one tick
short of the plan, so the real final tick still fires normally and supplies the
last tick's accounting through the ordinary `os_tick_handler` path. This is the
same technique FreeRTOS's tickless idle uses. They also measure the real elapsed
time from SysTick itself rather than the DWT cycle counter, which is not
reliable across an actual sleep on most implementations.

Remaining work:

- The ARMv6-M and ARMv7-M/E-M ports (`os_arch_port_v6m.c`,
  `os_arch_port_v7m.c`) still need the identical fix. Same register layout, not
  yet ported over.
- The idle task still runs a plain `WFI` loop and does not yet call
  `os_tickless_idle_process()`. That function is exposed in `ahura.h` and
  exercised directly by the self-test suite through `test_tickless_sleep()`,
  ahead of the wiring landing.
- A deeper-sleep path, such as STOP mode where SysTick itself stops, would need
  an always-running wake and measurement source instead.
  `OS_CONFIG_LPTIM_CLOCK_HZ` is reserved for that but unused so far.

---

## Testing and examples

### Self-test suite

The suite is not copied into the application. It is a normal buildable module
with its own `CMakeLists.txt` (`ahura_kernel/test/CMakeLists.txt`), producing a
static library `os_test` that links against `ahura_kernel` and supplies the
strong override of the weak `os_test()`. Any project that already builds the
kernel can add it:

```cmake
add_subdirectory(ahura_kernel)
add_subdirectory(ahura_kernel/test)   # builds the os_test library

target_link_libraries(my_app PRIVATE
    ahura_kernel
    -Wl,--whole-archive
    os_test
    -Wl,--no-whole-archive
)
```

> **`--whole-archive` is required, not optional.** `os_test` only *overrides*
> the weak `os_test()`. It never adds a new undefined symbol for the linker to
> resolve. A normal static-library link only pulls in an archive member when
> something is still undefined at that point, and since `os_kernel.c.o` (pulled
> in for `os_init` and `os_start` anyway) already *defines* `os_test` weakly,
> the linker never looks inside `libos_test.a`. The entire suite then silently
> disappears from the build with no warning. Whole-archive forces every object
> into the link so the strong definition can win. Note that `os_cb.c` and
> `os_main.c` do not need this, because they are compiled directly into the
> application's object list rather than packaged into an archive.

Once linked, `os_init()` creates and starts the self-test task by itself, gated
by `OS_CONFIG_TEST_ENABLE`, which is off by default in the template so each
project opts in. There is nothing to call:

```c
os_init();   /* creates and starts tsk_test too, since os_test is linked and TEST_ENABLE=1 */
os_start();
```

`tsk_main` is **not** created alongside `tsk_test`, so the suite runs alone
rather than letting the application's own task race it for CPU time and
task-table slots. Set `OS_CONFIG_TEST_ENABLE` back to `0` to get `tsk_main`
running normally again.

The task runs `os_test()` once. It exercises whichever
`OS_CONFIG_<FEATURE>_ENABLE` switches are on, covering tasks, delays, critical
sections, mutexes and priority inheritance, semaphores, queues, event groups,
task notifications, timers, work items, the kernel heap, stack watermarks, CPU
usage, and the intrusive list. It prints a detailed PASS/FAIL log via `printf`
with a pass/fail summary, then finishes with a **benchmark table**: each hot
kernel path timed with the CPU cycle counter, sampled repeatedly with the
minimum kept, and the measurement overhead subtracted. The header reports the
core profile, optimization level, and clocks, so a result is always
interpretable.

The suite depends on nothing but `ahura.h`, with no board or HAL headers, so it
runs on real hardware for any arch or board the kernel supports. Retarget
`printf`'s destination, typically a UART, in the application to see the log.

> Benchmark numbers from a `-O0` debug build run several times slower than a
> release build. The table says which kind of build produced it, so compare like
> with like.

### Examples

[`ahura_examples/kernel/`](../ahura_examples/kernel/) has one small, focused
example per kernel feature, each meant to be copied over `os_main.c`. Same rule
as the self-test suite: they depend on nothing but `ahura.h`, with no board or
HAL headers.

| Example | Demonstrates |
|---|---|
| `os_main_hello.c` | The minimal application: `os_main()`, `os_delay_ms`, `printf` |
| `os_main_task.c` | Task lifecycle: create, start, pause, resume, delete |
| `os_main_delay.c` | `os_delay_ms`, `os_delay_us`, `os_delay_s` |
| `os_main_critical.c` | Critical sections protecting a shared counter |
| `os_main_mutex.c` | Mutual exclusion with `os_mutex_*` |
| `os_main_semaphore.c` | Counting semaphore, producer and consumer |
| `os_main_queue.c` | Message queue, producer and consumer |
| `os_main_event.c` | Event group, waiting on multiple bits |
| `os_main_notify.c` | Task notifications with `os_task_notify_*` |
| `os_main_timer.c` | One-shot and periodic software timers |
| `os_main_work.c` | Deferrable work queue |
| `os_main_mem.c` | Kernel heap with `os_mem_alloc` and `os_mem_free` |
| `os_main_stack_watermark.c` | Worst-case stack headroom |
| `os_main_cpu_usage.c` | CPU load sampling |
| `os_main_list.c` | The intrusive list utility |

Each file needs its matching `OS_CONFIG_<FEATURE>_ENABLE` on, and a compile-time
`#error` says so if it is not. Each one is a complete, standalone `os_main()`,
so copy any of them over the project's `os_main.c` to see it run.

---

## Internals

### Source layout

#### Top-level files

- `ahura.h` is the public umbrella API, and the only header applications
  include. It declares `os_main()` and `os_test()` as well, even though the
  kernel only ships weak defaults for them, because overriding or linking the
  real body is the application's job. Neither carries the `_cb` suffix used
  elsewhere in this header. That suffix is reserved for callbacks the kernel
  queries for platform behavior, such as `os_clock_hz_get_cb` and
  `os_tickless_pre_sleep_cb`, whereas `os_main()` and `os_test()` are where the
  application's or suite's own code runs, even though they are wired up the same
  way with a weak default and a strong override.
- `os_config_template.h` is the template for the application's `os_config.h`. It
  lists every build-time option at its default value: tick rate, task and timer
  limits, stack sizes, heap size, TrustZone mode, core count, and the per-feature
  `OS_CONFIG_<FEATURE>_ENABLE` switches for mutex (always with single-level
  priority inheritance, like FreeRTOS and Zephyr), semaphore, queue, event,
  timer, work, task notifications, alloc, stack watermark, CPU usage, the
  default application task, and the self-test task. The intrusive list module
  has no switch, since the scheduler runs on it. This file is never included by
  the kernel, so copy it into the project. See
  [Configuration](#configuration). Disabling a feature compiles out its code and
  API, and disabling timer, work, the default task, or the self-test task also
  removes the corresponding kernel service task and its stack.
- `os_cb_template.c` is the template for the application-side callbacks, and is
  deliberately not compiled into the kernel. See [Application
  callbacks](#application-callbacks).
- `os_main_template.c` is the template for the default application task's body,
  also deliberately not compiled into the kernel. See [Default application
  task](#default-application-task).
- `test/` holds the kernel self-test suite (`os_test.c`), its own buildable
  module with the target `os_test` and its own `CMakeLists.txt`. See [Self-test
  suite](#self-test-suite).

#### `core/` portable kernel modules

All filenames are `os_`-prefixed:

- `os_kernel.c` covers the lifecycle (`os_init`, `os_start`, the running flag),
  the platform clock callback (`os_clock_hz_get_cb`, see [Platform
  clock](#platform-clock)), the default application task (`os_main`, see
  [Default application task](#default-application-task)), and the self-test task
  (`os_test`, see [Self-test suite](#self-test-suite)).
- `os_mem.c` is the kernel heap (`os_mem_alloc` and `os_mem_free`), a first-fit
  allocator with coalescing over a static `OS_CONFIG_HEAP_SIZE` heap.
- `os_task.c` holds the static TCB pool and O(1) list-based scheduling: one FIFO
  ready list per priority plus a ready bitmap, where the highest set bit is the
  next priority to run and costs a single `CLZ` on ARMv7-M and up, round-robin
  by list rotation, and a delay list holding only the finite-delay sleepers.
  This is also where mutex priority inheritance's effective-priority changes and
  task notifications (`os_task_notify_give` and `os_task_notify_wait`) live,
  since both are entirely about the TCB rather than separate kernel objects.
- `os_tick.c` is the tick counter and tick handler, which wakes delays, drives
  timers, and preempts.
- `os_delay.c` provides blocking millisecond and second delays plus a
  DWT-precise microsecond busy-wait.
- `os_critical.c` implements PRIMASK-based nesting critical sections.
- `os_mutex.c`, `os_semaphore.c`, `os_queue.c`, and `os_event.c` are the sync
  and IPC primitives with `timeout_ms` waits.
- `os_timer.c` holds the software timers. Expiry is detected by the tick and
  callbacks run on the kernel timer task `tsk_timer` at the highest priority.
- `os_work.c` is the deferrable work queue in the style of Zephyr. Items run on
  the kernel work task `tsk_work`, also at the highest priority.
- `os_list.c` is the intrusive doubly-linked list. It is always compiled, since
  the scheduler itself runs on it and it cannot be configured out, and it is
  also public API.
- `os_internal.h` is the internal cross-module contract, not for applications.

#### `arch/arm/` port layer

This layer covers the SysTick tick source, SVC first-task start, PendSV context
switch, initial stack frames, and the cycle counter. Shared code is organized by
architecture, the same split Zephyr and CMSIS-RTX use: one v6m implementation,
one v7m implementation, one v8m implementation, with thin per-core wrapper
folders on top.

- `common/os_arch_port_v7m.c` is the ARMv7-M (M3) and ARMv7E-M (M4, M7)
  implementation. It is Thumb-2 and FPU-aware, saving `s16-s31` and a per-task
  `EXC_RETURN` when built with a hard or softfp float ABI.
- `common/os_arch_port_v8m.c` is the ARMv8-M mainline (M33, M35P) and ARMv8.1-M
  (M52, M55, M85) implementation. It is a superset of the v7m port that always
  saves and restores `PSPLIM` per task and programs `MSPLIM` for the handler
  stack when the linker script provides the stack-bottom symbol, so a stack
  overflow raises a UsageFault instead of silently corrupting memory. TrustZone,
  in all three `OS_CONFIG_TRUSTZONE` modes, lives here.
- `common/os_arch_port_v6m.c` is the ARMv6-M (M0, M0+) and ARMv8-M baseline
  (M23) implementation. It uses the Thumb-1 subset and has no FPU, and the cycle
  counter is synthesized from SysTick because these cores have no DWT CYCCNT.
  Baseline does not belong in the v8m file because it cannot execute the
  mainline Thumb-2 ISA, so its TrustZone support is handled here. Non-secure
  v8-M baseline has no `PSPLIM`, so there is no stack-limit handling.
- Each shared file carries a `#error` guard against being compiled for the wrong
  architecture profile.
- `cortex_m0/`, `cortex_m0plus/`, and `cortex_m23/` are thin wrappers over the
  v6m port.
- `cortex_m3/`, `cortex_m4/`, and `cortex_m7/` are thin wrappers over the v7m
  port. The M7 additionally relies on the DWT LAR unlock done in `os_arch_init`.
- `cortex_m33/`, `cortex_m35p/`, `cortex_m52/`, `cortex_m55/`, and
  `cortex_m85/` are thin wrappers over the v8m port. On the v8.1-M cores,
  Helium/MVE state is covered by the existing s16-s31 save plus hardware lazy
  stacking of s0-s15, FPSCR, and VPR. The folder names follow GCC's `-mcpu`
  spelling, so it is `cortex_m0plus` because the core is the M0"plus", but
  `cortex_m35p` because that core's "P" means physical security rather than plus
  (`-mcpu=cortex-m35p`).
- The build selects the variant from `-mcpu`, falling back to `-march`, so
  `armv8.1-m.main` maps to `cortex_m55` and so on. All folders of one profile
  include the same shared port, so any core of the right architecture is
  equivalent. See `ahura_kernel/CMakeLists.txt`. Override the choice with
  `-DOS_ARCH_VARIANT=cortex_m4`. Note that GCC learned `-mcpu=cortex-m52` in GCC
  14, so older toolchains build that core with `-march=armv8.1-m.main+mve.fp`,
  which the fallback resolves automatically.
- The `MSPLIM` guard is active when the linker script provides the bottom of the
  main stack as `__StackLimit`, used by CMSIS-style scripts, or `_sstack`, used
  by several vendor-generated ones. Both are weak references, so either naming
  works unmodified, and when neither symbol exists the guard is skipped.
- TrustZone, the ARMv8-M Security Extension, is selected with
  `OS_CONFIG_TRUSTZONE`. See [TrustZone](#trustzone).
- Not covered yet: PAC/BTI (`-mbranch-protection` on the M85).

## Notes and constraints

- Do not block, whether by delaying or locking with a timeout, inside a critical
  section or an ISR.
- Work handlers and timer callbacks run on the highest-priority kernel tasks, so
  keep them short and non-blocking or user tasks will starve.
- Timers run in two modes, `OS_TIMER_MODE_ONE_SHOT` which fires once then stops,
  and `OS_TIMER_MODE_PERIODIC` which reloads every period. Select the mode in
  `os_timer_init`.
- Mutexes are task-only and non-recursive. See [Mutexes and priority
  inheritance](#mutexes-and-priority-inheritance).
- The project builds with the hard-float ABI, and the port saves and restores
  the FPU context (`s16-s31` plus a per-task `EXC_RETURN`) automatically.
