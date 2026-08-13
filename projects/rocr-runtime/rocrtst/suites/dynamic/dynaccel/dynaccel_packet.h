/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYNACCEL_PACKET_H_
#define DYNACCEL_PACKET_H_

#include <assert.h>
#include <stdint.h>

#include "inc/hsa.h"

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
