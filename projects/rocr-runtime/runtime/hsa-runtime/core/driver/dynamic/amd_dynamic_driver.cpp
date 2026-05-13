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

#include "core/driver/dynamic/amd_dynamic_driver.h"

#include <dlfcn.h>
#include <vector>

#include "core/inc/amd_memory_region.h"

namespace rocr {
namespace AMD {

hsa_status_t DynamicDriver::DiscoverDriver(std::unique_ptr<core::Driver>& driver) {
  using CreateFn = rocr_dynamic_driver_ftable_t* (*)();
  auto create_fn = reinterpret_cast<CreateFn>(dlsym(RTLD_DEFAULT, "rocr_dynamic_driver_create"));
  if (!create_fn) return HSA_STATUS_ERROR;

  auto* ftable = create_fn();
  if (!ftable) return HSA_STATUS_ERROR;

  driver.reset(new DynamicDriver(ftable));
  return HSA_STATUS_SUCCESS;
}

DynamicDriver::DynamicDriver(rocr_dynamic_driver_ftable_t* ftable)
    : Driver(core::DriverType::DYNAMIC, ftable->devnode_name ? ftable->devnode_name : ""),
      ftable_(ftable) {}

DynamicDriver::~DynamicDriver() {
  if (ftable_) {
    ftable_->destroy(ftable_->ctx);
    ftable_ = nullptr;
  }
}

hsa_status_t DynamicDriver::Init() {
  return ftable_->init(ftable_->ctx);
}

hsa_status_t DynamicDriver::ShutDown() {
  return ftable_->shut_down(ftable_->ctx);
}

hsa_status_t DynamicDriver::QueryKernelModeDriver(core::DriverQuery query) {
  return ftable_->query_kernel_mode_driver(ftable_->ctx, static_cast<int>(query));
}

hsa_status_t DynamicDriver::Open() {
  return ftable_->open(ftable_->ctx);
}

hsa_status_t DynamicDriver::Close() {
  return ftable_->close(ftable_->ctx);
}

hsa_status_t DynamicDriver::GetSystemProperties(HsaSystemProperties& sys_props) const {
  return ftable_->get_system_properties(ftable_->ctx, &sys_props);
}

hsa_status_t DynamicDriver::GetNodeProperties(HsaNodeProperties& node_props,
                                               uint32_t node_id) const {
  void* driver_data = nullptr;
  auto status = ftable_->get_node_properties(ftable_->ctx, &node_props, node_id, &driver_data);
  if (status == HSA_STATUS_SUCCESS && driver_data) {
    cached_driver_data_[node_id] = driver_data;
  }
  return status;
}

hsa_status_t DynamicDriver::GetEdgeProperties(
    std::vector<HsaIoLinkProperties>& io_link_props, uint32_t node_id) const {
  uint32_t count = 0;
  auto status = ftable_->get_edge_properties(ftable_->ctx, nullptr, &count, node_id);
  if (status != HSA_STATUS_SUCCESS) return status;
  if (count == 0) return HSA_STATUS_SUCCESS;

  std::vector<HsaIoLinkProperties> buf(count);
  status = ftable_->get_edge_properties(ftable_->ctx, buf.data(), &count, node_id);
  if (status != HSA_STATUS_SUCCESS) return status;

  io_link_props = std::move(buf);
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicDriver::GetMemoryProperties(
    uint32_t node_id, std::vector<HsaMemoryProperties>& mem_props) const {
  uint32_t count = 0;
  auto status = ftable_->get_memory_properties(ftable_->ctx, node_id, nullptr, &count);
  if (status != HSA_STATUS_SUCCESS) return status;
  if (count == 0) return HSA_STATUS_SUCCESS;

  std::vector<HsaMemoryProperties> buf(count);
  status = ftable_->get_memory_properties(ftable_->ctx, node_id, buf.data(), &count);
  if (status != HSA_STATUS_SUCCESS) return status;

  mem_props = std::move(buf);
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicDriver::GetCacheProperties(
    uint32_t node_id, uint32_t processor_id,
    std::vector<HsaCacheProperties>& cache_props) const {
  uint32_t count = 0;
  auto status = ftable_->get_cache_properties(ftable_->ctx, node_id, processor_id,
                                               nullptr, &count);
  if (status != HSA_STATUS_SUCCESS) return status;
  if (count == 0) return HSA_STATUS_SUCCESS;

  std::vector<HsaCacheProperties> buf(count);
  status = ftable_->get_cache_properties(ftable_->ctx, node_id, processor_id,
                                          buf.data(), &count);
  if (status != HSA_STATUS_SUCCESS) return status;

  cache_props = std::move(buf);
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicDriver::AllocateMemory(const core::MemoryRegion& mem_region,
                                            core::MemoryRegion::AllocateFlags alloc_flags,
                                            void** mem, size_t size, uint32_t node_id) {
  auto& amd_region = static_cast<const AMD::MemoryRegion&>(mem_region);
  return ftable_->allocate_memory(ftable_->ctx, node_id, size,
                                  static_cast<uint32_t>(alloc_flags),
                                  amd_region.mem_flags(), amd_region.mem_props(),
                                  mem);
}

hsa_status_t DynamicDriver::FreeMemory(void* mem, size_t size) {
  return ftable_->free_memory(ftable_->ctx, mem, size);
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
  return ftable_->update_queue(ftable_->ctx, queue_id, queue_pct,
                                static_cast<uint32_t>(priority), queue_addr,
                                queue_size_bytes, event);
}

hsa_status_t DynamicDriver::SetQueueCUMask(HSA_QUEUEID queue_id, uint32_t cu_mask_count,
                                            uint32_t* queue_cu_mask) const {
  return ftable_->set_queue_cu_mask(ftable_->ctx, queue_id, cu_mask_count, queue_cu_mask);
}

hsa_status_t DynamicDriver::AllocQueueGWS(HSA_QUEUEID queue_id, uint32_t num_gws,
                                           uint32_t* first_gws) const {
  return ftable_->alloc_queue_gws(ftable_->ctx, queue_id, num_gws, first_gws);
}

hsa_status_t DynamicDriver::ExportDMABuf(void* mem, size_t size, int* dmabuf_fd,
                                          size_t* offset) {
  return ftable_->export_dmabuf(ftable_->ctx, mem, size, dmabuf_fd, offset);
}

hsa_status_t DynamicDriver::ImportDMABuf(int dmabuf_fd, const core::Agent& agent,
                                          core::ShareableHandle* handle, void* mem) {
  return ftable_->import_dmabuf(ftable_->ctx, dmabuf_fd, agent.node_id(),
                                &handle->handle, mem);
}

hsa_status_t DynamicDriver::DestroyImportedShareableHandle(core::ShareableHandle* handle) {
  return ftable_->destroy_imported_shareable_handle(ftable_->ctx, &handle->handle);
}

hsa_status_t DynamicDriver::Map(core::ShareableHandle handle, void* mem, size_t offset,
                                 size_t size, hsa_access_permission_t perms) {
  return ftable_->map(ftable_->ctx, handle.handle, mem, offset, size,
                      static_cast<int>(perms));
}

hsa_status_t DynamicDriver::Unmap(core::ShareableHandle handle, void* mem, size_t offset,
                                   size_t size) {
  return ftable_->unmap(ftable_->ctx, handle.handle, mem, offset, size);
}

hsa_status_t DynamicDriver::CreateShareableHandle(void* va, void* mem, size_t size,
                                                    const core::Agent& agent,
                                                    core::ShareableHandle* handle,
                                                    uint64_t* offset, int* drm_fd,
                                                    uint64_t* drm_fd_offset) {
  return ftable_->create_shareable_handle(ftable_->ctx, va, mem, size, agent.node_id(),
                                           &handle->handle, offset, drm_fd, drm_fd_offset);
}

hsa_status_t DynamicDriver::DestroyShareableHandle(core::ShareableHandle* handle) {
  return ftable_->destroy_shareable_handle(ftable_->ctx, &handle->handle);
}

hsa_status_t DynamicDriver::SPMAcquire(uint32_t preferred_node_id) const {
  return ftable_->spm_acquire(ftable_->ctx, preferred_node_id);
}

hsa_status_t DynamicDriver::SPMRelease(uint32_t preferred_node_id) const {
  return ftable_->spm_release(ftable_->ctx, preferred_node_id);
}

hsa_status_t DynamicDriver::SPMSetDestBuffer(uint32_t preferred_node_id, uint32_t size_bytes,
                                              uint32_t* timeout, uint32_t* size_copied,
                                              void* dest_mem_addr,
                                              bool* is_spm_data_loss) const {
  return ftable_->spm_set_dest_buffer(ftable_->ctx, preferred_node_id, size_bytes,
                                       timeout, size_copied, dest_mem_addr, is_spm_data_loss);
}

hsa_status_t DynamicDriver::OpenSMI(uint32_t node_id, int* fd) const {
  if (ftable_->open_smi) {
    return ftable_->open_smi(ftable_->ctx, node_id, fd);
  }
  return Driver::OpenSMI(node_id, fd);
}

hsa_status_t DynamicDriver::SetTrapHandler(uint32_t node_id, const void* base,
                                            uint64_t base_size, const void* buffer_base,
                                            uint64_t buffer_base_size) const {
  return ftable_->set_trap_handler(ftable_->ctx, node_id, base, base_size,
                                    buffer_base, buffer_base_size);
}

hsa_status_t DynamicDriver::GetDeviceHandle(uint32_t node_id, void** device_handle) const {
  return ftable_->get_device_handle(ftable_->ctx, node_id, device_handle);
}

hsa_status_t DynamicDriver::GetClockCounters(uint32_t node_id,
                                              HsaClockCounters* clock_counter) const {
  return ftable_->get_clock_counters(ftable_->ctx, node_id, clock_counter);
}

hsa_status_t DynamicDriver::GetTileConfig(uint32_t node_id, HsaGpuTileConfig* config) const {
  return ftable_->get_tile_config(ftable_->ctx, node_id, config);
}

hsa_status_t DynamicDriver::IsModelEnabled(bool* enable) const {
  return ftable_->is_model_enabled(ftable_->ctx, enable);
}

hsa_status_t DynamicDriver::GetWallclockFrequency(uint32_t node_id, uint64_t* frequency) const {
  return ftable_->get_wallclock_frequency(ftable_->ctx, node_id, frequency);
}

hsa_status_t DynamicDriver::AllocateScratchMemory(uint32_t node_id, uint64_t size,
                                                    void** mem) const {
  return ftable_->allocate_scratch_memory(ftable_->ctx, node_id, size, mem);
}

hsa_status_t DynamicDriver::AvailableMemory(uint32_t node_id,
                                             uint64_t* available_size) const {
  return ftable_->available_memory(ftable_->ctx, node_id, available_size);
}

hsa_status_t DynamicDriver::RegisterMemory(void* ptr, uint64_t size,
                                            HsaMemFlags mem_flags) const {
  return ftable_->register_memory(ftable_->ctx, ptr, size, mem_flags);
}

hsa_status_t DynamicDriver::DeregisterMemory(void* ptr) const {
  return ftable_->deregister_memory(ftable_->ctx, ptr);
}

hsa_status_t DynamicDriver::MakeMemoryResident(const void* mem, size_t size,
                                                uint64_t* alternate_va,
                                                const HsaMemMapFlags* mem_flags,
                                                uint32_t num_nodes,
                                                const uint32_t* nodes) const {
  return ftable_->make_memory_resident(ftable_->ctx, mem, size, alternate_va,
                                        mem_flags, num_nodes, nodes);
}

hsa_status_t DynamicDriver::MakeMemoryUnresident(const void* mem) const {
  return ftable_->make_memory_unresident(ftable_->ctx, mem);
}

hsa_status_t DynamicDriver::GetQueueSaveAreaInfo(HSA_QUEUEID queue_id, void** address,
                                                  size_t* size) const {
  return ftable_->get_queue_save_area_info(ftable_->ctx, queue_id, address, size);
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
  return ftable_->create_queue(ftable_->ctx, node_id, static_cast<uint32_t>(type), queue_pct,
                                static_cast<uint32_t>(priority), sdma_engine_id, queue_addr,
                                queue_size_bytes, queue_metadata_size_bytes, event,
                                &queue_resource, driver_data);
}

hsa_status_t DynamicDriver::DestroyQueueWithDriverData(HSA_QUEUEID queue_id,
                                                        void* driver_data) const {
  return ftable_->destroy_queue(ftable_->ctx, queue_id, driver_data);
}

} // namespace AMD
} // namespace rocr
