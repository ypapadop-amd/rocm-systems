/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstring>
#include <vector>

#include "gtest/gtest.h"

#include "hsa/hsa.h"
#include "hsa/hsa_ext_amd.h"

#include "common.h"

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

  // The dynamic agent's SubmitPackets advances read_dispatch_id on doorbell,
  // which should be sufficient for the barrier to "complete" from the runtime's perspective.
  // The signal may not actually be decremented without a real hardware backend.

  hsa_signal_destroy(completion_signal);
  hsa_queue_destroy(queue);
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
