/*
 * Copyright (c) 2026 Andri Yngvason
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

#include "audio.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <aml.h>
#include <neatvnc.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

struct audio {
	struct pw_loop* loop;
	struct aml_handler* handler;
};

struct audio_capture {
	struct nvnc_client* client;
	struct pw_stream* stream;
	uint32_t frame_size;
};

static void on_loop_event(struct aml_handler* handler)
{
	struct audio* self = aml_get_userdata(handler);
	pw_loop_iterate(self->loop, 0);
}

struct audio* audio_new(void)
{
	pw_init(NULL, NULL);

	struct audio* self = calloc(1, sizeof(*self));
	if (!self)
		goto alloc_failure;

	self->loop = pw_loop_new(NULL);
	if (!self->loop)
		goto loop_failure;

	self->handler = aml_handler_new(pw_loop_get_fd(self->loop),
			on_loop_event, self, NULL);
	if (!self->handler)
		goto handler_failure;

	if (aml_start(aml_get_default(), self->handler) < 0)
		goto start_failure;

	pw_loop_enter(self->loop);
	return self;

start_failure:
	aml_unref(self->handler);
handler_failure:
	pw_loop_destroy(self->loop);
loop_failure:
	free(self);
alloc_failure:
	pw_deinit();
	return NULL;
}

void audio_destroy(struct audio* self)
{
	if (!self)
		return;

	pw_loop_leave(self->loop);
	aml_stop(aml_get_default(), self->handler);
	aml_unref(self->handler);
	pw_loop_destroy(self->loop);
	free(self);
	pw_deinit();
}

static enum spa_audio_format spa_format_from_nvnc(
		enum nvnc_audio_format format)
{
	switch (format) {
	case NVNC_AUDIO_FORMAT_U8:
		return SPA_AUDIO_FORMAT_U8;
	case NVNC_AUDIO_FORMAT_S8:
		return SPA_AUDIO_FORMAT_S8;
	case NVNC_AUDIO_FORMAT_U16:
		return SPA_AUDIO_FORMAT_U16_LE;
	case NVNC_AUDIO_FORMAT_S16:
		return SPA_AUDIO_FORMAT_S16_LE;
	case NVNC_AUDIO_FORMAT_U32:
		return SPA_AUDIO_FORMAT_U32_LE;
	case NVNC_AUDIO_FORMAT_S32:
		return SPA_AUDIO_FORMAT_S32_LE;
	case NVNC_AUDIO_FORMAT_UNSPEC:
		break;
	}
	return SPA_AUDIO_FORMAT_UNKNOWN;
}

static uint32_t sample_size_of(enum nvnc_audio_format format)
{
	switch (format) {
	case NVNC_AUDIO_FORMAT_U8:
	case NVNC_AUDIO_FORMAT_S8:
		return 1;
	case NVNC_AUDIO_FORMAT_U16:
	case NVNC_AUDIO_FORMAT_S16:
		return 2;
	case NVNC_AUDIO_FORMAT_U32:
	case NVNC_AUDIO_FORMAT_S32:
		return 4;
	case NVNC_AUDIO_FORMAT_UNSPEC:
		break;
	}
	return 0;
}

static void on_process(void* userdata)
{
	struct audio_capture* self = userdata;

	struct pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
	if (!buffer)
		return;

	struct spa_data* data = &buffer->buffer->datas[0];
	if (data->data) {
		uint32_t offset = SPA_MIN(data->chunk->offset, data->maxsize);
		uint32_t size = SPA_MIN(data->chunk->size,
				data->maxsize - offset);
		nvnc_client_audio_sink_feed(self->client,
				SPA_PTROFF(data->data, offset, void),
				size / self->frame_size, NVNC_NO_PTS);
	}

	pw_stream_queue_buffer(self->stream, buffer);
}

static void on_state_changed(void* userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char* error)
{
	if (state == PW_STREAM_STATE_ERROR)
		nvnc_log(NVNC_LOG_ERROR, "Audio capture failed: %s", error);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.process = on_process,
};

static bool is_config_supported(const struct nvnc_audio_config* config)
{
	enum nvnc_audio_format format = nvnc_audio_config_get_format(config);
	uint32_t channel_count = nvnc_audio_config_get_channel_count(config);

	return spa_format_from_nvnc(format) != SPA_AUDIO_FORMAT_UNKNOWN &&
		nvnc_audio_config_get_rate(config) > 0 &&
		channel_count > 0 && channel_count <= SPA_AUDIO_MAX_CHANNELS;
}

static struct spa_audio_info_raw audio_info_from_config(
		const struct nvnc_audio_config* config)
{
	struct spa_audio_info_raw info = {
		.format = spa_format_from_nvnc(
				nvnc_audio_config_get_format(config)),
		.rate = nvnc_audio_config_get_rate(config),
		.channels = nvnc_audio_config_get_channel_count(config),
	};

	const enum nvnc_audio_channel* positions =
		nvnc_audio_config_get_positions(config);
	for (uint32_t i = 0; i < info.channels; ++i)
		info.position[i] = positions[i];

	return info;
}

static int connect_stream(struct audio_capture* self,
		const struct nvnc_audio_config* config)
{
	uint8_t buffer[1024];
	struct spa_pod_builder builder =
		SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	struct spa_audio_info_raw info = audio_info_from_config(config);
	const struct spa_pod* params[] = {
		spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
				&info),
	};

	return pw_stream_connect(self->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
			params, SPA_N_ELEMENTS(params));
}

struct audio_capture* audio_capture_new(struct audio* audio,
		struct nvnc_client* client,
		const struct nvnc_audio_config* config)
{
	if (!is_config_supported(config)) {
		nvnc_log(NVNC_LOG_WARNING, "Unsupported audio configuration requested by client");
		return NULL;
	}

	struct audio_capture* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->client = client;
	self->frame_size =
		sample_size_of(nvnc_audio_config_get_format(config)) *
		nvnc_audio_config_get_channel_count(config);

	struct pw_properties* properties = pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Music",
			PW_KEY_STREAM_CAPTURE_SINK, "true",
			NULL);

	self->stream = pw_stream_new_simple(audio->loop, "wayvnc", properties,
			&stream_events, self);
	if (!self->stream)
		goto stream_failure;

	if (connect_stream(self, config) < 0)
		goto connect_failure;

	return self;

connect_failure:
	pw_stream_destroy(self->stream);
stream_failure:
	free(self);
	nvnc_log(NVNC_LOG_ERROR, "Failed to start audio capture");
	return NULL;
}

void audio_capture_destroy(struct audio_capture* self)
{
	if (!self)
		return;

	pw_stream_destroy(self->stream);
	free(self);
}
