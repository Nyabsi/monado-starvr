// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for panel strip bookkeeping.
 * @author Nyabsi <nyabsi@sovellus.cc>
 */

#include "util/comp_strips.h"

#include "catch_amalgamated.hpp"

//! The StarVR One's retail panel, which is where this came from.
static const struct xrt_panel_strip wired[8] = {
    {60, 300}, {600, 298}, {1020, 300}, {1560, 297}, {1980, 300}, {2520, 298}, {2940, 300}, {3480, 297},
};

TEST_CASE("comp_strips")
{
	struct comp_strips cs = {};

	SECTION("a panel with no strips becomes one strip covering all of it")
	{
		CHECK(comp_strips_init(&cs, NULL, 0, 3840, 2240));

		CHECK(cs.count == 1);
		CHECK(cs.strips[0].offset == 0);
		CHECK(cs.strips[0].width == 3840);
		CHECK(cs.height == 2240);
		CHECK(comp_strips_covered_columns(&cs) == 3840);
	}

	SECTION("an empty strip list is the same as no strip list")
	{
		const struct xrt_panel_strip none[1] = {};

		CHECK(comp_strips_init(&cs, none, 0, 1920, 1080));

		CHECK(cs.count == 1);
		CHECK(cs.strips[0].width == 1920);
	}

	SECTION("declared strips are kept as they were given")
	{
		CHECK(comp_strips_init(&cs, wired, 8, 3840, 2240));

		CHECK(cs.count == 8);
		CHECK(cs.height == 2240);

		for (uint32_t i = 0; i < 8; i++) {
			CHECK(cs.strips[i].offset == wired[i].offset);
			CHECK(cs.strips[i].width == wired[i].width);
		}

		// Well under the panel width, which is the whole point.
		CHECK(comp_strips_covered_columns(&cs) == 2390);
	}

	SECTION("a panel that says nothing usable about itself is refused")
	{
		// No height.
		CHECK_FALSE(comp_strips_init(&cs, NULL, 0, 3840, 0));

		// No width and no strips to make one from.
		CHECK_FALSE(comp_strips_init(&cs, NULL, 0, 0, 2240));

		// More strips than there is room for.
		const struct xrt_panel_strip many[1] = {{0, 100}};
		CHECK_FALSE(comp_strips_init(&cs, many, XRT_MAX_PANEL_STRIPS + 1, 3840, 2240));

		// A strip covering nothing, which would dispatch zero groups.
		const struct xrt_panel_strip empty[2] = {{0, 100}, {200, 0}};
		CHECK_FALSE(comp_strips_init(&cs, empty, 2, 3840, 2240));
	}

	SECTION("a refused panel leaves nothing behind to work over")
	{
		CHECK_FALSE(comp_strips_init(&cs, NULL, 0, 0, 0));

		CHECK(cs.count == 0);
		CHECK(comp_strips_covered_columns(&cs) == 0);
	}
}

TEST_CASE("comp_strips_clip")
{
	struct comp_strips cs = {};
	struct xrt_panel_strip clipped[XRT_MAX_PANEL_STRIPS] = {};

	SECTION("each half of a two display panel gets its own strips")
	{
		REQUIRE(comp_strips_init(&cs, wired, 8, 3840, 2240));

		const uint32_t left = comp_strips_clip(&cs, 0, 1920, clipped);
		CHECK(left == 4);
		CHECK(clipped[0].offset == 60);
		CHECK(clipped[3].offset == 1560);

		const uint32_t right = comp_strips_clip(&cs, 1920, 1920, clipped);
		CHECK(right == 4);
		CHECK(clipped[0].offset == 1980);
		CHECK(clipped[3].offset == 3480);
		CHECK(clipped[3].width == 297);
	}

	SECTION("a strip crossing the seam is cut in two")
	{
		const struct xrt_panel_strip crossing[1] = {{1900, 100}};
		REQUIRE(comp_strips_init(&cs, crossing, 1, 3840, 2240));

		CHECK(comp_strips_clip(&cs, 0, 1920, clipped) == 1);
		CHECK(clipped[0].offset == 1900);
		CHECK(clipped[0].width == 20);

		CHECK(comp_strips_clip(&cs, 1920, 1920, clipped) == 1);
		CHECK(clipped[0].offset == 1920);
		CHECK(clipped[0].width == 80);
	}

	SECTION("a display with none of the wired columns gets nothing")
	{
		const struct xrt_panel_strip only_left[1] = {{0, 100}};
		REQUIRE(comp_strips_init(&cs, only_left, 1, 3840, 2240));

		CHECK(comp_strips_clip(&cs, 1920, 1920, clipped) == 0);
	}

	SECTION("no strips means the whole display is covered")
	{
		REQUIRE(comp_strips_init(&cs, NULL, 0, 3840, 2240));

		CHECK(comp_strips_clip(&cs, 1920, 1920, clipped) == 1);
		CHECK(clipped[0].offset == 1920);
		CHECK(clipped[0].width == 1920);
	}
}
