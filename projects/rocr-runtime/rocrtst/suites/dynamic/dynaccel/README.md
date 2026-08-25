# DynAccel

DynAccel is a reference implementation of ROCr's **dynamic driver** interface
(`core/driver/dynamic/rocr_dynamic_driver.h`). It is a fake, software-only
accelerator: no real hardware, no kernel-mode driver, no ISA. It exists so
that:

- ROCr's dynamic-agent code path (agent discovery, memory, queues) can be
  exercised in CI without real accelerator hardware.
- Someone bringing up a new accelerator has a small, complete, working example
  of the interface to read and copy from.

If you are writing a driver for a real accelerator, `rocr_dynamic_driver.h` is
the actual contract you must satisfy — DynAccel is just one way to satisfy it.

## How the dynamic driver interface works

ROCr discovers a dynamic driver at topology time via
`dlsym(RTLD_DEFAULT, "rocr_dynamic_driver_create")`. That function allocates
a driver-private context and returns a `rocr_dynamic_driver_ftable_t*`: a
table of function pointers ROCr calls for everything from there on
(lifecycle, topology queries, memory, queues, ...). A driver only needs to
fill in the entries it supports — everything else can be left `NULL` (the
struct is designed to be sparsely populated; e.g. DynAccel leaves
`register_memory`, `set_queue_cu_mask`, `spm_*`, and most of the
performance/misc section unset).

In tests, the driver is loaded via `LD_PRELOAD=libdynaccel.so` rather than a
real `/dev/...` node, so `dlsym(RTLD_DEFAULT, ...)` finds
`rocr_dynamic_driver_create` in the preloaded library. See
`../CMakeLists.txt`'s `add_dynamic_test` for how each test binary is run both
with and without the driver preloaded.

## Main files

- **`dynaccel.c`** — the driver itself. Organized to mirror
  `rocr_dynamic_driver_ftable_t`'s sections:
  - *Lifecycle* (`init`/`shut_down`/`open`/`close`) — all no-ops here; a real
    driver would open the device node and set up interrupt handling.
  - *Topology* (`get_system_properties`/`get_node_properties`/
    `get_memory_properties`/`get_agent_properties`) — reports one fake node
    with one agent and two memory banks (system + device-local), all
    hardcoded.
  - *Memory* (`allocate_memory`/`free_memory`/`export_memory_handle`/
    `import_memory_handle`/`map`/`unmap`/`create_shareable_handle`/
    `destroy_memory_handle`) — backs every allocation with a `memfd` +
    `udmabuf` (falling back to plain anonymous `mmap` if `/dev/udmabuf` is
    unavailable, or if `DYNACCEL_DISABLE_UDMABUF` is set), so allocations are
    real dma-buf-shareable memory rather than a pure simulation. Only
    `ROCR_DYNAMIC_SHARE_DMABUF_FD` sharing is implemented — cross-node
    `FABRIC_HANDLE` sharing is out of scope for a single-machine driver like
    this one.
  - *Queue* (`create_queue`/`destroy_queue`) — see below.
  - *Cleanup* (`destroy_context`) — drains any queues/allocations a caller
    failed to tear down, so a leaked worker thread or fd can't outlive the
    driver.

  The queue implementation is the part most specific to DynAccel and least
  representative of a real driver: since there is no hardware to ring a
  doorbell, `create_queue` spins up a POSIX thread (`dynaccel_worker`) per
  queue that polls the doorbell/read-index pair ROCr writes to, and executes
  packets itself (see `dynaccel_execute`). A real driver instead hands the
  queue's ring buffer and doorbell to actual hardware and only needs to
  implement `create_queue`/`destroy_queue` to allocate/release the hardware
  resources — it does not need a software execution loop at all.

- **`dynaccel_packet.h`** — DynAccel's own packet format
  (`dynaccel_dispatch_packet_t`), used only by its `DYNACCEL_OPCODE_DISPATCH`
  packets to invoke an arbitrary host function pointer as a stand-in for a
  real kernel dispatch. This is **not** part of the dynamic driver interface;
  it is a convenience specific to this fake accelerator so tests can dispatch
  simple, checkable work (see `../dispatch.cc`) without a real ISA or
  compiler. A real driver has no equivalent of this file — it consumes
  standard AQL kernel-dispatch/agent-dispatch/barrier packets instead.

- **`CMakeLists.txt`** — builds `dynaccel.c` into `libdynaccel.so`.

## Related files (one level up)

- **`../rocr_dynamic_driver.h`** (in
  `runtime/hsa-runtime/core/driver/dynamic/`) — the actual interface
  contract: every ftable entry's expected semantics, argument ownership, and
  error behavior. Read this first when implementing a new driver.
- **`../memory.cc`** and **`../dispatch.cc`** — the test suites that exercise
  DynAccel through the public HSA API (agent discovery, allocate/map/unmap,
  export/import, queue create/destroy, packet submission/execution). They
  double as a reference for what a driver's memory and queue paths need to
  support, and are a reasonable template for validating a new driver the same
  way.
- **`../common.h`** — shared test helper (`discover_agents`) for finding
  agents of a given device type.
- **`../CMakeLists.txt`** — registers each test suite twice: once with no
  driver present (`*_nodriver`) and once with `libdynaccel.so` preloaded
  (`*_dynaccel`).
