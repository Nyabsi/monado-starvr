// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Direct mode target for the StarVR One's two panels.
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup comp_main
 */

#include "util/u_misc.h"
#include "util/u_pretty_print.h"

#include "util/comp_strips.h"

#include "vk/vk_cmd.h"
#include "vk/vk_cmd_pool.h"

#include "main/comp_window_direct.h"
#include "main/comp_window.h"

#ifdef XRT_HAVE_WAYLAND_DIRECT
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xf86drm.h>

#include "drm-lease-v1-client-protocol.h"
#endif

#include <stdlib.h>
#include <string.h>

/*!
 * The panel is scanned out by two DisplayPort links, one per eye, and the EDID
 * of each says so. Nothing on Linux joins them back together, so this takes both
 * and treats them as the halves of one panel that they are.
 */
#define STARVR_PANEL_NAME "ACER_DK3"
#define STARVR_PANEL_COUNT 2

static const char *const starvr_wanted[STARVR_PANEL_COUNT] = {
    STARVR_PANEL_NAME "_L",
    STARVR_PANEL_NAME "_R",
};

struct starvr_panel_info
{
	VkDisplayKHR display;
	VkExtent2D extent;
	char name[128];
};


/*
 *
 * Leasing the panels from a Wayland compositor.
 *
 */

#ifdef XRT_HAVE_WAYLAND_DIRECT

#define STARVR_MAX_OFFERED 32
#define STARVR_MAX_DISPATCH 64

struct starvr_lease_connector
{
	struct wp_drm_lease_connector_v1 *handle;

	uint32_t id;
	char name[64];
	char description[128];
	bool withdrawn;
};

struct starvr_lease
{
	struct comp_compositor *c;

	struct wl_display *display;

	struct wp_drm_lease_device_v1 *device;
	int drm_fd;
	char *path;
	bool device_done;

	struct starvr_lease_connector offered[STARVR_MAX_OFFERED];
	uint32_t offered_count;

	struct wp_drm_lease_v1 *lease;
	int leased_fd;
	bool finished;

	struct starvr_lease_connector taken[STARVR_PANEL_COUNT];
	uint32_t taken_count;
};

static void
connector_name(void *data, struct wp_drm_lease_connector_v1 *handle, const char *name)
{
	struct starvr_lease_connector *conn = data;
	snprintf(conn->name, sizeof(conn->name), "%s", name);
}

static void
connector_description(void *data, struct wp_drm_lease_connector_v1 *handle, const char *description)
{
	struct starvr_lease_connector *conn = data;
	snprintf(conn->description, sizeof(conn->description), "%s", description);
}

static void
connector_id(void *data, struct wp_drm_lease_connector_v1 *handle, uint32_t id)
{
	struct starvr_lease_connector *conn = data;
	conn->id = id;
}

static void
connector_done(void *data, struct wp_drm_lease_connector_v1 *handle)
{}

static void
connector_withdrawn(void *data, struct wp_drm_lease_connector_v1 *handle)
{
	struct starvr_lease_connector *conn = data;
	conn->withdrawn = true;
}

static const struct wp_drm_lease_connector_v1_listener connector_listener = {
    .name = connector_name,
    .description = connector_description,
    .connector_id = connector_id,
    .done = connector_done,
    .withdrawn = connector_withdrawn,
};

static void
device_drm_fd(void *data, struct wp_drm_lease_device_v1 *handle, int fd)
{
	struct starvr_lease *lease = data;

	if (lease->drm_fd >= 0) {
		close(fd);
		return;
	}

	lease->drm_fd = fd;
	lease->path = drmGetDeviceNameFromFd2(fd);
}

static void
device_connector(void *data, struct wp_drm_lease_device_v1 *handle, struct wp_drm_lease_connector_v1 *connector)
{
	struct starvr_lease *lease = data;

	if (lease->offered_count >= STARVR_MAX_OFFERED) {
		wp_drm_lease_connector_v1_destroy(connector);
		return;
	}

	struct starvr_lease_connector *conn = &lease->offered[lease->offered_count++];
	conn->handle = connector;

	wp_drm_lease_connector_v1_add_listener(connector, &connector_listener, conn);
}

static void
device_done(void *data, struct wp_drm_lease_device_v1 *handle)
{
	struct starvr_lease *lease = data;
	lease->device_done = true;
}

static void
device_released(void *data, struct wp_drm_lease_device_v1 *handle)
{
	struct starvr_lease *lease = data;
	lease->finished = true;
}

static const struct wp_drm_lease_device_v1_listener device_listener = {
    .drm_fd = device_drm_fd,
    .connector = device_connector,
    .done = device_done,
    .released = device_released,
};

static void
lease_fd(void *data, struct wp_drm_lease_v1 *handle, int32_t fd)
{
	struct starvr_lease *lease = data;
	lease->leased_fd = fd;
}

static void
lease_finished(void *data, struct wp_drm_lease_v1 *handle)
{
	struct starvr_lease *lease = data;

	if (lease->leased_fd >= 0) {
		close(lease->leased_fd);
		lease->leased_fd = -1;
	}

	lease->finished = true;
}

static const struct wp_drm_lease_v1_listener lease_listener = {
    .lease_fd = lease_fd,
    .finished = lease_finished,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct starvr_lease *lease = data;

	if (strcmp(interface, wp_drm_lease_device_v1_interface.name) != 0 || lease->device != NULL) {
		return;
	}

	lease->device = wl_registry_bind(registry, name, &wp_drm_lease_device_v1_interface, 1);
	wp_drm_lease_device_v1_add_listener(lease->device, &device_listener, lease);
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void
lease_destroy(struct starvr_lease **lease_ptr)
{
	struct starvr_lease *lease = *lease_ptr;
	if (lease == NULL) {
		return;
	}

	for (uint32_t i = 0; i < lease->offered_count; i++) {
		if (lease->offered[i].handle != NULL) {
			wp_drm_lease_connector_v1_destroy(lease->offered[i].handle);
		}
	}

	if (lease->lease != NULL) {
		wp_drm_lease_v1_destroy(lease->lease);
	}

	if (lease->leased_fd >= 0) {
		close(lease->leased_fd);
	}

	if (lease->device != NULL) {
		wp_drm_lease_device_v1_destroy(lease->device);
	}

	if (lease->drm_fd >= 0) {
		close(lease->drm_fd);
	}

	free(lease->path);

	if (lease->display != NULL) {
		wl_display_disconnect(lease->display);
	}

	free(lease);
	*lease_ptr = NULL;
}

/*!
 * Connect and let the compositor list what it is willing to lease, which it only
 * does for panels the kernel calls non-desktop.
 */
static struct starvr_lease *
lease_collect(struct comp_compositor *c)
{
	struct starvr_lease *lease = U_TYPED_CALLOC(struct starvr_lease);
	if (lease == NULL) {
		return NULL;
	}

	lease->c = c;
	lease->drm_fd = -1;
	lease->leased_fd = -1;

	lease->display = wl_display_connect(NULL);
	if (lease->display == NULL) {
		free(lease);
		return NULL;
	}

	struct wl_registry *registry = wl_display_get_registry(lease->display);
	wl_registry_add_listener(registry, &registry_listener, lease);
	wl_display_roundtrip(lease->display);
	wl_registry_destroy(registry);

	if (lease->device == NULL) {
		COMP_INFO(c, "Compositor does not do drm-lease");
		lease_destroy(&lease);
		return NULL;
	}

	wl_display_roundtrip(lease->display);

	for (int i = 0; i < STARVR_MAX_DISPATCH && !lease->device_done; i++) {
		if (wl_display_dispatch(lease->display) < 0) {
			break;
		}
	}

	return lease;
}

static uint32_t
lease_find_panels(struct starvr_lease *lease, struct starvr_lease_connector *out_conns[STARVR_PANEL_COUNT])
{
	uint32_t found = 0;

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		for (uint32_t j = 0; j < lease->offered_count; j++) {
			struct starvr_lease_connector *conn = &lease->offered[j];

			if (conn->withdrawn || (strstr(conn->description, starvr_wanted[i]) == NULL &&
			                        strstr(conn->name, starvr_wanted[i]) == NULL)) {
				continue;
			}

			out_conns[found++] = conn;
			break;
		}
	}

	return found;
}

static void
lease_log_offer(struct starvr_lease *lease)
{
	struct u_pp_sink_stack_only sink;
	u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);

	u_pp(dg, "Compositor offers %u connector(s) for lease:", lease->offered_count);
	for (uint32_t i = 0; i < lease->offered_count; i++) {
		u_pp(dg, "\n\t%s (%s) id %u", lease->offered[i].name, lease->offered[i].description,
		     lease->offered[i].id);
	}
	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		u_pp(dg, "\n\twanted: %s", starvr_wanted[i]);
	}

	COMP_INFO(lease->c, "%s", sink.buffer);
}

//! Answers whether both panels are on offer without taking anything.
static bool
lease_probe(struct comp_compositor *c)
{
	struct starvr_lease *lease = lease_collect(c);
	if (lease == NULL) {
		return false;
	}

	struct starvr_lease_connector *conns[STARVR_PANEL_COUNT] = {0};
	const uint32_t found = lease_find_panels(lease, conns);

	if (found != STARVR_PANEL_COUNT) {
		lease_log_offer(lease);
	}

	lease_destroy(&lease);

	return found == STARVR_PANEL_COUNT;
}

/*!
 * Take one lease covering both panels, so they are handed over together or not
 * at all and one fd is master over the pair.
 */
static struct starvr_lease *
lease_take(struct comp_compositor *c)
{
	struct starvr_lease *lease = lease_collect(c);
	if (lease == NULL) {
		return NULL;
	}

	struct starvr_lease_connector *conns[STARVR_PANEL_COUNT] = {0};
	if (lease_find_panels(lease, conns) != STARVR_PANEL_COUNT) {
		lease_log_offer(lease);
		lease_destroy(&lease);
		return NULL;
	}

	struct wp_drm_lease_request_v1 *request = wp_drm_lease_device_v1_create_lease_request(lease->device);
	if (request == NULL) {
		COMP_ERROR(c, "Failed to create lease request");
		lease_destroy(&lease);
		return NULL;
	}

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		wp_drm_lease_request_v1_request_connector(request, conns[i]->handle);
		lease->taken[i] = *conns[i];
	}
	lease->taken_count = STARVR_PANEL_COUNT;

	lease->lease = wp_drm_lease_request_v1_submit(request);
	wp_drm_lease_v1_add_listener(lease->lease, &lease_listener, lease);

	while (lease->leased_fd < 0 && !lease->finished) {
		if (wl_display_dispatch(lease->display) < 0) {
			break;
		}
	}

	if (lease->leased_fd < 0) {
		COMP_ERROR(c, "Compositor refused the lease");
		lease_destroy(&lease);
		return NULL;
	}

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		COMP_INFO(c, "Leased %s (%s) id %u on %s", lease->taken[i].name, lease->taken[i].description,
		          lease->taken[i].id, lease->path);
	}

	return lease;
}

//! Service the connection, and notice if the compositor took the lease back.
static void
lease_pump(struct starvr_lease *lease)
{
	if (lease == NULL) {
		return;
	}

	while (wl_display_prepare_read(lease->display) != 0) {
		wl_display_dispatch_pending(lease->display);
	}

	if (wl_display_flush(lease->display) < 0 && errno != EAGAIN) {
		wl_display_cancel_read(lease->display);
		return;
	}

	struct pollfd fds[] = {{.fd = wl_display_get_fd(lease->display), .events = POLLIN}};

	if (poll(fds, 1, 0) > 0) {
		wl_display_read_events(lease->display);
		wl_display_dispatch_pending(lease->display);
	} else {
		wl_display_cancel_read(lease->display);
	}
}

#endif // XRT_HAVE_WAYLAND_DIRECT


/*
 *
 * One panel, a swapchain on a display that has already been chosen.
 *
 */

struct starvr_panel_target
{
	struct comp_target_swapchain base;

	VkDisplayKHR display;
	VkExtent2D extent;
	void *xlib_dpy;
	char name[128];
};

static bool
panel_init_pre_vulkan(struct comp_target *ct)
{
	return true;
}

static bool
panel_init_post_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	struct starvr_panel_target *panel = (struct starvr_panel_target *)ct;

#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
	if (panel->xlib_dpy != NULL) {
		if (!comp_window_direct_init_swapchain(&panel->base, (Display *)panel->xlib_dpy, panel->display,
		                                       panel->extent.width, panel->extent.height)) {
			COMP_ERROR(ct->c, "Failed to take over '%s'", panel->name);
			return false;
		}
	} else
#endif
	{
		VkResult ret = comp_window_direct_create_surface(&panel->base, panel->display, panel->extent.width,
		                                                panel->extent.height);
		if (ret != VK_SUCCESS) {
			COMP_ERROR(ct->c, "Failed to create surface for '%s': %s", panel->name,
			           vk_result_string(ret));
			return false;
		}
	}

	ct->width = panel->extent.width;
	ct->height = panel->extent.height;

	return true;
}

static void
panel_flush(struct comp_target *ct)
{}

static void
panel_set_title(struct comp_target *ct, const char *title)
{}

static void
panel_destroy(struct comp_target *ct)
{
	struct starvr_panel_target *panel = (struct starvr_panel_target *)ct;

	comp_target_swapchain_cleanup(&panel->base);

	free(panel);
}

static struct comp_target *
panel_create(struct comp_compositor *c, const struct starvr_panel_info *info, void *xlib_dpy)
{
	struct starvr_panel_target *panel = U_TYPED_CALLOC(struct starvr_panel_target);
	if (panel == NULL) {
		return NULL;
	}

	comp_target_swapchain_init_and_set_fnptrs(&panel->base, COMP_TARGET_USE_DISPLAY_IF_AVAILABLE);

	snprintf(panel->name, sizeof(panel->name), "%s", info->name);

	panel->display = info->display;
	panel->extent = info->extent;
	panel->xlib_dpy = xlib_dpy;

	panel->base.display = info->display;
	panel->base.base.c = c;
	panel->base.base.name = panel->name;
	panel->base.base.init_pre_vulkan = panel_init_pre_vulkan;
	panel->base.base.init_post_vulkan = panel_init_post_vulkan;
	panel->base.base.flush = panel_flush;
	panel->base.base.set_title = panel_set_title;
	panel->base.base.destroy = panel_destroy;

	// Logs through the compositor and the name, so both have to be set first.
	comp_target_swapchain_override_extents(&panel->base, info->extent);

	return &panel->base.base;
}


/*
 *
 * The pair of panels, presented as one target.
 *
 */

struct starvr_image
{
	VkImage handle;
	VkDeviceMemory memory;
	VkImageView view;
};

struct starvr_target
{
	struct comp_target base;

	struct
	{
		struct comp_target *ct;
		VkOffset2D offset;
		uint32_t image_index;
	} panels[STARVR_PANEL_COUNT];

	struct starvr_image *own_images;
	uint32_t next_index;

	//! Which panel columns are wired up, straight from the device.
	struct comp_strips strips;

	struct vk_cmd_pool pool;
	VkCommandBuffer cmd;

#ifdef XRT_HAVE_WAYLAND_DIRECT
	struct starvr_lease *lease;
#endif
};

static inline struct starvr_target *
starvr_target(struct comp_target *ct)
{
	return (struct starvr_target *)ct;
}

static inline struct vk_bundle *
get_vk(struct starvr_target *st)
{
	return &st->base.c->base.vk;
}

static const VkImageSubresourceRange color_range = {
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .levelCount = 1,
    .layerCount = 1,
};

static void
destroy_own_images(struct starvr_target *st)
{
	struct vk_bundle *vk = get_vk(st);

	if (st->own_images == NULL) {
		return;
	}

	for (uint32_t i = 0; i < st->base.image_count; i++) {
		if (st->own_images[i].view != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, st->own_images[i].view, NULL);
		}
		if (st->own_images[i].handle != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, st->own_images[i].handle, NULL);
		}
		if (st->own_images[i].memory != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, st->own_images[i].memory, NULL);
		}
	}

	free(st->own_images);
	st->own_images = NULL;

	free(st->base.images);
	st->base.images = NULL;
	st->base.image_count = 0;
}

static bool
create_own_images(struct starvr_target *st, uint32_t image_count, VkImageUsageFlags usage)
{
	struct vk_bundle *vk = get_vk(st);
	VkResult ret;

	st->own_images = U_TYPED_ARRAY_CALLOC(struct starvr_image, image_count);
	st->base.images = U_TYPED_ARRAY_CALLOC(struct comp_target_image, image_count);
	if (st->own_images == NULL || st->base.images == NULL) {
		return false;
	}

	st->base.image_count = image_count;

	const VkExtent2D extent = {st->base.width, st->base.height};

	for (uint32_t i = 0; i < image_count; i++) {
		struct starvr_image *si = &st->own_images[i];

		ret = vk_create_image_simple(vk, extent, st->base.format, usage, &si->memory, &si->handle);
		if (ret != VK_SUCCESS) {
			COMP_ERROR(st->base.c, "vk_create_image_simple: %s", vk_result_string(ret));
			return false;
		}

		ret = vk_create_view(vk, si->handle, VK_IMAGE_VIEW_TYPE_2D, st->base.format, color_range, &si->view);
		if (ret != VK_SUCCESS) {
			COMP_ERROR(st->base.c, "vk_create_view: %s", vk_result_string(ret));
			return false;
		}

		VK_NAME_IMAGE(vk, si->handle, "starvr panel image");
		VK_NAME_IMAGE_VIEW(vk, si->view, "starvr panel image view");

		st->base.images[i].handle = si->handle;
		st->base.images[i].view = si->view;
	}

	return true;
}

static void
destroy_semaphore(struct starvr_target *st)
{
	struct vk_bundle *vk = get_vk(st);

	if (st->base.semaphores.render_complete != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, st->base.semaphores.render_complete, NULL);
		st->base.semaphores.render_complete = VK_NULL_HANDLE;
	}
}

static bool
create_semaphore(struct starvr_target *st)
{
	struct vk_bundle *vk = get_vk(st);

	VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

	// Nothing hands us our images, so there is no present to wait on.
	st->base.semaphores.present_complete = VK_NULL_HANDLE;
	st->base.semaphores.render_complete_is_timeline = false;

	VkResult ret = vk->vkCreateSemaphore(vk->device, &info, NULL, &st->base.semaphores.render_complete);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(st->base.c, "vkCreateSemaphore: %s", vk_result_string(ret));
		return false;
	}

	VK_NAME_SEMAPHORE(vk, st->base.semaphores.render_complete, "starvr render complete");

	return true;
}

static bool
starvr_target_init_pre_vulkan(struct comp_target *ct)
{
	struct starvr_target *st = starvr_target(ct);

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		if (!comp_target_init_pre_vulkan(st->panels[i].ct)) {
			return false;
		}
	}

	return true;
}

static bool
starvr_target_init_post_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	struct starvr_target *st = starvr_target(ct);

	uint32_t width = 0;
	uint32_t height = 0;

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		struct comp_target *panel = st->panels[i].ct;

		if (!comp_target_init_post_vulkan(panel, preferred_width, preferred_height)) {
			return false;
		}

		const uint32_t right = st->panels[i].offset.x + panel->width;
		const uint32_t bottom = st->panels[i].offset.y + panel->height;

		width = right > width ? right : width;
		height = bottom > height ? bottom : height;
	}

	ct->width = width;
	ct->height = height;
	ct->wait_for_present_supported = st->panels[0].ct->wait_for_present_supported;

	const struct xrt_hmd_parts *hmd = ct->c->xdev->hmd;

	if (!comp_strips_init(&st->strips, hmd->screens[0].strips, hmd->screens[0].strip_count, width, height)) {
		COMP_ERROR(ct->c, "Device gave an unusable panel description");
		return false;
	}

	const uint32_t covered = comp_strips_covered_columns(&st->strips);

	COMP_INFO(ct->c, "Panel is %ux%u over %u displays, %u of %u columns wired in %u strip(s)", width, height,
	          STARVR_PANEL_COUNT, covered, width, st->strips.count);

	return true;
}

static bool
starvr_target_check_ready(struct comp_target *ct)
{
	struct starvr_target *st = starvr_target(ct);

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		if (!comp_target_check_ready(st->panels[i].ct)) {
			return false;
		}
	}

	return true;
}

static bool
starvr_target_is_shared_presentable_image(struct comp_target *ct)
{
	return false;
}

static bool
starvr_target_has_images(struct comp_target *ct)
{
	struct starvr_target *st = starvr_target(ct);

	if (st->own_images == NULL) {
		return false;
	}

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		if (!comp_target_has_images(st->panels[i].ct)) {
			return false;
		}
	}

	return true;
}

static void
starvr_target_create_images(struct comp_target *ct,
                            const struct comp_target_create_images_info *create_info,
                            struct vk_bundle_queue *present_queue)
{
	struct starvr_target *st = starvr_target(ct);

	destroy_own_images(st);
	destroy_semaphore(st);

	struct comp_target_create_images_info panel_info = *create_info;
	panel_info.image_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	uint32_t image_count = 0;

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		struct comp_target *panel = st->panels[i].ct;

		panel_info.extent.width = panel->width;
		panel_info.extent.height = panel->height;

		comp_target_create_images(panel, &panel_info, present_queue);

		if (!comp_target_has_images(panel)) {
			COMP_ERROR(ct->c, "Panel %u has no images", i);
			return;
		}

		if (panel->image_count > image_count) {
			image_count = panel->image_count;
		}
	}

	// A format the panels can be written from, which the renderpass then uses.
	ct->format = st->panels[0].ct->format;
	ct->final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	ct->present_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
	ct->surface_transform = st->panels[0].ct->surface_transform;

	const VkImageUsageFlags usage = create_info->image_usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	if (!create_own_images(st, image_count, usage) || !create_semaphore(st)) {
		destroy_own_images(st);
		return;
	}

	st->next_index = 0;
}

static VkResult
starvr_target_acquire(struct comp_target *ct, uint32_t *out_index)
{
	struct starvr_target *st = starvr_target(ct);

	if (!starvr_target_has_images(ct)) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	*out_index = st->next_index;
	st->next_index = (st->next_index + 1) % ct->image_count;

	return VK_SUCCESS;
}

/*!
 * Put one panel's half of the rendered image into that panel's own image. The
 * panel passes go here, this is where the whole panel is available and each
 * display's image is about to be presented.
 *
 * The whole half is written, not just the wired strips: what the distortion
 * mesh samples is not the same set of columns, and anything left unwritten
 * shows up in the view.
 */
static void
write_panel(struct starvr_target *st, uint32_t index, VkImage src)
{
	struct vk_bundle *vk = get_vk(st);
	struct comp_target *panel = st->panels[index].ct;

	VkImage dst = panel->images[st->panels[index].image_index].handle;
	const VkOffset2D offset = st->panels[index].offset;

	VkImageCopy copy = {
	    .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
	    .srcOffset = {offset.x, offset.y, 0},
	    .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
	    .extent = {panel->width, panel->height, 1},
	};

	vk_cmd_image_barrier_locked(              //
	    vk,                                   //
	    st->cmd,                              //
	    dst,                                  //
	    0,                                    // srcAccessMask
	    VK_ACCESS_TRANSFER_WRITE_BIT,         // dstAccessMask
	    VK_IMAGE_LAYOUT_UNDEFINED,            // oldImageLayout
	    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // newImageLayout
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,    // srcStageMask
	    VK_PIPELINE_STAGE_TRANSFER_BIT,       // dstStageMask
	    color_range);

	vk->vkCmdCopyImage(                       //
	    st->cmd,                              //
	    src,                                  //
	    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, //
	    dst,                                  //
	    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, //
	    1,                                    //
	    &copy);

	vk_cmd_image_barrier_locked(              //
	    vk,                                   //
	    st->cmd,                              //
	    dst,                                  //
	    VK_ACCESS_TRANSFER_WRITE_BIT,         // srcAccessMask
	    0,                                    // dstAccessMask
	    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // oldImageLayout
	    panel->final_layout,                  // newImageLayout
	    VK_PIPELINE_STAGE_TRANSFER_BIT,       // srcStageMask
	    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dstStageMask
	    color_range);
}

static VkResult
starvr_target_present(struct comp_target *ct,
                      struct vk_bundle_queue *present_queue,
                      uint32_t index,
                      uint64_t timeline_semaphore_value,
                      int64_t desired_present_time_ns,
                      int64_t present_slop_ns)
{
	struct starvr_target *st = starvr_target(ct);
	struct vk_bundle *vk = get_vk(st);
	VkResult ret;

	VkSemaphore wait_sems[1 + STARVR_PANEL_COUNT] = {ct->semaphores.render_complete};
	VkPipelineStageFlags wait_stages[1 + STARVR_PANEL_COUNT] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
	uint32_t wait_count = 1;

	VkSemaphore signal_sems[STARVR_PANEL_COUNT] = {0};
	uint32_t signal_count = 0;

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		struct comp_target *panel = st->panels[i].ct;

		ret = comp_target_acquire(panel, &st->panels[i].image_index);
		if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
			return ret;
		}

		if (panel->semaphores.present_complete != VK_NULL_HANDLE) {
			wait_stages[wait_count] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			wait_sems[wait_count++] = panel->semaphores.present_complete;
		}

		signal_sems[signal_count++] = panel->semaphores.render_complete;
	}

	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	vk_cmd_pool_lock(&st->pool);

	ret = vk->vkBeginCommandBuffer(st->cmd, &begin_info);
	if (ret != VK_SUCCESS) {
		vk_cmd_pool_unlock(&st->pool);
		COMP_ERROR(ct->c, "vkBeginCommandBuffer: %s", vk_result_string(ret));
		return ret;
	}

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		write_panel(st, i, st->own_images[index].handle);
	}

	ret = vk->vkEndCommandBuffer(st->cmd);
	if (ret != VK_SUCCESS) {
		vk_cmd_pool_unlock(&st->pool);
		COMP_ERROR(ct->c, "vkEndCommandBuffer: %s", vk_result_string(ret));
		return ret;
	}

	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = wait_count,
	    .pWaitSemaphores = wait_sems,
	    .pWaitDstStageMask = wait_stages,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &st->cmd,
	    .signalSemaphoreCount = signal_count,
	    .pSignalSemaphores = signal_sems,
	};

	ret = vk_cmd_submit_locked(vk, present_queue, 1, &submit, VK_NULL_HANDLE);

	vk_cmd_pool_unlock(&st->pool);

	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "vk_cmd_submit_locked: %s", vk_result_string(ret));
		return ret;
	}

	VkResult first = VK_SUCCESS;

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		ret = comp_target_present(        //
		    st->panels[i].ct,             //
		    present_queue,                //
		    st->panels[i].image_index,    //
		    timeline_semaphore_value,     //
		    desired_present_time_ns,      //
		    present_slop_ns);
		if (ret != VK_SUCCESS && first == VK_SUCCESS) {
			first = ret;
		}
	}

	return first;
}

static VkResult
starvr_target_wait_for_present(struct comp_target *ct, time_duration_ns timeout_ns)
{
	struct starvr_target *st = starvr_target(ct);

	return comp_target_wait_for_present(st->panels[0].ct, timeout_ns);
}

static void
starvr_target_flush(struct comp_target *ct)
{
	struct starvr_target *st = starvr_target(ct);

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		comp_target_flush(st->panels[i].ct);
	}

#ifdef XRT_HAVE_WAYLAND_DIRECT
	lease_pump(st->lease);
#endif
}

static void
starvr_target_calc_frame_pacing(struct comp_target *ct,
                                int64_t *out_frame_id,
                                int64_t *out_wake_up_time_ns,
                                int64_t *out_desired_present_time_ns,
                                int64_t *out_present_slop_ns,
                                int64_t *out_predicted_display_time_ns)
{
	struct starvr_target *st = starvr_target(ct);

	/*
	 * Every panel keeps its own frame id, which its present needs, so they
	 * all get asked. The pace we keep is the first one's.
	 */
	for (uint32_t i = 1; i < STARVR_PANEL_COUNT; i++) {
		int64_t frame_id, wake_up_ns, desired_ns, slop_ns, display_ns;
		comp_target_calc_frame_pacing(st->panels[i].ct, &frame_id, &wake_up_ns, &desired_ns, &slop_ns,
		                              &display_ns);
	}

	comp_target_calc_frame_pacing(     //
	    st->panels[0].ct,              //
	    out_frame_id,                  //
	    out_wake_up_time_ns,           //
	    out_desired_present_time_ns,   //
	    out_present_slop_ns,           //
	    out_predicted_display_time_ns);
}

static void
starvr_target_mark_timing_point(struct comp_target *ct,
                                enum comp_target_timing_point point,
                                int64_t frame_id,
                                int64_t when_ns)
{
	struct starvr_target *st = starvr_target(ct);

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		st->panels[i].ct->mark_timing_point(st->panels[i].ct, point, frame_id, when_ns);
	}
}

static VkResult
starvr_target_update_timings(struct comp_target *ct)
{
	struct starvr_target *st = starvr_target(ct);
	VkResult first = VK_SUCCESS;

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		VkResult ret = comp_target_update_timings(st->panels[i].ct);
		if (ret != VK_SUCCESS && first == VK_SUCCESS) {
			first = ret;
		}
	}

	return first;
}

static void
starvr_target_info_gpu(
    struct comp_target *ct, int64_t frame_id, int64_t gpu_start_ns, int64_t gpu_end_ns, int64_t when_ns)
{
	struct starvr_target *st = starvr_target(ct);

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		comp_target_info_gpu(st->panels[i].ct, frame_id, gpu_start_ns, gpu_end_ns, when_ns);
	}
}

static void
starvr_target_set_title(struct comp_target *ct, const char *title)
{}

static VkResult
starvr_target_queue_supports_present(struct comp_target *ct, struct vk_bundle_queue *queue, VkBool32 *out_supported)
{
	struct starvr_target *st = starvr_target(ct);

	*out_supported = VK_FALSE;

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		VkBool32 supported = VK_FALSE;
		VkResult ret = comp_target_queue_supports_present(st->panels[i].ct, queue, &supported);
		if (ret != VK_SUCCESS) {
			return ret;
		}
		if (!supported) {
			return VK_SUCCESS;
		}
	}

	*out_supported = VK_TRUE;

	return VK_SUCCESS;
}

static void
starvr_target_destroy(struct comp_target *ct)
{
	struct starvr_target *st = starvr_target(ct);
	struct vk_bundle *vk = get_vk(st);

	destroy_own_images(st);
	destroy_semaphore(st);

	if (st->cmd != VK_NULL_HANDLE) {
		vk_cmd_pool_lock(&st->pool);
		vk->vkFreeCommandBuffers(vk->device, st->pool.pool, 1, &st->cmd);
		vk_cmd_pool_unlock(&st->pool);
		st->cmd = VK_NULL_HANDLE;
	}

	vk_cmd_pool_destroy(vk, &st->pool);

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		comp_target_destroy(&st->panels[i].ct);
	}

#ifdef XRT_HAVE_WAYLAND_DIRECT
	lease_destroy(&st->lease);
#endif

	free(st);
}


/*
 *
 * Finding the panels.
 *
 */

static bool
name_matches(const char *display_name, char side)
{
	const char *found = strstr(display_name, STARVR_PANEL_NAME);
	if (found == NULL) {
		return false;
	}

	const size_t len = strlen(found);
	if (len < 2) {
		return false;
	}

	return found[len - 2] == '_' && found[len - 1] == side;
}

static bool
find_panels(struct comp_compositor *c, struct vk_bundle *vk, struct starvr_panel_info out_panels[STARVR_PANEL_COUNT])
{
	VkDisplayPropertiesKHR *display_props = NULL;
	uint32_t display_count = 0;

	if (vk->instance == VK_NULL_HANDLE) {
		return false;
	}

	VkResult ret = vk_enumerate_physical_device_display_properties( //
	    vk,                                                         //
	    vk->physical_device,                                        //
	    &display_count,                                             //
	    &display_props);
	if (ret != VK_SUCCESS || display_count == 0) {
		COMP_INFO(c, "No Vulkan displays to take (%s, %u found), is something else holding them?",
		          vk_result_string(ret), display_count);
		return false;
	}

	const char sides[STARVR_PANEL_COUNT] = {'L', 'R'};
	bool found[STARVR_PANEL_COUNT] = {false, false};

	for (uint32_t side = 0; side < STARVR_PANEL_COUNT; side++) {
		for (uint32_t i = 0; i < display_count; i++) {
			VkDisplayPropertiesKHR *p = &display_props[i];

			if (p->displayName == NULL || !name_matches(p->displayName, sides[side])) {
				continue;
			}

			out_panels[side].display = p->display;
			out_panels[side].extent = p->physicalResolution;
			snprintf(out_panels[side].name, sizeof(out_panels[side].name), "%s", p->displayName);
			found[side] = true;
			break;
		}
	}

	if (!found[0] || !found[1]) {
		struct u_pp_sink_stack_only sink;
		u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);

		u_pp(dg, "Looking for '%s' and '%s', found %u display(s):", starvr_wanted[0], starvr_wanted[1],
		     display_count);
		for (uint32_t i = 0; i < display_count; i++) {
			u_pp(dg, "\n\t%s %ux%u", display_props[i].displayName,
			     display_props[i].physicalResolution.width, display_props[i].physicalResolution.height);
		}

		COMP_INFO(c, "%s", sink.buffer);
	}

	free(display_props);

	return found[0] && found[1];
}

#ifdef XRT_HAVE_WAYLAND_DIRECT
/*!
 * Turn each leased connector into a display Vulkan can present to.
 */
static bool
displays_from_lease(struct comp_compositor *c,
                    struct starvr_lease *lease,
                    struct starvr_panel_info out_panels[STARVR_PANEL_COUNT])
{
	struct vk_bundle *vk = &c->base.vk;

	if (vk->vkGetDrmDisplayEXT == NULL || vk->vkAcquireDrmDisplayEXT == NULL) {
		COMP_ERROR(c, "No VK_EXT_acquire_drm_display, cannot use leased panels");
		return false;
	}

	const uint32_t half_width = (uint32_t)c->xdev->hmd->screens[0].w_pixels / STARVR_PANEL_COUNT;
	const uint32_t height = (uint32_t)c->xdev->hmd->screens[0].h_pixels;

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		const struct starvr_lease_connector *conn = &lease->taken[i];

		/*
		 * The lease fd is the one with master rights over the leased
		 * connectors; the device fd the compositor hands out for
		 * enumeration cannot always be asked about them.
		 */
		const int fds[2] = {lease->leased_fd, lease->drm_fd};

		VkDisplayKHR display = VK_NULL_HANDLE;
		VkResult ret = VK_ERROR_UNKNOWN;

		for (uint32_t f = 0; f < ARRAY_SIZE(fds) && ret != VK_SUCCESS; f++) {
			ret = vk->vkGetDrmDisplayEXT(vk->physical_device, fds[f], conn->id, &display);
			if (ret != VK_SUCCESS) {
				COMP_DEBUG(c, "vkGetDrmDisplayEXT for %s on fd %d: %s", conn->name, fds[f],
				           vk_result_string(ret));
			}
		}

		if (ret != VK_SUCCESS) {
			COMP_ERROR(c, "vkGetDrmDisplayEXT for %s: %s", conn->name, vk_result_string(ret));
			return false;
		}

		/*
		 * One lease fd is master over every connector in it, and Mesa
		 * only tracks one acquired fd, so acquiring the first display
		 * takes them all and asking again is refused but harmless.
		 */
		ret = vk->vkAcquireDrmDisplayEXT(vk->physical_device, lease->leased_fd, display);
		if (ret != VK_SUCCESS && i == 0) {
			COMP_ERROR(c, "vkAcquireDrmDisplayEXT for %s: %s", conn->name, vk_result_string(ret));
			return false;
		}
		if (ret != VK_SUCCESS) {
			COMP_DEBUG(c, "vkAcquireDrmDisplayEXT for %s: %s, already covered by the lease", conn->name,
			           vk_result_string(ret));
		}

		out_panels[i].display = display;
		out_panels[i].extent.width = half_width;
		out_panels[i].extent.height = height;
		snprintf(out_panels[i].name, sizeof(out_panels[i].name), "%s", conn->description);
	}

	return true;
}
#endif


/*
 *
 * Factory.
 *
 */

/*!
 * Detection happens before the compositor has a Vulkan instance, and the panels
 * can only be seen through one, so this stands up a throwaway.
 */
static bool
detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	if (c->xdev == NULL || c->xdev->name != XRT_DEVICE_STARVR_ONE) {
		return false;
	}

	struct vk_bundle temp = {0};
	temp.log_level = U_LOGGING_WARN;

	if (vk_get_loader_functions(&temp, vkGetInstanceProcAddr) != VK_SUCCESS) {
		return false;
	}

	const char *extension_names[] = {
	    COMP_INSTANCE_EXTENSIONS_COMMON,
	    VK_KHR_DISPLAY_EXTENSION_NAME,
	};

	VkInstanceCreateInfo instance_create_info = {
	    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	    .enabledExtensionCount = ARRAY_SIZE(extension_names),
	    .ppEnabledExtensionNames = extension_names,
	};

	if (temp.vkCreateInstance(&instance_create_info, NULL, &temp.instance) != VK_SUCCESS) {
		return false;
	}

	bool detected = false;
	struct starvr_panel_info panels[STARVR_PANEL_COUNT] = {0};

	if (vk_get_instance_functions(&temp) == VK_SUCCESS &&
	    vk_select_physical_device(&temp, c->settings.selected_gpu_index) == VK_SUCCESS) {
		detected = find_panels(c, &temp, panels);
	}

	temp.vkDestroyInstance(temp.instance, NULL);

#ifdef XRT_HAVE_WAYLAND_DIRECT
	/*
	 * Under a Wayland session the panels belong to the compositor until it
	 * leases them out, so nothing is enumerable until then.
	 */
	if (!detected) {
		detected = lease_probe(c);
	}
#endif

	if (!detected) {
		COMP_INFO(c, "StarVR One found but its panels are not, falling back to another target");
	}

	return detected;
}

static bool
create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct starvr_panel_info panels[STARVR_PANEL_COUNT] = {0};
	struct starvr_lease *lease = NULL;

	bool found = find_panels(c, &c->base.vk, panels);

#ifdef XRT_HAVE_WAYLAND_DIRECT
	if (!found) {
		lease = lease_take(c);
		found = lease != NULL && displays_from_lease(c, lease, panels);
	}
#endif

	if (!found) {
		COMP_ERROR(c, "Could not find both StarVR panels");
		goto err_lease;
	}

	if (panels[0].extent.height != panels[1].extent.height) {
		COMP_ERROR(c, "StarVR panels disagree about their height, %u and %u", panels[0].extent.height,
		           panels[1].extent.height);
		goto err_lease;
	}

	void *dpy = NULL;

#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
	Display *xlib_dpy = NULL;
	struct comp_target_swapchain probe = {0};
	probe.base.c = c;

	if (comp_window_direct_connect(&probe, &xlib_dpy)) {
		dpy = xlib_dpy;
	} else {
		COMP_INFO(c, "No X server to lease the panels from, taking them directly");
	}
#endif

	struct starvr_target *st = U_TYPED_CALLOC(struct starvr_target);
	if (st == NULL) {
		goto err_lease;
	}

	st->base.c = c;
	st->base.name = "StarVR One";

	st->base.init_pre_vulkan = starvr_target_init_pre_vulkan;
	st->base.init_post_vulkan = starvr_target_init_post_vulkan;
	st->base.check_ready = starvr_target_check_ready;
	st->base.is_shared_presentable_image = starvr_target_is_shared_presentable_image;
	st->base.create_images = starvr_target_create_images;
	st->base.has_images = starvr_target_has_images;
	st->base.acquire = starvr_target_acquire;
	st->base.present = starvr_target_present;
	st->base.wait_for_present = starvr_target_wait_for_present;
	st->base.flush = starvr_target_flush;
	st->base.calc_frame_pacing = starvr_target_calc_frame_pacing;
	st->base.mark_timing_point = starvr_target_mark_timing_point;
	st->base.update_timings = starvr_target_update_timings;
	st->base.info_gpu = starvr_target_info_gpu;
	st->base.set_title = starvr_target_set_title;
	st->base.queue_supports_present = starvr_target_queue_supports_present;
	st->base.destroy = starvr_target_destroy;

#ifdef XRT_HAVE_WAYLAND_DIRECT
	st->lease = lease;
#endif

	for (uint32_t i = 0; i < STARVR_PANEL_COUNT; i++) {
		st->panels[i].ct = panel_create(c, &panels[i], dpy);
		if (st->panels[i].ct == NULL) {
			starvr_target_destroy(&st->base);
			return false;
		}

		COMP_INFO(c, "StarVR panel %u is '%s', %ux%u", i, panels[i].name, panels[i].extent.width,
		          panels[i].extent.height);
	}

	st->panels[1].offset.x = (int32_t)panels[0].extent.width;

	struct vk_bundle *vk = &c->base.vk;

	VkResult ret = vk_cmd_pool_init(vk, &st->pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(c, "vk_cmd_pool_init: %s", vk_result_string(ret));
		starvr_target_destroy(&st->base);
		return false;
	}

	ret = vk_cmd_pool_create_cmd_buffer(vk, &st->pool, &st->cmd);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(c, "vk_cmd_pool_create_cmd_buffer: %s", vk_result_string(ret));
		starvr_target_destroy(&st->base);
		return false;
	}

	VK_NAME_COMMAND_BUFFER(vk, st->cmd, "starvr panel command buffer");

	*out_ct = &st->base;

	return true;

err_lease:
#ifdef XRT_HAVE_WAYLAND_DIRECT
	lease_destroy(&lease);
#endif
	return false;
}

static const char *instance_extensions[] = {
    VK_KHR_DISPLAY_EXTENSION_NAME,
    VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME,
#ifdef XRT_HAVE_WAYLAND_DIRECT
    VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME,
#endif
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
    VK_EXT_ACQUIRE_XLIB_DISPLAY_EXTENSION_NAME,
#endif
};

const struct comp_target_factory comp_target_factory_starvr = {
    .name = "StarVR One Direct-Mode",
    .identifier = "starvr",
    .requires_vulkan_for_create = true,
    .is_deferred = false,
    .required_instance_version = 0,
    .required_instance_extensions = instance_extensions,
    .required_instance_extension_count = ARRAY_SIZE(instance_extensions),
    .optional_device_extensions = NULL,
    .optional_device_extension_count = 0,
    .detect = detect,
    .create_target = create_target,
};
