// Copyright 2026, Beyley Cardellio
// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Base timing source code
 *
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup base
 */

#pragma once

#include "xrt/xrt_frame.h"
#include "xrt/xrt_results.h"

#include "tracking/t_time_sync.h"

#include "os/os_threading.h"


#ifdef __cplusplus
extern "C" {
#endif

#define B_TIMING_SOURCE_MAX_SINKS 4

struct b_timing_source
{
	struct t_timing_event_source base;
	struct xrt_frame_node node;

	struct os_mutex mutex;

	bool running;

	struct t_timing_event_sink *sinks[B_TIMING_SOURCE_MAX_SINKS];
};

xrt_result_t
b_timing_source_create(struct xrt_frame_context *xfctx, struct b_timing_source **out_bts);

void
b_timing_source_push_event(struct b_timing_source *bts, const struct t_timing_event *event);

#ifdef __cplusplus
}
#endif
