// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Test stand-in for the controller-key queue declaration that lives on
 *         feat/linux-workspace-compositor (src/xrt/ipc/server/
 *         ipc_server_input_queue.h). tests_service_orchestrator_linux compiles
 *         the orchestrator with SERVICE_HAVE_CONTROLLER_KEY_QUEUE against this
 *         and records the calls. Must match the real signature.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void
ipc_server_input_queue_push_controller_key(uint32_t vk_code, uint32_t modifiers);

#ifdef __cplusplus
}
#endif
