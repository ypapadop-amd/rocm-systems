/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ROCR_DYNAMIC_DRIVER_H_
#define ROCR_DYNAMIC_DRIVER_H_

#include "hsakmt/hsakmttypes.h"
#include "inc/hsa.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Function table for a dynamically-loaded HSA driver.
 *
 * A shared library loaded via LD_PRELOAD exports a factory function
 * @c rocr_dynamic_driver_create that returns a pointer to this struct.
 * The HSA runtime discovers the factory with
 * @c dlsym(RTLD_DEFAULT, "rocr_dynamic_driver_create") during topology
 * discovery and delegates all driver operations through these function
 * pointers.
 *
 * Every function pointer receives @c ctx as its first argument. The
 * implementer stores whatever state it needs behind that opaque pointer.
 *
 * All function pointers except @c destroy_agent_data, @c destroy, and
 * @c open_smi are required (must be non-NULL). @c open_smi and
 * @c destroy_agent_data may be NULL; the runtime falls back to default
 * behavior when they are.
 */
typedef struct rocr_dynamic_driver_ftable_t {
  /** Opaque context owned by the driver implementation. Passed as the first
   *  argument to every function pointer in this table. */
  void* ctx;

  /** Device node name (e.g. "/dev/mydevice"). May be NULL. Used by the
   *  runtime's Driver base class for identification. */
  const char* devnode_name;

  /* ---- Lifecycle -------------------------------------------------------- */

  /**
   * @brief Initialize the driver.
   * @param[in] ctx  Driver context.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*init)(void* ctx);

  /**
   * @brief Shut down the driver, releasing any global resources.
   * @param[in] ctx  Driver context.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*shut_down)(void* ctx);

  /**
   * @brief Open the driver for use (called after init).
   * @param[in] ctx  Driver context.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*open)(void* ctx);

  /**
   * @brief Close the driver (called before shut_down).
   * @param[in] ctx  Driver context.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*close)(void* ctx);

  /**
   * @brief Query the kernel-mode driver for version or capability info.
   * @param[in] ctx    Driver context.
   * @param[in] query  Query identifier (cast of @c core::DriverQuery).
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*query_kernel_mode_driver)(void* ctx, int query);

  /* ---- Topology --------------------------------------------------------- */

  /**
   * @brief Retrieve system-wide properties.
   * @param[in]  ctx        Driver context.
   * @param[out] sys_props  Filled with system properties.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_system_properties)(void* ctx, HsaSystemProperties* sys_props);

  /**
   * @brief Retrieve properties for a single topology node.
   * @param[in]  ctx          Driver context.
   * @param[out] node_props   Filled with the node's properties.
   * @param[in]  node_id      Topology node index.
   * @param[out] driver_data  Optional opaque per-agent data owned by the
   *                          driver. Passed to the DynamicAgent constructor
   *                          and later freed via @c destroy_agent_data. May
   *                          be set to NULL if not needed.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_node_properties)(void* ctx, HsaNodeProperties* node_props,
                                      uint32_t node_id, void** driver_data);

  /**
   * @brief Retrieve I/O link (edge) properties for a node.
   *
   * Uses a two-call pattern: call once with @p io_link_props = NULL to get
   * the count in @p num_links, then call again with a buffer of that size.
   *
   * @param[in]     ctx            Driver context.
   * @param[out]    io_link_props  Buffer to receive link properties, or NULL
   *                               to query count only.
   * @param[in,out] num_links      On input with non-NULL buffer: capacity.
   *                               On output: number of links written (or
   *                               total count when querying).
   * @param[in]     node_id        Topology node index.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_edge_properties)(void* ctx, HsaIoLinkProperties* io_link_props,
                                      uint32_t* num_links, uint32_t node_id);

  /**
   * @brief Retrieve memory properties for a node.
   *
   * Two-call pattern: call with @p mem_props = NULL to get count, then with
   * a buffer.
   *
   * @param[in]     ctx        Driver context.
   * @param[in]     node_id    Topology node index.
   * @param[out]    mem_props  Buffer for memory properties, or NULL to query
   *                           count.
   * @param[in,out] num_props  Number of memory regions.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_memory_properties)(void* ctx, uint32_t node_id,
                                        HsaMemoryProperties* mem_props, uint32_t* num_props);

  /**
   * @brief Retrieve cache properties for a processor on a node.
   *
   * Two-call pattern: call with @p cache_props = NULL to get count, then
   * with a buffer.
   *
   * @param[in]     ctx           Driver context.
   * @param[in]     node_id       Topology node index.
   * @param[in]     processor_id  Processor index within the node.
   * @param[out]    cache_props   Buffer for cache properties, or NULL to
   *                              query count.
   * @param[in,out] num_props     Number of cache levels.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_cache_properties)(void* ctx, uint32_t node_id, uint32_t processor_id,
                                       HsaCacheProperties* cache_props, uint32_t* num_props);

  /* ---- Memory ----------------------------------------------------------- */

  /**
   * @brief Allocate device or system memory.
   * @param[in]  ctx          Driver context.
   * @param[in]  node_id      Topology node that owns the allocation.
   * @param[in]  size         Allocation size in bytes.
   * @param[in]  alloc_flags  Allocation flags (cast of
   *                          @c core::MemoryRegion::AllocateFlags).
   * @param[in]  mem_flags    KFD-level memory flags describing the heap.
   * @param[in]  mem_props    Memory properties of the target region.
   * @param[out] mem          Pointer to the allocated memory.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*allocate_memory)(void* ctx, uint32_t node_id, size_t size,
                                  uint32_t alloc_flags, HsaMemFlags mem_flags,
                                  HsaMemoryProperties mem_props, void** mem);

  /**
   * @brief Free memory previously allocated with @c allocate_memory.
   * @param[in] ctx   Driver context.
   * @param[in] mem   Pointer returned by @c allocate_memory.
   * @param[in] size  Size of the allocation.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*free_memory)(void* ctx, void* mem, size_t size);

  /**
   * @brief Allocate scratch memory for a node.
   * @param[in]  ctx      Driver context.
   * @param[in]  node_id  Topology node index.
   * @param[in]  size     Scratch size in bytes.
   * @param[out] mem      Pointer to the allocated scratch memory.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*allocate_scratch_memory)(void* ctx, uint32_t node_id, uint64_t size, void** mem);

  /**
   * @brief Query the amount of available (free) memory on a node.
   * @param[in]  ctx             Driver context.
   * @param[in]  node_id         Topology node index.
   * @param[out] available_size  Available memory in bytes.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*available_memory)(void* ctx, uint32_t node_id, uint64_t* available_size);

  /**
   * @brief Register host memory with the driver so it can be accessed by
   *        the device.
   * @param[in] ctx        Driver context.
   * @param[in] ptr        Host pointer to register.
   * @param[in] size       Size in bytes.
   * @param[in] mem_flags  Memory flags.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*register_memory)(void* ctx, void* ptr, uint64_t size, HsaMemFlags mem_flags);

  /**
   * @brief Deregister previously registered host memory.
   * @param[in] ctx  Driver context.
   * @param[in] ptr  Pointer previously passed to @c register_memory.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*deregister_memory)(void* ctx, void* ptr);

  /**
   * @brief Pin memory so it remains resident in device-accessible space.
   * @param[in]  ctx           Driver context.
   * @param[in]  mem           Pointer to the memory region.
   * @param[in]  size          Size in bytes.
   * @param[out] alternate_va  Alternate virtual address for device access,
   *                           or 0 if same as @p mem.
   * @param[in]  mem_flags     Mapping flags.
   * @param[in]  num_nodes     Number of entries in @p nodes.
   * @param[in]  nodes         Array of node IDs that should be able to
   *                           access the memory.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*make_memory_resident)(void* ctx, const void* mem, size_t size,
                                       uint64_t* alternate_va, const HsaMemMapFlags* mem_flags,
                                       uint32_t num_nodes, const uint32_t* nodes);

  /**
   * @brief Unpin memory previously made resident.
   * @param[in] ctx  Driver context.
   * @param[in] mem  Pointer previously passed to @c make_memory_resident.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*make_memory_unresident)(void* ctx, const void* mem);

  /* ---- DMA-buf / Sharing ------------------------------------------------ */

  /**
   * @brief Export a memory allocation as a DMA-buf file descriptor.
   * @param[in]  ctx        Driver context.
   * @param[in]  mem        Pointer to the allocated memory.
   * @param[in]  size       Allocation size in bytes.
   * @param[out] dmabuf_fd  File descriptor for the exported DMA-buf.
   * @param[out] offset     Offset within the DMA-buf where the data starts.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*export_dmabuf)(void* ctx, void* mem, size_t size, int* dmabuf_fd, size_t* offset);

  /**
   * @brief Import a DMA-buf file descriptor for use by a node.
   * @param[in]  ctx        Driver context.
   * @param[in]  dmabuf_fd  File descriptor of the DMA-buf to import.
   * @param[in]  node_id    Topology node that will access the buffer.
   * @param[out] handle     Opaque handle representing the imported buffer.
   * @param[in]  mem        Pointer to associate with the import (may be
   *                        NULL).
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*import_dmabuf)(void* ctx, int dmabuf_fd, uint32_t node_id,
                                uint64_t* handle, void* mem);

  /**
   * @brief Destroy a handle obtained from @c import_dmabuf.
   * @param[in]     ctx     Driver context.
   * @param[in,out] handle  Handle to destroy; set to 0 on success.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*destroy_imported_shareable_handle)(void* ctx, uint64_t* handle);

  /**
   * @brief Map a shared memory region into the caller's address space.
   * @param[in] ctx     Driver context.
   * @param[in] handle  Handle from @c import_dmabuf or
   *                    @c create_shareable_handle.
   * @param[in] mem     Base address for the mapping.
   * @param[in] offset  Byte offset within the shared region.
   * @param[in] size    Number of bytes to map.
   * @param[in] perms   Access permissions (cast of
   *                    @c hsa_access_permission_t).
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*map)(void* ctx, uint64_t handle, void* mem, size_t offset, size_t size, int perms);

  /**
   * @brief Unmap a previously mapped shared memory region.
   * @param[in] ctx     Driver context.
   * @param[in] handle  Handle used in the corresponding @c map call.
   * @param[in] mem     Base address of the mapping.
   * @param[in] offset  Byte offset within the shared region.
   * @param[in] size    Number of bytes to unmap.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*unmap)(void* ctx, uint64_t handle, void* mem, size_t offset, size_t size);

  /**
   * @brief Create a shareable handle for an existing memory allocation.
   *
   * The returned handle can be passed to another process or agent for
   * mapping via @c map.
   *
   * @param[in]  ctx             Driver context.
   * @param[in]  va              Virtual address of the allocation.
   * @param[in]  mem             Backing memory pointer.
   * @param[in]  size            Allocation size in bytes.
   * @param[in]  node_id         Topology node that owns the allocation.
   * @param[out] handle          Opaque shareable handle.
   * @param[out] offset          Offset within the exportable object.
   * @param[out] drm_fd          DRM file descriptor (if applicable).
   * @param[out] drm_fd_offset   Offset within the DRM object.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*create_shareable_handle)(void* ctx, void* va, void* mem, size_t size,
                                          uint32_t node_id, uint64_t* handle, uint64_t* offset,
                                          int* drm_fd, uint64_t* drm_fd_offset);

  /**
   * @brief Destroy a handle created by @c create_shareable_handle.
   * @param[in]     ctx     Driver context.
   * @param[in,out] handle  Handle to destroy; set to 0 on success.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*destroy_shareable_handle)(void* ctx, uint64_t* handle);

  /* ---- Queue ------------------------------------------------------------ */

  /**
   * @brief Create a hardware queue (command processor context).
   * @param[in]  ctx                       Driver context.
   * @param[in]  node_id                   Topology node for the queue.
   * @param[in]  type                      Queue type (cast of
   *                                       @c HSA_QUEUE_TYPE).
   * @param[in]  queue_pct                 Queue percentage (0-100) for
   *                                       resource partitioning.
   * @param[in]  priority                  Queue priority (cast of
   *                                       @c hsa_amd_queue_priority_internal_t).
   * @param[in]  sdma_engine_id            SDMA engine to bind (0 for
   *                                       default).
   * @param[in]  queue_addr                Ring buffer address, or NULL if
   *                                       the driver should allocate one.
   * @param[in]  queue_size_bytes          Ring buffer size in bytes.
   * @param[in]  queue_metadata_size_bytes Size of per-queue metadata.
   * @param[in]  event                     Event for queue error
   *                                       notification, or NULL.
   * @param[out] queue_resource            Filled with the queue's hardware
   *                                       resource handles (e.g. QueueId).
   * @param[out] driver_data               Opaque per-queue data owned by
   *                                       the driver, freed via
   *                                       @c destroy_queue.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*create_queue)(void* ctx, uint32_t node_id, uint32_t type, uint32_t queue_pct,
                                uint32_t priority, uint32_t sdma_engine_id, void* queue_addr,
                                uint64_t queue_size_bytes, uint64_t queue_metadata_size_bytes,
                                HsaEvent* event, HsaQueueResource* queue_resource,
                                void** driver_data);

  /**
   * @brief Destroy a queue created by @c create_queue.
   * @param[in] ctx          Driver context.
   * @param[in] queue_id     Queue identifier from
   *                         @c HsaQueueResource::QueueId.
   * @param[in] driver_data  Per-queue data returned by @c create_queue.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*destroy_queue)(void* ctx, uint64_t queue_id, void* driver_data);

  /**
   * @brief Update queue parameters (priority, size, event) on a live queue.
   * @param[in] ctx              Driver context.
   * @param[in] queue_id         Queue identifier.
   * @param[in] queue_pct        New queue percentage.
   * @param[in] priority         New priority.
   * @param[in] queue_addr       New ring buffer address (or current
   *                             address).
   * @param[in] queue_size_bytes New ring buffer size in bytes.
   * @param[in] event            New error event, or NULL.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*update_queue)(void* ctx, uint64_t queue_id, uint32_t queue_pct,
                                uint32_t priority, void* queue_addr,
                                uint64_t queue_size_bytes, HsaEvent* event);

  /**
   * @brief Set the compute-unit mask for a queue, controlling which CUs
   *        the queue may dispatch to.
   * @param[in] ctx            Driver context.
   * @param[in] queue_id       Queue identifier.
   * @param[in] cu_mask_count  Number of 32-bit words in @p queue_cu_mask.
   * @param[in] queue_cu_mask  Bitmask of enabled compute units.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*set_queue_cu_mask)(void* ctx, uint64_t queue_id, uint32_t cu_mask_count,
                                    uint32_t* queue_cu_mask);

  /**
   * @brief Allocate global wave sync (GWS) resources for a queue.
   * @param[in]  ctx        Driver context.
   * @param[in]  queue_id   Queue identifier.
   * @param[in]  num_gws    Number of GWS slots requested.
   * @param[out] first_gws  Index of the first allocated GWS slot.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*alloc_queue_gws)(void* ctx, uint64_t queue_id, uint32_t num_gws,
                                   uint32_t* first_gws);

  /**
   * @brief Get the save area info for a queue (used for context
   *        save/restore).
   * @param[in]  ctx       Driver context.
   * @param[in]  queue_id  Queue identifier.
   * @param[out] address   Base address of the save area.
   * @param[out] size      Size of the save area in bytes.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_queue_save_area_info)(void* ctx, uint64_t queue_id,
                                            void** address, size_t* size);

  /* ---- Performance / Misc ----------------------------------------------- */

  /**
   * @brief Acquire the Streaming Performance Monitor (SPM) for a node.
   * @param[in] ctx                Driver context.
   * @param[in] preferred_node_id  Node from which to acquire SPM.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*spm_acquire)(void* ctx, uint32_t preferred_node_id);

  /**
   * @brief Release the SPM previously acquired with @c spm_acquire.
   * @param[in] ctx                Driver context.
   * @param[in] preferred_node_id  Node whose SPM to release.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*spm_release)(void* ctx, uint32_t preferred_node_id);

  /**
   * @brief Configure the SPM destination buffer and retrieve sampled data.
   * @param[in]     ctx                Driver context.
   * @param[in]     preferred_node_id  Target node.
   * @param[in]     size_bytes         Size of @p dest_mem_addr buffer.
   * @param[in,out] timeout            Timeout in milliseconds; updated with
   *                                   remaining time.
   * @param[out]    size_copied        Bytes of SPM data copied.
   * @param[out]    dest_mem_addr      Destination buffer for SPM data.
   * @param[out]    is_spm_data_loss   True if SPM data was lost due to
   *                                   buffer overflow.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*spm_set_dest_buffer)(void* ctx, uint32_t preferred_node_id, uint32_t size_bytes,
                                      uint32_t* timeout, uint32_t* size_copied,
                                      void* dest_mem_addr, bool* is_spm_data_loss);

  /**
   * @brief Open a System Management Interface (SMI) event handle.
   *
   * Optional — may be NULL. If NULL, the runtime falls back to its default
   * SMI implementation.
   *
   * @param[in]  ctx      Driver context.
   * @param[in]  node_id  Topology node index.
   * @param[out] fd       File descriptor for SMI events.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*open_smi)(void* ctx, uint32_t node_id, int* fd);

  /**
   * @brief Install a trap handler for a node.
   * @param[in] ctx              Driver context.
   * @param[in] node_id          Topology node index.
   * @param[in] base             Trap handler code base address.
   * @param[in] base_size        Size of the trap handler code in bytes.
   * @param[in] buffer_base      Trap handler data buffer base address.
   * @param[in] buffer_base_size Size of the data buffer in bytes.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*set_trap_handler)(void* ctx, uint32_t node_id, const void* base,
                                    uint64_t base_size, const void* buffer_base,
                                    uint64_t buffer_base_size);

  /**
   * @brief Get a platform-specific device handle (e.g. DRM fd) for a node.
   * @param[in]  ctx            Driver context.
   * @param[in]  node_id        Topology node index.
   * @param[out] device_handle  Opaque device handle.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_device_handle)(void* ctx, uint32_t node_id, void** device_handle);

  /**
   * @brief Read hardware clock counters for a node.
   * @param[in]  ctx            Driver context.
   * @param[in]  node_id        Topology node index.
   * @param[out] clock_counter  Filled with GPU and CPU clock values.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_clock_counters)(void* ctx, uint32_t node_id,
                                     HsaClockCounters* clock_counter);

  /**
   * @brief Retrieve GPU tile configuration for a node.
   * @param[in]  ctx      Driver context.
   * @param[in]  node_id  Topology node index.
   * @param[out] config   Filled with tile/pipe configuration.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_tile_config)(void* ctx, uint32_t node_id, HsaGpuTileConfig* config);

  /**
   * @brief Check whether the driver's device model is enabled.
   * @param[in]  ctx     Driver context.
   * @param[out] enable  True if the model is enabled.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*is_model_enabled)(void* ctx, bool* enable);

  /**
   * @brief Get the wall-clock frequency for a node (in Hz).
   * @param[in]  ctx        Driver context.
   * @param[in]  node_id    Topology node index.
   * @param[out] frequency  Clock frequency in Hz.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_wallclock_frequency)(void* ctx, uint32_t node_id, uint64_t* frequency);

  /* ---- Cleanup ---------------------------------------------------------- */

  /**
   * @brief Destroy per-agent data returned by @c get_node_properties.
   *
   * Optional — may be NULL. Called by the DynamicAgent destructor to free
   * the opaque @c driver_data pointer that was returned from
   * @c get_node_properties.
   *
   * @param[in] ctx          Driver context.
   * @param[in] driver_data  Per-agent data to free.
   */
  void (*destroy_agent_data)(void* ctx, void* driver_data);

  /**
   * @brief Destroy the driver context itself.
   *
   * Called once during DynamicDriver destruction. After this call, @c ctx
   * is invalid and no other function pointer in this table will be invoked.
   *
   * @param[in] ctx  Driver context to destroy.
   */
  void (*destroy)(void* ctx);
} rocr_dynamic_driver_ftable_t;

#ifdef __cplusplus
}
#endif

#endif // ROCR_DYNAMIC_DRIVER_H_
