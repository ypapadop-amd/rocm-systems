# DynAccel Reference Accelerator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up "DynAccel", a software accelerator implemented against the ROCr dynamic driver/agent interface, so that `rocrtst/suites/dynamic` exercises a real backend instead of asserting that everything fails.

**Architecture:** Two ftable additions (`create_queue` gains a `read_index_ptr`; a new `get_agent_properties`), the doorbell rewired to the GPU model where the driver owns the doorbell word and ROCr writes it directly, and a single-translation-unit C driver (`libdynaccel.so`) with udmabuf-backed memory and one polling worker thread per queue.

**Tech Stack:** C++17 (ROCr side), C11 (driver), CMake, GoogleTest, Linux `memfd_create` + `udmabuf`, pthreads.

**Spec:** `docs/superpowers/specs/2026-08-07-dynamic-reference-accelerator-design.md`

## Global Constraints

- Repo root: `/home/ypapadop/workspace-arch/rocm-systems`. All paths below are relative to `projects/rocr-runtime/` unless stated otherwise.
- Branch: `dynamic-agent`. Do not rebase or force-push during execution.
- Every new file starts with the existing SPDX header used throughout the tree:
  ```
  /*
   * Copyright © Advanced Micro Devices, Inc., or its affiliates.
   *
   * SPDX-License-Identifier: MIT
   */
  ```
- `libdynaccel.so` links **only** libc and pthread. It may `#include` `hsa.h` and `amd_hsa_signal.h` but must never call an `hsa_*` function.
- Driver code is C11 (`-std=c11`), compiled with `-Wall -Wextra -Wpedantic`, matching the sibling test targets in `rocrtst/suites/dynamic/CMakeLists.txt`.
- ROCr-side code is C++17 and follows the surrounding file's style. Do not reformat untouched lines.
- The ftable's `allocate_memory` slot keeps its `void** mem` out-parameter. Only `create_queue` and the new `get_agent_properties` change. Do not widen `core::Driver::CreateQueue` — that would drag in `KfdDriver` and `XdnaDriver`.
- Commit after every task. Use `git add <explicit paths>`, never `git add -A`.

## Build and test commands

The dynamic suite is a **standalone** CMake project (`rocrtst/suites/dynamic/CMakeLists.txt` declares `project(dynamic_tests)` and calls `find_package(hsa-runtime64)`), so it builds against an *installed* ROCr, not in-tree.

```bash
# One-time, and again after any ROCr-side change (Tasks 1-3):
cmake -S projects/rocr-runtime -B /tmp/rocr-build \
      -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/tmp/rocr-install
cmake --build /tmp/rocr-build -j"$(nproc)"
cmake --install /tmp/rocr-build

# The test suite (Tasks 4-6):
cmake -S projects/rocr-runtime/rocrtst/suites/dynamic -B /tmp/dynaccel-build \
      -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/tmp/rocr-install
cmake --build /tmp/dynaccel-build -j"$(nproc)"
ctest --test-dir /tmp/dynaccel-build --output-on-failure
```

**These commands are unverified** — there is no build tree in the workspace and I have not run them. If ROCr's top-level CMake needs extra options (it wraps `add_subdirectory` in a helper at `CMakeLists.txt:70-84`), fix the invocation in Task 1 and correct this section as part of that task's commit.

`/dev/udmabuf` on this host is `root:kvm`. If the executing user is not in `kvm`, the driver falls back to plain `mmap` and the dma-buf tests skip themselves — that is expected, not a failure.

## File structure

| File | Responsibility | Task |
| --- | --- | --- |
| `runtime/hsa-runtime/core/driver/dynamic/rocr_dynamic_driver.h` | C ABI: mirror handle gains `vaddr`; `create_queue` gains `read_index_ptr`; new `get_agent_properties` + props struct | 1, 2, 3 |
| `runtime/hsa-runtime/core/inc/amd_dynamic_driver.h` / `core/driver/dynamic/amd_dynamic_driver.cpp` | `DriverMemoryHandle` migration; `GetAgentProperties`; `CreateQueueWithReadIndex` | 1, 2, 3 |
| `runtime/hsa-runtime/core/inc/amd_dynamic_agent.h` / `core/runtime/amd_dynamic_agent.cpp` | Missing overrides; cached agent props; `IterateCache`; region fine/coarse grain | 1, 2 |
| `runtime/hsa-runtime/core/inc/amd_dynamic_aql_queue.h` / `core/runtime/amd_dynamic_aql_queue.cpp` | Doorbell rewiring; delete `SubmitPackets` | 3 |
| `rocrtst/suites/dynamic/dynaccel/dynaccel_packet.h` | Public 64-byte packet ABI, included by tests | 4 |
| `rocrtst/suites/dynamic/dynaccel/dynaccel.c` | The whole driver, ftable order top-to-bottom | 4, 5, 6 |
| `rocrtst/suites/dynamic/dynaccel/CMakeLists.txt` | `libdynaccel.so` target | 4 |
| `rocrtst/suites/dynamic/CMakeLists.txt` | Doubled CTest registration with/without `LD_PRELOAD` | 4 |
| `rocrtst/suites/dynamic/dispatch.cc`, `memory.cc` | New and strengthened tests | 4, 5, 6 |

---

### Task 1: Make DynamicDriver and DynamicAgent compile

`DynamicAgent` omits two pure-virtual overrides and references an undeclared member; `DynamicDriver` still uses the pre-refactor memory signatures. Nothing builds until this lands, so this task's verification is the build itself — there is no test to write first.

**Files:**
- Modify: `runtime/hsa-runtime/core/driver/dynamic/rocr_dynamic_driver.h` (mirror handle struct, ~line 46)
- Modify: `runtime/hsa-runtime/core/inc/amd_dynamic_driver.h:38-41`
- Modify: `runtime/hsa-runtime/core/driver/dynamic/amd_dynamic_driver.cpp:20-38, 154-168`
- Modify: `runtime/hsa-runtime/core/inc/amd_dynamic_agent.h`
- Modify: `runtime/hsa-runtime/core/runtime/amd_dynamic_agent.cpp`

**Interfaces:**
- Consumes: `core::DriverMemoryHandle` (`core/inc/driver.h:76-92`), `core::Driver::AllocateMemory/FreeMemory` (`core/inc/driver.h:172,178`).
- Produces: `DynamicDriver::AllocateMemory(const core::MemoryRegion&, core::MemoryRegion::AllocateFlags, size_t, uint32_t, core::DriverMemoryHandle*)`, `DynamicDriver::FreeMemory(const core::DriverMemoryHandle&)`, `DynamicAgent::VisitRegion`, `DynamicAgent::GetNearestCpuAgent`, member `DynamicAgent::supported_isas_`.

- [ ] **Step 1: Add `vaddr` to the C ABI mirror handle**

`core::DriverMemoryHandle` has a `vaddr` field the mirror struct lacks, and the header claims the two are field-compatible. In `rocr_dynamic_driver.h`, inside `rocr_dynamic_driver_memory_handle_t`, insert after the `handle` member:

```c
  /** Virtual address mapped by the driver for this allocation, or NULL if the
   *  driver owns no mapping. When set, free_memory unmaps it. */
  void* vaddr;
```

- [ ] **Step 2: Carry `vaddr` through both conversion helpers**

In `amd_dynamic_driver.cpp`, add one line to each helper (they start at lines 20 and 31):

```cpp
  out.vaddr = handle.vaddr;        // in ToFtableHandle
  handle->vaddr = in.vaddr;        // in FromFtableHandle
```

- [ ] **Step 3: Migrate the AllocateMemory/FreeMemory declarations**

In `amd_dynamic_driver.h`, replace lines 38-41 with:

```cpp
  hsa_status_t AllocateMemory(const core::MemoryRegion& mem_region,
                              core::MemoryRegion::AllocateFlags alloc_flags, size_t size,
                              uint32_t node_id, core::DriverMemoryHandle* handle) override;
  hsa_status_t FreeMemory(const core::DriverMemoryHandle& handle) override;
```

- [ ] **Step 4: Migrate the definitions**

The ftable slot keeps `void** mem`; the driver synthesizes the handle exactly as `KfdDriver::AllocateMemory` does. Replace `amd_dynamic_driver.cpp:154-168` with:

```cpp
hsa_status_t DynamicDriver::AllocateMemory(const core::MemoryRegion& mem_region,
                                           core::MemoryRegion::AllocateFlags alloc_flags,
                                           size_t size, uint32_t node_id,
                                           core::DriverMemoryHandle* handle) {
  if (!ftable_->allocate_memory) return HSA_STATUS_ERROR;
  const auto& amd_region = static_cast<const AMD::MemoryRegion&>(mem_region);

  void* mem = nullptr;
  const hsa_status_t status = ftable_->allocate_memory(
      ctx_, node_id, size, static_cast<uint32_t>(alloc_flags), amd_region.mem_flags(),
      amd_region.mem_props(), &mem);
  if (status != HSA_STATUS_SUCCESS) return status;

  // Mirrors KfdDriver: the allocation address is both the native id and the
  // mapping. Export-only fields stay default and are filled lazily on export.
  handle->handle = reinterpret_cast<uint64_t>(mem);
  handle->vaddr = mem;
  handle->size = size;
  return HSA_STATUS_SUCCESS;
}

hsa_status_t DynamicDriver::FreeMemory(const core::DriverMemoryHandle& handle) {
  if (!ftable_->free_memory) return HSA_STATUS_ERROR;
  return ftable_->free_memory(ctx_, reinterpret_cast<void*>(handle.handle), handle.size);
}
```

- [ ] **Step 5: Declare the missing agent overrides and the missing member**

In `amd_dynamic_agent.h`, add to the public section after `IterateRegion`:

```cpp
  hsa_status_t VisitRegion(bool include_peer,
                           hsa_status_t (*callback)(hsa_region_t region, void* data),
                           void* data) const override;

  core::Agent* GetNearestCpuAgent() const override;
```

and add to the private section, next to `regions_`:

```cpp
  std::vector<const core::Isa*> supported_isas_;
```

- [ ] **Step 6: Define them**

In `amd_dynamic_agent.cpp`, after `DynamicAgent::IterateRegion`:

```cpp
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
```

Add `#include <cassert>` to the include block if not already present.

- [ ] **Step 7: Build**

Run:
```bash
cmake -S projects/rocr-runtime -B /tmp/rocr-build \
      -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/tmp/rocr-install
cmake --build /tmp/rocr-build -j"$(nproc)" 2>&1 | tail -40
```
Expected: builds clean. If the top-level CMake invocation needs different options, fix it and update the "Build and test commands" section of this plan in the same commit.

- [ ] **Step 8: Commit**

```bash
git add projects/rocr-runtime/runtime/hsa-runtime/core/driver/dynamic/rocr_dynamic_driver.h \
        projects/rocr-runtime/runtime/hsa-runtime/core/driver/dynamic/amd_dynamic_driver.cpp \
        projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_dynamic_driver.h \
        projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_dynamic_agent.h \
        projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_dynamic_agent.cpp
git commit -m "fix(dynamic-agent): sync DynamicDriver/DynamicAgent to core interfaces"
```

---

### Task 2: Agent properties, cache iteration, and region grain

Replaces `DynamicAgent::GetInfo`'s hardcoded zeros with values the driver supplies, wires `IterateCache` to the `get_cache_properties` slot that already exists but is never called, and fixes the fine/coarse grain flags so `memory.cc` finds the right pool.

**Files:**
- Modify: `runtime/hsa-runtime/core/driver/dynamic/rocr_dynamic_driver.h`
- Modify: `runtime/hsa-runtime/core/inc/amd_dynamic_driver.h`, `core/driver/dynamic/amd_dynamic_driver.cpp`
- Modify: `runtime/hsa-runtime/core/inc/amd_dynamic_agent.h`, `core/runtime/amd_dynamic_agent.cpp`

**Interfaces:**
- Consumes: Task 1's compiling `DynamicDriver`.
- Produces: `rocr_dynamic_driver_agent_props_t`; ftable slot `get_agent_properties`; `DynamicDriver::GetAgentProperties(uint32_t node_id, rocr_dynamic_driver_agent_props_t* props) const`; `DynamicAgent` member `props_`.

- [ ] **Step 1: Define the properties struct in the C ABI header**

In `rocr_dynamic_driver.h`, after `rocr_dynamic_driver_memory_handle_t`. Plain scalars only — no hsakmt types, so an out-of-tree driver need not replicate KFD layouts:

```c
/**
 * @brief Agent-level properties reported to the runtime.
 *
 * Filled once during agent construction and used to answer
 * @c hsa_agent_get_info. Fields left zero keep the runtime's defaults.
 */
typedef struct rocr_dynamic_driver_agent_props_t {
  char     name[64];
  char     vendor_name[64];
  char     product_name[64];
  char     uuid[24];
  uint32_t wavefront_size;
  uint16_t workgroup_max_dim[3];
  uint32_t workgroup_max_size;
  uint32_t grid_max_dim[3];
  uint32_t grid_max_size;
  uint32_t compute_unit_count;
  uint32_t max_clock_frequency;
  uint32_t cacheline_size;
  uint32_t cache_size[4];
  uint32_t queue_min_size;
  uint32_t queue_max_size;
  uint32_t queues_max;
  uint32_t bdfid;
  uint32_t chip_id;
  uint32_t asic_revision;
  uint8_t  profile;                     /**< hsa_profile_t */
  uint8_t  default_float_rounding_mode; /**< hsa_default_float_rounding_mode_t */
  uint8_t  reserved[2];
} rocr_dynamic_driver_agent_props_t;
```

- [ ] **Step 2: Add the ftable slot**

In `rocr_dynamic_driver_ftable_t`, at the end of the `Topology` section (after `get_cache_properties`):

```c
  /**
   * @brief Retrieve agent-level properties for a node.
   *
   * Optional. When NULL the runtime reports zeros for every attribute.
   *
   * @param[in]  ctx      Driver context.
   * @param[in]  node_id  Topology node index.
   * @param[out] props    Filled with the agent's properties.
   * @return HSA_STATUS_SUCCESS on success.
   */
  hsa_status_t (*get_agent_properties)(rocr_dynamic_driver_context_t* ctx, uint32_t node_id,
                                       rocr_dynamic_driver_agent_props_t* props);
```

- [ ] **Step 3: Expose it on DynamicDriver**

Declaration in `amd_dynamic_driver.h`, after `GetCacheProperties`:

```cpp
  hsa_status_t GetAgentProperties(uint32_t node_id,
                                  rocr_dynamic_driver_agent_props_t* props) const;
```

This is a `DynamicDriver`-only method, not a `core::Driver` override — no other driver has agent properties in the ftable sense.

Definition in `amd_dynamic_driver.cpp`, after `GetCacheProperties`:

```cpp
hsa_status_t DynamicDriver::GetAgentProperties(uint32_t node_id,
                                               rocr_dynamic_driver_agent_props_t* props) const {
  if (!ftable_->get_agent_properties) return HSA_STATUS_ERROR;
  return ftable_->get_agent_properties(ctx_, node_id, props);
}
```

- [ ] **Step 4: Cache the properties on the agent**

In `amd_dynamic_agent.h`, replace the three trailing constants

```cpp
  const uint32_t min_aql_size_ = 0x40;
  const uint32_t max_aql_size_ = 0x40;
  const uint32_t max_queues_ = 1;
```

with

```cpp
  rocr_dynamic_driver_agent_props_t props_ = {};
```

and add `#include "core/driver/dynamic/rocr_dynamic_driver.h"` to the include block.

In the `DynamicAgent` constructor in `amd_dynamic_agent.cpp`, before `InitRegionList()`:

```cpp
  // Defaults if the driver does not implement get_agent_properties.
  props_.queue_min_size = 0x40;
  props_.queue_max_size = 0x40;
  props_.queues_max = 1;
  props_.profile = HSA_PROFILE_BASE;
  props_.default_float_rounding_mode = HSA_DEFAULT_FLOAT_ROUNDING_MODE_NEAR;
  static_cast<DynamicDriver&>(driver()).GetAgentProperties(node, &props_);
```

Add `#include "core/inc/amd_dynamic_driver.h"`.

- [ ] **Step 5: Serve GetInfo from the cached struct**

In `DynamicAgent::GetInfo`, replace the bodies of these cases so they read `props_` instead of literals. Leave every other case exactly as it is:

```cpp
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
    case HSA_AGENT_INFO_QUEUES_MAX:
      *static_cast<uint32_t*>(value) = props_.queues_max;
      break;
    case HSA_AGENT_INFO_QUEUE_MIN_SIZE:
      *static_cast<uint32_t*>(value) = props_.queue_min_size;
      break;
    case HSA_AGENT_INFO_QUEUE_MAX_SIZE:
      *static_cast<uint32_t*>(value) = props_.queue_max_size;
      break;
    case HSA_AGENT_INFO_CACHE_SIZE:
      std::memcpy(value, props_.cache_size, sizeof(props_.cache_size));
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
    case HSA_AMD_AGENT_INFO_BDFID:
      *static_cast<uint32_t*>(value) = props_.bdfid;
      break;
    case HSA_AMD_AGENT_INFO_ASIC_REVISION:
      *static_cast<uint32_t*>(value) = props_.asic_revision;
      break;
```

`HSA_AGENT_INFO_CACHE_SIZE` is `uint32_t[4]` in the HSA spec; the previous single-`uint32_t` write was wrong and the `memcpy` above fixes it.

Also replace the `HSA_AMD_AGENT_INFO_UUID` case body to use the driver-supplied string:

```cpp
    case HSA_AMD_AGENT_INFO_UUID: {
      auto ptr = static_cast<char*>(value);
      std::strncpy(ptr, props_.uuid[0] ? props_.uuid : "DYN-XX", sizeof(props_.uuid));
      ptr[sizeof(props_.uuid) - 1] = '\0';
      break;
    }
```

Add `#include <cstring>` if absent (it is already included).

- [ ] **Step 6: Update QueueCreate's bounds check**

In `DynamicAgent::QueueCreate`, replace

```cpp
  if (size < min_aql_size_ || size > max_aql_size_) {
```

with

```cpp
  if (size < props_.queue_min_size || size > props_.queue_max_size) {
```

- [ ] **Step 7: Wire IterateCache to the existing ftable slot**

Replace the body of `DynamicAgent::IterateCache`:

```cpp
hsa_status_t DynamicAgent::IterateCache(
    hsa_status_t (*callback)(hsa_cache_t cache, void* data), void* data) const {
  std::vector<HsaCacheProperties> cache_props;
  if (driver().GetCacheProperties(node_id(), 0, cache_props) != HSA_STATUS_SUCCESS ||
      cache_props.empty()) {
    return HSA_STATUS_ERROR_INVALID_CACHE;
  }

  AMD::callback_t<decltype(callback)> call(callback);
  for (const auto& props : cache_props) {
    core::Cache cache(std::string(reinterpret_cast<const char*>(props.CacheName)),
                      props.CacheLevel, props.CacheSize);
    hsa_status_t err = call(core::Cache::Convert(&cache), data);
    if (err != HSA_STATUS_SUCCESS) return err;
  }
  return HSA_STATUS_SUCCESS;
}
```

Add `#include "core/inc/cache.h"` and `#include <string>`. If `core::Cache`'s constructor signature differs from `(std::string, uint32_t level, uint32_t size)`, match the usage in `core/runtime/amd_gpu_agent.cpp`'s `IterateCache` and adjust — do not invent a signature.

- [ ] **Step 8: Fix region grain flags**

`MemoryRegion`'s first constructor parameter is `fine_grain` (`core/inc/amd_memory_region.h:80`), and `InitRegionList` currently passes `false` for every region — so today even the kernarg region is coarse-grained, and `discover_first_global_coarse_grain_mem_pool` in `memory.cc` could match the wrong pool. In `DynamicAgent::InitRegionList`, in the `else` branch, replace the push_back with:

```cpp
      const bool fine_grain = (props.HeapType == HSA_HEAPTYPE_SYSTEM);
      regions_.push_back(std::make_shared<MemoryRegion>(fine_grain, kernarg, false, false, true,
                                                        this, props));
```

Leave the empty-`mem_props_vec` fallback branch alone except to make its single system region fine-grained:

```cpp
    regions_.push_back(
        std::make_shared<MemoryRegion>(true, true, false, false, true, this, sys_mem_props));
```

- [ ] **Step 9: Build and install**

Run:
```bash
cmake --build /tmp/rocr-build -j"$(nproc)" 2>&1 | tail -40 && cmake --install /tmp/rocr-build
```
Expected: builds clean.

- [ ] **Step 10: Commit**

```bash
git add projects/rocr-runtime/runtime/hsa-runtime/core/driver/dynamic/rocr_dynamic_driver.h \
        projects/rocr-runtime/runtime/hsa-runtime/core/driver/dynamic/amd_dynamic_driver.cpp \
        projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_dynamic_driver.h \
        projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_dynamic_agent.h \
        projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_dynamic_agent.cpp
git commit -m "feat(dynamic-agent): driver-supplied agent properties, cache iteration, region grain"
```

---

### Task 3: Doorbell rewiring

Today `DynamicAqlQueue` copies `AieAqlQueue`, whose `StoreRelaxed` calls `SubmitPackets` and whose `hardware_doorbell_ptr` stays NULL. Switch to the GPU model (`amd_aql_queue.cpp:291,482-491`): the driver owns the doorbell word and returns it via `HsaQueueResource`; ROCr writes it with a fenced store and never calls the driver on dispatch.

**Files:**
- Modify: `runtime/hsa-runtime/core/driver/dynamic/rocr_dynamic_driver.h` (`create_queue`)
- Modify: `runtime/hsa-runtime/core/inc/amd_dynamic_driver.h`, `core/driver/dynamic/amd_dynamic_driver.cpp`
- Modify: `runtime/hsa-runtime/core/inc/amd_dynamic_aql_queue.h`, `core/runtime/amd_dynamic_aql_queue.cpp`

**Interfaces:**
- Consumes: Task 1's build.
- Produces: ftable `create_queue` with a `uint64_t* read_index_ptr` parameter; `DynamicDriver::CreateQueueWithReadIndex(...)`.

- [ ] **Step 1: Add `read_index_ptr` to the ftable's create_queue**

In `rocr_dynamic_driver.h`, change the `create_queue` slot to insert the new parameter after `queue_size_bytes`, and document it plus the doorbell contract:

```c
  /**
   * ... existing doc ...
   * @param[in]  queue_addr      Ring buffer base address. Never NULL.
   * @param[in]  queue_size_bytes Ring buffer size in bytes.
   * @param[in]  read_index_ptr  Address of the runtime's read dispatch index.
   *                             The driver stores its completion progress here.
   * @param[out] queue_resource  Must be filled with QueueId and with
   *                             Queue_DoorBell_aql pointing at a driver-owned
   *                             uint64_t. The runtime writes the index of the
   *                             last submitted packet to that word; the driver
   *                             observes it. Initialize it to UINT64_MAX.
   */
  hsa_status_t (*create_queue)(rocr_dynamic_driver_context_t* ctx, uint32_t node_id, uint32_t type,
                               uint32_t queue_pct, uint32_t priority, uint32_t sdma_engine_id,
                               void* queue_addr, uint64_t queue_size_bytes,
                               uint64_t* read_index_ptr, uint64_t queue_metadata_size_bytes,
                               HsaEvent* event, HsaQueueResource* queue_resource);
```

- [ ] **Step 2: Add the DynamicDriver entry point**

`core::Driver::CreateQueue` must not change — that would touch `KfdDriver` and `XdnaDriver` for no reason. `DynamicAqlQueue` already knows its driver's concrete type, so add a `DynamicDriver`-only method.

Declaration in `amd_dynamic_driver.h`, next to `CreateQueue`:

```cpp
  /// @brief CreateQueue variant that also hands the driver the runtime's read
  /// dispatch index. Used by DynamicAqlQueue; core::Driver::CreateQueue
  /// forwards here with a null read index for interface compatibility.
  hsa_status_t CreateQueueWithReadIndex(uint32_t node_id, HSA_QUEUE_TYPE type, uint32_t queue_pct,
                                        HSA::hsa_amd_queue_priority_internal_t priority,
                                        uint32_t sdma_engine_id, void* queue_addr,
                                        uint64_t queue_size_bytes, uint64_t* read_index_ptr,
                                        uint64_t queue_metadata_size_bytes, HsaEvent* event,
                                        HsaQueueResource& queue_resource) const;
```

Definition in `amd_dynamic_driver.cpp`, replacing the existing `CreateQueue` body:

```cpp
hsa_status_t DynamicDriver::CreateQueueWithReadIndex(
    uint32_t node_id, HSA_QUEUE_TYPE type, uint32_t queue_pct,
    HSA::hsa_amd_queue_priority_internal_t priority, uint32_t sdma_engine_id, void* queue_addr,
    uint64_t queue_size_bytes, uint64_t* read_index_ptr, uint64_t queue_metadata_size_bytes,
    HsaEvent* event, HsaQueueResource& queue_resource) const {
  if (!ftable_->create_queue) return HSA_STATUS_ERROR;
  return ftable_->create_queue(ctx_, node_id, static_cast<uint32_t>(type), queue_pct,
                               static_cast<uint32_t>(priority), sdma_engine_id, queue_addr,
                               queue_size_bytes, read_index_ptr, queue_metadata_size_bytes, event,
                               &queue_resource);
}

hsa_status_t DynamicDriver::CreateQueue(
    uint32_t node_id, HSA_QUEUE_TYPE type, uint32_t queue_pct,
    HSA::hsa_amd_queue_priority_internal_t priority, uint32_t sdma_engine_id, void* queue_addr,
    uint64_t queue_size_bytes, uint64_t queue_metadata_size_bytes, HsaEvent* event,
    HsaQueueResource& queue_resource) const {
  return CreateQueueWithReadIndex(node_id, type, queue_pct, priority, sdma_engine_id, queue_addr,
                                  queue_size_bytes, nullptr, queue_metadata_size_bytes, event,
                                  queue_resource);
}
```

- [ ] **Step 3: Pass the ring and read index; adopt the doorbell**

In `amd_dynamic_aql_queue.cpp`, in the constructor, replace the `CreateQueue` call:

```cpp
  HsaQueueResource queue_resource = {};
  hsa_status_t status = static_cast<DynamicDriver&>(agent->driver()).CreateQueueWithReadIndex(
      node_id, HSA_QUEUE_COMPUTE_AQL, 0, rocr::HSA::HSA_AMD_QUEUE_PRIORITY_NORMAL, 0,
      ring_buf_, queue_size_bytes, &amd_queue_.read_dispatch_id, 0, nullptr, queue_resource);
  if (status != HSA_STATUS_SUCCESS) {
    throw hsa_exception(status, "Failed to create a hardware context for a Dynamic queue.");
  }
  queue_id_ = queue_resource.QueueId;
```

Note `ring_buf_` replaces the `nullptr` that was previously passed as `queue_addr`.

Then replace the doorbell initialization:

```cpp
  signal_.hardware_doorbell_ptr = queue_resource.Queue_DoorBell_aql;
```

This line must come **after** the `CreateQueueWithReadIndex` call, so move it out of the earlier `signal_` block, which keeps only:

```cpp
  signal_.kind = AMD_SIGNAL_KIND_DOORBELL;
  signal_.queue_ptr = &amd_queue_;
```

Add `#include "core/inc/amd_dynamic_driver.h"`.

- [ ] **Step 4: Make StoreRelaxed write the doorbell and delete SubmitPackets**

Replace `DynamicAqlQueue::StoreRelaxed` and `SubmitPackets` with just:

```cpp
void DynamicAqlQueue::StoreRelaxed(hsa_signal_value_t value) {
  // Mirrors AqlQueue: the doorbell is a plain memory write the device observes.
  _mm_sfence();
  *(signal_.hardware_doorbell_ptr) = uint64_t(value);
}
```

`StoreRelease` keeps its existing body (release fence then `StoreRelaxed`). Remove the `SubmitPackets` declaration from `amd_dynamic_aql_queue.h` and the `active_` check it carried — `Inactivate` still owns `active_`.

- [ ] **Step 5: Build and install**

Run:
```bash
cmake --build /tmp/rocr-build -j"$(nproc)" 2>&1 | tail -40 && cmake --install /tmp/rocr-build
```
Expected: builds clean.

- [ ] **Step 6: Commit**

```bash
git add projects/rocr-runtime/runtime/hsa-runtime/core/driver/dynamic/rocr_dynamic_driver.h \
        projects/rocr-runtime/runtime/hsa-runtime/core/driver/dynamic/amd_dynamic_driver.cpp \
        projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_dynamic_driver.h \
        projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_dynamic_aql_queue.h \
        projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_dynamic_aql_queue.cpp
git commit -m "feat(dynamic-agent): driver-owned doorbell and ring plumbing for dynamic queues"
```

---

### Task 4: DynAccel skeleton — packet ABI, topology, build, and CTest wiring

First task that produces a loadable driver. Ends with a dynamic agent visible to `hsa_iterate_agents` with real properties.

**Files:**
- Create: `rocrtst/suites/dynamic/dynaccel/dynaccel_packet.h`
- Create: `rocrtst/suites/dynamic/dynaccel/dynaccel.c`
- Create: `rocrtst/suites/dynamic/dynaccel/CMakeLists.txt`
- Modify: `rocrtst/suites/dynamic/CMakeLists.txt`
- Modify: `rocrtst/suites/dynamic/dispatch.cc`

**Interfaces:**
- Consumes: `rocr_dynamic_driver_ftable_t` and `rocr_dynamic_driver_agent_props_t` from Tasks 2-3.
- Produces: `libdynaccel.so` exporting `rocr_dynamic_driver_create`; `dynaccel_dispatch_packet_t`; `DYNACCEL_OPCODE_DISPATCH`; CTest targets `dispatch_nodriver`, `dispatch_dynaccel`, `memory_nodriver`, `memory_dynaccel`.

- [ ] **Step 1: Write the failing test**

Append to `rocrtst/suites/dynamic/dispatch.cc`:

```cpp
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

  uint32_t queue_max_size = 0;
  ASSERT_EQ(hsa_agent_get_info(dynamic_agents.front(), HSA_AGENT_INFO_QUEUE_MAX_SIZE,
                               &queue_max_size),
            HSA_STATUS_SUCCESS);
  EXPECT_EQ(queue_max_size, 64u);

  EXPECT_EQ(hsa_shut_down(), HSA_STATUS_SUCCESS);
}
```

- [ ] **Step 2: Write the packet header**

Create `rocrtst/suites/dynamic/dynaccel/dynaccel_packet.h` with the SPDX header, then:

```c
#ifndef DYNACCEL_PACKET_H_
#define DYNACCEL_PACKET_H_

#include <assert.h>
#include <stdint.h>

#include "hsa/hsa.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Packet type, carried in the standard AQL header type field. */
typedef enum {
  /** Ready for the packet processor. Aliases HSA_PACKET_TYPE_VENDOR_SPECIFIC. */
  DYNACCEL_PACKET_TYPE_READY = 0,
  /** Already consumed. Aliases HSA_PACKET_TYPE_INVALID. */
  DYNACCEL_PACKET_TYPE_INVALID = 1,
} dynaccel_packet_type_t;

/** Vendor opcode. */
typedef enum {
  DYNACCEL_OPCODE_DISPATCH = 0,
  DYNACCEL_OPCODE_NOP = 1,
} dynaccel_opcode_t;

/** Signature every dispatched function must have. */
typedef void (*dynaccel_kernel_t)(uint64_t* args, uint32_t num_args);

/** 64-byte AQL-compliant dispatch packet. */
typedef struct dynaccel_dispatch_packet_s {
  union {
    struct {
      uint16_t header;
      uint16_t opcode;
    };
    uint32_t full_header;
  };
  uint16_t num_kernargs;
  uint16_t reserved0;
  hsa_signal_t completion_signal;
  uint64_t function;
  void* kernarg_address;
  uint64_t reserved1[4];
} dynaccel_dispatch_packet_t;

static_assert(sizeof(dynaccel_dispatch_packet_t) == 64, "AQL packets are 64 bytes");

#ifdef __cplusplus
}
#endif

#endif  // DYNACCEL_PACKET_H_
```

- [ ] **Step 3: Write the driver skeleton**

Create `rocrtst/suites/dynamic/dynaccel/dynaccel.c` with the SPDX header, then the state, topology, and factory. Queue and memory slots stay NULL until Tasks 5-6.

```c
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/driver/dynamic/rocr_dynamic_driver.h"
#include "dynaccel_packet.h"

#define DYNACCEL_QUEUE_PKTS 64

struct rocr_dynamic_driver_context_t {
  int udmabuf_fd;              /* -1 => plain-mmap fallback */
  pthread_mutex_t lock;        /* guards allocs and queues */
  struct dynaccel_alloc* allocs;
  struct dynaccel_queue* queues;
  uint64_t next_id;
};

/* ---- Lifecycle ---- */

static hsa_status_t dynaccel_init(rocr_dynamic_driver_context_t* ctx) {
  (void)ctx;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_shut_down(rocr_dynamic_driver_context_t* ctx) {
  (void)ctx;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_open(rocr_dynamic_driver_context_t* ctx) {
  (void)ctx;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_close(rocr_dynamic_driver_context_t* ctx) {
  (void)ctx;
  return HSA_STATUS_SUCCESS;
}

/* ---- Topology ---- */

static hsa_status_t dynaccel_get_system_properties(rocr_dynamic_driver_context_t* ctx,
                                                   HsaSystemProperties* sys_props) {
  if (!ctx || !sys_props) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  memset(sys_props, 0, sizeof(*sys_props));
  sys_props->NumNodes = 1;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_get_node_properties(rocr_dynamic_driver_context_t* ctx,
                                                 HsaNodeProperties* node_props, uint32_t node_id) {
  if (!ctx || !node_props || node_id != 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  memset(node_props, 0, sizeof(*node_props));
  node_props->NumMemoryBanks = 2;
  /* No CPU/compute/neural cores: ROCr routes the node to DiscoverDynamic. */
  strncpy((char*)node_props->AMDName, "DynAccel", sizeof(node_props->AMDName) - 1);
  strncpy((char*)node_props->MarketingName, "DynAccel", sizeof(node_props->MarketingName) - 1);
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_get_memory_properties(rocr_dynamic_driver_context_t* ctx,
                                                   uint32_t node_id,
                                                   HsaMemoryProperties* mem_props,
                                                   uint32_t* num_props) {
  if (!ctx || !num_props || node_id != 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  if (!mem_props) {           /* count query */
    *num_props = 2;
    return HSA_STATUS_SUCCESS;
  }
  if (*num_props < 2) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  memset(mem_props, 0, 2 * sizeof(*mem_props));
  mem_props[0].HeapType = HSA_HEAPTYPE_SYSTEM;                /* kernarg, fine grain */
  mem_props[0].SizeInBytes = 256ull << 20;
  mem_props[1].HeapType = HSA_HEAPTYPE_FRAME_BUFFER_PUBLIC;   /* device, coarse grain */
  mem_props[1].SizeInBytes = 256ull << 20;
  *num_props = 2;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_get_agent_properties(rocr_dynamic_driver_context_t* ctx,
                                                  uint32_t node_id,
                                                  rocr_dynamic_driver_agent_props_t* props) {
  if (!ctx || !props || node_id != 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  memset(props, 0, sizeof(*props));
  strncpy(props->name, "DynAccel", sizeof(props->name) - 1);
  strncpy(props->vendor_name, "AMD", sizeof(props->vendor_name) - 1);
  strncpy(props->product_name, "DynAccel Reference Accelerator",
          sizeof(props->product_name) - 1);
  strncpy(props->uuid, "DYN-00000000", sizeof(props->uuid) - 1);
  props->queue_min_size = DYNACCEL_QUEUE_PKTS;
  props->queue_max_size = DYNACCEL_QUEUE_PKTS;
  props->queues_max = 8;
  props->cacheline_size = 64;
  props->compute_unit_count = 1;
  props->max_clock_frequency = 1000;
  props->profile = (uint8_t)HSA_PROFILE_BASE;
  props->default_float_rounding_mode = (uint8_t)HSA_DEFAULT_FLOAT_ROUNDING_MODE_NEAR;
  return HSA_STATUS_SUCCESS;
}

/* ---- Cleanup ---- */

static void dynaccel_destroy_context(rocr_dynamic_driver_context_t* ctx) {
  if (!ctx) return;
  pthread_mutex_destroy(&ctx->lock);
  free(ctx);
}

/* ---- Factory ---- */

static rocr_dynamic_driver_ftable_t g_ftable = {
    .devnode_name = "/dev/dynaccel",
    .init = dynaccel_init,
    .shut_down = dynaccel_shut_down,
    .open = dynaccel_open,
    .close = dynaccel_close,
    .get_system_properties = dynaccel_get_system_properties,
    .get_node_properties = dynaccel_get_node_properties,
    .get_memory_properties = dynaccel_get_memory_properties,
    .get_agent_properties = dynaccel_get_agent_properties,
    .destroy_context = dynaccel_destroy_context,
};

rocr_dynamic_driver_ftable_t* rocr_dynamic_driver_create(rocr_dynamic_driver_context_t** ctx_out) {
  if (!ctx_out) return NULL;

  rocr_dynamic_driver_context_t* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->udmabuf_fd = -1;   /* opened in Task 5 */
  if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
    free(ctx);
    return NULL;
  }

  *ctx_out = ctx;
  return &g_ftable;
}
```

Forward-declare `struct dynaccel_alloc;` and `struct dynaccel_queue;` above the context struct so the pointers compile before Tasks 5-6 define them.

- [ ] **Step 4: Write the library CMakeLists**

Create `rocrtst/suites/dynamic/dynaccel/CMakeLists.txt`:

```cmake
add_library(dynaccel SHARED dynaccel.c)
set_target_properties(dynaccel
  PROPERTIES
    C_STANDARD                11
    C_EXTENSIONS              ON      # _GNU_SOURCE, memfd_create
    POSITION_INDEPENDENT_CODE ON
)
target_compile_options(dynaccel PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(dynaccel PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${ROCR_SOURCE_DIR}/runtime/hsa-runtime      # core/driver/dynamic/rocr_dynamic_driver.h
  ${ROCR_SOURCE_DIR}/runtime/hsa-runtime/inc  # hsa.h, amd_hsa_signal.h
)
target_link_libraries(dynaccel PRIVATE Threads::Threads)
```

`rocr_dynamic_driver.h` is an in-tree header, not an installed one, so the suite needs the ROCr source path. Add to the top of `rocrtst/suites/dynamic/CMakeLists.txt`:

```cmake
set(ROCR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." CACHE PATH
    "Path to the rocr-runtime source tree")
find_package(Threads REQUIRED)
add_subdirectory(dynaccel)
```

- [ ] **Step 5: Double the CTest registration**

`NoDynamicDriver` tests must run *without* the driver. Replace the `add_dynamic_test` function and its two calls in `rocrtst/suites/dynamic/CMakeLists.txt` with:

```cmake
# Declare a dynamic-driver test executable built from <name>.cc, registered twice:
# once with no driver present, once with libdynaccel.so preloaded.
function(add_dynamic_test name)
  add_executable(${name} ${name}.cc)
  set_target_properties(${name}
    PROPERTIES
      CXX_STANDARD                17
      CXX_EXTENSIONS              OFF
      CXX_VISIBILITY_PRESET       hidden
      VISIBILITY_INLINES_HIDDEN   ON
      POSITION_INDEPENDENT_CODE   ON
  )
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dynaccel)
  target_link_libraries(${name} PRIVATE hsa-runtime64::hsa-runtime64 GTest::gtest_main)

  add_test(NAME ${name}_nodriver
           COMMAND ${name} --gtest_filter=*NoDynamicDriver*)

  add_test(NAME ${name}_dynaccel
           COMMAND ${name} --gtest_filter=-*NoDynamicDriver*)
  set_tests_properties(${name}_dynaccel PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:dynaccel>;HSA_ENABLE_INTERRUPT=0")
endfunction()

add_dynamic_test(memory)
add_dynamic_test(dispatch)
```

`HSA_ENABLE_INTERRUPT=0` selects `DefaultSignal`, which busy-waits on the signal value — deterministic latency, since the driver cannot raise the KFD interrupt.

Remove the now-unused `gtest_discover_tests` calls and keep `include(GoogleTest)` only if still referenced; the manual `add_test` calls above replace discovery because each binary needs two different filters and environments.

- [ ] **Step 6: Build and run the test**

Run:
```bash
cmake -S projects/rocr-runtime/rocrtst/suites/dynamic -B /tmp/dynaccel-build \
      -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/tmp/rocr-install
cmake --build /tmp/dynaccel-build -j"$(nproc)"

# The new test, against the preloaded driver:
LD_PRELOAD=/tmp/dynaccel-build/dynaccel/libdynaccel.so HSA_ENABLE_INTERRUPT=0 \
  /tmp/dynaccel-build/dispatch --gtest_filter=DynAccel.AgentDiscovered

# And the no-driver registration, which must stay green:
ctest --test-dir /tmp/dynaccel-build -R dispatch_nodriver --output-on-failure
```
Expected: both PASS.

Do **not** run the full `dispatch_dynaccel` registration yet — `create_queue` is NULL until Task 6, so every queue test in it fails by construction. Task 6 Step 5 is where that binary first goes green.

- [ ] **Step 7: Commit**

```bash
git add projects/rocr-runtime/rocrtst/suites/dynamic/dynaccel/ \
        projects/rocr-runtime/rocrtst/suites/dynamic/CMakeLists.txt \
        projects/rocr-runtime/rocrtst/suites/dynamic/dispatch.cc
git commit -m "feat(dynaccel): packet ABI, driver skeleton, topology, and CTest wiring"
```

---

### Task 5: udmabuf-backed memory

**Files:**
- Modify: `rocrtst/suites/dynamic/dynaccel/dynaccel.c`
- Modify: `rocrtst/suites/dynamic/memory.cc`

**Interfaces:**
- Consumes: the context and factory from Task 4.
- Produces: ftable slots `allocate_memory`, `free_memory`, `export_memory_handle`; `struct dynaccel_alloc`.

- [ ] **Step 1: Write the failing test**

Append to `rocrtst/suites/dynamic/memory.cc`:

```cpp
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
```

Add `#include <cstring>` to `memory.cc` if absent.

- [ ] **Step 2: Run it to verify it fails**

Run:
```bash
cmake --build /tmp/dynaccel-build -j"$(nproc)"
ctest --test-dir /tmp/dynaccel-build -R memory_dynaccel --output-on-failure
```
Expected: FAIL — `hsa_amd_memory_pool_allocate` returns an error because `allocate_memory` is NULL.

- [ ] **Step 3: Implement the allocator**

Add to `dynaccel.c`, above the topology section. Follows the libhsakmt idiom in `libhsakmt/src/fmm.c:1692`:

```c
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct dynaccel_alloc {
  uint64_t id;
  int dmabuf_fd;               /* -1 => plain mmap */
  void* ptr;
  size_t size;                 /* page-rounded */
  struct dynaccel_alloc* next;
};

static size_t dynaccel_round_up_page(size_t size) {
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  return (size + page - 1) & ~(page - 1);
}

/* Returns dmabuf fd, or -1 to signal the caller to fall back to plain mmap. */
static int dynaccel_make_udmabuf(int udmabuf_fd, size_t size) {
  if (udmabuf_fd < 0) return -1;

  int memfd = memfd_create("dynaccel", MFD_ALLOW_SEALING);
  if (memfd < 0) return -1;

  int dmabuf_fd = -1;
  if (ftruncate(memfd, (off_t)size) == 0 &&
      fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0) {
    struct udmabuf_create create = {
        .memfd = (uint32_t)memfd, .flags = 0, .offset = 0, .size = size};
    dmabuf_fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &create);
    if (dmabuf_fd < 0) dmabuf_fd = -1;
  }

  close(memfd);   /* udmabuf holds its own reference to the pages */
  return dmabuf_fd;
}

static hsa_status_t dynaccel_allocate_memory(rocr_dynamic_driver_context_t* ctx, uint32_t node_id,
                                             size_t size, uint32_t alloc_flags,
                                             HsaMemFlags mem_flags, HsaMemoryProperties mem_props,
                                             void** mem) {
  (void)node_id; (void)alloc_flags; (void)mem_flags; (void)mem_props;
  if (!ctx || !mem || size == 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  const size_t rounded = dynaccel_round_up_page(size);
  const int dmabuf_fd = dynaccel_make_udmabuf(ctx->udmabuf_fd, rounded);

  void* ptr = (dmabuf_fd >= 0)
      ? mmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0)
      : mmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED) {
    if (dmabuf_fd >= 0) close(dmabuf_fd);
    return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  }

  struct dynaccel_alloc* a = calloc(1, sizeof(*a));
  if (!a) {
    munmap(ptr, rounded);
    if (dmabuf_fd >= 0) close(dmabuf_fd);
    return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  }

  pthread_mutex_lock(&ctx->lock);
  a->id = ++ctx->next_id;
  a->dmabuf_fd = dmabuf_fd;
  a->ptr = ptr;
  a->size = rounded;
  a->next = ctx->allocs;
  ctx->allocs = a;
  pthread_mutex_unlock(&ctx->lock);

  *mem = ptr;
  return HSA_STATUS_SUCCESS;
}

/* Caller must hold ctx->lock. */
static struct dynaccel_alloc* dynaccel_find_alloc(rocr_dynamic_driver_context_t* ctx, void* ptr) {
  for (struct dynaccel_alloc* a = ctx->allocs; a; a = a->next) {
    if (a->ptr == ptr) return a;
  }
  return NULL;
}

static hsa_status_t dynaccel_free_memory(rocr_dynamic_driver_context_t* ctx, void* mem,
                                         size_t size) {
  (void)size;
  if (!ctx || !mem) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_alloc** link = &ctx->allocs;
  while (*link && (*link)->ptr != mem) link = &(*link)->next;
  struct dynaccel_alloc* a = *link;
  if (a) *link = a->next;
  pthread_mutex_unlock(&ctx->lock);

  if (!a) return HSA_STATUS_ERROR_INVALID_ALLOCATION;

  munmap(a->ptr, a->size);
  if (a->dmabuf_fd >= 0) close(a->dmabuf_fd);
  free(a);
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_export_memory_handle(rocr_dynamic_driver_context_t* ctx,
                                                  uint32_t node_id,
                                                  const rocr_dynamic_driver_memory_handle_t* handle,
                                                  int share_type, void* export_handle) {
  (void)node_id;
  if (!ctx || !handle || !export_handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  if (share_type != ROCR_DYNAMIC_SHARE_DMABUF_FD) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_alloc* a = dynaccel_find_alloc(ctx, (void*)(uintptr_t)handle->handle);
  const int fd = a ? a->dmabuf_fd : -1;
  pthread_mutex_unlock(&ctx->lock);

  if (!a) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
  if (fd < 0) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;  /* mmap fallback: not exportable */

  const int dup_fd = dup(fd);
  if (dup_fd < 0) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  *(int*)export_handle = dup_fd;
  return HSA_STATUS_SUCCESS;
}
```

- [ ] **Step 4: Open /dev/udmabuf in the factory**

In `rocr_dynamic_driver_create`, replace `ctx->udmabuf_fd = -1;` with:

```c
  /* DYNACCEL_DISABLE_UDMABUF forces the fallback path so CI covers both. */
  ctx->udmabuf_fd = getenv("DYNACCEL_DISABLE_UDMABUF") ? -1 : open("/dev/udmabuf", O_RDWR);
```

and in `dynaccel_destroy_context`, before `free(ctx)`:

```c
  if (ctx->udmabuf_fd >= 0) close(ctx->udmabuf_fd);
```

Add `#include <stdio.h>` only if needed; `getenv` comes from `stdlib.h`, already included.

- [ ] **Step 5: Register the slots**

Add to the `g_ftable` initializer:

```c
    .allocate_memory = dynaccel_allocate_memory,
    .free_memory = dynaccel_free_memory,
    .export_memory_handle = dynaccel_export_memory_handle,
```

- [ ] **Step 6: Run the test to verify it passes**

Run:
```bash
cmake --build /tmp/dynaccel-build -j"$(nproc)"
ctest --test-dir /tmp/dynaccel-build -R memory_dynaccel --output-on-failure
```
Expected: `Memory.UdmabufExport` PASSES, or SKIPS with the `/dev/udmabuf unavailable` message if the user is not in the `kvm` group. Both are acceptable.

Then verify the fallback path explicitly:
```bash
DYNACCEL_DISABLE_UDMABUF=1 LD_PRELOAD=/tmp/dynaccel-build/dynaccel/libdynaccel.so \
  HSA_ENABLE_INTERRUPT=0 /tmp/dynaccel-build/memory --gtest_filter=Memory.UdmabufExport
```
Expected: SKIPPED.

- [ ] **Step 7: Flip the interop expectations**

In `memory.cc`, `TEST(Memory, DMABufExportImportGPUtoDynamic)` and `TEST(Memory, DMABufExportImportDynamicToGPU)` currently assert failure. Replace each `EXPECT_NE(hsa_amd_interop_map_buffer(...), HSA_STATUS_SUCCESS)` block and its preceding comment with:

```cpp
  const hsa_status_t map_status =
      hsa_amd_interop_map_buffer(num_agents, agents, dma_buf_fd, 0, &import_size,
                                 reinterpret_cast<void**>(&import_buffer), nullptr, nullptr);
  if (map_status == HSA_STATUS_ERROR_OUT_OF_RESOURCES) {
    GTEST_SKIP() << "dma-buf interop unavailable in this configuration";
  }
  EXPECT_EQ(map_status, HSA_STATUS_SUCCESS);
  EXPECT_NE(import_buffer, nullptr);
  if (map_status == HSA_STATUS_SUCCESS) {
    EXPECT_EQ(hsa_amd_interop_unmap_buffer(import_buffer), HSA_STATUS_SUCCESS);
  }
```

These two tests also require a physical GPU (`ASSERT_FALSE(gpu_agents.empty())`), so they will not run on a GPU-less CI node regardless.

- [ ] **Step 8: Run the full memory suite**

Run:
```bash
cmake --build /tmp/dynaccel-build -j"$(nproc)"
ctest --test-dir /tmp/dynaccel-build -R memory --output-on-failure
```
Expected: `memory_nodriver` PASSES; `memory_dynaccel` passes or skips every test, with no failures. If `import_memory_handle` turns out to be needed for the interop path, implement it in this task rather than deferring — the ftable slot already exists.

- [ ] **Step 9: Commit**

```bash
git add projects/rocr-runtime/rocrtst/suites/dynamic/dynaccel/dynaccel.c \
        projects/rocr-runtime/rocrtst/suites/dynamic/memory.cc
git commit -m "feat(dynaccel): udmabuf-backed memory with mmap fallback and dma-buf export"
```

---

### Task 6: Queue, worker thread, and packet dispatch

**Files:**
- Modify: `rocrtst/suites/dynamic/dynaccel/dynaccel.c`
- Modify: `rocrtst/suites/dynamic/dispatch.cc`

**Interfaces:**
- Consumes: `dynaccel_dispatch_packet_t` and `dynaccel_kernel_t` (Task 4); the allocator (Task 5); the `read_index_ptr` create_queue parameter (Task 3).
- Produces: ftable slots `create_queue`, `destroy_queue`; `struct dynaccel_queue`.

- [ ] **Step 1: Write the failing tests**

Append to `rocrtst/suites/dynamic/dispatch.cc`. Add `#include "dynaccel_packet.h"` and `#include <atomic>` to the includes.

```cpp
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
```

Also strengthen the existing `TEST(Dispatch, SubmitBarrierPacket)`: replace the two-line comment about the signal not being decremented with a real wait:

```cpp
  EXPECT_EQ(hsa_signal_wait_scacquire(completion_signal, HSA_SIGNAL_CONDITION_EQ, 0,
                                      5000000000ull, HSA_WAIT_STATE_ACTIVE),
            0);
```

- [ ] **Step 2: Run to verify they fail**

Run:
```bash
cmake --build /tmp/dynaccel-build -j"$(nproc)"
ctest --test-dir /tmp/dynaccel-build -R dispatch_dynaccel --output-on-failure
```
Expected: FAIL — `hsa_queue_create` returns an error because `create_queue` is NULL.

- [ ] **Step 3: Implement the queue, worker, and signal completion**

Add to `dynaccel.c`, after the memory section. Needs `#include <sched.h>`, `#include <time.h>`, and `#include "amd_hsa_signal.h"`.

```c
struct dynaccel_queue {
  uint64_t id;
  _Alignas(64) uint64_t doorbell;  /* ROCr writes; worker polls */
  void* ring;
  uint32_t num_pkts;
  uint64_t* read_index;            /* -> amd_queue_.read_dispatch_id */
  atomic_bool run;
  pthread_t thread;
  struct dynaccel_queue* next;
};

/* Completes a signal the way hardware does -- see trap_handler.s send_signal().
   Never calls into the HSA runtime. */
static void dynaccel_complete(hsa_signal_t sig) {
  if (!sig.handle) return;
  amd_signal_t* s = (amd_signal_t*)(uintptr_t)sig.handle;

  atomic_fetch_sub_explicit((_Atomic int64_t*)&s->value, 1, memory_order_release);

  /* Mirrors the GPU. We cannot raise the interrupt, but every ROCr waiter
     re-reads value each loop iteration, so the store is always observed. */
  if (s->event_mailbox_ptr && s->event_id) {
    atomic_store_explicit((_Atomic uint32_t*)(uintptr_t)s->event_mailbox_ptr, s->event_id,
                          memory_order_release);
  }
}

static void dynaccel_execute(struct dynaccel_queue* q, uint64_t index) {
  dynaccel_dispatch_packet_t* p =
      &((dynaccel_dispatch_packet_t*)q->ring)[index % q->num_pkts];

  const uint16_t header = __atomic_load_n(&p->header, __ATOMIC_ACQUIRE);
  const uint16_t type = (header >> HSA_PACKET_HEADER_TYPE) & 0xFF;

  if (type == DYNACCEL_PACKET_TYPE_READY && p->opcode == DYNACCEL_OPCODE_DISPATCH &&
      p->function) {
    ((dynaccel_kernel_t)(uintptr_t)p->function)((uint64_t*)p->kernarg_address, p->num_kernargs);
  }
  /* BARRIER_AND / BARRIER_OR need no work: a single in-order worker per queue
     already satisfies them. Any other type is ignored. */

  dynaccel_complete(p->completion_signal);
  __atomic_store_n(&p->header,
                   (uint16_t)(DYNACCEL_PACKET_TYPE_INVALID << HSA_PACKET_HEADER_TYPE),
                   __ATOMIC_RELEASE);
}

static void dynaccel_backoff(int* idle) {
  if (++(*idle) < 64) {
    sched_yield();
  } else {
    const struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000};  /* 50 us */
    nanosleep(&ts, NULL);
  }
}

static void* dynaccel_worker(void* arg) {
  struct dynaccel_queue* q = (struct dynaccel_queue*)arg;
  int idle = 0;

  while (atomic_load_explicit(&q->run, memory_order_relaxed)) {
    /* The doorbell holds the index of the LAST packet written, so the ring is
       non-empty when db + 1 > read_index. Comparing against read_index rather
       than a cached doorbell is what makes the very first doorbell -- value 0,
       for the packet in slot 0 -- observable. */
    const uint64_t rd =
        atomic_load_explicit((_Atomic uint64_t*)q->read_index, memory_order_relaxed);
    const uint64_t db =
        atomic_load_explicit((_Atomic uint64_t*)&q->doorbell, memory_order_acquire);

    if ((int64_t)(db + 1 - rd) <= 0) {
      dynaccel_backoff(&idle);
      continue;
    }
    idle = 0;

    for (uint64_t i = rd; i <= db; ++i) {
      dynaccel_execute(q, i);
      atomic_store_explicit((_Atomic uint64_t*)q->read_index, i + 1, memory_order_release);
    }
  }
  return NULL;
}

static hsa_status_t dynaccel_create_queue(rocr_dynamic_driver_context_t* ctx, uint32_t node_id,
                                          uint32_t type, uint32_t queue_pct, uint32_t priority,
                                          uint32_t sdma_engine_id, void* queue_addr,
                                          uint64_t queue_size_bytes, uint64_t* read_index_ptr,
                                          uint64_t queue_metadata_size_bytes, HsaEvent* event,
                                          HsaQueueResource* queue_resource) {
  (void)type; (void)queue_pct; (void)priority; (void)sdma_engine_id;
  (void)queue_metadata_size_bytes; (void)event;
  if (!ctx || !queue_addr || !read_index_ptr || !queue_resource || node_id != 0) {
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  const uint64_t num_pkts = queue_size_bytes / sizeof(dynaccel_dispatch_packet_t);
  if (num_pkts == 0 || (num_pkts & (num_pkts - 1)) != 0) {
    return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
  }

  struct dynaccel_queue* q = calloc(1, sizeof(*q));
  if (!q) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;

  q->doorbell = UINT64_MAX;   /* empty: db + 1 == read_index == 0 */
  q->ring = queue_addr;
  q->num_pkts = (uint32_t)num_pkts;
  q->read_index = read_index_ptr;
  atomic_store_explicit(&q->run, true, memory_order_relaxed);

  pthread_mutex_lock(&ctx->lock);
  q->id = ++ctx->next_id;
  q->next = ctx->queues;
  ctx->queues = q;
  pthread_mutex_unlock(&ctx->lock);

  if (pthread_create(&q->thread, NULL, dynaccel_worker, q) != 0) {
    pthread_mutex_lock(&ctx->lock);
    ctx->queues = q->next;
    pthread_mutex_unlock(&ctx->lock);
    free(q);
    return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  }

  memset(queue_resource, 0, sizeof(*queue_resource));
  queue_resource->QueueId = (HSA_QUEUEID)q->id;
  queue_resource->Queue_DoorBell_aql = &q->doorbell;
  queue_resource->Queue_read_ptr_aql = read_index_ptr;
  return HSA_STATUS_SUCCESS;
}

static hsa_status_t dynaccel_destroy_queue(rocr_dynamic_driver_context_t* ctx,
                                           uint64_t queue_id) {
  if (!ctx) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  pthread_mutex_lock(&ctx->lock);
  struct dynaccel_queue** link = &ctx->queues;
  while (*link && (*link)->id != queue_id) link = &(*link)->next;
  struct dynaccel_queue* q = *link;
  if (q) *link = q->next;
  pthread_mutex_unlock(&ctx->lock);

  if (!q) return HSA_STATUS_ERROR_INVALID_QUEUE;

  atomic_store_explicit(&q->run, false, memory_order_release);
  pthread_join(q->thread, NULL);
  free(q);
  return HSA_STATUS_SUCCESS;
}
```

- [ ] **Step 4: Register the slots**

Add to the `g_ftable` initializer:

```c
    .create_queue = dynaccel_create_queue,
    .destroy_queue = dynaccel_destroy_queue,
```

- [ ] **Step 5: Run the tests to verify they pass**

Run:
```bash
cmake --build /tmp/dynaccel-build -j"$(nproc)"
ctest --test-dir /tmp/dynaccel-build --output-on-failure
```
Expected: every test in `dispatch_nodriver`, `dispatch_dynaccel`, `memory_nodriver`, and `memory_dynaccel` passes or skips. In particular `Dispatch.ExecuteFunction`, `Dispatch.PacketsExecuteInOrder`, `Dispatch.SubmitBarrierPacket`, `Dispatch.CreateQueue`, and `Dispatch.CreateDestroyMultipleQueues` must PASS.

If `Dispatch.CreateDestroyMultipleQueues` fails, check `props_.queues_max` (set to 8 in Task 4) against the four queues it creates.

- [ ] **Step 6: Run under ThreadSanitizer**

The worker races with the submitting thread by design; confirm the intended races are the only ones.

Run:
```bash
cmake -S projects/rocr-runtime/rocrtst/suites/dynamic -B /tmp/dynaccel-tsan \
      -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/tmp/rocr-install \
      -DCMAKE_C_FLAGS=-fsanitize=thread -DCMAKE_CXX_FLAGS=-fsanitize=thread
cmake --build /tmp/dynaccel-tsan -j"$(nproc)"
LD_PRELOAD=/tmp/dynaccel-tsan/dynaccel/libdynaccel.so HSA_ENABLE_INTERRUPT=0 \
  /tmp/dynaccel-tsan/dispatch --gtest_filter=Dispatch.ExecuteFunction
```
Expected: no TSan report from `dynaccel.c`. Reports originating inside ROCr itself are out of scope — record them, do not chase them. If TSan flags the doorbell or `read_index` accesses in `dynaccel.c`, that is a real finding: the atomics there are the synchronization contract and must be clean.

- [ ] **Step 7: Commit**

```bash
git add projects/rocr-runtime/rocrtst/suites/dynamic/dynaccel/dynaccel.c \
        projects/rocr-runtime/rocrtst/suites/dynamic/dispatch.cc
git commit -m "feat(dynaccel): per-queue worker thread and AQL packet dispatch"
```

---

## Verification checklist

Run before declaring the work complete:

- [ ] `cmake --build /tmp/rocr-build` succeeds with no new warnings in the five modified ROCr files.
- [ ] `ctest --test-dir /tmp/dynaccel-build --output-on-failure` reports zero failures.
- [ ] `Dispatch.ExecuteFunction` and `Dispatch.PacketsExecuteInOrder` PASS (not skip).
- [ ] `DYNACCEL_DISABLE_UDMABUF=1` run shows `Memory.UdmabufExport` SKIPPED and every dispatch test still PASSING.
- [ ] `git log --oneline origin/develop..HEAD` shows six new commits, one per task.
- [ ] `grep -rn "SubmitPackets" projects/rocr-runtime/runtime/hsa-runtime/core/*/amd_dynamic_*` returns nothing.
- [ ] `nm -D /tmp/dynaccel-build/dynaccel/libdynaccel.so | grep -c ' U hsa_'` returns 0 — the driver calls no HSA function.
