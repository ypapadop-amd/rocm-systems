/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_DRIVER_H_
#define HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_DRIVER_H_

#include <memory>

#include "core/driver/dynamic/rocr_dynamic_driver.h"
#include "core/inc/driver.h"

namespace rocr {
namespace AMD {

class DynamicDriver final : public core::Driver {
public:
  static hsa_status_t DiscoverDriver(std::unique_ptr<core::Driver>& driver);

  DynamicDriver(rocr_dynamic_driver_ftable_t* ftable, rocr_dynamic_driver_context_t* ctx);
  ~DynamicDriver();

  hsa_status_t Init() override;
  hsa_status_t ShutDown() override;
  hsa_status_t QueryKernelModeDriver(core::DriverQuery query) override;
  hsa_status_t Open() override;
  hsa_status_t Close() override;
  hsa_status_t GetSystemProperties(HsaSystemProperties& sys_props) const override;
  hsa_status_t GetNodeProperties(HsaNodeProperties& node_props, uint32_t node_id) const override;
  hsa_status_t GetEdgeProperties(std::vector<HsaIoLinkProperties>& io_link_props,
                                 uint32_t node_id) const override;
  hsa_status_t GetMemoryProperties(uint32_t node_id,
                                   std::vector<HsaMemoryProperties>& mem_props) const override;
  hsa_status_t GetCacheProperties(uint32_t node_id, uint32_t processor_id,
                                  std::vector<HsaCacheProperties>& cache_props) const override;
  hsa_status_t AllocateMemory(const core::MemoryRegion& mem_region,
                              core::MemoryRegion::AllocateFlags alloc_flags,
                              void** mem, size_t size, uint32_t node_id) override;
  hsa_status_t FreeMemory(void* mem, size_t size) override;
  hsa_status_t CreateQueue(uint32_t node_id, HSA_QUEUE_TYPE type, uint32_t queue_pct,
                           HSA::hsa_amd_queue_priority_internal_t priority, uint32_t sdma_engine_id,
                           void* queue_addr, uint64_t queue_size_bytes,
                           uint64_t queue_metadata_size_bytes, HsaEvent* event,
                           HsaQueueResource& queue_resource) const override;
  hsa_status_t DestroyQueue(HSA_QUEUEID queue_id) const override;
  hsa_status_t UpdateQueue(HSA_QUEUEID queue_id, uint32_t queue_pct,
                           HSA::hsa_amd_queue_priority_internal_t priority, void* queue_addr,
                           uint64_t queue_size_bytes, HsaEvent* event) const override;
  hsa_status_t SetQueueCUMask(HSA_QUEUEID queue_id, uint32_t cu_mask_count,
                              uint32_t* queue_cu_mask) const override;
  hsa_status_t AllocQueueGWS(HSA_QUEUEID queue_id, uint32_t num_gws,
                             uint32_t* first_gws) const override;
  hsa_status_t ExportMemoryHandle(const core::Agent& agent, const core::DriverMemoryHandle& handle,
                                  core::ShareType type, void* export_handle) override;
  hsa_status_t ImportMemoryHandle(const core::Agent& agent, core::DriverMemoryHandle* handle,
                                  core::ShareType type, void* import_handle,
                                  void* mem = nullptr) override;
  hsa_status_t Map(const core::DriverMemoryHandle& handle, void* mem, size_t offset, size_t size,
                   hsa_access_permission_t perms, uint32_t node_id) override;
  hsa_status_t Unmap(const core::DriverMemoryHandle& handle, void* mem, size_t offset, size_t size,
                     uint32_t node_id) override;
  hsa_status_t CreateShareableHandle(void* va, void* mem, size_t size, const core::Agent& agent,
                                     core::DriverMemoryHandle* handle, uint64_t* offset) override;
  hsa_status_t DestroyMemoryHandle(core::DriverMemoryHandle* handle) override;
  hsa_status_t SPMAcquire(uint32_t preferred_node_id) const override;
  hsa_status_t SPMRelease(uint32_t preferred_node_id) const override;
  hsa_status_t SPMSetDestBuffer(uint32_t preferred_node_id, uint32_t size_bytes,
                                uint32_t* timeout, uint32_t* size_copied,
                                void* dest_mem_addr, bool* is_spm_data_loss) const override;
  hsa_status_t OpenSMI(uint32_t node_id, int* fd) const override;
  hsa_status_t SetTrapHandler(uint32_t node_id, const void* base, uint64_t base_size,
                              const void* buffer_base, uint64_t buffer_base_size) const override;
  hsa_status_t GetDeviceHandle(uint32_t node_id, void** device_handle) const override;
  hsa_status_t GetDeviceFd(uint32_t node_id, int* fd) const override;
  hsa_status_t GetClockCounters(uint32_t node_id,
                                HsaClockCounters* clock_counter) const override;
  hsa_status_t GetTileConfig(uint32_t node_id, HsaGpuTileConfig* config) const override;
  hsa_status_t IsModelEnabled(bool* enable) const override;
  hsa_status_t GetWallclockFrequency(uint32_t node_id, uint64_t* frequency) const override;
  hsa_status_t AllocateScratchMemory(uint32_t node_id, uint64_t size, void** mem) const override;
  hsa_status_t AvailableMemory(uint32_t node_id, uint64_t* available_size) const override;
  hsa_status_t RegisterMemory(void* ptr, uint64_t size, HsaMemFlags mem_flags) const override;
  hsa_status_t DeregisterMemory(void* ptr) const override;
  hsa_status_t MakeMemoryResident(const void* mem, size_t size, uint64_t* alternate_va,
                                  const HsaMemMapFlags* mem_flags, uint32_t num_nodes,
                                  const uint32_t* nodes) const override;
  hsa_status_t MakeMemoryUnresident(const void* mem) const override;
  hsa_status_t GetQueueSaveAreaInfo(HSA_QUEUEID queue_id, void** address,
                                    size_t* size) const override;

private:
  rocr_dynamic_driver_ftable_t* ftable_;
  rocr_dynamic_driver_context_t* ctx_;
};

} // namespace AMD
} // namespace rocr

#endif // HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_DRIVER_H_
