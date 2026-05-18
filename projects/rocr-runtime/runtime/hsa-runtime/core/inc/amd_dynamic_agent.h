/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_AGENT_H_
#define HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_AGENT_H_

#include "core/inc/agent.h"
#include "core/inc/runtime.h"

namespace rocr {
namespace AMD {

class DynamicAgent : public core::Agent {
public:
  DynamicAgent(uint32_t node, const HsaNodeProperties& node_props, void* driver_data);
  ~DynamicAgent();

  hsa_status_t IterateRegion(hsa_status_t (*callback)(hsa_region_t region, void* data),
                             void* data) const override;

  hsa_status_t IterateCache(hsa_status_t (*callback)(hsa_cache_t cache, void* data),
                            void* data) const override;

  hsa_status_t IterateSupportedIsas(hsa_status_t (*callback)(hsa_isa_t isa, void* data),
                                    void* data) const override;

  hsa_status_t GetInfo(hsa_agent_info_t attribute, void* value) const override;

  void InitDerivedCuid() override;

  hsa_status_t QueueCreate(size_t size, hsa_queue_type32_t queue_type, uint64_t flags,
                           core::HsaEventCallback event_callback, void* data,
                           uint32_t private_segment_size, uint32_t group_segment_size,
                           bool metadata_prefetch,
                           core::Queue** queue) override;

  const std::vector<const core::Isa*>& supported_isas() const override { return supported_isas_; }

  const std::vector<std::shared_ptr<const core::MemoryRegion>>& regions() const override {
    return regions_;
  }

  void* driver_data() const { return driver_data_; }

  const std::function<void*(size_t, size_t, core::MemoryRegion::AllocateFlags)>&
  system_allocator() const { return system_allocator_; }

  const std::function<void(void*)>& system_deallocator() const { return system_deallocator_; }

private:
  void InitRegionList();
  void InitAllocators();

  std::vector<std::shared_ptr<const core::MemoryRegion>> regions_;
  std::function<void*(size_t, size_t, core::MemoryRegion::AllocateFlags)> system_allocator_;
  std::function<void(void*)> system_deallocator_;

  void* driver_data_;
  const HsaNodeProperties node_props_;

  const hsa_profile_t profile_ = HSA_PROFILE_BASE;
  const uint32_t min_aql_size_ = 0x40;
  const uint32_t max_aql_size_ = 0x40;
  const uint32_t max_queues_ = 1;
};

} // namespace AMD
} // namespace rocr

#endif // HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_AGENT_H_
