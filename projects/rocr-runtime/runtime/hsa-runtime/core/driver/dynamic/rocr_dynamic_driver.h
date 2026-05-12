////////////////////////////////////////////////////////////////////////////////
//
// The University of Illinois/NCSA
// Open Source License (NCSA)
//
// Copyright (c) 2026, Advanced Micro Devices, Inc. All rights reserved.
//
// Developed by:
//
//                 AMD Research and AMD HSA Software Development
//
//                 Advanced Micro Devices, Inc.
//
//                 www.amd.com
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
//  - Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimers.
//  - Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimers in
//    the documentation and/or other materials provided with the distribution.
//  - Neither the names of Advanced Micro Devices, Inc,
//    nor the names of its contributors may be used to endorse or promote
//    products derived from this Software without specific prior written
//    permission.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS WITH THE SOFTWARE.
//
////////////////////////////////////////////////////////////////////////////////

#ifndef ROCR_DYNAMIC_DRIVER_H_
#define ROCR_DYNAMIC_DRIVER_H_

#include "hsakmt/hsakmttypes.h"
#include "inc/hsa.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rocr_dynamic_driver_ftable_t {
  void* ctx;
  const char* devnode_name;

  // Lifecycle
  hsa_status_t (*init)(void* ctx);
  hsa_status_t (*shut_down)(void* ctx);
  hsa_status_t (*open)(void* ctx);
  hsa_status_t (*close)(void* ctx);
  hsa_status_t (*query_kernel_mode_driver)(void* ctx, int query);

  // Topology
  hsa_status_t (*get_system_properties)(void* ctx, HsaSystemProperties* sys_props);
  hsa_status_t (*get_node_properties)(void* ctx, HsaNodeProperties* node_props,
                                      uint32_t node_id, void** driver_data);
  hsa_status_t (*get_edge_properties)(void* ctx, HsaIoLinkProperties* io_link_props,
                                      uint32_t* num_links, uint32_t node_id);
  hsa_status_t (*get_memory_properties)(void* ctx, uint32_t node_id,
                                        HsaMemoryProperties* mem_props, uint32_t* num_props);
  hsa_status_t (*get_cache_properties)(void* ctx, uint32_t node_id, uint32_t processor_id,
                                       HsaCacheProperties* cache_props, uint32_t* num_props);

  // Memory
  hsa_status_t (*allocate_memory)(void* ctx, uint32_t node_id, size_t size,
                                  uint32_t alloc_flags, HsaMemFlags mem_flags,
                                  HsaMemoryProperties mem_props, void** mem);
  hsa_status_t (*free_memory)(void* ctx, void* mem, size_t size);
  hsa_status_t (*allocate_scratch_memory)(void* ctx, uint32_t node_id, uint64_t size, void** mem);
  hsa_status_t (*available_memory)(void* ctx, uint32_t node_id, uint64_t* available_size);
  hsa_status_t (*register_memory)(void* ctx, void* ptr, uint64_t size, HsaMemFlags mem_flags);
  hsa_status_t (*deregister_memory)(void* ctx, void* ptr);
  hsa_status_t (*make_memory_resident)(void* ctx, const void* mem, size_t size,
                                       uint64_t* alternate_va, const HsaMemMapFlags* mem_flags,
                                       uint32_t num_nodes, const uint32_t* nodes);
  hsa_status_t (*make_memory_unresident)(void* ctx, const void* mem);

  // DMA-buf / Sharing
  hsa_status_t (*export_dmabuf)(void* ctx, void* mem, size_t size, int* dmabuf_fd, size_t* offset);
  hsa_status_t (*import_dmabuf)(void* ctx, int dmabuf_fd, uint32_t node_id,
                                uint64_t* handle, void* mem);
  hsa_status_t (*destroy_imported_shareable_handle)(void* ctx, uint64_t* handle);
  hsa_status_t (*map)(void* ctx, uint64_t handle, void* mem, size_t offset, size_t size, int perms);
  hsa_status_t (*unmap)(void* ctx, uint64_t handle, void* mem, size_t offset, size_t size);
  hsa_status_t (*create_shareable_handle)(void* ctx, void* va, void* mem, size_t size,
                                          uint32_t node_id, uint64_t* handle, uint64_t* offset,
                                          int* drm_fd, uint64_t* drm_fd_offset);
  hsa_status_t (*destroy_shareable_handle)(void* ctx, uint64_t* handle);

  // Queue
  hsa_status_t (*create_queue)(void* ctx, uint32_t node_id, uint32_t type, uint32_t queue_pct,
                                uint32_t priority, uint32_t sdma_engine_id, void* queue_addr,
                                uint64_t queue_size_bytes, uint64_t queue_metadata_size_bytes,
                                HsaEvent* event, HsaQueueResource* queue_resource,
                                void** driver_data);
  hsa_status_t (*destroy_queue)(void* ctx, uint64_t queue_id, void* driver_data);
  hsa_status_t (*update_queue)(void* ctx, uint64_t queue_id, uint32_t queue_pct,
                                uint32_t priority, void* queue_addr,
                                uint64_t queue_size_bytes, HsaEvent* event);
  hsa_status_t (*set_queue_cu_mask)(void* ctx, uint64_t queue_id, uint32_t cu_mask_count,
                                    uint32_t* queue_cu_mask);
  hsa_status_t (*alloc_queue_gws)(void* ctx, uint64_t queue_id, uint32_t num_gws,
                                   uint32_t* first_gws);
  hsa_status_t (*get_queue_save_area_info)(void* ctx, uint64_t queue_id,
                                            void** address, size_t* size);

  // Performance / Misc
  hsa_status_t (*spm_acquire)(void* ctx, uint32_t preferred_node_id);
  hsa_status_t (*spm_release)(void* ctx, uint32_t preferred_node_id);
  hsa_status_t (*spm_set_dest_buffer)(void* ctx, uint32_t preferred_node_id, uint32_t size_bytes,
                                      uint32_t* timeout, uint32_t* size_copied,
                                      void* dest_mem_addr, bool* is_spm_data_loss);
  hsa_status_t (*open_smi)(void* ctx, uint32_t node_id, int* fd);
  hsa_status_t (*set_trap_handler)(void* ctx, uint32_t node_id, const void* base,
                                    uint64_t base_size, const void* buffer_base,
                                    uint64_t buffer_base_size);
  hsa_status_t (*get_device_handle)(void* ctx, uint32_t node_id, void** device_handle);
  hsa_status_t (*get_clock_counters)(void* ctx, uint32_t node_id,
                                     HsaClockCounters* clock_counter);
  hsa_status_t (*get_tile_config)(void* ctx, uint32_t node_id, HsaGpuTileConfig* config);
  hsa_status_t (*is_model_enabled)(void* ctx, bool* enable);
  hsa_status_t (*get_wallclock_frequency)(void* ctx, uint32_t node_id, uint64_t* frequency);

  // Cleanup
  void (*destroy_agent_data)(void* ctx, void* driver_data);
  void (*destroy)(void* ctx);
} rocr_dynamic_driver_ftable_t;

#ifdef __cplusplus
}
#endif

#endif // ROCR_DYNAMIC_DRIVER_H_
