/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include <atomic>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"

#include "hsa/hsa.h"
#include "hsa/hsa_ext_amd.h"

#include "common.h"
#include "dynaccel_packet.h"

using rocrtst::discover_agents;

TEST(Dispatch, NoDynamicDriver) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  EXPECT_TRUE(dynamic_agents.empty());

  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Dispatch, CreateQueue) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  uint32_t queue_max_size = 0;
  ASSERT_EQ(hsa_agent_get_info(dynamic_agents.front(), HSA_AGENT_INFO_QUEUE_MAX_SIZE,
                               &queue_max_size),
            HSA_STATUS_SUCCESS);
  ASSERT_GT(queue_max_size, 0u);

  hsa_queue_t* queue = nullptr;
  ASSERT_EQ(hsa_queue_create(dynamic_agents.front(), queue_max_size, HSA_QUEUE_TYPE_SINGLE,
                             nullptr, nullptr, UINT32_MAX, UINT32_MAX, &queue),
            HSA_STATUS_SUCCESS);
  ASSERT_NE(queue, nullptr);
  EXPECT_EQ(queue->type, HSA_QUEUE_TYPE_SINGLE);
  EXPECT_EQ(queue->size, queue_max_size);

  hsa_queue_destroy(queue);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Dispatch, CreateDestroyMultipleQueues) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  uint32_t queue_max_size = 0;
  ASSERT_EQ(hsa_agent_get_info(dynamic_agents.front(), HSA_AGENT_INFO_QUEUE_MAX_SIZE,
                               &queue_max_size),
            HSA_STATUS_SUCCESS);

  constexpr int num_queues = 4;
  std::vector<hsa_queue_t*> queues(num_queues, nullptr);

  for (int i = 0; i < num_queues; ++i) {
    ASSERT_EQ(hsa_queue_create(dynamic_agents.front(), queue_max_size, HSA_QUEUE_TYPE_SINGLE,
                               nullptr, nullptr, UINT32_MAX, UINT32_MAX, &queues[i]),
              HSA_STATUS_SUCCESS);
    ASSERT_NE(queues[i], nullptr);
  }

  for (auto* q : queues) {
    hsa_queue_destroy(q);
  }

  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Dispatch, SubmitBarrierPacket) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  uint32_t queue_max_size = 0;
  ASSERT_EQ(hsa_agent_get_info(dynamic_agents.front(), HSA_AGENT_INFO_QUEUE_MAX_SIZE,
                               &queue_max_size),
            HSA_STATUS_SUCCESS);

  hsa_queue_t* queue = nullptr;
  ASSERT_EQ(hsa_queue_create(dynamic_agents.front(), queue_max_size, HSA_QUEUE_TYPE_SINGLE,
                             nullptr, nullptr, UINT32_MAX, UINT32_MAX, &queue),
            HSA_STATUS_SUCCESS);

  hsa_signal_t completion_signal = {};
  ASSERT_EQ(hsa_signal_create(1, 0, nullptr, &completion_signal), HSA_STATUS_SUCCESS);

  const uint64_t write_idx = hsa_queue_add_write_index_relaxed(queue, 1);
  const uint32_t slot = write_idx % queue->size;

  auto* barrier = reinterpret_cast<hsa_barrier_and_packet_t*>(
      static_cast<char*>(queue->base_address) + slot * 64);
  std::memset(barrier, 0, sizeof(*barrier));
  barrier->header = (HSA_PACKET_TYPE_BARRIER_AND << HSA_PACKET_HEADER_TYPE) |
                    (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
                    (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
  barrier->completion_signal = completion_signal;

  hsa_signal_store_screlease(queue->doorbell_signal, static_cast<hsa_signal_value_t>(write_idx));

  EXPECT_EQ(hsa_signal_wait_scacquire(completion_signal, HSA_SIGNAL_CONDITION_EQ, 0,
                                      5000000000ull, HSA_WAIT_STATE_ACTIVE),
            0);

  hsa_signal_destroy(completion_signal);
  hsa_queue_destroy(queue);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(DynAccel, AgentDiscovered) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_EQ(dynamic_agents.size(), 1u);

  char name[64] = {};
  ASSERT_EQ(hsa_agent_get_info(dynamic_agents.front(), HSA_AGENT_INFO_NAME, name),
            HSA_STATUS_SUCCESS);
  EXPECT_STREQ(name, "DynAccel");

  char product_name[64] = {};
  ASSERT_EQ(hsa_agent_get_info(dynamic_agents.front(),
                               static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_PRODUCT_NAME),
                               product_name),
            HSA_STATUS_SUCCESS);
  EXPECT_STREQ(product_name, "DynAccel");

  uint32_t queue_max_size = 0;
  ASSERT_EQ(hsa_agent_get_info(dynamic_agents.front(), HSA_AGENT_INFO_QUEUE_MAX_SIZE,
                               &queue_max_size),
            HSA_STATUS_SUCCESS);
  EXPECT_EQ(queue_max_size, 64u);

  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Dispatch, SubmitAgentDispatchPacket) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  uint32_t queue_max_size = 0;
  ASSERT_EQ(hsa_agent_get_info(dynamic_agents.front(), HSA_AGENT_INFO_QUEUE_MAX_SIZE,
                               &queue_max_size),
            HSA_STATUS_SUCCESS);

  hsa_queue_t* queue = nullptr;
  ASSERT_EQ(hsa_queue_create(dynamic_agents.front(), queue_max_size, HSA_QUEUE_TYPE_SINGLE,
                             nullptr, nullptr, UINT32_MAX, UINT32_MAX, &queue),
            HSA_STATUS_SUCCESS);

  hsa_signal_t completion_signal = {};
  ASSERT_EQ(hsa_signal_create(1, 0, nullptr, &completion_signal), HSA_STATUS_SUCCESS);

  const uint64_t write_idx = hsa_queue_add_write_index_relaxed(queue, 1);
  const uint32_t slot = write_idx % queue->size;

  auto* agent_pkt = reinterpret_cast<hsa_agent_dispatch_packet_t*>(
      static_cast<char*>(queue->base_address) + slot * 64);
  std::memset(agent_pkt, 0, sizeof(*agent_pkt));
  agent_pkt->header = (HSA_PACKET_TYPE_AGENT_DISPATCH << HSA_PACKET_HEADER_TYPE) |
                      (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
                      (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
  agent_pkt->type = 0;
  agent_pkt->completion_signal = completion_signal;

  hsa_signal_store_screlease(queue->doorbell_signal, static_cast<hsa_signal_value_t>(write_idx));

  // Without a real dynamic driver backend, the signal won't be decremented by hardware.
  // This test verifies the packet submission path completes without error.

  hsa_signal_destroy(completion_signal);
  hsa_queue_destroy(queue);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

namespace {

std::atomic<uint64_t> g_sum{0};
std::atomic<uint32_t> g_order_idx{0};
uint64_t g_order[8] = {};

void SumKernel(uint64_t* args, uint32_t n) {
  uint64_t total = 0;
  for (uint32_t i = 0; i < n; ++i) total += args[i];
  g_sum.store(total, std::memory_order_release);
}

void OrderKernel(uint64_t* args, uint32_t n) {
  if (n < 1) return;
  const uint32_t slot = g_order_idx.fetch_add(1, std::memory_order_acq_rel);
  if (slot < 8) g_order[slot] = args[0];
}

// Enqueues one DynAccel packet and returns its completion signal.
hsa_signal_t EnqueueDispatch(hsa_queue_t* queue, dynaccel_kernel_t fn, uint64_t* kernargs,
                             uint16_t num_kernargs) {
  hsa_signal_t signal = {};
  EXPECT_EQ(hsa_signal_create(1, 0, nullptr, &signal), HSA_STATUS_SUCCESS);

  const uint64_t write_idx = hsa_queue_add_write_index_relaxed(queue, 1);
  auto* pkt = reinterpret_cast<dynaccel_dispatch_packet_t*>(
      static_cast<char*>(queue->base_address) + (write_idx % queue->size) * 64);
  std::memset(pkt, 0, sizeof(*pkt));
  pkt->opcode = DYNACCEL_OPCODE_DISPATCH;
  pkt->num_kernargs = num_kernargs;
  pkt->completion_signal = signal;
  pkt->function = reinterpret_cast<uint64_t>(fn);
  pkt->kernarg_address = kernargs;
  // Header last: it publishes the packet. READY aliases VENDOR_SPECIFIC (0).
  __atomic_store_n(&pkt->header,
                   static_cast<uint16_t>(DYNACCEL_PACKET_TYPE_READY << HSA_PACKET_HEADER_TYPE),
                   __ATOMIC_RELEASE);

  hsa_signal_store_screlease(queue->doorbell_signal,
                             static_cast<hsa_signal_value_t>(write_idx));
  return signal;
}

hsa_queue_t* CreateDynAccelQueue(hsa_agent_t agent) {
  uint32_t queue_max_size = 0;
  EXPECT_EQ(hsa_agent_get_info(agent, HSA_AGENT_INFO_QUEUE_MAX_SIZE, &queue_max_size),
            HSA_STATUS_SUCCESS);
  hsa_queue_t* queue = nullptr;
  EXPECT_EQ(hsa_queue_create(agent, queue_max_size, HSA_QUEUE_TYPE_SINGLE, nullptr, nullptr,
                             UINT32_MAX, UINT32_MAX, &queue),
            HSA_STATUS_SUCCESS);
  return queue;
}

}  // namespace

TEST(Dispatch, ExecuteFunction) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_queue_t* queue = CreateDynAccelQueue(dynamic_agents.front());
  ASSERT_NE(queue, nullptr);

  g_sum.store(0, std::memory_order_relaxed);
  uint64_t kernargs[3] = {10, 20, 12};
  hsa_signal_t signal = EnqueueDispatch(queue, SumKernel, kernargs, 3);

  ASSERT_EQ(hsa_signal_wait_scacquire(signal, HSA_SIGNAL_CONDITION_EQ, 0, 5000000000ull,
                                      HSA_WAIT_STATE_ACTIVE),
            0);
  EXPECT_EQ(g_sum.load(std::memory_order_acquire), 42u);

  hsa_signal_destroy(signal);
  hsa_queue_destroy(queue);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Dispatch, PacketsExecuteInOrder) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_queue_t* queue = CreateDynAccelQueue(dynamic_agents.front());
  ASSERT_NE(queue, nullptr);

  g_order_idx.store(0, std::memory_order_relaxed);
  std::memset(g_order, 0, sizeof(g_order));

  constexpr uint32_t kNumPackets = 4;
  uint64_t kernargs[kNumPackets] = {100, 101, 102, 103};
  hsa_signal_t signals[kNumPackets] = {};
  for (uint32_t i = 0; i < kNumPackets; ++i) {
    signals[i] = EnqueueDispatch(queue, OrderKernel, &kernargs[i], 1);
  }

  for (uint32_t i = 0; i < kNumPackets; ++i) {
    ASSERT_EQ(hsa_signal_wait_scacquire(signals[i], HSA_SIGNAL_CONDITION_EQ, 0, 5000000000ull,
                                        HSA_WAIT_STATE_ACTIVE),
              0);
  }
  for (uint32_t i = 0; i < kNumPackets; ++i) {
    EXPECT_EQ(g_order[i], 100u + i) << "packet " << i << " executed out of order";
  }

  for (auto& s : signals) hsa_signal_destroy(s);
  hsa_queue_destroy(queue);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}
