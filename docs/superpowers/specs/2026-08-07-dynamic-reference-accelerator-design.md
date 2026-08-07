# DynAccel — a reference accelerator on the ROCr dynamic driver interface

**Date:** 2026-08-07
**Branch:** `dynamic-agent`
**Status:** design approved, ready for implementation planning

## Purpose

Build an imaginary software accelerator ("DynAccel") that plugs into ROCr through the
dynamic driver/agent interface. It serves two goals at once:

1. **CI test backend** — makes `rocrtst/suites/dynamic/` meaningful. Today those tests
   assert that dispatch and interop *fail*, because no backend exists.
2. **Canonical worked example** — a third party writing a real dynamic driver copies
   this and follows the comments.

The device is deliberately simple: memory is `udmabuf`-backed so it produces genuine
dma-buf file descriptors a physical GPU can import, and packet processing is a worker
thread running a loop over the ring.

Because an AQL packet carries a raw function pointer, the "device" necessarily executes
in the submitting process's address space. That is the point — it keeps the example to a
single file while still exercising every real interface boundary.

## Non-goals

- Emulating GPU semantics (ISA, code objects, grids, workgroups). `supported_isas` stays
  empty; no code object loading.
- PM4 packets, CU masking, SPM, trap handlers. Those ftable slots stay NULL.
- Multi-node topology. One node, one agent.
- Being fast. Correctness and readability only.

## Scope of ROCr-side change

### Compile blockers (must land first)

`DynamicAgent` and `DynamicDriver` do not currently build against the refactored
`core::Driver` / `core::Agent` interfaces.

| File | Fix |
| --- | --- |
| `core/inc/amd_dynamic_agent.h` | Add missing `VisitRegion` and `GetNearestCpuAgent` overrides (both pure virtual in `core/inc/agent.h:266,337`). Declare the `supported_isas_` member that `supported_isas()` returns but which is never defined. |
| `core/inc/amd_dynamic_driver.h:38-41`, `core/driver/dynamic/amd_dynamic_driver.cpp` | `AllocateMemory` / `FreeMemory` still use the old raw-pointer signatures marked `override`. Migrate to `core::DriverMemoryHandle` per `core/inc/driver.h:172,178`, matching `KfdDriver` and `XdnaDriver`. |

### ftable changes

Only two. The doorbell needs none — see "Doorbell" below.

```c
/* create_queue: queue_addr becomes the real ring base (was passed as nullptr);
   read_index_ptr is new. */
hsa_status_t (*create_queue)(rocr_dynamic_driver_context_t* ctx, uint32_t node_id,
                             uint32_t type, uint32_t queue_pct, uint32_t priority,
                             uint32_t sdma_engine_id,
                             void*     queue_addr,        /* ring base */
                             uint64_t  queue_size_bytes,
                             uint64_t* read_index_ptr,    /* &amd_queue_.read_dispatch_id */
                             uint64_t  queue_metadata_size_bytes,
                             HsaEvent* event,
                             HsaQueueResource* queue_resource);

/* Replaces DynamicAgent::GetInfo's hardcoded table.
   NULL pointer => runtime keeps today's zero defaults. */
hsa_status_t (*get_agent_properties)(rocr_dynamic_driver_context_t* ctx, uint32_t node_id,
                                     rocr_dynamic_driver_agent_props_t* props);
```

`rocr_dynamic_driver_agent_props_t` uses plain scalars and char arrays, deliberately
**not** hsakmt types. An out-of-tree driver must not have to replicate unversioned KFD
struct layouts across a `dlopen` boundary.

```c
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
  uint8_t  profile;                        /* hsa_profile_t */
  uint8_t  default_float_rounding_mode;    /* hsa_default_float_rounding_mode_t */
  uint8_t  reserved[2];
} rocr_dynamic_driver_agent_props_t;
```

`DynamicAgent` caches this struct at construction and serves `GetInfo` from it.

### Wiring that needs no new ftable entry

- `DynamicAgent::IterateCache` returns `HSA_STATUS_ERROR_INVALID_CACHE` unconditionally
  even though `get_cache_properties` already exists in the table and is simply never
  called. Wire it up.
- `DynamicAgent::InitRegionList` must mark the frame-buffer region coarse-grained.
  `memory.cc` finds the pool via `discover_first_global_coarse_grain_mem_pool`.

### Recorded follow-up, explicitly out of scope

Prepending `uint32_t struct_size; uint32_t abi_version;` to
`rocr_dynamic_driver_ftable_t` would be cheap `dlopen`-boundary insurance, letting the
runtime reject a stale driver rather than call through a garbage function pointer. It is
**not** part of this change — it touches every driver and is unrelated to standing up the
accelerator. Filed here so it is not lost.

## Doorbell

The doorbell is a memory write, not a driver call. `AqlQueue::StoreRelaxed`
(`core/runtime/amd_aql_queue.cpp:482-491`) does:

```cpp
_mm_sfence();
*(signal_.hardware_doorbell_ptr) = uint64_t(value);
```

with `hardware_doorbell_ptr` set from `queue_rsrc.Queue_DoorBell_aql` at line 291.
`AieAqlQueue` deviates by calling `SubmitPackets`, and `DynamicAqlQueue` inherited that
deviation. We follow the GPU path instead.

The driver owns the doorbell word. `create_queue` allocates it alongside the worker
thread and returns its address through the existing `HsaQueueResource` out-param:

```c
queue_resource->Queue_DoorBell_aql = &q->doorbell;
queue_resource->QueueId            = id;
```

`DynamicAqlQueue` then sets `signal_.hardware_doorbell_ptr` from it and reimplements
`StoreRelaxed` as the fence-plus-store above. `SubmitPackets` is deleted.

Consequently the worker **polls** the doorbell word: acquire loads with a
`pause` / `sched_yield` / short-sleep backoff. That is the honest emulation of a command
processor watching its doorbell. No futex, because ROCr's store carries no wake.

## Packet format

Modeled on `inc/hsa_ext_amd_aie.h`: standard `uint16_t header` with `READY = 0`
deliberately aliasing `HSA_PACKET_TYPE_VENDOR_SPECIFIC` and `INVALID = 1` aliasing
`HSA_PACKET_TYPE_INVALID`, a vendor `uint16_t opcode`, `completion_signal` at offset 8,
and device payload in the remaining bytes.

The header ships **with the device**, not in ROCr's `inc/`. AIE is a first-party device
so ROCr knows its packet; a third-party dynamic driver defines its own format and ROCr
stays ignorant of it. Demonstrating that separation is part of the point.

```c
/* dynaccel_packet.h */
typedef enum {
  DYNACCEL_PACKET_TYPE_READY   = 0,  /* aliases HSA_PACKET_TYPE_VENDOR_SPECIFIC */
  DYNACCEL_PACKET_TYPE_INVALID = 1,  /* aliases HSA_PACKET_TYPE_INVALID */
} dynaccel_packet_type_t;

typedef enum {
  DYNACCEL_OPCODE_DISPATCH = 0,
  DYNACCEL_OPCODE_NOP      = 1,
} dynaccel_opcode_t;

typedef struct dynaccel_dispatch_packet_s {
  union {
    struct { uint16_t header; uint16_t opcode; };
    uint32_t full_header;
  };
  uint16_t     num_kernargs;       /* slots at kernarg_address */
  uint16_t     reserved0;
  hsa_signal_t completion_signal;  /* offset 8, matching AIE's layout choice */
  uint64_t     function;           /* void (*)(uint64_t* args, uint32_t n) */
  void*        kernarg_address;    /* -> uint64_t[num_kernargs] */
  uint64_t     reserved1[4];
} dynaccel_dispatch_packet_t;

_Static_assert(sizeof(dynaccel_dispatch_packet_t) == 64, "AQL packets are 64 bytes");
```

Type erasure is one uniform callee signature: `void fn(uint64_t* args, uint32_t n)`. The
kernarg buffer is a normal agent allocation, so using it exercises the allocation path.

## Accelerator implementation

### Structure

Single translation unit, C, optimized for reading top to bottom.

```
rocrtst/suites/dynamic/
  dynaccel/
    dynaccel_packet.h   # public: packet struct + opcodes; included by tests
    dynaccel.c          # the whole driver, ftable order top-to-bottom
    CMakeLists.txt      # -> libdynaccel.so
  dispatch.cc  memory.cc  common.h  CMakeLists.txt
```

`dynaccel.c` is ordered: state → udmabuf helpers → worker → one function per ftable slot
→ `rocr_dynamic_driver_create` at the bottom.

It links **only libc and pthread**. It consumes `hsa.h` and `amd_hsa_signal.h` as
headers and never calls into the runtime it serves.

### State

```c
typedef struct dynaccel_alloc {
  uint64_t id;
  int      dmabuf_fd;              /* -1 => mmap fallback */
  void*    ptr;
  size_t   size;
  struct dynaccel_alloc* next;
} dynaccel_alloc_t;

typedef struct dynaccel_queue {
  uint64_t  id;
  _Alignas(64) uint64_t doorbell;  /* ROCr writes; worker polls.
                                      MUST be initialized to UINT64_MAX in
                                      create_queue: the doorbell holds the last
                                      written index, so the "empty" state is
                                      db + 1 == read_index == 0. Initializing it
                                      to 0 would make the worker execute a stale
                                      slot 0 before any doorbell is rung. */
  void*     ring;
  uint32_t  num_pkts;
  uint64_t* read_index;            /* -> amd_queue_.read_dispatch_id */
  atomic_bool run;
  pthread_t thread;
  struct dynaccel_queue* next;
} dynaccel_queue_t;

struct rocr_dynamic_driver_context_t {   /* driver defines the opaque type */
  int               udmabuf_fd;          /* -1 => fallback mode */
  pthread_mutex_t   lock;                /* guards both lists */
  dynaccel_alloc_t* allocs;
  dynaccel_queue_t* queues;
  uint64_t          next_id;
};
```

### Topology

One node. `get_system_properties` reports `NumNodes = 1`. `get_node_properties` reports
`NumCPUCores = NumFComputeCores = NumNeuralCores = 0` with `AMDName` and
`MarketingName` set to `"DynAccel"`. `get_memory_properties` reports two banks:

| Heap type | Backing | Role |
| --- | --- | --- |
| `HSA_HEAPTYPE_SYSTEM` | plain `mmap` | kernarg / fine-grained. `InitRegionList` flags the first SYSTEM heap `kernarg`, which is what `InitAllocators` looks for. |
| `HSA_HEAPTYPE_FRAME_BUFFER_PUBLIC` | udmabuf | coarse-grained global device memory; the exportable one. |

### Memory — udmabuf per allocation

Follows the libhsakmt idiom in `libhsakmt/src/fmm.c:1692`.

```c
allocate_memory(device region, size):
  memfd = memfd_create("dynaccel", MFD_ALLOW_SEALING)
  ftruncate(memfd, round_up(size, PAGE))
  fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)
  dmabuf_fd = ioctl(udmabuf_fd, UDMABUF_CREATE, {memfd, 0, size})
  close(memfd)
  ptr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, dmabuf_fd, 0)
  handle = { .handle = id, .dmabuf_fd = dmabuf_fd, .size = size }

export_memory_handle(handle, DMABUF_FD, out):
  *(int*)out = dup(handle->dmabuf_fd)      /* offset always 0 */
```

One fd per allocation, so exports are always at offset 0 and there is no allocator to
explain. Fine at test scale.

### Worker loop

```c
static void* dynaccel_worker(void* arg) {
  dynaccel_queue_t* q = arg;
  int idle = 0;
  while (atomic_load_explicit(&q->run, memory_order_relaxed)) {
    /* AQL: the doorbell carries the index of the LAST packet written, so the
       ring is non-empty when db + 1 > read_index. Comparing against read_index
       rather than a cached previous doorbell is what makes the very first
       doorbell -- value 0, for the packet in slot 0 -- observable. */
    uint64_t rd = atomic_load_explicit((_Atomic uint64_t*)q->read_index,
                                       memory_order_relaxed);
    uint64_t db = atomic_load_explicit((_Atomic uint64_t*)&q->doorbell,
                                       memory_order_acquire);
    if ((int64_t)(db + 1 - rd) <= 0) { dynaccel_backoff(&idle); continue; }
    idle = 0;
    for (uint64_t i = rd; i <= db; ++i) {
      dynaccel_execute(q, i);
      atomic_store_explicit((_Atomic uint64_t*)q->read_index, i + 1,
                            memory_order_release);
    }
  }
  return NULL;
}
```

`dynaccel_execute` switches on the header's packet-type field:

- `VENDOR_SPECIFIC` (`READY`) with `opcode == DISPATCH` and non-NULL `function` →
  `((void(*)(uint64_t*,uint32_t))p->function)(p->kernarg_address, p->num_kernargs)`
- `BARRIER_AND` / `BARRIER_OR` → no-op; a single in-order worker already satisfies them
- anything else → ignored

Then it completes the signal and stamps the header `HSA_PACKET_TYPE_INVALID`.

### Signal completion without calling HSA

`core/runtime/trap_handler/trap_handler.s:664-673` documents what hardware does. We copy
it:

```c
static void dynaccel_complete(hsa_signal_t sig) {
  if (!sig.handle) return;
  amd_signal_t* s = (amd_signal_t*)sig.handle;

  atomic_fetch_sub_explicit((_Atomic int64_t*)&s->value, 1, memory_order_release);

  /* Mirrors the GPU; the interrupt is the one thing we cannot raise. */
  if (s->event_mailbox_ptr && s->event_id) {
    atomic_store_explicit((_Atomic uint32_t*)s->event_mailbox_ptr,
                          s->event_id, memory_order_release);
  }
}
```

This is safe because `InterruptSignal::WaitRelaxed`
(`core/runtime/interrupt_signal.cpp:157-198`) re-loads `signal_.value` at the top of
every loop iteration, spins or `mwaitx`es on it while under `kMaxElapsed`, and even the
passive `hsaKmtWaitOnEvent_Ext` path uses a bounded `wait_ms` before looping back to
re-read. A plain atomic store is therefore always observed — immediately in the common
case, never a permanent block.

The unraisable interrupt is a latency concern, not a correctness one, and only for
`HSA_WAIT_STATE_BLOCKED` waits exceeding `kMaxElapsed`. Tests set `HSA_ENABLE_INTERRUPT=0`
to get `DefaultSignal` (`core/runtime/default_signal.cpp:56` leaves `event_mailbox_ptr`
NULL, pure busy-wait) for deterministic latency.

## Error handling

- **No `/dev/udmabuf`** (root:kvm here, often absent in containers): `udmabuf_fd = -1`;
  allocations fall back to `mmap(MAP_SHARED | MAP_ANONYMOUS)`; `export_memory_handle`
  returns `HSA_STATUS_ERROR_OUT_OF_RESOURCES`. Everything else works, so dispatch and
  queue coverage is unaffected. `DYNACCEL_DISABLE_UDMABUF=1` forces this path so CI
  exercises both.
- Tests needing real dma-buf attempt the export and `GTEST_SKIP()` on
  `OUT_OF_RESOURCES` — no new capability API.
- `create_queue` rejects non-power-of-two or oversized rings with
  `HSA_STATUS_ERROR_INVALID_QUEUE_CREATION`. `destroy_queue` clears `run` and joins.
- Every ftable slot null-checks `ctx` and its out-params.
- One mutex guards the two lists. A worker touches only its own queue.

## Testing

CTest registers each suite twice, because `NoDynamicDriver` must run *without* the
driver present:

| Registration | `LD_PRELOAD` | gtest filter |
| --- | --- | --- |
| `dispatch_nodriver`, `memory_nodriver` | — | `*NoDynamicDriver*` |
| `dispatch_dynaccel`, `memory_dynaccel` | `libdynaccel.so` | `-*NoDynamicDriver*` |

The driver-backed registrations also set `HSA_ENABLE_INTERRUPT=0`.

New and strengthened coverage:

- `Dispatch.ExecuteFunction` — packet whose function sums its kernargs into a device
  buffer; wait on the completion signal; assert the result.
- `Dispatch.PacketsExecuteInOrder` — N packets appending to a buffer; assert sequence.
- `Dispatch.BarrierCompletes` — the existing barrier test's signal genuinely reaches 0,
  so `EXPECT_EQ(hsa_signal_wait(...), 0)` replaces today's comment explaining that it
  is not decremented.
- `Memory.UdmabufExport` — allocate from the device pool, export, assert `fd > 0`.
- `Memory.DMABufExportImportGPUtoDynamic` and `...DynamicToGPU` — flip from
  `EXPECT_NE(..., SUCCESS)` to real interop when a physical GPU is present.

## Build

`libdynaccel.so` is a new CMake target under `rocrtst/suites/dynamic/dynaccel/`,
compiled with `-Wall -Wextra -Wpedantic` to match the sibling test targets. It needs the
ROCr public headers (`hsa.h`, `amd_hsa_signal.h`) on its include path and links only
`pthread`.
