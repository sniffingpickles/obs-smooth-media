#include "stream-decoder.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <libavdevice/avdevice.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <stdlib.h>
#include <string.h>

static bool sd_initialized = false;

/* Aborts blocking FFmpeg I/O (av_read_frame, avformat_open_input) when the
 * source is being torn down. Called repeatedly by FFmpeg while it waits on
 * the network, so it must be cheap and non-blocking. */
static int interrupt_callback(void *data)
{
	struct stream_decoder *sd = data;
	if (sd->kill)
		return 1;
	/* Also honor the caller's external abort flag. This is what lets a
	 * blocking avformat_open_input be cancelled during teardown even
	 * though stream_decoder_create() hasn't returned a handle yet. */
	if (sd->abort_flag && *sd->abort_flag)
		return 1;
	return 0;
}

/* get_format callback: tell the decoder to use our negotiated GPU surface
 * format when offered, otherwise fall back to the first software format so
 * decoding continues on the CPU instead of failing outright. */
static enum AVPixelFormat sd_get_hw_format(AVCodecContext *avctx,
					   const enum AVPixelFormat *fmts)
{
	struct stream_decode_ctx *ctx = avctx->opaque;

	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == ctx->hw_pix_fmt)
			return *p;
	}

	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
		const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
		if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL))
			return *p;
	}

	return fmts[0];
}

/* Try to attach a hardware device to the decoder. On any failure we leave
 * ctx in software mode (ctx->hw stays false) so decoding still works. */
static void try_enable_hw_decode(struct stream_decode_ctx *ctx)
{
	for (int i = 0;; i++) {
		const AVCodecHWConfig *cfg = avcodec_get_hw_config(ctx->codec, i);
		if (!cfg)
			break;
		if (!(cfg->methods &
		      AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
			continue;

		AVBufferRef *hw_ctx = NULL;
		if (av_hwdevice_ctx_create(&hw_ctx, cfg->device_type, NULL,
					   NULL, 0) < 0)
			continue;

		ctx->hw_ctx = hw_ctx;
		ctx->hw_pix_fmt = cfg->pix_fmt;
		ctx->decoder->hw_device_ctx = av_buffer_ref(hw_ctx);
		ctx->decoder->opaque = ctx;
		ctx->decoder->get_format = sd_get_hw_format;
		ctx->sw_frame = av_frame_alloc();
		if (!ctx->sw_frame) {
			av_buffer_unref(&ctx->decoder->hw_device_ctx);
			av_buffer_unref(&ctx->hw_ctx);
			ctx->decoder->get_format = NULL;
			ctx->decoder->opaque = NULL;
			return;
		}
		ctx->hw = true;
		return;
	}
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

static void free_decoder(struct stream_decode_ctx *ctx);

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

	/* Hardware decoding is video-only and best-effort: if no GPU device
	 * can be attached, try_enable_hw_decode() leaves us in software mode. */
	if (type == AVMEDIA_TYPE_VIDEO && hw)
		try_enable_hw_decode(ctx);

	ret = avcodec_open2(ctx->decoder, ctx->codec, NULL);
	if (ret < 0) {
		free_decoder(ctx);
		return false;
	}

	ctx->frame = av_frame_alloc();
	if (!ctx->frame) {
		free_decoder(ctx);
		return false;
	}
	if (!ctx->hw)
		ctx->sw_frame = ctx->frame;

	return true;
}

static void free_decoder(struct stream_decode_ctx *ctx)
{
	/* In software mode sw_frame aliases frame; only free it separately
	 * when hardware decoding allocated a distinct download target. */
	if (ctx->sw_frame && ctx->sw_frame != ctx->frame)
		av_frame_free(&ctx->sw_frame);
	if (ctx->frame)
		av_frame_free(&ctx->frame);
	if (ctx->decoder)
		avcodec_free_context(&ctx->decoder);
	if (ctx->hw_ctx)
		av_buffer_unref(&ctx->hw_ctx);
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
	sd->abort_flag = info->abort_flag;
	sd->video_cb = info->video_cb;
	sd->audio_cb = info->audio_cb;
	sd->stop_cb = info->stop_cb;

	/* Open format context */
	const AVInputFormat *format = NULL;
	if (sd->format_name && *sd->format_name) {
		format = av_find_input_format(sd->format_name);
	}

	AVDictionary *opts = NULL;
	/* "buffer_size" is the UDP/RTP socket receive buffer in BYTES. Do NOT
	 * set it for RIST: there the option means milliseconds (valid range
	 * 0–30000), so a byte value like 2 MB is out of range and makes librist
	 * reject the connection — i.e. RIST playback fails to open. It's
	 * harmlessly ignored by SRT/RTMP, but skip it for RIST entirely. */
	bool is_rist = sd->url && strncmp(sd->url, "rist", 4) == 0;
	if (sd->buffering > 0 && !is_rist) {
		av_dict_set_int(&opts, "buffer_size", sd->buffering, 0);
	}

	if (sd->ffmpeg_options && *sd->ffmpeg_options) {
		av_dict_parse_string(&opts, sd->ffmpeg_options, "=", " ", 0);
	}

	sd->fmt_ctx = avformat_alloc_context();
	if (sd->buffering == 0) {
		sd->fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;
	}

	/* Set protocol-appropriate timeouts (microseconds) for network streams.
	 *
	 * IMPORTANT: do NOT set "timeout" for RTMP. FFmpeg's native RTMP
	 * protocol treats "timeout" as the *listen* timeout and it IMPLIES
	 * listen (server) mode — the connection then tries to bind() to the
	 * remote address and fails with "can't assign requested address"
	 * (WSAEADDRNOTAVAIL on Windows). For RTMP we use the AVIO-level
	 * rw_timeout instead, which is the client read/write timeout and does
	 * not change the connection mode. For SRT/RIST, "timeout" is the
	 * (correct) connection timeout. */
	if (sd->url) {
		if (strncmp(sd->url, "rtmp", 4) == 0) {
			av_dict_set(&opts, "rw_timeout", "30000000", 0);
		} else if (strncmp(sd->url, "srt", 3) == 0) {
			av_dict_set(&opts, "timeout", "30000000", 0);
		} else if (strncmp(sd->url, "rist", 4) == 0) {
			av_dict_set(&opts, "timeout", "30000000", 0);
		}
	}
	sd->fmt_ctx->interrupt_callback.callback = interrupt_callback;
	sd->fmt_ctx->interrupt_callback.opaque = sd;

	int ret = avformat_open_input(&sd->fmt_ctx, sd->url, format,
				      opts ? &opts : NULL);
	av_dict_free(&opts);

	if (ret < 0) {
		if (info->open_result)
			*info->open_result = ret;
		stream_decoder_destroy(sd);
		return NULL;
	}

	ret = avformat_find_stream_info(sd->fmt_ctx, NULL);
	if (ret < 0) {
		if (info->open_result)
			*info->open_result = ret;
		stream_decoder_destroy(sd);
		return NULL;
	}

	sd->has_video = init_decoder(&sd->video, sd->fmt_ctx,
				     AVMEDIA_TYPE_VIDEO, sd->hw_decode);
	sd->has_audio = init_decoder(&sd->audio, sd->fmt_ctx,
				     AVMEDIA_TYPE_AUDIO, false);

	if (!sd->has_video && !sd->has_audio) {
		if (info->open_result)
			*info->open_result = AVERROR_STREAM_NOT_FOUND;
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
				struct stream_decode_ctx *ctx, AVFrame *f)
{
	if (!sd->video_cb)
		return;

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
				struct stream_decode_ctx *ctx, AVFrame *f)
{
	if (!sd->audio_cb)
		return;

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

		/* If the frame lives in GPU memory, download it to system
		 * memory before handing it to OBS. On transfer failure we
		 * drop the frame rather than feed OBS a GPU surface. */
		AVFrame *out = ctx->frame;
		if (ctx->hw && ctx->frame->format == ctx->hw_pix_fmt) {
			av_frame_unref(ctx->sw_frame);
			if (av_hwframe_transfer_data(ctx->sw_frame, ctx->frame,
						     0) < 0) {
				av_frame_unref(ctx->frame);
				continue;
			}
			av_frame_copy_props(ctx->sw_frame, ctx->frame);
			out = ctx->sw_frame;
		}

		if (ctx->audio) {
			deliver_audio_frame(sd, ctx, out);
		} else {
			deliver_video_frame(sd, ctx, out);
		}

		if (out != ctx->frame)
			av_frame_unref(out);
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

void stream_decoder_global_cleanup(void)
{
	if (sd_initialized) {
		avformat_network_deinit();
		sd_initialized = false;
	}
}

void stream_decoder_log_protocols(void)
{
	struct dstr list = {0};
	void *opaque = NULL;
	const char *name;
	bool have_rist = false, have_srt = false, have_rtmp = false;

	while ((name = avio_enum_protocols(&opaque, 0)) != NULL) {
		if (list.len)
			dstr_cat(&list, ", ");
		dstr_cat(&list, name);
		if (strcmp(name, "rist") == 0)
			have_rist = true;
		else if (strcmp(name, "srt") == 0)
			have_srt = true;
		else if (strcmp(name, "rtmp") == 0)
			have_rtmp = true;
	}

	blog(LOG_INFO, "[obs-smooth-media] FFmpeg input protocols: %s",
	     list.array ? list.array : "(none)");
	blog(LOG_INFO,
	     "[obs-smooth-media] protocol support — RTMP:%s SRT:%s RIST:%s",
	     have_rtmp ? "yes" : "NO", have_srt ? "yes" : "NO",
	     have_rist ? "yes"
		       : "NO (librist not built into this OBS's FFmpeg)");
	dstr_free(&list);
}
