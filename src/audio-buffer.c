#include "audio-buffer.h"
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MIN_BUFFER_NS  (80000000LL)   /* 80ms */
#define DEFAULT_MAX_BUFFER_NS  (500000000LL)  /* 500ms */

static void free_frame_data(struct audio_buf_frame *f)
{
	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
		if (f->data[i]) {
			free(f->data[i]);
			f->data[i] = NULL;
		}
		f->data_size[i] = 0;
	}
	f->valid = false;
}

void audio_buffer_init(struct audio_buffer *ab)
{
	memset(ab, 0, sizeof(*ab));
	ab->min_buffer_ns = DEFAULT_MIN_BUFFER_NS;
	ab->max_buffer_ns = DEFAULT_MAX_BUFFER_NS;
	ab->last_output_pts = -1;
}

void audio_buffer_free(struct audio_buffer *ab)
{
	for (int i = 0; i < AUDIO_BUF_MAX_FRAMES; i++)
		free_frame_data(&ab->frames[i]);
	memset(ab, 0, sizeof(*ab));
}

void audio_buffer_reset(struct audio_buffer *ab)
{
	int64_t min_ns = ab->min_buffer_ns;
	int64_t max_ns = ab->max_buffer_ns;
	audio_buffer_free(ab);
	audio_buffer_init(ab);
	ab->min_buffer_ns = min_ns;
	ab->max_buffer_ns = max_ns;
}

static int64_t frame_duration_ns(uint32_t samples, uint32_t sample_rate)
{
	if (sample_rate == 0)
		return 0;
	return (int64_t)samples * 1000000000LL / (int64_t)sample_rate;
}

static void recalc_buffered(struct audio_buffer *ab)
{
	int64_t total = 0;
	for (int i = 0; i < ab->count; i++) {
		int idx = (ab->read_pos + i) % AUDIO_BUF_MAX_FRAMES;
		struct audio_buf_frame *f = &ab->frames[idx];
		if (f->valid)
			total += frame_duration_ns(f->frames, f->sample_rate);
	}
	ab->total_buffered_ns = total;
}

bool audio_buffer_push(struct audio_buffer *ab, const uint8_t *const *data,
		       const size_t *data_sizes, uint32_t frames,
		       uint32_t sample_rate, uint32_t channels,
		       int format, int speakers, int64_t pts_ns)
{
	/* If buffer is full, drop oldest frame */
	if (ab->count >= AUDIO_BUF_MAX_FRAMES) {
		free_frame_data(&ab->frames[ab->read_pos]);
		ab->read_pos = (ab->read_pos + 1) % AUDIO_BUF_MAX_FRAMES;
		ab->count--;
		ab->frames_dropped++;
	}

	/* Also enforce max buffer duration — drop oldest if too much latency */
	while (ab->count > 0 && ab->total_buffered_ns > ab->max_buffer_ns) {
		free_frame_data(&ab->frames[ab->read_pos]);
		ab->read_pos = (ab->read_pos + 1) % AUDIO_BUF_MAX_FRAMES;
		ab->count--;
		ab->frames_dropped++;
		recalc_buffered(ab);
	}

	struct audio_buf_frame *f = &ab->frames[ab->write_pos];

	/* Copy audio data */
	int num_planes = 0;
	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
		if (data[i] && data_sizes[i] > 0) {
			/* Realloc if needed */
			if (f->data_size[i] < data_sizes[i]) {
				free(f->data[i]);
				f->data[i] = malloc(data_sizes[i]);
				if (!f->data[i]) {
					f->data_size[i] = 0;
					return false;
				}
			}
			memcpy(f->data[i], data[i], data_sizes[i]);
			f->data_size[i] = data_sizes[i];
			num_planes++;
		} else {
			/* Clear unused planes */
			if (f->data[i]) {
				free(f->data[i]);
				f->data[i] = NULL;
			}
			f->data_size[i] = 0;
		}
	}

	f->frames = frames;
	f->sample_rate = sample_rate;
	f->channels = channels;
	f->pts_ns = pts_ns;
	f->format = format;
	f->speakers = speakers;
	f->valid = true;

	ab->write_pos = (ab->write_pos + 1) % AUDIO_BUF_MAX_FRAMES;
	ab->count++;
	ab->frames_in++;

	/* Update buffered duration */
	ab->total_buffered_ns += frame_duration_ns(frames, sample_rate);

	/* Check if primed */
	if (!ab->primed && ab->total_buffered_ns >= ab->min_buffer_ns)
		ab->primed = true;

	return true;
}

bool audio_buffer_pop(struct audio_buffer *ab, struct audio_buf_frame **out)
{
	*out = NULL;

	if (ab->count == 0)
		return false;

	/* Don't output until primed */
	if (!ab->primed)
		return false;

	struct audio_buf_frame *f = &ab->frames[ab->read_pos];
	if (!f->valid)
		return false;

	*out = f;

	ab->last_output_pts = f->pts_ns;
	ab->total_buffered_ns -= frame_duration_ns(f->frames, f->sample_rate);
	if (ab->total_buffered_ns < 0)
		ab->total_buffered_ns = 0;

	ab->read_pos = (ab->read_pos + 1) % AUDIO_BUF_MAX_FRAMES;
	ab->count--;
	ab->frames_out++;

	/* Once primed, stay primed. Only audio_buffer_reset() clears this.
	 * Un-priming on empty caused a stutter cycle: fill 500ms → burst
	 * drain → silence while re-filling → repeat every 500ms. */

	return true;
}

int64_t audio_buffer_level_ns(const struct audio_buffer *ab)
{
	return ab->total_buffered_ns;
}

bool audio_buffer_is_ready(const struct audio_buffer *ab)
{
	return ab->primed;
}

double audio_buffer_fill_ratio(const struct audio_buffer *ab)
{
	if (ab->max_buffer_ns <= 0)
		return 0.0;
	double ratio = (double)ab->total_buffered_ns / (double)ab->max_buffer_ns;
	if (ratio > 1.0)
		ratio = 1.0;
	return ratio;
}
