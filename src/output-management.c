/*
 * Copyright (c) 2023 The wayvnc authors
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <neatvnc.h>
#include <string.h>
#include <wayland-client.h>

#include "output.h"
#include "output-management.h"

#include "wlr-output-management-unstable-v1.h"

struct output_manager_head {
	struct zwlr_output_head_v1* head;
	struct wl_list link;
	char* name;
	bool enabled;
};

static struct wl_list heads;
static uint32_t last_config_serial;
static struct zwlr_output_manager_v1* wlr_output_manager;

/* single head properties */
static void output_head_name(void* data,
		struct zwlr_output_head_v1* output_head, const char* name)
{
	struct output_manager_head* head = data;
	nvnc_trace("Got head name: %s", name);
	free(head->name);
	head->name = strdup(name);
}

static void output_head_description(void* data,
		struct zwlr_output_head_v1* output_head,
		const char* description)
{
	nvnc_trace("Got head description: %s", description);
}

static void output_head_physical_size(void* data,
		struct zwlr_output_head_v1* output_head,
		int32_t width, int32_t height)
{
	nvnc_trace("Got head size: %dx%d", width, height);
}

static void output_head_mode(void* data,
		struct zwlr_output_head_v1* output_head,
		struct zwlr_output_mode_v1* mode)
{
	nvnc_trace("Got head mode");
}

static void output_head_enabled(void* data,
		struct zwlr_output_head_v1* output_head, int32_t enabled)
{
	nvnc_trace("Got head enabled: %s", enabled ? "yes" : "no");
	struct output_manager_head* head = data;
	head->enabled = !!enabled;
}

static void output_head_current_mode(void* data,
		struct zwlr_output_head_v1* output_head,
		struct zwlr_output_mode_v1* mode)
{
	nvnc_trace("Got head current mode");
}

static void output_head_position(void* data,
		struct zwlr_output_head_v1* output_head, int32_t x, int32_t y)
{
	nvnc_trace("Got head position: %d,%d", x, y);
}

static void output_head_transform(void* data,
		struct zwlr_output_head_v1* output_head, int32_t transform)
{
	nvnc_trace("Got head transform: %d", transform);
}

static void output_head_scale(void* data,
		struct zwlr_output_head_v1* output_head, wl_fixed_t scale_f)
{
	double scale = wl_fixed_to_double(scale_f);
	nvnc_trace("Got head scale: %.2f", scale);
}

static void output_head_finished(void* data,
		struct zwlr_output_head_v1* output_head)
{
	nvnc_trace("head gone, removing");
	struct output_manager_head* head = data;
	zwlr_output_head_v1_destroy(output_head);
	wl_list_remove(&head->link);
	free(head->name);
	head->name = NULL;
	head->head = NULL;
	free(head);
}

struct zwlr_output_head_v1_listener wlr_output_head_listener = {
	.name = output_head_name,
	.description = output_head_description,
	.physical_size = output_head_physical_size,
	.mode = output_head_mode,
	.enabled = output_head_enabled,
	.current_mode = output_head_current_mode,
	.position = output_head_position,
	.transform = output_head_transform,
	.scale = output_head_scale,
	.finished = output_head_finished,
};

/* config object */
static void output_manager_config_succeeded(void* data,
		struct zwlr_output_configuration_v1* config)
{
	nvnc_trace("config request succeeded");
	zwlr_output_configuration_v1_destroy(config);
}

static void output_manager_config_failed(void* data,
		struct zwlr_output_configuration_v1* config)
{
	nvnc_trace("config request failed");
	zwlr_output_configuration_v1_destroy(config);
}

static void output_manager_config_cancelled(void* data,
		struct zwlr_output_configuration_v1* config)
{
	nvnc_trace("config request cancelled");
	zwlr_output_configuration_v1_destroy(config);
}

struct zwlr_output_configuration_v1_listener wlr_output_config_listener = {
	.succeeded = output_manager_config_succeeded,
	.failed = output_manager_config_failed,
	.cancelled = output_manager_config_cancelled,
};

/* manager itself */
static void output_manager_done(void* data,
		struct zwlr_output_manager_v1* zwlr_output_manager_v1,
		uint32_t serial)
{
	last_config_serial = serial;
	nvnc_trace("Got new serial: %u", serial);
}

static void output_manager_finished(void* data,
		struct zwlr_output_manager_v1* zwlr_output_manager_v1)
{
	nvnc_trace("output-manager destroyed");
	wlr_output_manager = NULL;
}

static void output_manager_head(void* data,
		struct zwlr_output_manager_v1* zwlr_output_manager_v1,
		struct zwlr_output_head_v1* output_head)
{
	struct output_manager_head* head = calloc(1, sizeof(*head));
	if (!head) {
		nvnc_log(NVNC_LOG_ERROR, "OOM");
		return;
	}

	head->head = output_head;
	wl_list_insert(heads.prev, &head->link);
	nvnc_trace("New head, now at %lu", wl_list_length(&heads));

	zwlr_output_head_v1_add_listener(head->head,
		&wlr_output_head_listener, head);
}

static const struct zwlr_output_manager_v1_listener
		wlr_output_manager_listener = {
	.head = output_manager_head,
	.done = output_manager_done,
	.finished = output_manager_finished,
};

/* Public API */
void wlr_output_manager_setup(struct zwlr_output_manager_v1* output_manager)
{
	if (wlr_output_manager)
		return;

	wl_list_init(&heads);
	wlr_output_manager = output_manager;
	zwlr_output_manager_v1_add_listener(wlr_output_manager,
			&wlr_output_manager_listener, NULL);
}

void wlr_output_manager_destroy(void)
{
	if (!wlr_output_manager)
		return;

	struct output_manager_head* head;
	struct output_manager_head* tmp;
	wl_list_for_each_safe(head, tmp, &heads, link) {
		wl_list_remove(&head->link);
		free(head->name);
		free(head);
	}

	zwlr_output_manager_v1_destroy(wlr_output_manager);
	wlr_output_manager = NULL;

	last_config_serial = 0;
}

bool wlr_output_manager_resize_output(struct output* output,
		uint16_t width, uint16_t height)
{
	if (!wlr_output_manager) {
		nvnc_log(NVNC_LOG_INFO,
			"output-management protocol not available, not resizing output");
		return false;
	}

	if (!output->is_headless) {
		nvnc_log(NVNC_LOG_INFO,
			"not resizing output %s: not a headless one",
			output->name);
		return false;
	}

	// TODO: This could be synced to --max-fps
	int refresh_rate = 0;

	struct zwlr_output_configuration_v1* config;
	struct zwlr_output_configuration_head_v1* config_head;

	config = zwlr_output_manager_v1_create_configuration(
		wlr_output_manager, last_config_serial);
	zwlr_output_configuration_v1_add_listener(config,
			&wlr_output_config_listener, NULL);

	struct output_manager_head* head;
	wl_list_for_each(head, &heads, link) {
		if (!head->enabled) {
			nvnc_trace("disabling output %s", head->name);
			zwlr_output_configuration_v1_disable_head(
				config, head->head);
			continue;
		}

		config_head = zwlr_output_configuration_v1_enable_head(
			config, head->head);
		if (head->name && strcmp(head->name, output->name) == 0) {
			nvnc_trace("reconfiguring output %s", head->name);
			zwlr_output_configuration_head_v1_set_custom_mode(
				config_head, width, height, refresh_rate);

			/* It doesn't make any sense to have rotation on a
			 * headless display, so we set the transform here to be
			 * sure.
			 */
			zwlr_output_configuration_head_v1_set_transform(
					config_head, WL_OUTPUT_TRANSFORM_NORMAL);
		}
	}

	nvnc_trace("applying new output config");
	zwlr_output_configuration_v1_apply(config);
	return true;
}
