/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_AGENT_H_
#define HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_AGENT_H_

#include "core/driver/dynamic/rocr_dynamic_driver.h"
#include "core/inc/agent.h"
#include "core/inc/runtime.h"

namespace rocr {
namespace AMD {

class DynamicAgent : public core::Agent {
public:
  DynamicAgent(uint32_t node, const HsaNodeProperties& node_props);
  ~DynamicAgent();

  hsa_status_t IterateRegion(hsa_status_t (*callback)(hsa_region_t region, void* data),
                             void* data) const override;

  hsa_status_t VisitRegion(bool include_peer,
                           hsa_status_t (*callback)(hsa_region_t region, void* data),
                           void* data) const override;

  core::Agent* GetNearestCpuAgent() const override;

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

  const std::function<void*(size_t, size_t, core::MemoryRegion::AllocateFlags)>&
  system_allocator() const { return system_allocator_; }

  const std::function<void(void*)>& system_deallocator() const { return system_deallocator_; }

  const rocr_dynamic_driver_agent_props_t& properties() const { return props_; }

private:
  void InitRegionList();
  void InitAllocators();

  std::vector<std::shared_ptr<const core::MemoryRegion>> regions_;
  std::vector<const core::Isa*> supported_isas_;
  std::function<void*(size_t, size_t, core::MemoryRegion::AllocateFlags)> system_allocator_;
  std::function<void(void*)> system_deallocator_;

  const HsaNodeProperties node_props_;

  rocr_dynamic_driver_agent_props_t props_ = {};
};

} // namespace AMD
} // namespace rocr

#endif // HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_AGENT_H_
