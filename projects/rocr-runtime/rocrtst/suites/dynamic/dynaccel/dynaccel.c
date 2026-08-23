/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core/driver/dynamic/rocr_dynamic_driver.h"
#include "dynaccel_packet.h"

#define DYNACCEL_QUEUE_PKTS 64

struct dynaccel_alloc;
struct dynaccel_queue;

struct rocr_dynamic_driver_context_t {
  int udmabuf_fd;              /* -1 => plain-mmap fallback */
  pthread_mutex_t lock;        /* guards allocs and queues */
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
                                             HsaMemFlags mem_flags, HsaMemoryProperties mem_props,
                                             void** mem) {
  (void)node_id; (void)alloc_flags; (void)mem_flags; (void)mem_props;
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

static hsa_status_t dynaccel_free_memory(rocr_dynamic_driver_context_t* ctx, void* mem,
                                         size_t size) {
  (void)size;
  if (!ctx || !mem) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

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

static hsa_status_t dynaccel_export_memory_handle(rocr_dynamic_driver_context_t* ctx,
                                                  uint32_t node_id,
                                                  const rocr_dynamic_driver_memory_handle_t* handle,
                                                  int share_type, void* export_handle) {
  (void)node_id;
  if (!ctx || !handle || !export_handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  if (share_type != ROCR_DYNAMIC_SHARE_DMABUF_FD) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_alloc* a = dynaccel_find_alloc(ctx, (void*)(uintptr_t)handle->handle);
  const int fd = a ? a->dmabuf_fd : -1;
  pthread_mutex_unlock(&ctx->lock);

  if (!a) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
  if (fd < 0) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;  /* mmap fallback: not exportable */

  const int dup_fd = dup(fd);
  if (dup_fd < 0) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  *(int*)export_handle = dup_fd;
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
  return HSA_STATUS_SUCCESS;
}

/* ---- Cleanup ---- */

static void dynaccel_destroy_context(rocr_dynamic_driver_context_t* ctx) {
  if (!ctx) return;
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
