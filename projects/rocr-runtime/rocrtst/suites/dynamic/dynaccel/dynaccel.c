/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
    .destroy_context = dynaccel_destroy_context,
};

rocr_dynamic_driver_ftable_t* rocr_dynamic_driver_create(rocr_dynamic_driver_context_t** ctx_out) {
  if (!ctx_out) return NULL;

  rocr_dynamic_driver_context_t* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->udmabuf_fd = -1;   /* opened in Task 5 */
  if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
    free(ctx);
    return NULL;
  }

  *ctx_out = ctx;
  return &g_ftable;
}
