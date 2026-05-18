/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include "core/inc/amd_dynamic_aql_queue.h"

#include <atomic>
#include <cassert>

#include "core/inc/amd_dynamic_driver.h"
#include "core/inc/queue.h"
#include "core/inc/runtime.h"
#include "core/inc/signal.h"

namespace rocr {
namespace AMD {

DynamicAqlQueue::DynamicAqlQueue(core::SharedQueue* shared_queue, DynamicAgent* agent,
                                 size_t req_size_pkts, uint32_t node_id, uint64_t flags)
    : Queue(shared_queue, flags, agent),
      LocalSignal(0, false),
      DoorbellSignal(signal()),
      queue_size_bytes_(req_size_pkts * sizeof(core::AqlPacket)) {
  if (agent->device_type() != core::Agent::DeviceType::kDynamicDevice) {
    throw hsa_exception(HSA_STATUS_ERROR_INVALID_AGENT,
                        "Attempting to create a Dynamic queue on a non-Dynamic agent.");
  }

  ring_buf_ =
      agent->system_allocator()(queue_size_bytes_, 4096, core::MemoryRegion::AllocateNoFlags);
  if (!ring_buf_) {
    throw hsa_exception(HSA_STATUS_ERROR_INVALID_QUEUE_CREATION,
                        "Could not allocate a ring buffer for a Dynamic queue.");
  }
  MAKE_NAMED_SCOPE_GUARD(ring_buf_guard, [&] { agent->system_deallocator()(ring_buf_); });

  amd_queue_.hsa_queue.type = HSA_QUEUE_TYPE_SINGLE;
  amd_queue_.hsa_queue.features = 0;
  amd_queue_.hsa_queue.size = req_size_pkts;
  amd_queue_.hsa_queue.base_address = ring_buf_;
  amd_queue_.hsa_queue.doorbell_signal = Signal::Convert(this);
  amd_queue_.hsa_queue.id = GetQueueId();

  amd_queue_.write_dispatch_id = 0;
  amd_queue_.read_dispatch_id = 0;

  signal_.hardware_doorbell_ptr = nullptr;
  signal_.kind = AMD_SIGNAL_KIND_DOORBELL;
  signal_.queue_ptr = &amd_queue_;

  HsaQueueResource queue_resource = {};
  auto& dyn_driver = static_cast<DynamicDriver&>(agent->driver());
  hsa_status_t status = dyn_driver.CreateQueueWithDriverData(
      node_id, HSA_QUEUE_COMPUTE_AQL, 0, rocr::HSA::HSA_AMD_QUEUE_PRIORITY_NORMAL, 0, nullptr,
      queue_size_bytes_, 0, nullptr, queue_resource, &driver_data_);
  if (status != HSA_STATUS_SUCCESS) {
    throw hsa_exception(status, "Failed to create a hardware context for a Dynamic queue.");
  }
  queue_id_ = queue_resource.QueueId;

  active_ = true;

  ring_buf_guard.Dismiss();
}

DynamicAqlQueue::~DynamicAqlQueue() {
  DynamicAqlQueue::Inactivate();
  if (ring_buf_) {
    auto& agent = static_cast<DynamicAgent&>(*GetAgent());
    agent.system_deallocator()(ring_buf_);
  }
  if (shared_queue_) {
    core::Runtime::runtime_singleton_->system_deallocator()(shared_queue_);
  }
}

hsa_status_t DynamicAqlQueue::Inactivate() {
  bool active = active_.exchange(false, std::memory_order_relaxed);
  if (active) {
    auto& dyn_driver = static_cast<DynamicDriver&>(GetAgent()->driver());
    auto err = dyn_driver.DestroyQueueWithDriverData(queue_id_, driver_data_);
    assert(err == HSA_STATUS_SUCCESS && "Destroy queue failed.");
    (void)err;
    driver_data_ = nullptr;
    atomic::Fence(std::memory_order_acquire);
  }
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicAqlQueue::SetPriority(HSA::hsa_amd_queue_priority_internal_t priority) {
  return HSA_STATUS_SUCCESS;
}

void DynamicAqlQueue::Destroy() { delete this; }

uint64_t DynamicAqlQueue::LoadReadIndexRelaxed() {
  return atomic::Load(&amd_queue_.read_dispatch_id, std::memory_order_relaxed);
}

uint64_t DynamicAqlQueue::LoadReadIndexAcquire() {
  return atomic::Load(&amd_queue_.read_dispatch_id, std::memory_order_acquire);
}

uint64_t DynamicAqlQueue::LoadWriteIndexRelaxed() {
  return atomic::Load(&amd_queue_.write_dispatch_id, std::memory_order_relaxed);
}

uint64_t DynamicAqlQueue::LoadWriteIndexAcquire() {
  return atomic::Load(&amd_queue_.write_dispatch_id, std::memory_order_acquire);
}

void DynamicAqlQueue::StoreWriteIndexRelaxed(uint64_t value) {
  atomic::Store(&amd_queue_.write_dispatch_id, value, std::memory_order_relaxed);
}

void DynamicAqlQueue::StoreWriteIndexRelease(uint64_t value) {
  atomic::Store(&amd_queue_.write_dispatch_id, value, std::memory_order_release);
}

uint64_t DynamicAqlQueue::CasWriteIndexRelaxed(uint64_t expected, uint64_t value) {
  return atomic::Cas(&amd_queue_.write_dispatch_id, value, expected,
                     std::memory_order_relaxed);
}

uint64_t DynamicAqlQueue::CasWriteIndexAcquire(uint64_t expected, uint64_t value) {
  return atomic::Cas(&amd_queue_.write_dispatch_id, value, expected,
                     std::memory_order_acquire);
}

uint64_t DynamicAqlQueue::CasWriteIndexRelease(uint64_t expected, uint64_t value) {
  return atomic::Cas(&amd_queue_.write_dispatch_id, value, expected,
                     std::memory_order_release);
}

uint64_t DynamicAqlQueue::CasWriteIndexAcqRel(uint64_t expected, uint64_t value) {
  return atomic::Cas(&amd_queue_.write_dispatch_id, value, expected,
                     std::memory_order_acq_rel);
}

uint64_t DynamicAqlQueue::AddWriteIndexRelaxed(uint64_t value) {
  return atomic::Add(&amd_queue_.write_dispatch_id, value, std::memory_order_relaxed);
}

uint64_t DynamicAqlQueue::AddWriteIndexAcquire(uint64_t value) {
  return atomic::Add(&amd_queue_.write_dispatch_id, value, std::memory_order_acquire);
}

uint64_t DynamicAqlQueue::AddWriteIndexRelease(uint64_t value) {
  return atomic::Add(&amd_queue_.write_dispatch_id, value, std::memory_order_release);
}

uint64_t DynamicAqlQueue::AddWriteIndexAcqRel(uint64_t value) {
  return atomic::Add(&amd_queue_.write_dispatch_id, value, std::memory_order_acq_rel);
}

void DynamicAqlQueue::StoreRelaxed(hsa_signal_value_t value) { SubmitPackets(); }

void DynamicAqlQueue::SubmitPackets() {
  if (!active_.load(std::memory_order_relaxed)) {
    return;
  }

  const uint64_t last_pkt_idx = LoadWriteIndexAcquire();
  atomic::Store(&amd_queue_.read_dispatch_id, last_pkt_idx, std::memory_order_release);
}

void DynamicAqlQueue::StoreRelease(hsa_signal_value_t value) {
  std::atomic_thread_fence(std::memory_order_release);
  StoreRelaxed(value);
}

hsa_status_t DynamicAqlQueue::GetInfo(hsa_queue_info_attribute_t attribute, void* value) {
  switch (attribute) {
    case HSA_AMD_QUEUE_INFO_AGENT:
      *static_cast<hsa_agent_t*>(value) = GetAgent()->public_handle();
      break;
    case HSA_AMD_QUEUE_INFO_DOORBELL_ID:
      *static_cast<uint64_t*>(value) = reinterpret_cast<uint64_t>(signal_.hardware_doorbell_ptr);
      break;
    case HSA_QUEUE_INFO_USE_COUNT:
      *static_cast<uint32_t*>(value) = static_cast<uint32_t>(-1);
      break;
    case HSA_QUEUE_INFO_HW_ID:
      *static_cast<uint32_t*>(value) = public_handle()->id;
      break;
    default:
      return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicAqlQueue::GetCUMasking(uint32_t num_cu_mask_count, uint32_t* cu_mask) {
  return HSA_STATUS_ERROR_INVALID_QUEUE;
}

hsa_status_t DynamicAqlQueue::SetCUMasking(uint32_t num_cu_mask_count, const uint32_t* cu_mask) {
  return HSA_STATUS_ERROR_INVALID_QUEUE;
}

void DynamicAqlQueue::ExecutePM4(uint32_t* cmd_data, size_t cmd_size_b,
                                 hsa_fence_scope_t acquireFence,
                                 hsa_fence_scope_t releaseFence, hsa_signal_t* signal) {
  assert(false && "Dynamic AQL queue does not support PM4 packets.");
}

} // namespace AMD
} // namespace rocr
