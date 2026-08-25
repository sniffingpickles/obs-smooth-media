#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libavutil/samplefmt.h>

#define AUDIO_SPEED_MAX_PLANES 8

struct SwrContext;

struct audio_speed_converter {
	struct SwrContext *context;
	uint8_t *storage;
	size_t storage_capacity;
	enum AVSampleFormat format;
	int sample_rate;
	int channels;
};

struct audio_speed_frame {
	uint8_t *data[AUDIO_SPEED_MAX_PLANES];
	size_t data_size[AUDIO_SPEED_MAX_PLANES];
	uint32_t frames;
};

void audio_speed_init(struct audio_speed_converter *converter);
void audio_speed_reset(struct audio_speed_converter *converter);
void audio_speed_free(struct audio_speed_converter *converter);

/* Resample one PCM chunk at a slightly different speed while preserving the
 * sample rate reported to OBS. Output storage belongs to converter and stays
 * valid until the next convert/reset/free call. */
bool audio_speed_convert(struct audio_speed_converter *converter,
			 const uint8_t *const *input, uint32_t input_frames,
			 int sample_rate, int channels,
			 enum AVSampleFormat format, double speed,
			 struct audio_speed_frame *output);
