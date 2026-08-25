/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "amd_hsa_signal.h"
#include "core/driver/dynamic/rocr_dynamic_driver.h"
#include "dynaccel_packet.h"

#define DYNACCEL_QUEUE_PKTS 64

struct dynaccel_alloc;
struct dynaccel_queue;

struct rocr_dynamic_driver_context_t {
  int udmabuf_fd;              /* -1 => plain-mmap fallback */
  /* Guards list membership of allocs and queues only. It is deliberately
   * not held across munmap/close/dup, so free/export mutual exclusion on a
   * single allocation is not enforced by this lock - see the note above
   * dynaccel_free_memory. */
  pthread_mutex_t lock;
  struct dynaccel_alloc* allocs;
  struct dynaccel_queue* queues;
  uint64_t next_id;
};

/* ---- Lifecycle ---- */

static hsa_status_t dynaccel_init(rocr_dynamic_driver_context_t* ctx) {
  (void)ctx;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_shut_down(rocr_dynamic_driver_context_t* ctx) {
  (void)ctx;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_open(rocr_dynamic_driver_context_t* ctx) {
  (void)ctx;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_close(rocr_dynamic_driver_context_t* ctx) {
  (void)ctx;
  return HSA_STATUS_SUCCESS;
}

/* ---- Memory ---- */

struct dynaccel_alloc {
  uint64_t id;
  int dmabuf_fd;               /* -1 => plain mmap */
  void* ptr;
  size_t size;                 /* page-rounded */
  struct dynaccel_alloc* next;
};

static size_t dynaccel_round_up_page(size_t size) {
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  return (size + page - 1) & ~(page - 1);
}

/* Returns dmabuf fd, or -1 to signal the caller to fall back to plain mmap. */
static int dynaccel_make_udmabuf(int udmabuf_fd, size_t size) {
  if (udmabuf_fd < 0) return -1;

  int memfd = memfd_create("dynaccel", MFD_ALLOW_SEALING);
  if (memfd < 0) return -1;

  int dmabuf_fd = -1;
  if (ftruncate(memfd, (off_t)size) == 0 &&
      fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0) {
    struct udmabuf_create create = {
        .memfd = (uint32_t)memfd, .flags = 0, .offset = 0, .size = size};
    dmabuf_fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &create);
    if (dmabuf_fd < 0) dmabuf_fd = -1;
  }

  close(memfd);   /* udmabuf holds its own reference to the pages */
  return dmabuf_fd;
}

static hsa_status_t dynaccel_allocate_memory(rocr_dynamic_driver_context_t* ctx, uint32_t node_id,
                                             size_t size, uint32_t alloc_flags,
                                             HsaMemFlags mem_flags, void** mem) {
  (void)node_id; (void)alloc_flags; (void)mem_flags;
  if (!ctx || !mem || size == 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  const size_t rounded = dynaccel_round_up_page(size);
  const int dmabuf_fd = dynaccel_make_udmabuf(ctx->udmabuf_fd, rounded);

  void* ptr = (dmabuf_fd >= 0)
      ? mmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0)
      : mmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED) {
    if (dmabuf_fd >= 0) close(dmabuf_fd);
    return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  }

  struct dynaccel_alloc* a = calloc(1, sizeof(*a));
  if (!a) {
    munmap(ptr, rounded);
    if (dmabuf_fd >= 0) close(dmabuf_fd);
    return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  }

  pthread_mutex_lock(&ctx->lock);
  a->id = ++ctx->next_id;
  a->dmabuf_fd = dmabuf_fd;
  a->ptr = ptr;
  a->size = rounded;
  a->next = ctx->allocs;
  ctx->allocs = a;
  pthread_mutex_unlock(&ctx->lock);

  *mem = ptr;
  return HSA_STATUS_SUCCESS;
}

/* Caller must hold ctx->lock. */
static struct dynaccel_alloc* dynaccel_find_alloc(rocr_dynamic_driver_context_t* ctx, void* ptr) {
  for (struct dynaccel_alloc* a = ctx->allocs; a; a = a->next) {
    if (a->ptr == ptr) return a;
  }
  return NULL;
}

/* Imported allocations are keyed by id rather than ptr: at import time the
 * destination VA is not known yet (it is supplied later by map()), so ptr
 * cannot serve as the lookup key the way it does for locally-owned
 * allocations. Caller must hold ctx->lock. */
static struct dynaccel_alloc* dynaccel_find_alloc_by_id(rocr_dynamic_driver_context_t* ctx,
                                                        uint64_t id) {
  for (struct dynaccel_alloc* a = ctx->allocs; a; a = a->next) {
    if (a->id == id) return a;
  }
  return NULL;
}

/* Unlinks under ctx->lock, then munmaps/closes/frees outside it - so a
 * concurrent export_memory_handle that has already read a->dmabuf_fd could
 * in principle dup() an fd number this call is about to close and the
 * kernel could reuse. That race is not reachable today only because ROCr
 * serializes the two callers above the driver: Runtime::DmaBufExport holds
 * memory_lock_ shared for its whole body, and Runtime::FreeMemory must take
 * that lock exclusively before it can reach this function. A driver copying
 * this pattern for a caller that does not offer the same guarantee must
 * refcount the allocation instead, so a free cannot complete while an
 * export still holds a reference to it. Shared by free_memory and
 * destroy_memory_handle, which differ only in whose allocation table entry
 * they release. */
static hsa_status_t dynaccel_unlink_and_release(rocr_dynamic_driver_context_t* ctx, void* mem) {
  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_alloc** link = &ctx->allocs;
  while (*link && (*link)->ptr != mem) link = &(*link)->next;
  struct dynaccel_alloc* a = *link;
  if (a) *link = a->next;
  pthread_mutex_unlock(&ctx->lock);

  if (!a) return HSA_STATUS_ERROR_INVALID_ALLOCATION;

  munmap(a->ptr, a->size);
  if (a->dmabuf_fd >= 0) close(a->dmabuf_fd);
  free(a);
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_free_memory(rocr_dynamic_driver_context_t* ctx, void* mem,
                                         size_t size) {
  (void)size;
  if (!ctx || !mem) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  return dynaccel_unlink_and_release(ctx, mem);
}

static hsa_status_t dynaccel_export_memory_handle(rocr_dynamic_driver_context_t* ctx,
                                                  uint32_t node_id,
                                                  const rocr_dynamic_driver_memory_handle_t* handle,
                                                  int share_type, void* export_handle) {
  (void)node_id;
  if (!ctx || !handle || !export_handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  if (share_type != ROCR_DYNAMIC_SHARE_DMABUF_FD) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  /* Locally-owned allocations are keyed by ptr (handle->handle == ptr);
   * imported allocations are keyed by id instead (see import_memory_handle),
   * so a handle produced by import_memory_handle must be looked up by id to
   * support re-exporting it. */
  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_alloc* a = dynaccel_find_alloc(ctx, (void*)(uintptr_t)handle->handle);
  if (!a) a = dynaccel_find_alloc_by_id(ctx, handle->handle);
  const int fd = a ? a->dmabuf_fd : -1;
  pthread_mutex_unlock(&ctx->lock);

  if (!a) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
  if (fd < 0) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;  /* mmap fallback: not exportable */

  const int dup_fd = dup(fd);
  if (dup_fd < 0) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  *(int*)export_handle = dup_fd;
  return HSA_STATUS_SUCCESS;
}

/* Imports only DMABUF_FD handles: FABRIC_HANDLE sharing is cross-node and this
 * driver only ever runs within a single machine. The imported allocation gets
 * its own dynaccel_alloc entry, keyed by a freshly assigned id rather than by
 * ptr, since the destination VA is not known until a later map() call (mem is
 * always NULL on the runtime's actual call path; the "bypass import" case
 * documented in the header is honored by recording mem as the ptr directly). */
static hsa_status_t dynaccel_import_memory_handle(rocr_dynamic_driver_context_t* ctx,
                                                  uint32_t node_id,
                                                  rocr_dynamic_driver_memory_handle_t* handle,
                                                  int share_type, void* import_handle, void* mem) {
  (void)node_id;
  if (!ctx || !handle || !import_handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  if (share_type != ROCR_DYNAMIC_SHARE_DMABUF_FD) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  const rocr_dynamic_driver_memory_handle_t* src = import_handle;
  if (src->dmabuf_fd < 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  const int dup_fd = dup(src->dmabuf_fd);
  if (dup_fd < 0) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;

  struct dynaccel_alloc* a = calloc(1, sizeof(*a));
  if (!a) {
    close(dup_fd);
    return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  }

  pthread_mutex_lock(&ctx->lock);
  a->id = ++ctx->next_id;
  a->dmabuf_fd = dup_fd;
  a->ptr = mem;
  a->size = src->size;
  a->next = ctx->allocs;
  ctx->allocs = a;
  pthread_mutex_unlock(&ctx->lock);

  *handle = (rocr_dynamic_driver_memory_handle_t){0};
  handle->handle = a->id;
  handle->vaddr = mem;
  handle->dmabuf_fd = dup_fd;
  handle->size = a->size;
  return HSA_STATUS_SUCCESS;
}

/* This driver never needs to leave the allocating process to make an allocation
 * shareable, so create_shareable_handle is a validating no-op: the allocation
 * handle produced by allocate_memory is already usable as a shareable handle
 * (real cross-process sharing goes through export_memory_handle/
 * import_memory_handle instead). */
static hsa_status_t dynaccel_create_shareable_handle(rocr_dynamic_driver_context_t* ctx, void* va,
                                                     void* mem, size_t size, uint32_t node_id,
                                                     rocr_dynamic_driver_memory_handle_t* handle,
                                                     uint64_t* offset) {
  (void)va; (void)size; (void)node_id; (void)handle;
  if (!ctx || !mem || !offset) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_alloc* a = dynaccel_find_alloc(ctx, mem);
  pthread_mutex_unlock(&ctx->lock);
  if (!a) return HSA_STATUS_ERROR_INVALID_ALLOCATION;

  *offset = 0;
  return HSA_STATUS_SUCCESS;
}

static int dynaccel_perms_to_prot(int perms) {
  int prot = PROT_NONE;
  if (perms & HSA_ACCESS_PERMISSION_RO) prot |= PROT_READ;
  if (perms & HSA_ACCESS_PERMISSION_RW) prot |= PROT_WRITE;
  return prot;
}

/* Maps a slice of an imported allocation's dmabuf at the caller-chosen VA.
 * mem is always a page inside a VA range the runtime already reserved
 * (PROT_NONE, MAP_ANONYMOUS) before calling map(), so MAP_FIXED here only
 * ever replaces that reservation's own mapping - never a foreign one. */
static hsa_status_t dynaccel_map(rocr_dynamic_driver_context_t* ctx, uint32_t node_id,
                                 const rocr_dynamic_driver_memory_handle_t* handle, void* mem,
                                 size_t offset, size_t size, int perms) {
  (void)node_id;
  if (!ctx || !handle || !mem || size == 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_alloc* a = dynaccel_find_alloc_by_id(ctx, handle->handle);
  const int fd = a ? a->dmabuf_fd : -1;
  const size_t alloc_size = a ? a->size : 0;
  pthread_mutex_unlock(&ctx->lock);
  if (!a) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
  if (fd < 0) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
  if (offset > alloc_size || size > alloc_size - offset) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  if (mmap(mem, size, dynaccel_perms_to_prot(perms), MAP_SHARED | MAP_FIXED, fd,
           (off_t)offset) == MAP_FAILED) {
    return HSA_STATUS_ERROR;
  }

  pthread_mutex_lock(&ctx->lock);
  a->ptr = mem;
  pthread_mutex_unlock(&ctx->lock);
  return HSA_STATUS_SUCCESS;
}

/* Restores the PROT_NONE/anonymous mapping the runtime's VA reservation had
 * before map() overwrote it with the dmabuf mapping, rather than leaving a
 * bare hole behind: mirrors the CPU-agent path (RemoveAccess() -> mprotect
 * PROT_NONE) which preserves the reservation for a later re-map instead of
 * releasing the address range. */
static hsa_status_t dynaccel_unmap(rocr_dynamic_driver_context_t* ctx, uint32_t node_id,
                                   const rocr_dynamic_driver_memory_handle_t* handle, void* mem,
                                   size_t offset, size_t size) {
  (void)node_id;
  if (!ctx || !handle || !mem || size == 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_alloc* a = dynaccel_find_alloc_by_id(ctx, handle->handle);
  const size_t alloc_size = a ? a->size : 0;
  pthread_mutex_unlock(&ctx->lock);
  if (!a) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
  if (offset > alloc_size || size > alloc_size - offset) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  if (mmap(mem, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
    return HSA_STATUS_ERROR;
  }

  pthread_mutex_lock(&ctx->lock);
  a->ptr = NULL;
  pthread_mutex_unlock(&ctx->lock);
  return HSA_STATUS_SUCCESS;
}

/* Imported allocations are keyed by id (see import_memory_handle above), so
 * they cannot go through dynaccel_unlink_and_release's ptr-based lookup -
 * this is the id-keyed equivalent. Guards the munmap on ptr being set since
 * a handle destroyed before any map() call was ever made has none. */
static hsa_status_t dynaccel_unlink_and_release_by_id(rocr_dynamic_driver_context_t* ctx,
                                                      uint64_t id) {
  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_alloc** link = &ctx->allocs;
  while (*link && (*link)->id != id) link = &(*link)->next;
  struct dynaccel_alloc* a = *link;
  if (a) *link = a->next;
  pthread_mutex_unlock(&ctx->lock);

  if (!a) return HSA_STATUS_ERROR_INVALID_ALLOCATION;

  if (a->ptr) munmap(a->ptr, a->size);
  if (a->dmabuf_fd >= 0) close(a->dmabuf_fd);
  free(a);
  return HSA_STATUS_SUCCESS;
}

/* Because create_shareable_handle above never allocates a separate resource,
 * this is the sole teardown path for VMem-created handles (they are never
 * passed to free_memory) - so it must release the same allocation
 * free_memory would. See the race note above free_memory: the same
 * unlink-then-release ordering applies here. Also handles import_memory_handle
 * handles, which are id-keyed rather than ptr-keyed - see
 * dynaccel_find_alloc_by_id. */
static hsa_status_t dynaccel_destroy_memory_handle(rocr_dynamic_driver_context_t* ctx,
                                                   rocr_dynamic_driver_memory_handle_t* handle) {
  if (!ctx || !handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  pthread_mutex_lock(&ctx->lock);
  const bool is_import = dynaccel_find_alloc_by_id(ctx, handle->handle) != NULL;
  pthread_mutex_unlock(&ctx->lock);

  const hsa_status_t status = is_import
      ? dynaccel_unlink_and_release_by_id(ctx, handle->handle)
      : dynaccel_unlink_and_release(ctx, (void*)(uintptr_t)handle->handle);
  if (status != HSA_STATUS_SUCCESS) return status;

  *handle = (rocr_dynamic_driver_memory_handle_t){0};
  return HSA_STATUS_SUCCESS;
}

/* ---- Queues ---- */

struct dynaccel_queue {
  uint64_t id;
  _Alignas(64) uint64_t doorbell;  /* ROCr writes; worker polls */
  void* ring;
  uint32_t num_pkts;
  uint64_t* read_index;            /* -> amd_queue_.read_dispatch_id */
  atomic_bool run;
  pthread_t thread;
  struct dynaccel_queue* next;
};

/* Completes a signal the way hardware does -- see trap_handler.s send_signal().
   Never calls into the HSA runtime. */
static void dynaccel_complete(hsa_signal_t sig) {
  if (!sig.handle) return;
  amd_signal_t* s = (amd_signal_t*)(uintptr_t)sig.handle;

  atomic_fetch_sub_explicit((_Atomic int64_t*)&s->value, 1, memory_order_release);

  /* Mirrors the GPU. We cannot raise the interrupt, but every ROCr waiter
     re-reads value each loop iteration, so the store is always observed. */
  if (s->event_mailbox_ptr && s->event_id) {
    atomic_store_explicit((_Atomic uint32_t*)(uintptr_t)s->event_mailbox_ptr, s->event_id,
                          memory_order_release);
  }
}

/* Every standard AQL packet (kernel dispatch, agent dispatch, barrier and/or)
   places its completion signal in the last 8 bytes of the 64-byte packet.
   DynAccel's own dispatch packet (type DYNACCEL_PACKET_TYPE_READY) does not
   follow that convention -- see dynaccel_dispatch_packet_t -- so its
   completion_signal field must be read separately. */
static hsa_signal_t dynaccel_standard_completion_signal(const void* pkt) {
  hsa_signal_t sig;
  memcpy(&sig, (const char*)pkt + 64 - sizeof(sig), sizeof(sig));
  return sig;
}

static void dynaccel_execute(struct dynaccel_queue* q, uint64_t index) {
  dynaccel_dispatch_packet_t* p =
      &((dynaccel_dispatch_packet_t*)q->ring)[index % q->num_pkts];

  const uint16_t header = __atomic_load_n(&p->header, __ATOMIC_ACQUIRE);
  const uint16_t type = (header >> HSA_PACKET_HEADER_TYPE) & 0xFF;

  hsa_signal_t completion_signal;
  if (type == DYNACCEL_PACKET_TYPE_READY) {
    completion_signal = p->completion_signal;
    if (p->opcode == DYNACCEL_OPCODE_DISPATCH && p->function) {
      ((dynaccel_kernel_t)(uintptr_t)p->function)((uint64_t*)p->kernarg_address, p->num_kernargs);
    }
  } else {
    /* Kernel dispatch / agent dispatch / barrier and/or: DynAccel does not
       execute these, only completes them. A single in-order worker per
       queue already satisfies barrier semantics.

       This branch also matches HSA_PACKET_TYPE_INVALID (1) and would read a
       bogus completion signal at offset 56 for it. That is harmless only under
       the invariant that a submitter never rings the doorbell for a slot whose
       header is still INVALID -- the doorbell/read_index protocol never asks
       the worker to execute a slot it hasn't first seen published with a
       non-INVALID type, so this branch is unreachable for INVALID in practice. */
    completion_signal = dynaccel_standard_completion_signal(p);
  }

  dynaccel_complete(completion_signal);
  __atomic_store_n(&p->header,
                   (uint16_t)(DYNACCEL_PACKET_TYPE_INVALID << HSA_PACKET_HEADER_TYPE),
                   __ATOMIC_RELEASE);
}

static void dynaccel_backoff(int* idle) {
  if (++(*idle) < 64) {
    sched_yield();
  } else {
    const struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000};  /* 50 us */
    nanosleep(&ts, NULL);
  }
}

static void* dynaccel_worker(void* arg) {
  struct dynaccel_queue* q = (struct dynaccel_queue*)arg;
  int idle = 0;

  while (atomic_load_explicit(&q->run, memory_order_relaxed)) {
    /* The doorbell holds the index of the LAST packet written, so the ring is
       non-empty when db + 1 > read_index. Comparing against read_index rather
       than a cached doorbell is what makes the very first doorbell -- value 0,
       for the packet in slot 0 -- observable. */
    const uint64_t rd =
        atomic_load_explicit((_Atomic uint64_t*)q->read_index, memory_order_relaxed);
    const uint64_t db =
        atomic_load_explicit((_Atomic uint64_t*)&q->doorbell, memory_order_acquire);

    if ((int64_t)(db + 1 - rd) <= 0) {
      dynaccel_backoff(&idle);
      continue;
    }
    idle = 0;

    for (uint64_t i = rd; i <= db; ++i) {
      dynaccel_execute(q, i);
      atomic_store_explicit((_Atomic uint64_t*)q->read_index, i + 1, memory_order_release);
    }
  }
  return NULL;
}

static hsa_status_t dynaccel_create_queue(rocr_dynamic_driver_context_t* ctx, uint32_t node_id,
                                          uint32_t type, uint32_t queue_pct, uint32_t priority,
                                          uint32_t sdma_engine_id, void* queue_addr,
                                          uint64_t queue_size_bytes, uint64_t* read_index_ptr,
                                          uint64_t queue_metadata_size_bytes, HsaEvent* event,
                                          HsaQueueResource* queue_resource) {
  (void)type; (void)queue_pct; (void)priority; (void)sdma_engine_id;
  (void)queue_metadata_size_bytes; (void)event;
  if (!ctx || !queue_addr || !read_index_ptr || !queue_resource || node_id != 0) {
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  const uint64_t num_pkts = queue_size_bytes / sizeof(dynaccel_dispatch_packet_t);
  if (num_pkts == 0 || (num_pkts & (num_pkts - 1)) != 0) {
    return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
  }

  struct dynaccel_queue* q = calloc(1, sizeof(*q));
  if (!q) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;

  q->doorbell = UINT64_MAX;   /* empty: db + 1 == read_index == 0 */
  q->ring = queue_addr;
  q->num_pkts = (uint32_t)num_pkts;
  q->read_index = read_index_ptr;
  atomic_store_explicit(&q->run, true, memory_order_relaxed);

  /* Publish q into ctx->queues only after the worker thread exists. The worker never
     consults ctx->queues, so there is no reason to link q in earlier -- and doing so
     would make the failure path below racy: a concurrent create_queue could push a new
     node in front of q between the unlock and the re-lock, and "ctx->queues = q->next"
     would then drop that node (and leak its thread) instead of unlinking q. Creating the
     thread before linking removes the race instead of handling it. */
  if (pthread_create(&q->thread, NULL, dynaccel_worker, q) != 0) {
    free(q);
    return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  }

  pthread_mutex_lock(&ctx->lock);
  q->id = ++ctx->next_id;
  q->next = ctx->queues;
  ctx->queues = q;
  pthread_mutex_unlock(&ctx->lock);

  memset(queue_resource, 0, sizeof(*queue_resource));
  queue_resource->QueueId = (HSA_QUEUEID)q->id;
  queue_resource->Queue_DoorBell_aql = &q->doorbell;
  queue_resource->Queue_read_ptr_aql = read_index_ptr;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_destroy_queue(rocr_dynamic_driver_context_t* ctx,
                                           uint64_t queue_id) {
  if (!ctx) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_queue** link = &ctx->queues;
  while (*link && (*link)->id != queue_id) link = &(*link)->next;
  struct dynaccel_queue* q = *link;
  if (q) *link = q->next;
  pthread_mutex_unlock(&ctx->lock);

  if (!q) return HSA_STATUS_ERROR_INVALID_QUEUE;

  atomic_store_explicit(&q->run, false, memory_order_release);
  pthread_join(q->thread, NULL);
  free(q);
  return HSA_STATUS_SUCCESS;
}

/* ---- Topology ---- */

static hsa_status_t dynaccel_get_system_properties(rocr_dynamic_driver_context_t* ctx,
                                                   HsaSystemProperties* sys_props) {
  if (!ctx || !sys_props) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  memset(sys_props, 0, sizeof(*sys_props));
  sys_props->NumNodes = 1;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_get_node_properties(rocr_dynamic_driver_context_t* ctx,
                                                 HsaNodeProperties* node_props, uint32_t node_id) {
  if (!ctx || !node_props || node_id != 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  memset(node_props, 0, sizeof(*node_props));
  node_props->NumMemoryBanks = 2;
  /* No CPU/compute/neural cores: ROCr routes the node to DiscoverDynamic. */
  /* AMDName is a plain HSAuint8 byte buffer, so strncpy is correct here. */
  strncpy((char*)node_props->AMDName, "DynAccel", sizeof(node_props->AMDName) - 1);
  /*
   * MarketingName, unlike AMDName, is HSAuint16[HSA_PUBLIC_NAME_SIZE] -- one
   * ASCII character per 16-bit element, not a byte buffer. strncpy-ing into
   * it as if it were char* packs two bytes per element, and readers that
   * pull out the low byte of each element (as HSA_AMD_AGENT_INFO_PRODUCT_NAME
   * does) get every other character back. Copy element-by-element instead.
   */
  {
    static const char kMarketingName[] = "DynAccel";
    size_t i;
    for (i = 0; i < sizeof(kMarketingName) - 1 && i < HSA_PUBLIC_NAME_SIZE; ++i) {
      node_props->MarketingName[i] = (HSAuint16)kMarketingName[i];
    }
  }
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_get_memory_properties(rocr_dynamic_driver_context_t* ctx,
                                                   uint32_t node_id,
                                                   HsaMemoryProperties* mem_props,
                                                   uint32_t* num_props) {
  if (!ctx || !num_props || node_id != 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  if (!mem_props) {           /* count query */
    *num_props = 2;
    return HSA_STATUS_SUCCESS;
  }
  if (*num_props < 2) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  memset(mem_props, 0, 2 * sizeof(*mem_props));
  mem_props[0].HeapType = HSA_HEAPTYPE_SYSTEM;                /* kernarg, fine grain */
  mem_props[0].SizeInBytes = 256ull << 20;
  mem_props[1].HeapType = HSA_HEAPTYPE_FRAME_BUFFER_PUBLIC;   /* device, coarse grain */
  mem_props[1].SizeInBytes = 256ull << 20;
  *num_props = 2;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_get_agent_properties(rocr_dynamic_driver_context_t* ctx,
                                                  uint32_t node_id,
                                                  rocr_dynamic_driver_agent_props_t* props) {
  if (!ctx || !props || node_id != 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  memset(props, 0, sizeof(*props));
  strncpy(props->name, "DynAccel", sizeof(props->name) - 1);
  strncpy(props->vendor_name, "AMD", sizeof(props->vendor_name) - 1);
  strncpy(props->product_name, "DynAccel Reference Accelerator",
          sizeof(props->product_name) - 1);
  strncpy(props->uuid, "DYN-00000000", sizeof(props->uuid) - 1);
  props->queue_min_size = DYNACCEL_QUEUE_PKTS;
  props->queue_max_size = DYNACCEL_QUEUE_PKTS;
  props->queues_max = 8;
  props->cacheline_size = 64;
  props->compute_unit_count = 1;
  props->max_clock_frequency = 1000;
  props->profile = (uint8_t)HSA_PROFILE_BASE;
  props->default_float_rounding_mode = (uint8_t)HSA_DEFAULT_FLOAT_ROUNDING_MODE_NEAR;
  /* dynaccel packets are agent-dispatch, delivered one at a time per queue. */
  props->feature = (uint8_t)HSA_AGENT_FEATURE_AGENT_DISPATCH;
  props->queue_type = (uint8_t)HSA_QUEUE_TYPE_SINGLE;
  return HSA_STATUS_SUCCESS;
}

/* ---- Cleanup ---- */

static void dynaccel_destroy_context(rocr_dynamic_driver_context_t* ctx) {
  if (!ctx) return;

  /* Callers are expected to destroy every queue before tearing down the
     context, but stop any that are still running here so a missed
     destroy_queue call cannot leak a worker thread past this point. Signal
     every worker to stop before joining any of them, so their backoff
     windows overlap instead of stacking one after another. */
  for (struct dynaccel_queue* q = ctx->queues; q; q = q->next) {
    atomic_store_explicit(&q->run, false, memory_order_release);
  }
  while (ctx->queues) {
    struct dynaccel_queue* q = ctx->queues;
    ctx->queues = q->next;
    pthread_join(q->thread, NULL);
    free(q);
  }

  /* Callers are expected to free/destroy every allocation before tearing down
     the context, but release any still-registered ones here so a missed
     free_memory/destroy_memory_handle call cannot leak a udmabuf fd or
     mapping past this point. */
  while (ctx->allocs) {
    struct dynaccel_alloc* a = ctx->allocs;
    ctx->allocs = a->next;
    if (a->ptr) munmap(a->ptr, a->size);
    if (a->dmabuf_fd >= 0) close(a->dmabuf_fd);
    free(a);
  }

  if (ctx->udmabuf_fd >= 0) close(ctx->udmabuf_fd);
  pthread_mutex_destroy(&ctx->lock);
  free(ctx);
}

/* ---- Factory ---- */

static rocr_dynamic_driver_ftable_t g_ftable = {
    .devnode_name = "/dev/dynaccel",
    .init = dynaccel_init,
    .shut_down = dynaccel_shut_down,
    .open = dynaccel_open,
    .close = dynaccel_close,
    .get_system_properties = dynaccel_get_system_properties,
    .get_node_properties = dynaccel_get_node_properties,
    .get_memory_properties = dynaccel_get_memory_properties,
    .get_agent_properties = dynaccel_get_agent_properties,
    .allocate_memory = dynaccel_allocate_memory,
    .free_memory = dynaccel_free_memory,
    .export_memory_handle = dynaccel_export_memory_handle,
    .import_memory_handle = dynaccel_import_memory_handle,
    .map = dynaccel_map,
    .unmap = dynaccel_unmap,
    .create_shareable_handle = dynaccel_create_shareable_handle,
    .destroy_memory_handle = dynaccel_destroy_memory_handle,
    .create_queue = dynaccel_create_queue,
    .destroy_queue = dynaccel_destroy_queue,
    .destroy_context = dynaccel_destroy_context,
};

rocr_dynamic_driver_ftable_t* rocr_dynamic_driver_create(rocr_dynamic_driver_context_t** ctx_out) {
  if (!ctx_out) return NULL;

  rocr_dynamic_driver_context_t* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  /* DYNACCEL_DISABLE_UDMABUF forces the fallback path so CI covers both. */
  ctx->udmabuf_fd = getenv("DYNACCEL_DISABLE_UDMABUF") ? -1 : open("/dev/udmabuf", O_RDWR);
  if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
    free(ctx);
    return NULL;
  }

  *ctx_out = ctx;
  return &g_ftable;
}
