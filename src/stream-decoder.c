#include "stream-decoder.h"

#include <libavdevice/avdevice.h>
#include <libavutil/mastering_display_metadata.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static inline uint64_t sd_gettime_ns(void)
{
	LARGE_INTEGER freq, cnt;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&cnt);
	return (uint64_t)(cnt.QuadPart * 1000000000ULL / freq.QuadPart);
}
#else
#include <time.h>
static inline uint64_t sd_gettime_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

static bool sd_initialized = false;

static int interrupt_callback(void *data)
{
	struct stream_decoder *sd = data;
	if (sd->kill)
		return 1;

	uint64_t ts = sd_gettime_ns();
	if ((ts - sd->interrupt_poll_ts) > 20000000) {
		sd->interrupt_poll_ts = ts;
		if (sd->kill)
			return 1;
	}
	return 0;
}

static uint16_t get_max_luminance(const AVStream *stream)
{
	uint32_t max_luminance = 0;
	for (int i = 0; i < stream->codecpar->nb_coded_side_data; i++) {
		const AVPacketSideData *sd = &stream->codecpar->coded_side_data[i];
		if (sd->type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA) {
			const AVMasteringDisplayMetadata *m =
				(AVMasteringDisplayMetadata *)sd->data;
			if (m->has_luminance)
				max_luminance = (uint32_t)(av_q2d(m->max_luminance) + 0.5);
		} else if (sd->type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL) {
			const AVContentLightMetadata *m =
				(AVContentLightMetadata *)sd->data;
			max_luminance = m->MaxCLL;
		}
	}
	return (uint16_t)max_luminance;
}

static bool init_decoder(struct stream_decode_ctx *ctx, AVFormatContext *fmt,
			 enum AVMediaType type, bool hw)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->audio = (type == AVMEDIA_TYPE_AUDIO);

	int ret = av_find_best_stream(fmt, type, -1, -1, NULL, 0);
	if (ret < 0)
		return false;

	ctx->stream = fmt->streams[ret];
	enum AVCodecID id = ctx->stream->codecpar->codec_id;

	if (type == AVMEDIA_TYPE_VIDEO)
		ctx->max_luminance = get_max_luminance(ctx->stream);

	ctx->codec = avcodec_find_decoder(id);
	if (!ctx->codec)
		return false;

	ctx->decoder = avcodec_alloc_context3(ctx->codec);
	if (!ctx->decoder)
		return false;

	ret = avcodec_parameters_to_context(ctx->decoder, ctx->stream->codecpar);
	if (ret < 0) {
		avcodec_free_context(&ctx->decoder);
		return false;
	}

	if (ctx->decoder->thread_count == 1 &&
	    id != AV_CODEC_ID_PNG && id != AV_CODEC_ID_TIFF &&
	    id != AV_CODEC_ID_JPEG2000 && id != AV_CODEC_ID_MPEG4 &&
	    id != AV_CODEC_ID_WEBP)
		ctx->decoder->thread_count = 0;

	ret = avcodec_open2(ctx->decoder, ctx->codec, NULL);
	if (ret < 0) {
		avcodec_free_context(&ctx->decoder);
		return false;
	}

	ctx->frame = av_frame_alloc();
	ctx->sw_frame = ctx->frame;
	if (!ctx->frame) {
		avcodec_free_context(&ctx->decoder);
		return false;
	}

	return true;
}

static void free_decoder(struct stream_decode_ctx *ctx)
{
	if (ctx->frame) {
		av_frame_free(&ctx->frame);
	}
	if (ctx->hw_frame) {
		av_frame_free(&ctx->hw_frame);
	}
	if (ctx->decoder) {
		avcodec_free_context(&ctx->decoder);
	}
	if (ctx->hw_ctx) {
		av_buffer_unref(&ctx->hw_ctx);
	}
	memset(ctx, 0, sizeof(*ctx));
}

struct stream_decoder *stream_decoder_create(
	const struct stream_decoder_info *info)
{
	if (!sd_initialized) {
		avdevice_register_all();
		avformat_network_init();
		sd_initialized = true;
	}

	struct stream_decoder *sd = calloc(1, sizeof(*sd));
	if (!sd)
		return NULL;

	sd->url = info->url ? strdup(info->url) : NULL;
	sd->format_name = info->format_name ? strdup(info->format_name) : NULL;
	sd->ffmpeg_options = info->ffmpeg_options ? strdup(info->ffmpeg_options) : NULL;
	sd->buffering = info->buffering_bytes;
	sd->hw_decode = info->hardware_decoding;
	sd->opaque = info->opaque;
	sd->video_cb = info->video_cb;
	sd->audio_cb = info->audio_cb;
	sd->stop_cb = info->stop_cb;

	/* Open format context */
	const AVInputFormat *format = NULL;
	if (sd->format_name && *sd->format_name) {
		format = av_find_input_format(sd->format_name);
	}

	AVDictionary *opts = NULL;
	if (sd->buffering > 0) {
		av_dict_set_int(&opts, "buffer_size", sd->buffering, 0);
	}

	if (sd->ffmpeg_options && *sd->ffmpeg_options) {
		av_dict_parse_string(&opts, sd->ffmpeg_options, "=", " ", 0);
	}

	sd->fmt_ctx = avformat_alloc_context();
	if (sd->buffering == 0) {
		sd->fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;
	}

	/* Set timeout and interrupt for network streams */
	av_dict_set(&opts, "stimeout", "30000000", 0);
	sd->fmt_ctx->interrupt_callback.callback = interrupt_callback;
	sd->fmt_ctx->interrupt_callback.opaque = sd;

	int ret = avformat_open_input(&sd->fmt_ctx, sd->url, format,
				      opts ? &opts : NULL);
	av_dict_free(&opts);

	if (ret < 0) {
		stream_decoder_destroy(sd);
		return NULL;
	}

	if (avformat_find_stream_info(sd->fmt_ctx, NULL) < 0) {
		stream_decoder_destroy(sd);
		return NULL;
	}

	sd->has_video = init_decoder(&sd->video, sd->fmt_ctx,
				     AVMEDIA_TYPE_VIDEO, sd->hw_decode);
	sd->has_audio = init_decoder(&sd->audio, sd->fmt_ctx,
				     AVMEDIA_TYPE_AUDIO, false);

	if (!sd->has_video && !sd->has_audio) {
		stream_decoder_destroy(sd);
		return NULL;
	}

	sd->running = true;
	return sd;
}

void stream_decoder_destroy(struct stream_decoder *sd)
{
	if (!sd)
		return;

	sd->kill = true;
	sd->running = false;

	free_decoder(&sd->video);
	free_decoder(&sd->audio);

	if (sd->fmt_ctx)
		avformat_close_input(&sd->fmt_ctx);

	free(sd->url);
	free(sd->format_name);
	free(sd->ffmpeg_options);
	free(sd);
}

static void deliver_video_frame(struct stream_decoder *sd,
				struct stream_decode_ctx *ctx)
{
	if (!sd->video_cb)
		return;

	AVFrame *f = ctx->sw_frame;

	struct decoded_video_frame vf;
	memset(&vf, 0, sizeof(vf));

	for (int i = 0; i < 4; i++) {
		vf.data[i] = f->data[i];
		vf.linesize[i] = f->linesize[i];
	}

	vf.width = f->width;
	vf.height = f->height;
	vf.format = f->format;
	vf.colorspace = f->colorspace;
	vf.color_range = f->color_range;
	vf.color_trc = f->color_trc;
	vf.color_primaries = f->color_primaries;
	vf.keyframe = !!(f->flags & AV_FRAME_FLAG_KEY);
	vf.max_luminance = ctx->max_luminance;

	/* Convert PTS to nanoseconds */
	if (f->best_effort_timestamp != AV_NOPTS_VALUE) {
		vf.pts_ns = av_rescale_q(f->best_effort_timestamp,
					 ctx->stream->time_base,
					 (AVRational){1, 1000000000});
	} else {
		vf.pts_ns = ctx->next_pts_ns;
	}

	/* Compute next PTS */
	int64_t duration = f->duration;
	if (duration > 0) {
		duration = av_rescale_q(duration, ctx->stream->time_base,
					(AVRational){1, 1000000000});
	} else {
		/* Estimate from frame rate */
		if (ctx->decoder->time_base.num > 0) {
			duration = av_rescale_q(ctx->decoder->time_base.num,
						ctx->decoder->time_base,
						(AVRational){1, 1000000000});
		} else {
			duration = 33333333LL; /* ~30fps fallback */
		}
	}

	ctx->last_pts_ns = vf.pts_ns;
	ctx->next_pts_ns = vf.pts_ns + duration;

	sd->video_cb(sd->opaque, &vf);
}

static void deliver_audio_frame(struct stream_decoder *sd,
				struct stream_decode_ctx *ctx)
{
	if (!sd->audio_cb)
		return;

	AVFrame *f = ctx->frame;

	struct decoded_audio_frame af;
	memset(&af, 0, sizeof(af));

	int planes = av_sample_fmt_is_planar(f->format)
			     ? f->ch_layout.nb_channels
			     : 1;
	int bytes_per_sample = av_get_bytes_per_sample(f->format);

	for (int i = 0; i < planes && i < 8; i++) {
		af.data[i] = f->data[i];
		if (av_sample_fmt_is_planar(f->format))
			af.data_size[i] = f->nb_samples * bytes_per_sample;
		else
			af.data_size[i] = f->nb_samples * bytes_per_sample *
					  f->ch_layout.nb_channels;
	}

	af.frames = f->nb_samples;
	af.sample_rate = f->sample_rate;
	af.channels = f->ch_layout.nb_channels;
	af.format = f->format;

	/* Convert PTS to nanoseconds */
	if (f->best_effort_timestamp != AV_NOPTS_VALUE) {
		af.pts_ns = av_rescale_q(f->best_effort_timestamp,
					 ctx->stream->time_base,
					 (AVRational){1, 1000000000});
	} else {
		af.pts_ns = ctx->next_pts_ns;
	}

	/* Compute next PTS from sample count */
	int64_t duration = (int64_t)f->nb_samples * 1000000000LL /
			   (int64_t)f->sample_rate;

	ctx->last_pts_ns = af.pts_ns;
	ctx->next_pts_ns = af.pts_ns + duration;

	sd->audio_cb(sd->opaque, &af);
}

static bool decode_packet(struct stream_decoder *sd,
			  struct stream_decode_ctx *ctx, AVPacket *pkt)
{
	int ret = avcodec_send_packet(ctx->decoder, pkt);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return false;

	while (ret >= 0 || ret == AVERROR(EAGAIN)) {
		ret = avcodec_receive_frame(ctx->decoder, ctx->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0)
			return false;

		/* For video: skip until first keyframe on network streams */
		if (!ctx->audio && !ctx->got_first_keyframe) {
			if (!(ctx->frame->flags & AV_FRAME_FLAG_KEY)) {
				av_frame_unref(ctx->frame);
				continue;
			}
			ctx->got_first_keyframe = true;
		}

		if (ctx->audio) {
			deliver_audio_frame(sd, ctx);
		} else {
			deliver_video_frame(sd, ctx);
		}

		av_frame_unref(ctx->frame);
	}

	return true;
}

bool stream_decoder_read_next(struct stream_decoder *sd)
{
	if (!sd->running || sd->kill)
		return false;

	AVPacket *pkt = av_packet_alloc();
	if (!pkt)
		return false;

	int ret = av_read_frame(sd->fmt_ctx, pkt);
	if (ret < 0) {
		av_packet_free(&pkt);
		if (ret == AVERROR_EOF || ret == AVERROR_EXIT) {
			sd->running = false;
			if (sd->stop_cb)
				sd->stop_cb(sd->opaque);
		}
		return ret == AVERROR_EOF;
	}

	/* Route packet to the correct decoder */
	if (sd->has_video &&
	    pkt->stream_index == sd->video.stream->index) {
		decode_packet(sd, &sd->video, pkt);
	} else if (sd->has_audio &&
		   pkt->stream_index == sd->audio.stream->index) {
		decode_packet(sd, &sd->audio, pkt);
	}

	av_packet_free(&pkt);
	return true;
}

void stream_decoder_request_stop(struct stream_decoder *sd)
{
	if (sd)
		sd->kill = true;
}

bool stream_decoder_should_stop(const struct stream_decoder *sd)
{
	return sd ? sd->kill : true;
}
