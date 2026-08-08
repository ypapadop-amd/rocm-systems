/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_AQL_QUEUE_H_
#define HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_AQL_QUEUE_H_

#include "core/inc/amd_dynamic_agent.h"
#include "core/inc/queue.h"
#include "core/inc/runtime.h"
#include "core/inc/signal.h"

namespace rocr {
namespace AMD {

class DynamicAqlQueue : public core::Queue,
                        private core::LocalSignal,
                        core::DoorbellSignal {
 public:
  static __forceinline bool IsType(core::Signal *signal) {
    return signal->IsType(&rtti_id());
  }

  static __forceinline bool IsType(core::Queue *queue) {
    return queue->IsType(&rtti_id());
  }

  DynamicAqlQueue(core::SharedQueue* shared_queue, DynamicAgent* agent, size_t req_size_pkts,
                  uint32_t node_id, uint64_t flags);
  ~DynamicAqlQueue();

  hsa_status_t Inactivate() override;
  hsa_status_t SetPriority(HSA::hsa_amd_queue_priority_internal_t priority) override;
  void Destroy() override;
  uint64_t LoadReadIndexRelaxed() override;
  uint64_t LoadReadIndexAcquire() override;
  uint64_t LoadWriteIndexRelaxed() override;
  uint64_t LoadWriteIndexAcquire() override;
  void StoreReadIndexRelaxed(uint64_t value) override { assert(false); }
  void StoreReadIndexRelease(uint64_t value) override { assert(false); }
  void StoreWriteIndexRelaxed(uint64_t value) override;
  void StoreWriteIndexRelease(uint64_t value) override;
  uint64_t CasWriteIndexRelaxed(uint64_t expected, uint64_t value) override;
  uint64_t CasWriteIndexAcquire(uint64_t expected, uint64_t value) override;
  uint64_t CasWriteIndexRelease(uint64_t expected, uint64_t value) override;
  uint64_t CasWriteIndexAcqRel(uint64_t expected, uint64_t value) override;
  uint64_t AddWriteIndexRelaxed(uint64_t value) override;
  uint64_t AddWriteIndexAcquire(uint64_t value) override;
  uint64_t AddWriteIndexRelease(uint64_t value) override;
  uint64_t AddWriteIndexAcqRel(uint64_t value) override;
  void StoreRelaxed(hsa_signal_value_t value) override;
  void StoreRelease(hsa_signal_value_t value) override;
  hsa_status_t GetInfo(hsa_queue_info_attribute_t attribute,
                       void *value) override;
  hsa_status_t GetCUMasking(uint32_t num_cu_mask_count, uint32_t* cu_mask) override;
  hsa_status_t SetCUMasking(uint32_t num_cu_mask_count, const uint32_t* cu_mask) override;
  void ExecutePM4(uint32_t* cmd_data, size_t cmd_size_b, hsa_fence_scope_t acquireFence,
                  hsa_fence_scope_t releaseFence, hsa_signal_t* signal) override;

 protected:
  bool _IsA(Queue::rtti_t id) const override { return id == &rtti_id(); }

 private:
  static __forceinline int& rtti_id() {
    static int rtti_id_ = 0;
    return rtti_id_;
  }

  HSA_QUEUEID queue_id_ = INVALID_QUEUEID;
  void* ring_buf_ = nullptr;
  std::atomic<bool> active_ = false;
};

} // namespace AMD
} // namespace rocr

#endif  // HSA_RUNTIME_CORE_INC_AMD_DYNAMIC_AQL_QUEUE_H_
