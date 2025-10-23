#pragma once

#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct leiasr;

xrt_result_t leiasr_create(struct leiasr** out);
void leiasr_destroy(struct leiasr* leiasr);

#ifdef __cplusplus
}
#endif
