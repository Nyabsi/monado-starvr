// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Working over the wired parts of a panel.
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_defines.h"

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

//! Largest push constant block @ref comp_strips_dispatch will carry.
#define COMP_STRIPS_MAX_PUSH_SIZE (128)

/*!
 * The strips of a panel, ready to work over. Built once from whatever
 * @ref xrt_hmd_parts said, which lets a compositor treat "this panel is one
 * rectangle" and "this panel is four column ranges with holes between them" as
 * the same thing.
 *
 * @ingroup comp_util
 */
struct comp_strips
{
	uint32_t count;
	struct xrt_panel_strip strips[XRT_MAX_PANEL_STRIPS];

	//! Rows to cover, which every strip shares.
	uint32_t height;
};

/*!
 * Take the strips a device declared, or make up the single strip that covers a
 * whole panel if it declared none. Returns false if the device said something
 * about its panel that cannot be true.
 *
 * @ingroup comp_util
 */
bool
comp_strips_init(struct comp_strips *cs,
                 const struct xrt_panel_strip *strips,
                 uint32_t strip_count,
                 uint32_t width,
                 uint32_t height);

/*!
 * How many panel columns the strips actually cover, for logging and for working
 * out what the holes are costing.
 *
 * @ingroup comp_util
 */
uint32_t
comp_strips_covered_columns(const struct comp_strips *cs);

/*!
 * The parts of the strips that fall inside @p offset to @p offset + @p width,
 * still in panel columns, for a panel scanned out by more than one display.
 *
 * @return How many of @p out_strips were filled in.
 *
 * @ingroup comp_util
 */
uint32_t
comp_strips_clip(const struct comp_strips *cs,
                 uint32_t offset,
                 uint32_t width,
                 struct xrt_panel_strip out_strips[XRT_MAX_PANEL_STRIPS]);

/*!
 * Dispatch a compute shader once per strip.
 *
 * The shader is told which column its strip starts at by having that written
 * into its push constants: @p push is copied for each strip and the offset
 * patched in at @p offset_of_column, so the shader can turn a local invocation
 * into a panel coordinate. Everything else in @p push is left alone.
 *
 * @ingroup comp_util
 */
void
comp_strips_dispatch(struct comp_strips *cs,
                     struct vk_bundle *vk,
                     VkCommandBuffer cmd,
                     VkPipelineLayout layout,
                     uint32_t group_size,
                     const void *push,
                     size_t push_size,
                     size_t offset_of_column);

#ifdef __cplusplus
}
#endif
