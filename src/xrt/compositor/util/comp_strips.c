// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Working over the wired parts of a panel.
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup comp_util
 */

#include "comp_strips.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <assert.h>
#include <string.h>

bool
comp_strips_init(struct comp_strips *cs,
                 const struct xrt_panel_strip *strips,
                 uint32_t strip_count,
                 uint32_t width,
                 uint32_t height)
{
	U_ZERO(cs);

	if (height == 0) {
		U_LOG_E("Panel has no height");
		return false;
	}

	cs->height = height;

	if (strips == NULL || strip_count == 0) {
		if (width == 0) {
			U_LOG_E("Panel has no width and no strips either");
			return false;
		}

		cs->count = 1;
		cs->strips[0].offset = 0;
		cs->strips[0].width = width;

		return true;
	}

	if (strip_count > XRT_MAX_PANEL_STRIPS) {
		U_LOG_E("Panel claims %u strips, only %u fit", strip_count, XRT_MAX_PANEL_STRIPS);
		return false;
	}

	for (uint32_t i = 0; i < strip_count; i++) {
		if (strips[i].width == 0) {
			U_LOG_E("Panel strip %u is empty", i);
			return false;
		}

		cs->strips[i] = strips[i];
	}

	cs->count = strip_count;

	return true;
}

uint32_t
comp_strips_covered_columns(const struct comp_strips *cs)
{
	uint32_t total = 0;

	for (uint32_t i = 0; i < cs->count; i++) {
		total += cs->strips[i].width;
	}

	return total;
}

uint32_t
comp_strips_clip(const struct comp_strips *cs,
                 uint32_t offset,
                 uint32_t width,
                 struct xrt_panel_strip out_strips[XRT_MAX_PANEL_STRIPS])
{
	const uint32_t end = offset + width;
	uint32_t count = 0;

	for (uint32_t i = 0; i < cs->count; i++) {
		const uint32_t strip_start = cs->strips[i].offset;
		const uint32_t strip_end = strip_start + cs->strips[i].width;

		const uint32_t start = strip_start > offset ? strip_start : offset;
		const uint32_t stop = strip_end < end ? strip_end : end;

		if (start >= stop) {
			continue;
		}

		out_strips[count].offset = start;
		out_strips[count].width = stop - start;
		count++;
	}

	return count;
}

void
comp_strips_dispatch(struct comp_strips *cs,
                     struct vk_bundle *vk,
                     VkCommandBuffer cmd,
                     VkPipelineLayout layout,
                     uint32_t group_size,
                     const void *push,
                     size_t push_size,
                     size_t offset_of_column)
{
	assert(group_size > 0);
	assert(push_size <= COMP_STRIPS_MAX_PUSH_SIZE);
	assert(offset_of_column + sizeof(int32_t) <= push_size);

	const uint32_t group_y = (cs->height + group_size - 1) / group_size;

	uint8_t scratch[COMP_STRIPS_MAX_PUSH_SIZE];
	memcpy(scratch, push, push_size);

	for (uint32_t i = 0; i < cs->count; i++) {
		const struct xrt_panel_strip *strip = &cs->strips[i];

		// Tell the shader where its strip starts on the panel.
		const int32_t offset = (int32_t)strip->offset;
		memcpy(scratch + offset_of_column, &offset, sizeof(offset));

		vk->vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)push_size, scratch);

		const uint32_t group_x = (strip->width + group_size - 1) / group_size;

		vk->vkCmdDispatch(cmd, group_x, group_y, 1);
	}
}
