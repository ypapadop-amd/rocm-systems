/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include "core/inc/amd_dynamic_driver.h"

#include <dlfcn.h>
#include <vector>

#include "core/inc/amd_memory_region.h"

namespace rocr {
namespace AMD {

hsa_status_t DynamicDriver::DiscoverDriver(std::unique_ptr<core::Driver>& driver) {
  using CreateFn =
      rocr_dynamic_driver_ftable_t* (*)(rocr_dynamic_driver_context_t**);
  auto create_fn = reinterpret_cast<CreateFn>(dlsym(RTLD_DEFAULT, "rocr_dynamic_driver_create"));
  if (!create_fn) return HSA_STATUS_ERROR;

  rocr_dynamic_driver_context_t* ctx = nullptr;
  auto* ftable = create_fn(&ctx);
  if (!ftable) return HSA_STATUS_ERROR;

  driver.reset(new DynamicDriver(ftable, ctx));
  return HSA_STATUS_SUCCESS;
}

DynamicDriver::DynamicDriver(rocr_dynamic_driver_ftable_t* ftable,
                             rocr_dynamic_driver_context_t* ctx)
    : Driver(core::DriverType::DYNAMIC, ftable->devnode_name ? ftable->devnode_name : ""),
      ftable_(ftable),
      ctx_(ctx) {}

DynamicDriver::~DynamicDriver() {
  if (ftable_) {
    ftable_->destroy_context(ctx_);
    ftable_ = nullptr;
  }
}

hsa_status_t DynamicDriver::Init() {
  if (!ftable_->init) return HSA_STATUS_ERROR;
  return ftable_->init(ctx_);
}

hsa_status_t DynamicDriver::ShutDown() {
  if (!ftable_->shut_down) return HSA_STATUS_ERROR;
  return ftable_->shut_down(ctx_);
}

hsa_status_t DynamicDriver::QueryKernelModeDriver(core::DriverQuery query) {
  if (!ftable_->query_kernel_mode_driver) return HSA_STATUS_ERROR;
  return ftable_->query_kernel_mode_driver(ctx_, static_cast<int>(query));
}

hsa_status_t DynamicDriver::Open() {
  if (!ftable_->open) return HSA_STATUS_ERROR;
  return ftable_->open(ctx_);
}

hsa_status_t DynamicDriver::Close() {
  if (!ftable_->close) return HSA_STATUS_ERROR;
  return ftable_->close(ctx_);
}

hsa_status_t DynamicDriver::GetSystemProperties(HsaSystemProperties& sys_props) const {
  if (!ftable_->get_system_properties) return HSA_STATUS_ERROR;
  return ftable_->get_system_properties(ctx_, &sys_props);
}

hsa_status_t DynamicDriver::GetNodeProperties(HsaNodeProperties& node_props,
                                               uint32_t node_id) const {
  if (!ftable_->get_node_properties) return HSA_STATUS_ERROR;
  void* driver_data = nullptr;
  auto status = ftable_->get_node_properties(ctx_, &node_props, node_id, &driver_data);
  if (status == HSA_STATUS_SUCCESS && driver_data) {
    cached_driver_data_[node_id] = driver_data;
  }
  return status;
}

hsa_status_t DynamicDriver::GetEdgeProperties(
    std::vector<HsaIoLinkProperties>& io_link_props, uint32_t node_id) const {
  if (!ftable_->get_edge_properties) return HSA_STATUS_ERROR;
  uint32_t count = 0;
  auto status = ftable_->get_edge_properties(ctx_, nullptr, &count, node_id);
  if (status != HSA_STATUS_SUCCESS) return status;
  if (count == 0) return HSA_STATUS_SUCCESS;

  std::vector<HsaIoLinkProperties> buf(count);
  status = ftable_->get_edge_properties(ctx_, buf.data(), &count, node_id);
  if (status != HSA_STATUS_SUCCESS) return status;

  io_link_props = std::move(buf);
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicDriver::GetMemoryProperties(
    uint32_t node_id, std::vector<HsaMemoryProperties>& mem_props) const {
  if (!ftable_->get_memory_properties) return HSA_STATUS_ERROR;
  uint32_t count = 0;
  auto status = ftable_->get_memory_properties(ctx_, node_id, nullptr, &count);
  if (status != HSA_STATUS_SUCCESS) return status;
  if (count == 0) return HSA_STATUS_SUCCESS;

  std::vector<HsaMemoryProperties> buf(count);
  status = ftable_->get_memory_properties(ctx_, node_id, buf.data(), &count);
  if (status != HSA_STATUS_SUCCESS) return status;

  mem_props = std::move(buf);
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicDriver::GetCacheProperties(
    uint32_t node_id, uint32_t processor_id,
    std::vector<HsaCacheProperties>& cache_props) const {
  if (!ftable_->get_cache_properties) return HSA_STATUS_ERROR;
  uint32_t count = 0;
  auto status = ftable_->get_cache_properties(ctx_, node_id, processor_id,
                                               nullptr, &count);
  if (status != HSA_STATUS_SUCCESS) return status;
  if (count == 0) return HSA_STATUS_SUCCESS;

  std::vector<HsaCacheProperties> buf(count);
  status = ftable_->get_cache_properties(ctx_, node_id, processor_id,
                                          buf.data(), &count);
  if (status != HSA_STATUS_SUCCESS) return status;

  cache_props = std::move(buf);
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicDriver::AllocateMemory(const core::MemoryRegion& mem_region,
                                            core::MemoryRegion::AllocateFlags alloc_flags,
                                            void** mem, size_t size, uint32_t node_id) {
  if (!ftable_->allocate_memory) return HSA_STATUS_ERROR;
  auto& amd_region = static_cast<const AMD::MemoryRegion&>(mem_region);
  return ftable_->allocate_memory(ctx_, node_id, size,
                                  static_cast<uint32_t>(alloc_flags),
                                  amd_region.mem_flags(), amd_region.mem_props(),
                                  mem);
}

hsa_status_t DynamicDriver::FreeMemory(void* mem, size_t size) {
  if (!ftable_->free_memory) return HSA_STATUS_ERROR;
  return ftable_->free_memory(ctx_, mem, size);
}

hsa_status_t DynamicDriver::CreateQueue(
    uint32_t node_id, HSA_QUEUE_TYPE type, uint32_t queue_pct,
    HSA::hsa_amd_queue_priority_internal_t priority, uint32_t sdma_engine_id,
    void* queue_addr, uint64_t queue_size_bytes, uint64_t queue_metadata_size_bytes,
    HsaEvent* event, HsaQueueResource& queue_resource) const {
  void* driver_data = nullptr;
  return CreateQueueWithDriverData(node_id, type, queue_pct, priority, sdma_engine_id,
                                    queue_addr, queue_size_bytes, queue_metadata_size_bytes,
                                    event, queue_resource, &driver_data);
}

hsa_status_t DynamicDriver::DestroyQueue(HSA_QUEUEID queue_id) const {
  return DestroyQueueWithDriverData(queue_id, nullptr);
}

hsa_status_t DynamicDriver::UpdateQueue(
    HSA_QUEUEID queue_id, uint32_t queue_pct,
    HSA::hsa_amd_queue_priority_internal_t priority, void* queue_addr,
    uint64_t queue_size_bytes, HsaEvent* event) const {
  if (!ftable_->update_queue) return HSA_STATUS_ERROR;
  return ftable_->update_queue(ctx_, queue_id, queue_pct,
                                static_cast<uint32_t>(priority), queue_addr,
                                queue_size_bytes, event);
}

hsa_status_t DynamicDriver::SetQueueCUMask(HSA_QUEUEID queue_id, uint32_t cu_mask_count,
                                            uint32_t* queue_cu_mask) const {
  if (!ftable_->set_queue_cu_mask) return HSA_STATUS_ERROR;
  return ftable_->set_queue_cu_mask(ctx_, queue_id, cu_mask_count, queue_cu_mask);
}

hsa_status_t DynamicDriver::AllocQueueGWS(HSA_QUEUEID queue_id, uint32_t num_gws,
                                           uint32_t* first_gws) const {
  if (!ftable_->alloc_queue_gws) return HSA_STATUS_ERROR;
  return ftable_->alloc_queue_gws(ctx_, queue_id, num_gws, first_gws);
}

hsa_status_t DynamicDriver::ExportDMABuf(void* mem, size_t size, int* dmabuf_fd,
                                          size_t* offset) {
  if (!ftable_->export_dmabuf) return HSA_STATUS_ERROR;
  return ftable_->export_dmabuf(ctx_, mem, size, dmabuf_fd, offset);
}

hsa_status_t DynamicDriver::ImportDMABuf(int dmabuf_fd, const core::Agent& agent,
                                          core::ShareableHandle* handle, void* mem) {
  if (!ftable_->import_dmabuf) return HSA_STATUS_ERROR;
  return ftable_->import_dmabuf(ctx_, dmabuf_fd, agent.node_id(),
                                &handle->handle, mem);
}

hsa_status_t DynamicDriver::DestroyImportedShareableHandle(core::ShareableHandle* handle) {
  if (!ftable_->destroy_imported_shareable_handle) return HSA_STATUS_ERROR;
  return ftable_->destroy_imported_shareable_handle(ctx_, &handle->handle);
}

hsa_status_t DynamicDriver::Map(core::ShareableHandle handle, void* mem, size_t offset,
                                 size_t size, hsa_access_permission_t perms) {
  if (!ftable_->map) return HSA_STATUS_ERROR;
  return ftable_->map(ctx_, handle.handle, mem, offset, size,
                      static_cast<int>(perms));
}

hsa_status_t DynamicDriver::Unmap(core::ShareableHandle handle, void* mem, size_t offset,
                                   size_t size) {
  if (!ftable_->unmap) return HSA_STATUS_ERROR;
  return ftable_->unmap(ctx_, handle.handle, mem, offset, size);
}

hsa_status_t DynamicDriver::CreateShareableHandle(void* va, void* mem, size_t size,
                                                    const core::Agent& agent,
                                                    core::ShareableHandle* handle,
                                                    uint64_t* offset, int* drm_fd,
                                                    uint64_t* drm_fd_offset) {
  if (!ftable_->create_shareable_handle) return HSA_STATUS_ERROR;
  return ftable_->create_shareable_handle(ctx_, va, mem, size, agent.node_id(),
                                           &handle->handle, offset, drm_fd, drm_fd_offset);
}

hsa_status_t DynamicDriver::DestroyShareableHandle(core::ShareableHandle* handle) {
  if (!ftable_->destroy_shareable_handle) return HSA_STATUS_ERROR;
  return ftable_->destroy_shareable_handle(ctx_, &handle->handle);
}

hsa_status_t DynamicDriver::SPMAcquire(uint32_t preferred_node_id) const {
  if (!ftable_->spm_acquire) return HSA_STATUS_ERROR;
  return ftable_->spm_acquire(ctx_, preferred_node_id);
}

hsa_status_t DynamicDriver::SPMRelease(uint32_t preferred_node_id) const {
  if (!ftable_->spm_release) return HSA_STATUS_ERROR;
  return ftable_->spm_release(ctx_, preferred_node_id);
}

hsa_status_t DynamicDriver::SPMSetDestBuffer(uint32_t preferred_node_id, uint32_t size_bytes,
                                              uint32_t* timeout, uint32_t* size_copied,
                                              void* dest_mem_addr,
                                              bool* is_spm_data_loss) const {
  if (!ftable_->spm_set_dest_buffer) return HSA_STATUS_ERROR;
  return ftable_->spm_set_dest_buffer(ctx_, preferred_node_id, size_bytes,
                                       timeout, size_copied, dest_mem_addr, is_spm_data_loss);
}

hsa_status_t DynamicDriver::OpenSMI(uint32_t node_id, int* fd) const {
  if (!ftable_->open_smi) return Driver::OpenSMI(node_id, fd);
  return ftable_->open_smi(ctx_, node_id, fd);
}

hsa_status_t DynamicDriver::SetTrapHandler(uint32_t node_id, const void* base,
                                            uint64_t base_size, const void* buffer_base,
                                            uint64_t buffer_base_size) const {
  if (!ftable_->set_trap_handler) return HSA_STATUS_ERROR;
  return ftable_->set_trap_handler(ctx_, node_id, base, base_size,
                                    buffer_base, buffer_base_size);
}

hsa_status_t DynamicDriver::GetDeviceHandle(uint32_t node_id, void** device_handle) const {
  if (!ftable_->get_device_handle) return HSA_STATUS_ERROR;
  return ftable_->get_device_handle(ctx_, node_id, device_handle);
}

hsa_status_t DynamicDriver::GetClockCounters(uint32_t node_id,
                                              HsaClockCounters* clock_counter) const {
  if (!ftable_->get_clock_counters) return HSA_STATUS_ERROR;
  return ftable_->get_clock_counters(ctx_, node_id, clock_counter);
}

hsa_status_t DynamicDriver::GetTileConfig(uint32_t node_id, HsaGpuTileConfig* config) const {
  if (!ftable_->get_tile_config) return HSA_STATUS_ERROR;
  return ftable_->get_tile_config(ctx_, node_id, config);
}

hsa_status_t DynamicDriver::IsModelEnabled(bool* enable) const {
  if (!ftable_->is_model_enabled) return HSA_STATUS_ERROR;
  return ftable_->is_model_enabled(ctx_, enable);
}

hsa_status_t DynamicDriver::GetWallclockFrequency(uint32_t node_id, uint64_t* frequency) const {
  if (!ftable_->get_wallclock_frequency) return HSA_STATUS_ERROR;
  return ftable_->get_wallclock_frequency(ctx_, node_id, frequency);
}

hsa_status_t DynamicDriver::AllocateScratchMemory(uint32_t node_id, uint64_t size,
                                                    void** mem) const {
  if (!ftable_->allocate_scratch_memory) return HSA_STATUS_ERROR;
  return ftable_->allocate_scratch_memory(ctx_, node_id, size, mem);
}

hsa_status_t DynamicDriver::AvailableMemory(uint32_t node_id,
                                             uint64_t* available_size) const {
  if (!ftable_->available_memory) return HSA_STATUS_ERROR;
  return ftable_->available_memory(ctx_, node_id, available_size);
}

hsa_status_t DynamicDriver::RegisterMemory(void* ptr, uint64_t size,
                                            HsaMemFlags mem_flags) const {
  if (!ftable_->register_memory) return HSA_STATUS_ERROR;
  return ftable_->register_memory(ctx_, ptr, size, mem_flags);
}

hsa_status_t DynamicDriver::DeregisterMemory(void* ptr) const {
  if (!ftable_->deregister_memory) return HSA_STATUS_ERROR;
  return ftable_->deregister_memory(ctx_, ptr);
}

hsa_status_t DynamicDriver::MakeMemoryResident(const void* mem, size_t size,
                                                uint64_t* alternate_va,
                                                const HsaMemMapFlags* mem_flags,
                                                uint32_t num_nodes,
                                                const uint32_t* nodes) const {
  if (!ftable_->make_memory_resident) return HSA_STATUS_ERROR;
  return ftable_->make_memory_resident(ctx_, mem, size, alternate_va,
                                        mem_flags, num_nodes, nodes);
}

hsa_status_t DynamicDriver::MakeMemoryUnresident(const void* mem) const {
  if (!ftable_->make_memory_unresident) return HSA_STATUS_ERROR;
  return ftable_->make_memory_unresident(ctx_, mem);
}

hsa_status_t DynamicDriver::GetQueueSaveAreaInfo(HSA_QUEUEID queue_id, void** address,
                                                  size_t* size) const {
  if (!ftable_->get_queue_save_area_info) return HSA_STATUS_ERROR;
  return ftable_->get_queue_save_area_info(ctx_, queue_id, address, size);
}

void DynamicDriver::DestroyAgentData(void* driver_data) const {
  if (ftable_->destroy_agent_data && driver_data) {
    ftable_->destroy_agent_data(ctx_, driver_data);
  }
}

void* DynamicDriver::GetCachedDriverData(uint32_t node_id) const {
  auto it = cached_driver_data_.find(node_id);
  if (it != cached_driver_data_.end()) {
    return it->second;
  }
  return nullptr;
}

hsa_status_t DynamicDriver::CreateQueueWithDriverData(
    uint32_t node_id, HSA_QUEUE_TYPE type, uint32_t queue_pct,
    HSA::hsa_amd_queue_priority_internal_t priority, uint32_t sdma_engine_id,
    void* queue_addr, uint64_t queue_size_bytes, uint64_t queue_metadata_size_bytes,
    HsaEvent* event, HsaQueueResource& queue_resource, void** driver_data) const {
  if (!ftable_->create_queue) return HSA_STATUS_ERROR;
  return ftable_->create_queue(ctx_, node_id, static_cast<uint32_t>(type), queue_pct,
                                static_cast<uint32_t>(priority), sdma_engine_id, queue_addr,
                                queue_size_bytes, queue_metadata_size_bytes, event,
                                &queue_resource, driver_data);
}

hsa_status_t DynamicDriver::DestroyQueueWithDriverData(HSA_QUEUEID queue_id,
                                                        void* driver_data) const {
  if (!ftable_->destroy_queue) return HSA_STATUS_ERROR;
  return ftable_->destroy_queue(ctx_, queue_id, driver_data);
}

} // namespace AMD
} // namespace rocr
