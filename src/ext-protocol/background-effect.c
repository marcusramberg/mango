#include "mango/ext-protocol/background-effect.h"
#include "ext-background-effect-v1-protocol.h"
#include "mango/animation/client.h"
#include "mango/common/util.h"
#include "mango/config/parse.h"
#include "mango/manage/client.h"
#include "mango/manage/layer.h"
#include <math.h>
#include <scenefx/types/wlr_scene.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/addon.h>

struct effect_rect {
	pixman_box32_t box;
	struct wlr_scene_blur *blur;
};

struct background_effect {
	struct wl_list link;
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_addon addon;
	pixman_region32_t region;
	bool dirty;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *buffer;
	struct effect_rect *rects;
	int nrects;
	int32_t width, height;
	struct fx_corner_radii corners;
	struct wl_event_source *idle;
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener tree_destroy;
	struct wl_listener buffer_destroy;
};

static struct wl_list effects;

struct find_buffer_data {
	struct wlr_surface *surface;
	struct wlr_scene_buffer *buffer;
};

static void find_buffer(struct wlr_scene_buffer *buffer, int32_t sx, int32_t sy,
						void *data) {
	struct find_buffer_data *fd = data;
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (!fd->buffer && scene_surface && scene_surface->surface == fd->surface)
		fd->buffer = buffer;
}

static void handle_tree_destroy(struct wl_listener *listener, void *data) {
	struct background_effect *effect =
		wl_container_of(listener, effect, tree_destroy);
	wl_list_remove(&effect->tree_destroy.link);
	wl_list_remove(&effect->buffer_destroy.link);
	free(effect->rects);
	effect->rects = NULL;
	effect->nrects = 0;
	effect->tree = NULL;
	effect->buffer = NULL;
}

static void effect_schedule_update(struct background_effect *effect);

static void handle_buffer_destroy(struct wl_listener *listener, void *data) {
	struct background_effect *effect =
		wl_container_of(listener, effect, buffer_destroy);
	/* Not destroying the tree here: it is the buffer's sibling, and the
	 * parent may be tearing down its children. */
	wl_list_remove(&effect->buffer_destroy.link);
	wl_list_init(&effect->buffer_destroy.link);
	effect->buffer = NULL;
	wlr_scene_node_set_enabled(&effect->tree->node, false);
	effect_schedule_update(effect);
}

static void effect_clear_tree(struct background_effect *effect) {
	if (effect->tree)
		wlr_scene_node_destroy(&effect->tree->node);
}

/* Returns the tree holding the surface's buffer, or NULL when mango's own
 * blur already covers it or a no_blur rule opts it out. */
static struct wlr_scene_tree *
effect_search_tree(struct wlr_surface *surface,
				   struct fx_corner_radii *corners) {
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_xdg_surface *xdg_surface;

	if ((layer_surface = wlr_layer_surface_v1_try_from_wlr_surface(root))) {
		LayerSurface *l = layer_surface->data;
		if (!l || l->no_blur || (config.blur && config.blur_layer && l->blur))
			return NULL;
		return l->scene;
	}

	if (!(xdg_surface = wlr_xdg_surface_try_from_wlr_surface(root)))
		return NULL;

	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP)
		return root->data;

	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		return NULL;

	Client *c = xdg_surface->data;
	if (!c || c->no_blur || config.blur)
		return NULL;
	if (surface == root && c->mon)
		*corners = set_client_corner_location(c);
	return root->data;
}

/* Maps the rects through the buffer's current position, scale, clip and
 * opacity, so the blur follows mango's animations. */
static void effect_sync(struct background_effect *effect) {
	struct wlr_scene_buffer *buffer = effect->buffer;
	if (!buffer)
		return;

	struct wlr_fbox full, src = buffer->src_box;
	wlr_surface_get_buffer_source_box(effect->surface, &full);
	if (wlr_fbox_empty(&full))
		return;
	if (wlr_fbox_empty(&src))
		src = full;

	/* src_box is in buffer pixels; convert to surface-local units */
	double unit_x = effect->width / full.width;
	double unit_y = effect->height / full.height;
	double src_x = (src.x - full.x) * unit_x, src_y = (src.y - full.y) * unit_y;
	double src_w = src.width * unit_x, src_h = src.height * unit_y;
	struct wlr_box visible = {
		.x = buffer->node.x,
		.y = buffer->node.y,
		.width = buffer->dst_width ? buffer->dst_width : lround(src_w),
		.height = buffer->dst_height ? buffer->dst_height : lround(src_h),
	};
	double fx = visible.width / src_w, fy = visible.height / src_h;
	struct fx_corner_radii c = effect->corners;

	wlr_scene_node_set_enabled(&effect->tree->node, true);
	for (int i = 0; i < effect->nrects; i++) {
		pixman_box32_t *r = &effect->rects[i].box;
		struct wlr_scene_blur *blur = effect->rects[i].blur;
		int32_t x1 = visible.x + lround((r->x1 - src_x) * fx);
		int32_t y1 = visible.y + lround((r->y1 - src_y) * fy);
		int32_t x2 = visible.x + lround((r->x2 - src_x) * fx);
		int32_t y2 = visible.y + lround((r->y2 - src_y) * fy);
		struct wlr_box box = {x1, y1, x2 - x1, y2 - y1}, b;

		bool on = wlr_box_intersection(&b, &box, &visible);
		wlr_scene_node_set_enabled(&blur->node, on);
		if (!on)
			continue;

		bool left = b.x == visible.x, top = b.y == visible.y;
		bool right = b.x + b.width == visible.x + visible.width;
		bool bottom = b.y + b.height == visible.y + visible.height;
		wlr_scene_node_set_position(&blur->node, b.x, b.y);
		wlr_scene_blur_set_size(blur, b.width, b.height);
		wlr_scene_blur_set_corner_radii(
			blur, corner_radii_new(top && left ? c.top_left : 0,
								   top && right ? c.top_right : 0,
								   bottom && right ? c.bottom_right : 0,
								   bottom && left ? c.bottom_left : 0));
		wlr_scene_blur_set_strength(blur, buffer->opacity);
		wlr_scene_blur_set_alpha(blur, buffer->opacity);
	}
}

void background_effect_sync_all(void) {
	struct background_effect *effect;
	wl_list_for_each(effect, &effects, link) effect_sync(effect);
}

static void effect_update(void *data) {
	struct background_effect *effect = data;
	effect->idle = NULL;
	struct wlr_surface *surface = effect->surface;
	struct fx_corner_radii corners = corner_radii_none();
	struct wlr_scene_tree *search = effect_search_tree(surface, &corners);
	int32_t width = surface->current.width;
	int32_t height = surface->current.height;

	if (!search || !pixman_region32_not_empty(&effect->region)) {
		effect_clear_tree(effect);
		effect->dirty = false;
		return;
	}

	struct find_buffer_data fd = {.surface = surface};
	wlr_scene_node_for_each_buffer(&search->node, find_buffer, &fd);
	if (!fd.buffer)
		return;

	effect->corners = corners;
	if (!effect->tree || effect->buffer != fd.buffer || effect->dirty ||
		effect->width != width || effect->height != height ||
		effect->tree->node.parent != fd.buffer->node.parent) {
		effect_clear_tree(effect);
		effect->dirty = false;
		effect->width = width;
		effect->height = height;

		effect->tree = wlr_scene_tree_create(fd.buffer->node.parent);
		effect->tree_destroy.notify = handle_tree_destroy;
		wl_signal_add(&effect->tree->node.events.destroy,
					  &effect->tree_destroy);
		effect->buffer = fd.buffer;
		effect->buffer_destroy.notify = handle_buffer_destroy;
		wl_signal_add(&fd.buffer->node.events.destroy, &effect->buffer_destroy);
		wlr_scene_node_place_below(&effect->tree->node, &fd.buffer->node);

		pixman_region32_t clipped;
		pixman_region32_init(&clipped);
		pixman_region32_intersect_rect(&clipped, &effect->region, 0, 0, width,
									   height);
		pixman_box32_t *rects =
			pixman_region32_rectangles(&clipped, &effect->nrects);
		effect->rects = ecalloc(effect->nrects ? effect->nrects : 1,
								sizeof(*effect->rects));
		for (int i = 0; i < effect->nrects; i++) {
			effect->rects[i].box = rects[i];
			effect->rects[i].blur = wlr_scene_blur_create(effect->tree, 0, 0);
		}
		pixman_region32_fini(&clipped);
	}
	effect_sync(effect);
}

/* Deferred so the scene nodes for a surface that is mapping exist first. */
static void effect_schedule_update(struct background_effect *effect) {
	if (effect->idle)
		return;
	struct wl_display *display =
		wl_client_get_display(wl_resource_get_client(effect->resource));
	effect->idle = wl_event_loop_add_idle(wl_display_get_event_loop(display),
										  effect_update, effect);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct background_effect *effect =
		wl_container_of(listener, effect, commit);
	effect_schedule_update(effect);
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct background_effect *effect = wl_container_of(listener, effect, map);
	effect_schedule_update(effect);
}

static void effect_destroy(struct background_effect *effect) {
	effect_clear_tree(effect);
	if (effect->idle)
		wl_event_source_remove(effect->idle);
	wl_list_remove(&effect->commit.link);
	wl_list_remove(&effect->map.link);
	wl_list_remove(&effect->link);
	wlr_addon_finish(&effect->addon);
	pixman_region32_fini(&effect->region);
	wl_resource_set_user_data(effect->resource, NULL);
	free(effect);
}

static void addon_handle_destroy(struct wlr_addon *addon) {
	struct background_effect *effect = wl_container_of(addon, effect, addon);
	effect_destroy(effect);
}

static const struct wlr_addon_interface addon_impl = {
	.name = "ext_background_effect_surface_v1",
	.destroy = addon_handle_destroy,
};

static void surface_handle_destroy(struct wl_client *client,
								   struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void surface_handle_set_blur_region(struct wl_client *client,
										   struct wl_resource *resource,
										   struct wl_resource *region) {
	struct background_effect *effect = wl_resource_get_user_data(resource);
	if (!effect) {
		wl_resource_post_error(
			resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
			"surface destroyed");
		return;
	}
	if (region)
		pixman_region32_copy(&effect->region, wlr_region_from_resource(region));
	else
		pixman_region32_clear(&effect->region);
	effect->dirty = true;
}

static const struct ext_background_effect_surface_v1_interface surface_impl = {
	.destroy = surface_handle_destroy,
	.set_blur_region = surface_handle_set_blur_region,
};

static void surface_resource_destroy(struct wl_resource *resource) {
	struct background_effect *effect = wl_resource_get_user_data(resource);
	if (effect)
		effect_destroy(effect);
}

static void manager_handle_destroy(struct wl_client *client,
								   struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void
manager_handle_get_background_effect(struct wl_client *client,
									 struct wl_resource *resource, uint32_t id,
									 struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	if (wlr_addon_find(&surface->addons, NULL, &addon_impl)) {
		wl_resource_post_error(
			resource,
			EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
			"surface already has a background effect object");
		return;
	}

	struct wl_resource *effect_resource =
		wl_resource_create(client, &ext_background_effect_surface_v1_interface,
						   wl_resource_get_version(resource), id);
	if (!effect_resource) {
		wl_client_post_no_memory(client);
		return;
	}

	struct background_effect *effect = ecalloc(1, sizeof(*effect));
	effect->resource = effect_resource;
	effect->surface = surface;
	pixman_region32_init(&effect->region);
	wlr_addon_init(&effect->addon, &surface->addons, NULL, &addon_impl);
	effect->commit.notify = handle_commit;
	wl_signal_add(&surface->events.commit, &effect->commit);
	effect->map.notify = handle_map;
	wl_signal_add(&surface->events.map, &effect->map);
	wl_list_insert(&effects, &effect->link);
	wl_resource_set_implementation(effect_resource, &surface_impl, effect,
								   surface_resource_destroy);
}

static const struct ext_background_effect_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_background_effect = manager_handle_get_background_effect,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
						 uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &ext_background_effect_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
	ext_background_effect_manager_v1_send_capabilities(
		resource, EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
}

void background_effect_init(struct wl_display *display) {
	wl_list_init(&effects);
	wl_global_create(display, &ext_background_effect_manager_v1_interface, 1,
					 NULL, manager_bind);
}
