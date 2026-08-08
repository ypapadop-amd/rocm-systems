/*
 * Copyright © Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ROCRTST_SUITES_DYNAMIC_COMMON_H_
#define ROCRTST_SUITES_DYNAMIC_COMMON_H_

#include <vector>

#include "hsa/hsa.h"

namespace rocrtst {

/// @brief hsa_iterate_agents callback collecting every agent of @p DeviceType
/// into the std::vector<hsa_agent_t> pointed to by @p data.
template <hsa_device_type_t DeviceType>
hsa_status_t discover_agents(hsa_agent_t agent, void* data) {
  if (!data) {
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
  }

  hsa_device_type_t device_type = {};
  const auto status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &device_type);
  if (status != HSA_STATUS_SUCCESS) {
    return status;
  }

  if (device_type == DeviceType) {
    auto* const agents = static_cast<std::vector<hsa_agent_t>*>(data);
    agents->push_back(agent);
  }

  return HSA_STATUS_SUCCESS;
}

}  // namespace rocrtst

#endif  // ROCRTST_SUITES_DYNAMIC_COMMON_H_
