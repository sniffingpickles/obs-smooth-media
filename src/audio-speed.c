#include "audio-speed.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

static bool configure_converter(struct audio_speed_converter *converter,
				int sample_rate, int channels,
				enum AVSampleFormat format)
{
	if (converter->context && converter->sample_rate == sample_rate &&
	    converter->channels == channels && converter->format == format)
		return true;

	audio_speed_reset(converter);
	if (sample_rate <= 0 || sample_rate > 768000 || channels <= 0 ||
	    channels > AUDIO_SPEED_MAX_PLANES ||
	    av_get_bytes_per_sample(format) <= 0)
		return false;

	AVChannelLayout layout;
	av_channel_layout_default(&layout, channels);
	struct SwrContext *context = NULL;
	int result = swr_alloc_set_opts2(&context, &layout, format, sample_rate,
					 &layout, format, sample_rate, 0, NULL);
	av_channel_layout_uninit(&layout);
	if (result < 0 || !context) {
		swr_free(&context);
		return false;
	}

	/* Keep the filter active at 1.0x so later compensation changes do not
	 * require reinitializing the resampler in the middle of playback. */
	if (av_opt_set_int(context, "flags", SWR_FLAG_RESAMPLE, 0) < 0 ||
	    swr_init(context) < 0) {
		swr_free(&context);
		return false;
	}

	converter->context = context;
	converter->sample_rate = sample_rate;
	converter->channels = channels;
	converter->format = format;
	return true;
}

void audio_speed_init(struct audio_speed_converter *converter)
{
	if (!converter)
		return;
	memset(converter, 0, sizeof(*converter));
	converter->format = AV_SAMPLE_FMT_NONE;
}

void audio_speed_reset(struct audio_speed_converter *converter)
{
	if (!converter)
		return;
	swr_free(&converter->context);
	converter->format = AV_SAMPLE_FMT_NONE;
	converter->sample_rate = 0;
	converter->channels = 0;
}

void audio_speed_free(struct audio_speed_converter *converter)
{
	if (!converter)
		return;
	audio_speed_reset(converter);
	av_freep(&converter->storage);
	converter->storage_capacity = 0;
}

bool audio_speed_convert(struct audio_speed_converter *converter,
			 const uint8_t *const *input, uint32_t input_frames,
			 int sample_rate, int channels,
			 enum AVSampleFormat format, double speed,
			 struct audio_speed_frame *output)
{
	if (!converter || !input || !input[0] || !output || input_frames == 0 ||
	    !isfinite(speed) || speed < 0.90 || speed > 1.10 ||
	    !configure_converter(converter, sample_rate, channels, format))
		return false;

	memset(output, 0, sizeof(*output));
	double desired_double = (double)input_frames / speed;
	if (desired_double < 1.0 || desired_double > (double)INT_MAX)
		return false;
	int desired_frames = (int)(desired_double + 0.5);
	int input_count = (int)input_frames;

	if (swr_set_compensation(converter->context,
				 desired_frames - input_count,
				 desired_frames) < 0)
		return false;

	int output_capacity =
		swr_get_out_samples(converter->context, input_count);
	if (output_capacity < desired_frames)
		output_capacity = desired_frames;
	if (output_capacity <= 0 || output_capacity > INT_MAX - 64)
		return false;
	output_capacity += 64;

	int line_size = 0;
	int required = av_samples_get_buffer_size(&line_size, channels,
						  output_capacity, format, 1);
	if (required <= 0)
		return false;

	if ((size_t)required > converter->storage_capacity) {
		uint8_t *storage =
			av_realloc(converter->storage, (size_t)required);
		if (!storage)
			return false;
		converter->storage = storage;
		converter->storage_capacity = (size_t)required;
	}

	uint8_t *planes[AUDIO_SPEED_MAX_PLANES] = {0};
	if (av_samples_fill_arrays(planes, &line_size, converter->storage,
				   channels, output_capacity, format, 1) < 0)
		return false;

	int converted = swr_convert(converter->context, planes, output_capacity,
				    input, input_count);
	if (converted <= 0) {
		audio_speed_reset(converter);
		return false;
	}

	int bytes_per_sample = av_get_bytes_per_sample(format);
	bool planar = av_sample_fmt_is_planar(format) != 0;
	int plane_count = planar ? channels : 1;
	for (int plane = 0; plane < plane_count; plane++) {
		size_t samples = (size_t)converted;
		size_t bytes = (size_t)bytes_per_sample;
		size_t samples_per_frame = planar ? 1U : (size_t)channels;
		if (samples > SIZE_MAX / bytes ||
		    samples * bytes > SIZE_MAX / samples_per_frame) {
			audio_speed_reset(converter);
			return false;
		}
		output->data[plane] = planes[plane];
		output->data_size[plane] = samples * bytes * samples_per_frame;
	}
	output->frames = (uint32_t)converted;
	return true;
}
