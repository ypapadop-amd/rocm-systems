/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include "core/inc/amd_dynamic_agent.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <string_view>

#include "core/inc/amd_dynamic_aql_queue.h"
#include "core/inc/amd_dynamic_driver.h"
#include "core/inc/amd_memory_region.h"
#include "core/inc/cache.h"
#include "core/inc/driver.h"
#include "core/inc/runtime.h"
#include "core/util/os.h"

namespace rocr {
namespace AMD {

DynamicAgent::DynamicAgent(uint32_t node, const HsaNodeProperties& node_props)
    : core::Agent(
          core::Runtime::runtime_singleton_->AgentDriver(core::DriverType::DYNAMIC),
          node, core::Agent::DeviceType::kDynamicDevice),
      node_props_(node_props) {
  // Defaults if the driver does not implement get_agent_properties.
  props_.queue_min_size = 0x40;
  props_.queue_max_size = 0x40;
  props_.queues_max = 1;
  props_.profile = HSA_PROFILE_BASE;
  props_.default_float_rounding_mode = HSA_DEFAULT_FLOAT_ROUNDING_MODE_NEAR;
  static_cast<DynamicDriver&>(driver()).GetAgentProperties(node, &props_);

  InitRegionList();
  InitAllocators();
}

DynamicAgent::~DynamicAgent() {
  regions_.clear();
}

hsa_status_t DynamicAgent::IterateRegion(
    hsa_status_t (*callback)(hsa_region_t region, void* data), void* data) const {
  AMD::callback_t<decltype(callback)> call(callback);
  for (const auto& r : regions_) {
    hsa_region_t region_handle(core::MemoryRegion::Convert(r.get()));
    hsa_status_t err = call(region_handle, data);
    if (err != HSA_STATUS_SUCCESS) {
      return err;
    }
  }
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicAgent::VisitRegion(bool include_peer,
                                       hsa_status_t (*callback)(hsa_region_t region, void* data),
                                       void* data) const {
  // A dynamic agent owns no peer regions, so peer inclusion changes nothing.
  return IterateRegion(callback, data);
}

core::Agent* DynamicAgent::GetNearestCpuAgent() const {
  // Mirrors AieAgent: dynamic agents are associated with the first CPU agent.
  assert(!core::Runtime::runtime_singleton_->cpu_agents().empty());
  return core::Runtime::runtime_singleton_->cpu_agents()[0];
}

hsa_status_t DynamicAgent::IterateCache(
    hsa_status_t (*callback)(hsa_cache_t cache, void* data), void* data) const {
  std::vector<HsaCacheProperties> cache_props;
  if (driver().GetCacheProperties(node_id(), 0, cache_props) != HSA_STATUS_SUCCESS ||
      cache_props.empty()) {
    return HSA_STATUS_ERROR_INVALID_CACHE;
  }

  // HsaCacheProperties has no name field; mirror GpuAgent::InitCacheList, which
  // synthesizes the cache name from the device name and level.
  const std::string device_name(reinterpret_cast<const char*>(node_props_.AMDName));

  AMD::callback_t<decltype(callback)> call(callback);
  for (const auto& props : cache_props) {
    core::Cache cache(device_name + " L" + std::to_string(props.CacheLevel), props.CacheLevel,
                      props.CacheSize);
    hsa_status_t err = call(core::Cache::Convert(&cache), data);
    if (err != HSA_STATUS_SUCCESS) return err;
  }
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicAgent::IterateSupportedIsas(
    hsa_status_t (*callback)(hsa_isa_t isa, void* data), void* data) const {
  AMD::callback_t<decltype(callback)> call(callback);
  for (const auto& isa : supported_isas()) {
    hsa_status_t err = call(core::Isa::Handle(isa), data);
    if (err != HSA_STATUS_SUCCESS) return err;
  }
  return HSA_STATUS_SUCCESS;
}

void DynamicAgent::InitDerivedCuid() {}

hsa_status_t DynamicAgent::GetInfo(hsa_agent_info_t attribute, void* value) const {
  const size_t attribute_ = static_cast<size_t>(attribute);

  switch (attribute_) {
    case HSA_AGENT_INFO_NAME:
      std::copy_n(node_props_.AMDName, HSA_PUBLIC_NAME_SIZE, static_cast<char*>(value));
      break;
    case HSA_AGENT_INFO_VENDOR_NAME: {
      constexpr std::string_view vendor_name_info("AMD");
      assert(vendor_name_info.size() < HSA_PUBLIC_NAME_SIZE);
      auto ptr = static_cast<char*>(value);
      std::copy(vendor_name_info.begin(), vendor_name_info.end(), ptr);
      std::fill(std::next(ptr, vendor_name_info.size()), std::next(ptr, HSA_PUBLIC_NAME_SIZE), 0);
      break;
    }
    case HSA_AGENT_INFO_FEATURE:
      *static_cast<hsa_agent_feature_t*>(value) = HSA_AGENT_FEATURE_AGENT_DISPATCH;
      break;
    case HSA_AGENT_INFO_MACHINE_MODEL:
      *static_cast<hsa_machine_model_t*>(value) = HSA_MACHINE_MODEL_LARGE;
      break;
    case HSA_AGENT_INFO_BASE_PROFILE_DEFAULT_FLOAT_ROUNDING_MODES:
    case HSA_AGENT_INFO_DEFAULT_FLOAT_ROUNDING_MODE:
      *static_cast<hsa_default_float_rounding_mode_t*>(value) =
          HSA_DEFAULT_FLOAT_ROUNDING_MODE_NEAR;
      break;
    case HSA_AGENT_INFO_PROFILE:
      *static_cast<hsa_profile_t*>(value) = static_cast<hsa_profile_t>(props_.profile);
      break;
    case HSA_AGENT_INFO_WAVEFRONT_SIZE:
      *static_cast<uint32_t*>(value) = props_.wavefront_size;
      break;
    case HSA_AGENT_INFO_WORKGROUP_MAX_DIM:
      std::memcpy(value, props_.workgroup_max_dim, sizeof(props_.workgroup_max_dim));
      break;
    case HSA_AGENT_INFO_WORKGROUP_MAX_SIZE:
      *static_cast<uint32_t*>(value) = props_.workgroup_max_size;
      break;
    case HSA_AGENT_INFO_GRID_MAX_DIM:
      std::memcpy(value, props_.grid_max_dim, sizeof(props_.grid_max_dim));
      break;
    case HSA_AGENT_INFO_GRID_MAX_SIZE:
      *static_cast<uint32_t*>(value) = props_.grid_max_size;
      break;
    case HSA_AGENT_INFO_FBARRIER_MAX_SIZE:
      *static_cast<uint32_t*>(value) = 0;
      break;
    case HSA_AGENT_INFO_QUEUES_MAX:
      *static_cast<uint32_t*>(value) = props_.queues_max;
      break;
    case HSA_AGENT_INFO_QUEUE_MIN_SIZE:
      *static_cast<uint32_t*>(value) = props_.queue_min_size;
      break;
    case HSA_AGENT_INFO_QUEUE_MAX_SIZE:
      *static_cast<uint32_t*>(value) = props_.queue_max_size;
      break;
    case HSA_AGENT_INFO_QUEUE_TYPE:
      *static_cast<hsa_queue_type32_t*>(value) = HSA_QUEUE_TYPE_SINGLE;
      break;
    case HSA_AGENT_INFO_NODE:
      *static_cast<uint32_t*>(value) = node_id();
      break;
    case HSA_AGENT_INFO_DEVICE:
      *static_cast<hsa_device_type_t*>(value) = HSA_DEVICE_TYPE_DYNAMIC;
      break;
    case HSA_AGENT_INFO_CACHE_SIZE:
      std::memcpy(value, props_.cache_size, sizeof(props_.cache_size));
      break;
    case HSA_AGENT_INFO_VERSION_MAJOR:
      *static_cast<uint32_t*>(value) = 1;
      break;
    case HSA_AGENT_INFO_VERSION_MINOR:
      *static_cast<uint32_t*>(value) = 0;
      break;
    case HSA_AMD_AGENT_INFO_CHIP_ID:
      *static_cast<uint32_t*>(value) = props_.chip_id;
      break;
    case HSA_AMD_AGENT_INFO_CACHELINE_SIZE:
      *static_cast<uint32_t*>(value) = props_.cacheline_size;
      break;
    case HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT:
      *static_cast<uint32_t*>(value) = props_.compute_unit_count;
      break;
    case HSA_AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY:
      *static_cast<uint32_t*>(value) = props_.max_clock_frequency;
      break;
    case HSA_AMD_AGENT_INFO_DRIVER_NODE_ID:
      *static_cast<uint32_t*>(value) = node_id();
      break;
    case HSA_AMD_AGENT_INFO_MAX_ADDRESS_WATCH_POINTS:
      *static_cast<uint32_t*>(value) = 0;
      break;
    case HSA_AMD_AGENT_INFO_BDFID:
      *static_cast<uint32_t*>(value) = props_.bdfid;
      break;
    case HSA_AMD_AGENT_INFO_NUM_SIMDS_PER_CU:
      *static_cast<uint32_t*>(value) = 0;
      break;
    case HSA_AMD_AGENT_INFO_NUM_SHADER_ENGINES:
      *static_cast<uint32_t*>(value) = 0;
      break;
    case HSA_AMD_AGENT_INFO_NUM_SHADER_ARRAYS_PER_SE:
      *static_cast<uint32_t*>(value) = 0;
      break;
    case HSA_EXT_AGENT_INFO_IMAGE_1D_MAX_ELEMENTS:
    case HSA_EXT_AGENT_INFO_IMAGE_1DA_MAX_ELEMENTS:
    case HSA_EXT_AGENT_INFO_IMAGE_1DB_MAX_ELEMENTS:
    case HSA_EXT_AGENT_INFO_IMAGE_2D_MAX_ELEMENTS:
    case HSA_EXT_AGENT_INFO_IMAGE_2DA_MAX_ELEMENTS:
    case HSA_EXT_AGENT_INFO_IMAGE_2DDEPTH_MAX_ELEMENTS:
    case HSA_EXT_AGENT_INFO_IMAGE_2DADEPTH_MAX_ELEMENTS:
    case HSA_EXT_AGENT_INFO_IMAGE_3D_MAX_ELEMENTS:
    case HSA_EXT_AGENT_INFO_IMAGE_ARRAY_MAX_LAYERS:
      *static_cast<uint32_t*>(value) = 0;
      break;
    case HSA_AMD_AGENT_INFO_PRODUCT_NAME:
      std::copy_n(node_props_.MarketingName, HSA_PUBLIC_NAME_SIZE, static_cast<char*>(value));
      break;
    case HSA_AMD_AGENT_INFO_UUID: {
      // HSA_AMD_AGENT_INFO_UUID is documented (hsa_ext_amd.h) as an Ascii
      // string with a maximum of 21 chars including NUL. Bound the copy by
      // that contract, not by the size of props_.uuid, so callers that
      // allocated exactly 21 bytes are never overrun.
      constexpr size_t kUuidMaxLen = 21;
      auto ptr = static_cast<char*>(value);
      const char* src = props_.uuid[0] ? props_.uuid : "DYN-XX";
      std::strncpy(ptr, src, kUuidMaxLen - 1);
      ptr[kUuidMaxLen - 1] = '\0';
      break;
    }
    case HSA_AMD_AGENT_INFO_ASIC_REVISION:
      *static_cast<uint32_t*>(value) = props_.asic_revision;
      break;
    case HSA_AMD_AGENT_INFO_SVM_DIRECT_HOST_ACCESS:
      assert(regions_.size() != 0 && "No device local memory found!");
      *static_cast<bool*>(value) = true;
      break;
    case HSA_AMD_AGENT_INFO_MEMORY_PROPERTIES:
      std::memset(value, 0, sizeof(uint8_t) * 8);
      break;
    case HSA_AMD_AGENT_INFO_CLOCK_COUNTERS:
      std::memset(value, 0, sizeof(hsa_amd_clock_counters_t));
      break;
    default:
      *static_cast<uint32_t*>(value) = 0;
      return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicAgent::QueueCreate(size_t size, hsa_queue_type32_t queue_type,
                                       uint64_t flags,
                                       core::HsaEventCallback event_callback, void* data,
                                       uint32_t private_segment_size,
                                       uint32_t group_segment_size,
                                       bool metadata_prefetch,
                                       core::Queue** queue) {
  if ((flags & HSA_AMD_QUEUE_CREATE_DEVICE_MEM_RING_BUF) != 0 ||
      (flags & HSA_AMD_QUEUE_CREATE_DEVICE_MEM_QUEUE_DESCRIPTOR) != 0) {
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  if (!IsPowerOfTwo(size)) {
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  if (size < props_.queue_min_size || size > props_.queue_max_size) {
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  core::SharedQueue* shared_queue =
      static_cast<core::SharedQueue*>(core::Runtime::runtime_singleton_->system_allocator()(
          sizeof(core::SharedQueue), MemoryRegion::GetPageSize(), 0, node_id()));

  if (!shared_queue) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;

  try {
    auto aql_queue(new DynamicAqlQueue(shared_queue, this, size, node_id(), flags));
    *queue = aql_queue;
    return HSA_STATUS_SUCCESS;
  } catch (const hsa_exception& e) {
    core::Runtime::runtime_singleton_->system_deallocator()(shared_queue);
    return e.error_code();
  }
}

void DynamicAgent::InitRegionList() {
  std::vector<HsaMemoryProperties> mem_props_vec;
  driver().GetMemoryProperties(node_id(), mem_props_vec);

  if (mem_props_vec.empty()) {
    const uint64_t total_system_memory = os::HostTotalPhysicalMemory();

    HsaMemoryProperties sys_mem_props = {};
    sys_mem_props.HeapType = HSA_HEAPTYPE_SYSTEM;
    sys_mem_props.SizeInBytes = total_system_memory;

    regions_.push_back(
        std::make_shared<MemoryRegion>(true, true, false, false, true, this, sys_mem_props));
  } else {
    regions_.reserve(mem_props_vec.size());
    bool first_system = true;
    for (const auto& props : mem_props_vec) {
      bool kernarg = first_system && (props.HeapType == HSA_HEAPTYPE_SYSTEM);
      if (kernarg) first_system = false;
      const bool fine_grain = (props.HeapType == HSA_HEAPTYPE_SYSTEM);
      regions_.push_back(std::make_shared<MemoryRegion>(fine_grain, kernarg, false, false, true,
                                                        this, props));
    }
  }
}

void DynamicAgent::InitAllocators() {
  for (const auto& region : regions()) {
    const MemoryRegion* amd_mem_region(
        static_cast<const MemoryRegion*>(region.get()));
    if (amd_mem_region->kernarg()) {
      const core::MemoryRegion* region_ptr = region.get();
      system_allocator_ =
          [region_ptr](size_t size, size_t align,
                       core::MemoryRegion::AllocateFlags alloc_flags) -> void* {
        void* mem(nullptr);
        return (core::Runtime::runtime_singleton_->AllocateMemory(
                    region_ptr, size, alloc_flags, &mem) == HSA_STATUS_SUCCESS)
                   ? mem
                   : nullptr;
      };

      system_deallocator_ = [](void* ptr) {
        core::Runtime::runtime_singleton_->FreeMemory(ptr);
      };
      break;
    }
  }
}

} // namespace AMD
} // namespace rocr
