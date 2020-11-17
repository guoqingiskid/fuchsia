// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2014 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_KERNEL_MP_H_
#define ZIRCON_KERNEL_INCLUDE_KERNEL_MP_H_

#include <limits.h>
#include <stdint.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

#include <fbl/intrusive_double_list.h>
#include <kernel/cpu.h>
#include <kernel/mutex.h>
#include <kernel/thread.h>
#include <ktl/atomic.h>

// NOTE(abdulla): This is located here to break a circular dependency.
enum interrupt_eoi {
  // Deactivate and drop priority of the interrupt.
  IRQ_EOI_DEACTIVATE = 0,
  // Only drop priority of the interrupt.
  IRQ_EOI_PRIORITY_DROP = 1,
};

typedef void (*mp_ipi_task_func_t)(void* context);
typedef void (*mp_sync_task_t)(void* context);

// by default, mp_reschedule does not signal to cpus that are running realtime
// threads. Override this behavior.
#define MP_RESCHEDULE_FLAG_REALTIME (0x1)

typedef enum { MP_IPI_GENERIC, MP_IPI_RESCHEDULE, MP_IPI_INTERRUPT, MP_IPI_HALT } mp_ipi_t;

// When sending inter processor interrupts (IPIs), apis will take a combination of
// this enum and a bitmask. If MP_IPI_TARGET_MASK is used, the mask argument will
// contain a bitmap of every cpu that should receive the IPI. The other targets
// serve as shortcuts and potentially optimizations in the lower layers.
typedef enum { MP_IPI_TARGET_MASK, MP_IPI_TARGET_ALL, MP_IPI_TARGET_ALL_BUT_LOCAL } mp_ipi_target_t;

void mp_init(void);
void mp_prepare_current_cpu_idle_state(bool idle);

// Trigger a reschedule on another cpu. Used mostly by inner threading
// and scheduler logic. Must be holding the thread lock.
void mp_reschedule(cpu_mask_t mask, uint flags) TA_REQ(thread_lock);

// Trigger an interrupt on another cpu without a corresponding reschedule.
// Used by the hypervisor to trigger a vmexit.
void mp_interrupt(mp_ipi_target_t, cpu_mask_t mask);

// Make a cross cpu call to one or more cpus. Waits for all of the calls
// to complete before returning.
void mp_sync_exec(mp_ipi_target_t, cpu_mask_t mask, mp_sync_task_t task, void* context);

zx_status_t mp_hotplug_cpu_mask(cpu_mask_t mask);

// Unplug the cpu specified by |mask|, waiting, up to |deadline| for its "shutdown" thread to
// complete.
//
// If |leaked_thread| is non-null and a "shutdown" thread was created, it will be assigned to
// |leaked_thread| so the caller can |Forget| it.
zx_status_t mp_unplug_cpu_mask(cpu_mask_t mask, zx_time_t deadline,
                               Thread** leaked_thread = nullptr);

static inline zx_status_t mp_hotplug_cpu(cpu_num_t cpu) {
  return mp_hotplug_cpu_mask(cpu_num_to_mask(cpu));
}
static inline zx_status_t mp_unplug_cpu(cpu_num_t cpu) {
  return mp_unplug_cpu_mask(cpu_num_to_mask(cpu), ZX_TIME_INFINITE);
}

// called from arch code during reschedule irq
interrupt_eoi mp_mbx_reschedule_irq(void*);
// called from arch code during generic task irq
interrupt_eoi mp_mbx_generic_irq(void*);
// called from arch code during interrupt irq
interrupt_eoi mp_mbx_interrupt_irq(void*);

// represents a pending task for some number of CPUs to execute
struct mp_ipi_task
    : fbl::DoublyLinkedListable<mp_ipi_task*, fbl::NodeOptions::AllowRemoveFromContainer> {
  mp_ipi_task_func_t func;
  void* context;
};

// global mp state to track what the cpus are up to
struct mp_state {
  // cpus that are currently online
  ktl::atomic<cpu_mask_t> online_cpus;
  // cpus that are currently schedulable
  ktl::atomic<cpu_mask_t> active_cpus;

  // both are only safely accessible with thread lock held
  cpu_mask_t idle_cpus TA_GUARDED(thread_lock);
  cpu_mask_t realtime_cpus TA_GUARDED(thread_lock);

  SpinLock ipi_task_lock;
  // list of outstanding tasks for CPUs to execute.  Should only be
  // accessed with the ipi_task_lock held
  fbl::DoublyLinkedList<mp_ipi_task*> ipi_task_list[SMP_MAX_CPUS] TA_GUARDED(ipi_task_lock);

  // lock for serializing CPU hotplug/unplug operations
  DECLARE_LOCK(mp_state, Mutex) hotplug_lock;
};

extern struct mp_state mp;

// idle/busy is used to track if the cpu is running anything or has a non empty run queue
// idle == (cpu run queue empty & cpu running idle thread)
// busy == !idle
// must be called with the thread lock held
static inline cpu_mask_t mp_get_idle_mask(void) TA_REQ(thread_lock) { return mp.idle_cpus; }

static inline void mp_set_cpu_idle(cpu_num_t cpu) TA_REQ(thread_lock) {
  mp.idle_cpus |= cpu_num_to_mask(cpu);
}

static inline void mp_set_cpu_busy(cpu_num_t cpu) TA_REQ(thread_lock) {
  mp.idle_cpus &= ~cpu_num_to_mask(cpu);
}

static inline int mp_is_cpu_idle(cpu_num_t cpu) TA_REQ(thread_lock) {
  return mp_get_idle_mask() & cpu_num_to_mask(cpu);
}

// marks whether or not a cpu is currently running a realtime thread or not
// must be called with the thread lock held
static inline void mp_set_cpu_realtime(cpu_num_t cpu) TA_REQ(thread_lock) {
  mp.realtime_cpus |= cpu_num_to_mask(cpu);
}

static inline void mp_set_cpu_non_realtime(cpu_num_t cpu) TA_REQ(thread_lock) {
  mp.realtime_cpus &= ~cpu_num_to_mask(cpu);
}

static inline cpu_mask_t mp_get_realtime_mask(void) TA_REQ(thread_lock) { return mp.realtime_cpus; }

// tracks if a cpu is online and initialized
static inline void mp_set_curr_cpu_online(bool online) {
  if (online) {
    mp.online_cpus.fetch_or(cpu_num_to_mask(arch_curr_cpu_num()));
  } else {
    mp.online_cpus.fetch_and(~cpu_num_to_mask(arch_curr_cpu_num()));
  }
}

static inline cpu_mask_t mp_get_online_mask(void) {
  return mp.online_cpus.load();
}

static inline int mp_is_cpu_online(cpu_num_t cpu) {
  return mp_get_online_mask() & cpu_num_to_mask(cpu);
}

// tracks if a cpu is active and schedulable
static inline void mp_set_curr_cpu_active(bool active) {
  if (active) {
    mp.active_cpus.fetch_or(cpu_num_to_mask(arch_curr_cpu_num()));
  } else {
    mp.active_cpus.fetch_and(~cpu_num_to_mask(arch_curr_cpu_num()));
  }
  arch_set_blocking_disallowed(!active);
}

static inline cpu_mask_t mp_get_active_mask(void) {
  return mp.active_cpus.load();
}

static inline int mp_is_cpu_active(cpu_num_t cpu) {
  return mp_get_active_mask() & cpu_num_to_mask(cpu);
}

// Wait until all of the CPUs in the system have started up.
//
// Note: Do not call this until at least LK_INIT_LEVEL_PLATFORM + 1, or later.
// PLATFORM is the point at which CPUs check in.  If a call it made to wait
// before this, there is a chance that we are on the primary CPU and before the
// point that CPUs have been told to start, or that we are on a secondary CPU
// during early startup, and we have not reached our check-in point yet.
//
// Calling this function at a point in a situation like that is a guaranteed
// timeout.
zx_status_t mp_wait_for_all_cpus_started(Deadline deadline);

#endif  // ZIRCON_KERNEL_INCLUDE_KERNEL_MP_H_
