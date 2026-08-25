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

namespace {

hsa_status_t discover_first_global_coarse_grain_mem_pool(hsa_amd_memory_pool_t pool, void* data) {
  if (!data) {
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  hsa_amd_segment_t segment = {};
  auto status = hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &segment);
  if ((status != HSA_STATUS_SUCCESS) || (segment != HSA_AMD_SEGMENT_GLOBAL)) {
    return status;
  }

  hsa_amd_memory_pool_global_flag_t flags = {};
  status = hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &flags);
  if ((status != HSA_STATUS_SUCCESS) ||
      ((flags & HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED) == 0x0)) {
    return status;
  }

  std::size_t alloc_granule = 0;
  status = hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_GRANULE,
                                        &alloc_granule);
  if ((status != HSA_STATUS_SUCCESS) || (alloc_granule == 0)) {
    return status;
  }

  auto* global_memory_pool = static_cast<hsa_amd_memory_pool_t*>(data);
  *global_memory_pool = pool;

  return HSA_STATUS_INFO_BREAK;
}

}  // namespace

TEST(Memory, NoDynamicDriver) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  EXPECT_TRUE(dynamic_agents.empty());

  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, DMABufExportImportGPUtoDynamic) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> gpu_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_GPU>, &gpu_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(gpu_agents.empty());

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          gpu_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  std::uint32_t* buffer = {};
  ASSERT_EQ(hsa_amd_memory_pool_allocate(global_memory_pool, allocation_size, 0,
                                         reinterpret_cast<void**>(&buffer)),
            HSA_STATUS_SUCCESS);
  ASSERT_NE(buffer, nullptr);

  for (std::size_t i = 0; i < buffer_size; ++i) {
    buffer[i] = i;
  }

  int dma_buf_fd = -1;
  std::uint64_t dma_buf_offset = 0;
  EXPECT_EQ(hsa_amd_portable_export_dmabuf(buffer, allocation_size, &dma_buf_fd, &dma_buf_offset),
            HSA_STATUS_SUCCESS);
  EXPECT_GT(dma_buf_fd, 0);

  const std::uint32_t num_agents = dynamic_agents.size();
  auto* agents = dynamic_agents.data();
  std::size_t import_size = 0;
  std::uint32_t* import_buffer = nullptr;
  // Expected to fail until hsa_amd_interop_map_buffer for dynamic agents is supported
  EXPECT_NE(hsa_amd_interop_map_buffer(num_agents, agents, dma_buf_fd, 0, &import_size,
                                       reinterpret_cast<void**>(&import_buffer), nullptr, nullptr),
            HSA_STATUS_SUCCESS);

  // cleanup
  EXPECT_EQ(hsa_amd_portable_close_dmabuf(dma_buf_fd), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_memory_pool_free(buffer), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, DMABufExportImportDynamicToGPU) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> gpu_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_GPU>, &gpu_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(gpu_agents.empty());

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  std::uint32_t* buffer = {};
  ASSERT_EQ(hsa_amd_memory_pool_allocate(global_memory_pool, allocation_size, 0,
                                         reinterpret_cast<void**>(&buffer)),
            HSA_STATUS_SUCCESS);
  ASSERT_NE(buffer, nullptr);

  for (std::size_t i = 0; i < buffer_size; ++i) {
    buffer[i] = i;
  }

  int dma_buf_fd = -1;
  std::uint64_t dma_buf_offset = 0;
  EXPECT_EQ(hsa_amd_portable_export_dmabuf(buffer, allocation_size, &dma_buf_fd, &dma_buf_offset),
            HSA_STATUS_SUCCESS);
  EXPECT_GT(dma_buf_fd, 0);

  const std::uint32_t num_agents = gpu_agents.size();
  auto* agents = gpu_agents.data();
  std::size_t import_size = 0;
  std::uint32_t* import_buffer = nullptr;
  // Expected to fail until interop map for dynamic-to-GPU path is supported
  EXPECT_NE(hsa_amd_interop_map_buffer(num_agents, agents, dma_buf_fd, 0, &import_size,
                                       reinterpret_cast<void**>(&import_buffer), nullptr, nullptr),
            HSA_STATUS_SUCCESS);

  // cleanup
  EXPECT_EQ(hsa_amd_portable_close_dmabuf(dma_buf_fd), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_memory_pool_free(buffer), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, MemoryLock) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  std::vector<void*> agent_ptrs(dynamic_agents.size());

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  std::uint32_t* buffer = new std::uint32_t[buffer_size];
  ASSERT_NE(buffer, nullptr);

  for (std::size_t i = 0; i < buffer_size; ++i) {
    buffer[i] = i;
  }

  const std::uint32_t num_agents = dynamic_agents.size();
  auto* agents = dynamic_agents.data();
  // Not supported yet
  EXPECT_NE(hsa_amd_memory_lock(buffer, allocation_size, agents, num_agents, agent_ptrs.data()),
            HSA_STATUS_SUCCESS);

  delete[] buffer;

  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, PoolAllocateAllowAccessGPUtoDynamic) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> gpu_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_GPU>, &gpu_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(gpu_agents.empty());

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          gpu_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  std::uint32_t* buffer = {};
  ASSERT_EQ(hsa_amd_memory_pool_allocate(global_memory_pool, allocation_size, 0,
                                         reinterpret_cast<void**>(&buffer)),
            HSA_STATUS_SUCCESS);
  ASSERT_NE(buffer, nullptr);

  for (std::size_t i = 0; i < buffer_size; ++i) {
    buffer[i] = i;
  }

  const std::uint32_t num_agents = dynamic_agents.size();
  auto* agents = dynamic_agents.data();
  // Not supported yet
  EXPECT_NE(hsa_amd_agents_allow_access(num_agents, agents, nullptr, buffer), HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_amd_memory_pool_free(buffer), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, PoolAllocateAllowAccessDynamicToGPU) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> gpu_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_GPU>, &gpu_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(gpu_agents.empty());

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  std::uint32_t* buffer = {};
  ASSERT_EQ(hsa_amd_memory_pool_allocate(global_memory_pool, allocation_size, 0,
                                         reinterpret_cast<void**>(&buffer)),
            HSA_STATUS_SUCCESS);
  ASSERT_NE(buffer, nullptr);

  for (std::size_t i = 0; i < buffer_size; ++i) {
    buffer[i] = i;
  }

  const std::uint32_t num_agents = gpu_agents.size();
  auto* agents = gpu_agents.data();
  // Not supported yet
  EXPECT_NE(hsa_amd_agents_allow_access(num_agents, agents, nullptr, buffer), HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_amd_memory_pool_free(buffer), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemCreate) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  EXPECT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemMap) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  const std::uint64_t address = 0;
  const std::uint64_t alignment = 0;
  std::uint32_t* buffer = nullptr;
  ASSERT_EQ(hsa_amd_vmem_address_reserve_align(reinterpret_cast<void**>(&buffer), allocation_size,
                                               address, alignment, 0),
            HSA_STATUS_SUCCESS);
  ASSERT_NE(buffer, nullptr);

  const std::uint64_t offset = 0;
  EXPECT_EQ(hsa_amd_vmem_map(buffer, allocation_size, offset, memory_handle, 0),
            HSA_STATUS_SUCCESS);

  // Granting the owning dynamic agent RW access establishes a real mmap of the
  // underlying allocation at `buffer` (dynaccel emulates device access with a host
  // mmap), so a write/read through the VA is a genuine data-plane check.
  hsa_amd_memory_access_desc_t desc{HSA_ACCESS_PERMISSION_RW, dynamic_agents.front()};
  ASSERT_EQ(hsa_amd_vmem_set_access(buffer, allocation_size, &desc, 1), HSA_STATUS_SUCCESS);

  for (std::size_t i = 0; i < buffer_size; ++i) {
    buffer[i] = static_cast<std::uint32_t>(i);
  }
  for (std::size_t i = 0; i < buffer_size; ++i) {
    EXPECT_EQ(buffer[i], static_cast<std::uint32_t>(i));
  }

  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_address_free(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemMapOutOfBoundsRejected) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  // Reserve twice the allocation size so the oversized map below clears the
  // address-reservation bounds check and reaches the allocation's own bounds check.
  constexpr std::size_t reservation_size = allocation_size * 2;
  std::uint32_t* buffer = nullptr;
  ASSERT_EQ(hsa_amd_vmem_address_reserve_align(reinterpret_cast<void**>(&buffer),
                                               reservation_size, 0, 0, 0),
            HSA_STATUS_SUCCESS);
  ASSERT_NE(buffer, nullptr);

  ASSERT_EQ(hsa_amd_vmem_map(buffer, reservation_size, 0, memory_handle, 0), HSA_STATUS_SUCCESS);

  hsa_amd_memory_access_desc_t desc{HSA_ACCESS_PERMISSION_RW, dynamic_agents.front()};
  EXPECT_NE(hsa_amd_vmem_set_access(buffer, reservation_size, &desc, 1), HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, reservation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_address_free(buffer, reservation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemSetAccess) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> cpu_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_CPU>, &cpu_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(cpu_agents.empty());

  std::vector<hsa_agent_t> gpu_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_GPU>, &gpu_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(gpu_agents.empty());

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          gpu_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  const std::uint64_t address = 0;
  const std::uint64_t alignment = 0;
  std::uint32_t* buffer = nullptr;
  ASSERT_EQ(hsa_amd_vmem_address_reserve_align(reinterpret_cast<void**>(&buffer), allocation_size,
                                               address, alignment, 0),
            HSA_STATUS_SUCCESS);

  const std::uint64_t offset = 0;
  ASSERT_EQ(hsa_amd_vmem_map(buffer, allocation_size, offset, memory_handle, 0),
            HSA_STATUS_SUCCESS);

  std::vector<hsa_amd_memory_access_desc_t> memory_access_desc;
  memory_access_desc.reserve(cpu_agents.size() + gpu_agents.size() + dynamic_agents.size());
  for (auto const& agent : cpu_agents) {
    memory_access_desc.push_back(hsa_amd_memory_access_desc_t{HSA_ACCESS_PERMISSION_RW, agent});
  }
  for (auto const& agent : gpu_agents) {
    memory_access_desc.push_back(hsa_amd_memory_access_desc_t{HSA_ACCESS_PERMISSION_RW, agent});
  }
  for (auto const& agent : dynamic_agents) {
    memory_access_desc.push_back(hsa_amd_memory_access_desc_t{HSA_ACCESS_PERMISSION_RW, agent});
  }

  EXPECT_EQ(hsa_amd_vmem_set_access(buffer, allocation_size, memory_access_desc.data(),
                                    memory_access_desc.size()),
            HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_address_free(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemSetAccessDynamicToGPU) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> gpu_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_GPU>, &gpu_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(gpu_agents.empty());

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  const std::uint64_t address = 0;
  const std::uint64_t alignment = 0;
  std::uint32_t* buffer = nullptr;
  ASSERT_EQ(hsa_amd_vmem_address_reserve_align(reinterpret_cast<void**>(&buffer), allocation_size,
                                               address, alignment, 0),
            HSA_STATUS_SUCCESS);

  const std::uint64_t offset = 0;
  ASSERT_EQ(hsa_amd_vmem_map(buffer, allocation_size, offset, memory_handle, 0),
            HSA_STATUS_SUCCESS);

  // The dynamic agent lazily exports its allocation as a dmabuf fd, which the GPU
  // driver then imports natively (see Runtime::VMemorySetAccessPerHandle).
  hsa_amd_memory_access_desc_t desc{HSA_ACCESS_PERMISSION_RW, gpu_agents.front()};
  EXPECT_EQ(hsa_amd_vmem_set_access(buffer, allocation_size, &desc, 1), HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_address_free(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemExportImportedHandleRejected) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  int dma_buf_fd = -1;
  ASSERT_EQ(hsa_amd_vmem_export_shareable_handle(&dma_buf_fd, memory_handle, 0),
            HSA_STATUS_SUCCESS);
  ASSERT_GT(dma_buf_fd, 0);

  hsa_amd_vmem_alloc_handle_t imported_handle = {};
  ASSERT_EQ(hsa_amd_vmem_import_shareable_handle(dma_buf_fd, &imported_handle),
            HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_portable_close_dmabuf(dma_buf_fd), HSA_STATUS_SUCCESS);

  // An already-imported handle can never be re-exported (see
  // Runtime::VMemoryExportShareableHandle), regardless of which driver owns it.
  int reexported_fd = -1;
  EXPECT_NE(hsa_amd_vmem_export_shareable_handle(&reexported_fd, imported_handle, 0),
            HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_amd_vmem_handle_release(imported_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemSetAccessPermissionChange) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  std::uint32_t* buffer = nullptr;
  ASSERT_EQ(hsa_amd_vmem_address_reserve_align(reinterpret_cast<void**>(&buffer), allocation_size,
                                               0, 0, 0),
            HSA_STATUS_SUCCESS);
  ASSERT_EQ(hsa_amd_vmem_map(buffer, allocation_size, 0, memory_handle, 0), HSA_STATUS_SUCCESS);

  hsa_amd_memory_access_desc_t rw_desc{HSA_ACCESS_PERMISSION_RW, dynamic_agents.front()};
  ASSERT_EQ(hsa_amd_vmem_set_access(buffer, allocation_size, &rw_desc, 1), HSA_STATUS_SUCCESS);

  hsa_access_permission_t perms = HSA_ACCESS_PERMISSION_NONE;
  ASSERT_EQ(hsa_amd_vmem_get_access(buffer, &perms, dynamic_agents.front()), HSA_STATUS_SUCCESS);
  EXPECT_EQ(perms, HSA_ACCESS_PERMISSION_RW);

  // Changing permissions for an already-allowed agent exercises the RemoveAccess +
  // EnableAccess branch in Runtime::VMemorySetAccessPerHandle (distinct from the
  // first-time-grant branch exercised by every other set_access test).
  hsa_amd_memory_access_desc_t ro_desc{HSA_ACCESS_PERMISSION_RO, dynamic_agents.front()};
  EXPECT_EQ(hsa_amd_vmem_set_access(buffer, allocation_size, &ro_desc, 1), HSA_STATUS_SUCCESS);

  perms = HSA_ACCESS_PERMISSION_NONE;
  ASSERT_EQ(hsa_amd_vmem_get_access(buffer, &perms, dynamic_agents.front()), HSA_STATUS_SUCCESS);
  EXPECT_EQ(perms, HSA_ACCESS_PERMISSION_RO);

  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_address_free(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemGetAccessBeforeSetAccess) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  std::uint32_t* buffer = nullptr;
  ASSERT_EQ(hsa_amd_vmem_address_reserve_align(reinterpret_cast<void**>(&buffer), allocation_size,
                                               0, 0, 0),
            HSA_STATUS_SUCCESS);
  ASSERT_EQ(hsa_amd_vmem_map(buffer, allocation_size, 0, memory_handle, 0), HSA_STATUS_SUCCESS);

  // No hsa_amd_vmem_set_access has been called yet for this agent: get_access should
  // report NONE rather than erroring (per Runtime::VMemoryGetAccess).
  hsa_access_permission_t perms = HSA_ACCESS_PERMISSION_RW;
  EXPECT_EQ(hsa_amd_vmem_get_access(buffer, &perms, dynamic_agents.front()), HSA_STATUS_SUCCESS);
  EXPECT_EQ(perms, HSA_ACCESS_PERMISSION_NONE);

  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_address_free(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemUnmapRemapCycle) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  std::uint32_t* buffer = nullptr;
  ASSERT_EQ(hsa_amd_vmem_address_reserve_align(reinterpret_cast<void**>(&buffer), allocation_size,
                                               0, 0, 0),
            HSA_STATUS_SUCCESS);

  hsa_amd_memory_access_desc_t desc{HSA_ACCESS_PERMISSION_RW, dynamic_agents.front()};

  // First map/access/write cycle.
  ASSERT_EQ(hsa_amd_vmem_map(buffer, allocation_size, 0, memory_handle, 0), HSA_STATUS_SUCCESS);
  ASSERT_EQ(hsa_amd_vmem_set_access(buffer, allocation_size, &desc, 1), HSA_STATUS_SUCCESS);
  for (std::size_t i = 0; i < buffer_size; ++i) buffer[i] = static_cast<std::uint32_t>(i);
  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, allocation_size), HSA_STATUS_SUCCESS);

  // Re-map the same reserved VA range against the same handle and confirm dynaccel's
  // unmap (PROT_NONE remap) + re-map bookkeeping (a->ptr) supports a second cycle.
  ASSERT_EQ(hsa_amd_vmem_map(buffer, allocation_size, 0, memory_handle, 0), HSA_STATUS_SUCCESS);
  ASSERT_EQ(hsa_amd_vmem_set_access(buffer, allocation_size, &desc, 1), HSA_STATUS_SUCCESS);
  for (std::size_t i = 0; i < buffer_size; ++i) {
    buffer[i] = static_cast<std::uint32_t>(buffer_size - i);
  }
  for (std::size_t i = 0; i < buffer_size; ++i) {
    EXPECT_EQ(buffer[i], static_cast<std::uint32_t>(buffer_size - i));
  }

  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_address_free(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemRetainAllocHandle) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  std::uint32_t* buffer = nullptr;
  ASSERT_EQ(hsa_amd_vmem_address_reserve_align(reinterpret_cast<void**>(&buffer), allocation_size,
                                               0, 0, 0),
            HSA_STATUS_SUCCESS);
  ASSERT_EQ(hsa_amd_vmem_map(buffer, allocation_size, 0, memory_handle, 0), HSA_STATUS_SUCCESS);

  hsa_amd_vmem_alloc_handle_t retained_handle = {};
  ASSERT_EQ(hsa_amd_vmem_retain_alloc_handle(&retained_handle, buffer), HSA_STATUS_SUCCESS);
  EXPECT_EQ(retained_handle.handle, memory_handle.handle);

  // The retain call incremented ref_count; both references must be released.
  EXPECT_EQ(hsa_amd_vmem_handle_release(retained_handle), HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_address_free(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemGetAllocPropertiesFromHandle) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  hsa_amd_memory_pool_t queried_pool = {};
  hsa_amd_memory_type_t queried_type = {};
  EXPECT_EQ(hsa_amd_vmem_get_alloc_properties_from_handle(memory_handle, &queried_pool,
                                                          &queried_type),
            HSA_STATUS_SUCCESS);
  EXPECT_EQ(queried_pool.handle, global_memory_pool.handle);
  EXPECT_EQ(queried_type, MEMORY_TYPE_PINNED);

  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemDoubleReleaseRejected) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  ASSERT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  // The handle is fully destroyed after the first release (ref_count reached 0 with no
  // outstanding mappings); releasing it again must be rejected, not use-after-free.
  EXPECT_NE(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemReleaseWhileMappedThenUnmap) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  std::uint32_t* buffer = nullptr;
  ASSERT_EQ(hsa_amd_vmem_address_reserve_align(reinterpret_cast<void**>(&buffer), allocation_size,
                                               0, 0, 0),
            HSA_STATUS_SUCCESS);
  ASSERT_EQ(hsa_amd_vmem_map(buffer, allocation_size, 0, memory_handle, 0), HSA_STATUS_SUCCESS);

  hsa_amd_memory_access_desc_t desc{HSA_ACCESS_PERMISSION_RW, dynamic_agents.front()};
  ASSERT_EQ(hsa_amd_vmem_set_access(buffer, allocation_size, &desc, 1), HSA_STATUS_SUCCESS);

  // Per the API docs, a handle may be released while mappings are still outstanding:
  // ref_count drops to 0 but use_count keeps the underlying allocation alive until the
  // last unmap (see Runtime::VMemoryHandleRelease / VMemoryHandleUnmap).
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);

  for (std::size_t i = 0; i < buffer_size; ++i) buffer[i] = static_cast<std::uint32_t>(i);
  for (std::size_t i = 0; i < buffer_size; ++i) {
    EXPECT_EQ(buffer[i], static_cast<std::uint32_t>(i));
  }

  // This unmap is what actually tears down the dynaccel allocation now that ref_count
  // and use_count both reach 0.
  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_address_free(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemCreateZeroSizeRejected) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  EXPECT_NE(hsa_amd_vmem_handle_create(global_memory_pool, 0, MEMORY_TYPE_PINNED, 0,
                                       &memory_handle),
            HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemCreateMisalignedSizeRejected) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  // Not a multiple of the memory region's page size: Runtime::VMemoryHandleCreate
  // rejects this before dynaccel ever sees the request.
  constexpr std::size_t allocation_size = 1;
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  EXPECT_NE(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, VMemSetAccessMixedPermissions) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> gpu_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_GPU>, &gpu_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(gpu_agents.empty());

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t global_memory_pool = {};
  ASSERT_EQ(
      hsa_amd_agent_iterate_memory_pools(
          dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &global_memory_pool),
      HSA_STATUS_INFO_BREAK);

  constexpr std::size_t buffer_size = 1024;
  constexpr std::size_t allocation_size = buffer_size * sizeof(std::uint32_t);
  hsa_amd_vmem_alloc_handle_t memory_handle = {};
  ASSERT_EQ(hsa_amd_vmem_handle_create(global_memory_pool, allocation_size, MEMORY_TYPE_PINNED,
                                       0, &memory_handle),
            HSA_STATUS_SUCCESS);

  std::uint32_t* buffer = nullptr;
  ASSERT_EQ(hsa_amd_vmem_address_reserve_align(reinterpret_cast<void**>(&buffer), allocation_size,
                                               0, 0, 0),
            HSA_STATUS_SUCCESS);
  ASSERT_EQ(hsa_amd_vmem_map(buffer, allocation_size, 0, memory_handle, 0), HSA_STATUS_SUCCESS);

  // Grant different permissions to different agents on the same handle in a single
  // set_access call; every existing multi-agent test grants the same permission to all.
  std::vector<hsa_amd_memory_access_desc_t> memory_access_desc = {
      {HSA_ACCESS_PERMISSION_RW, dynamic_agents.front()},
      {HSA_ACCESS_PERMISSION_RO, gpu_agents.front()},
  };
  ASSERT_EQ(hsa_amd_vmem_set_access(buffer, allocation_size, memory_access_desc.data(),
                                    memory_access_desc.size()),
            HSA_STATUS_SUCCESS);

  hsa_access_permission_t perms = HSA_ACCESS_PERMISSION_NONE;
  ASSERT_EQ(hsa_amd_vmem_get_access(buffer, &perms, dynamic_agents.front()), HSA_STATUS_SUCCESS);
  EXPECT_EQ(perms, HSA_ACCESS_PERMISSION_RW);

  perms = HSA_ACCESS_PERMISSION_NONE;
  ASSERT_EQ(hsa_amd_vmem_get_access(buffer, &perms, gpu_agents.front()), HSA_STATUS_SUCCESS);
  EXPECT_EQ(perms, HSA_ACCESS_PERMISSION_RO);

  EXPECT_EQ(hsa_amd_vmem_unmap(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_address_free(buffer, allocation_size), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_vmem_handle_release(memory_handle), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}

TEST(Memory, UdmabufExport) {
  ASSERT_EQ(hsa_init(), HSA_STATUS_SUCCESS);

  std::vector<hsa_agent_t> dynamic_agents;
  ASSERT_EQ(hsa_iterate_agents(discover_agents<HSA_DEVICE_TYPE_DYNAMIC>, &dynamic_agents),
            HSA_STATUS_SUCCESS);
  ASSERT_FALSE(dynamic_agents.empty());

  hsa_amd_memory_pool_t pool = {};
  ASSERT_EQ(hsa_amd_agent_iterate_memory_pools(
                dynamic_agents.front(), discover_first_global_coarse_grain_mem_pool, &pool),
            HSA_STATUS_INFO_BREAK);

  constexpr std::size_t allocation_size = 4096;
  void* buffer = nullptr;
  ASSERT_EQ(hsa_amd_memory_pool_allocate(pool, allocation_size, 0, &buffer), HSA_STATUS_SUCCESS);
  ASSERT_NE(buffer, nullptr);

  // The memory is writable regardless of whether udmabuf is available.
  std::memset(buffer, 0xAB, allocation_size);
  EXPECT_EQ(static_cast<unsigned char*>(buffer)[allocation_size - 1], 0xAB);

  int dma_buf_fd = -1;
  std::uint64_t dma_buf_offset = 0;
  const hsa_status_t st =
      hsa_amd_portable_export_dmabuf(buffer, allocation_size, &dma_buf_fd, &dma_buf_offset);
  if (st == HSA_STATUS_ERROR_OUT_OF_RESOURCES) {
    hsa_amd_memory_pool_free(buffer);
    hsa_shut_down();
    GTEST_SKIP() << "/dev/udmabuf unavailable; driver is in mmap fallback mode";
  }
  ASSERT_EQ(st, HSA_STATUS_SUCCESS);
  EXPECT_GT(dma_buf_fd, 0);
  EXPECT_EQ(dma_buf_offset, 0u);

  EXPECT_EQ(hsa_amd_portable_close_dmabuf(dma_buf_fd), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_amd_memory_pool_free(buffer), HSA_STATUS_SUCCESS);
  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}
