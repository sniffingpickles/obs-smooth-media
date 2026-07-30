#include "stream-decoder.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <libavdevice/avdevice.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool sd_initialized = false;
static pthread_mutex_t sd_init_mutex = PTHREAD_MUTEX_INITIALIZER;

#define DEFAULT_OPEN_TIMEOUT_US INT64_C(35000000)

static char *sd_strdup(const char *value)
{
	if (!value)
		return NULL;

	size_t size = strlen(value) + 1;
	char *copy = malloc(size);
	if (copy)
		memcpy(copy, value, size);
	return copy;
}

static int64_t saturating_add_i64(int64_t a, int64_t b)
{
	if (b > 0 && a > INT64_MAX - b)
		return INT64_MAX;
	if (b < 0 && a < INT64_MIN - b)
		return INT64_MIN;
	return a + b;
}

/* Aborts blocking FFmpeg I/O (av_read_frame, avformat_open_input) when the
 * source is being torn down. Called repeatedly by FFmpeg while it waits on
 * the network, so it must be cheap and non-blocking. */
static int interrupt_callback(void *data)
{
	struct stream_decoder *sd = data;
	if (os_atomic_load_bool(&sd->kill))
		return 1;
	/* Also honor the caller's external abort flag. This is what lets a
	 * blocking avformat_open_input be cancelled during teardown even
	 * though stream_decoder_create() hasn't returned a handle yet. */
	if (sd->abort_flag && os_atomic_load_bool(sd->abort_flag))
		return 1;
	if (sd->interrupt_deadline_us > 0 &&
	    av_gettime_relative() >= sd->interrupt_deadline_us)
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
	if (!ctx || !fmts)
		return AV_PIX_FMT_NONE;

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
		if (!ctx->decoder->hw_device_ctx) {
			av_buffer_unref(&ctx->hw_ctx);
			continue;
		}
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
	if (!stream || !stream->codecpar)
		return 0;

	uint32_t max_luminance = 0;
	for (int i = 0; i < stream->codecpar->nb_coded_side_data; i++) {
		const AVPacketSideData *sd = &stream->codecpar->coded_side_data[i];
		if (!sd->data)
			continue;
		if (sd->type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA &&
		    sd->size >= sizeof(AVMasteringDisplayMetadata)) {
			const AVMasteringDisplayMetadata *m =
				(AVMasteringDisplayMetadata *)sd->data;
			if (m->has_luminance) {
				double value = av_q2d(m->max_luminance);
				if (value > 0.0 && value < 65536.0)
					max_luminance =
						(uint32_t)(value + 0.5);
				else if (value >= 65536.0)
					max_luminance = UINT16_MAX;
			}
		} else if (sd->type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL &&
			   sd->size >= sizeof(AVContentLightMetadata)) {
			const AVContentLightMetadata *m =
				(AVContentLightMetadata *)sd->data;
			max_luminance = m->MaxCLL;
		}
	}
	if (max_luminance > UINT16_MAX)
		max_luminance = UINT16_MAX;
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

#ifdef SMOOTH_MEDIA_TSAN_SERIAL_FFMPEG
	/* Distribution FFmpeg libraries are not built with our TSan runtime.
	 * Keep their internal slice workers out of this test-only build so TSan
	 * remains focused on the plugin's own threads and synchronization. */
	ctx->decoder->thread_count = 1;
#else
	if (ctx->decoder->thread_count == 1 &&
	    id != AV_CODEC_ID_PNG && id != AV_CODEC_ID_TIFF &&
	    id != AV_CODEC_ID_JPEG2000 && id != AV_CODEC_ID_MPEG4 &&
	    id != AV_CODEC_ID_WEBP)
		ctx->decoder->thread_count = 0;
#endif

	/* Hardware decoding is video-only and best-effort: if no GPU device
	 * can be attached, try_enable_hw_decode() leaves us in software mode. */
	if (type == AVMEDIA_TYPE_VIDEO && hw) {
		try_enable_hw_decode(ctx);
		if (!ctx->hw)
			blog(LOG_WARNING,
			     "[obs-smooth-media] Hardware video decoding unavailable; using software");
	}

	ret = avcodec_open2(ctx->decoder, ctx->codec, NULL);
	if (ret < 0) {
		bool retry_software = ctx->hw;
		if (retry_software)
			blog(LOG_WARNING,
			     "[obs-smooth-media] Hardware decoder open failed; retrying in software");
		free_decoder(ctx);
		if (retry_software)
			return init_decoder(ctx, fmt, type, false);
		return false;
	}
	if (ctx->hw) {
		const char *pixel_format =
			av_get_pix_fmt_name(ctx->hw_pix_fmt);
		blog(LOG_INFO,
		     "[obs-smooth-media] Hardware video decoding enabled (pixel format: %s)",
		     pixel_format ? pixel_format : "unknown");
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
	if (!info || !info->url || !*info->url) {
		if (info && info->open_result)
			*info->open_result = AVERROR(EINVAL);
		return NULL;
	}

	if (info->open_result)
		*info->open_result = 0;

	pthread_mutex_lock(&sd_init_mutex);
	if (!sd_initialized) {
		avdevice_register_all();
		if (avformat_network_init() < 0) {
			pthread_mutex_unlock(&sd_init_mutex);
			if (info->open_result)
				*info->open_result = AVERROR(EIO);
			return NULL;
		}
		sd_initialized = true;
	}
	pthread_mutex_unlock(&sd_init_mutex);

	struct stream_decoder *sd = calloc(1, sizeof(*sd));
	if (!sd) {
		if (info->open_result)
			*info->open_result = AVERROR(ENOMEM);
		return NULL;
	}

	sd->url = sd_strdup(info->url);
	sd->format_name = sd_strdup(info->format_name);
	sd->ffmpeg_options = sd_strdup(info->ffmpeg_options);
	if (!sd->url || (info->format_name && !sd->format_name) ||
	    (info->ffmpeg_options && !sd->ffmpeg_options)) {
		if (info->open_result)
			*info->open_result = AVERROR(ENOMEM);
		stream_decoder_destroy(sd);
		return NULL;
	}
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
		if (!format) {
			if (info->open_result)
				*info->open_result =
					AVERROR_DEMUXER_NOT_FOUND;
			stream_decoder_destroy(sd);
			return NULL;
		}
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
		int parse_ret = av_dict_parse_string(
			&opts, sd->ffmpeg_options, "=", " ", 0);
		if (parse_ret < 0) {
			if (info->open_result)
				*info->open_result = parse_ret;
			av_dict_free(&opts);
			stream_decoder_destroy(sd);
			return NULL;
		}
	}

	sd->fmt_ctx = avformat_alloc_context();
	if (!sd->fmt_ctx) {
		if (info->open_result)
			*info->open_result = AVERROR(ENOMEM);
		av_dict_free(&opts);
		stream_decoder_destroy(sd);
		return NULL;
	}
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

	int64_t open_timeout_us = DEFAULT_OPEN_TIMEOUT_US;
	if (info->open_timeout_ms > 0) {
		if ((int64_t)info->open_timeout_ms >
		    INT64_MAX / INT64_C(1000))
			open_timeout_us = INT64_MAX;
		else
			open_timeout_us =
				(int64_t)info->open_timeout_ms * INT64_C(1000);
	}
	sd->interrupt_deadline_us = saturating_add_i64(
		av_gettime_relative(), open_timeout_us);

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
	sd->interrupt_deadline_us = 0;

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

	sd->packet = av_packet_alloc();
	if (!sd->packet) {
		if (info->open_result)
			*info->open_result = AVERROR(ENOMEM);
		stream_decoder_destroy(sd);
		return NULL;
	}

	os_atomic_set_bool(&sd->running, true);
	return sd;
}

void stream_decoder_destroy(struct stream_decoder *sd)
{
	if (!sd)
		return;

	os_atomic_set_bool(&sd->kill, true);
	os_atomic_set_bool(&sd->running, false);

	free_decoder(&sd->video);
	free_decoder(&sd->audio);
	av_packet_free(&sd->packet);

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
	vf.colorspace = (int)f->colorspace;
	vf.color_range = (int)f->color_range;
	vf.color_trc = (int)f->color_trc;
	vf.color_primaries = (int)f->color_primaries;
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
		/* Estimate from the stream's guessed display rate. Decoder
		 * time_base is not necessarily one frame and the old calculation
		 * multiplied its numerator twice for non-unit numerators. */
		AVRational rate = av_guess_frame_rate(
			sd->fmt_ctx, ctx->stream, f);
		if (rate.num > 0 && rate.den > 0) {
			duration = av_rescale_q(
				1, (AVRational){rate.den, rate.num},
				(AVRational){1, 1000000000});
		} else {
			duration = 33333333LL; /* ~30fps fallback */
		}
	}

	ctx->last_pts_ns = vf.pts_ns;
	ctx->next_pts_ns = saturating_add_i64(vf.pts_ns, duration);

	sd->video_cb(sd->opaque, &vf);
}

static void deliver_audio_frame(struct stream_decoder *sd,
				struct stream_decode_ctx *ctx, AVFrame *f)
{
	if (!sd->audio_cb)
		return;

	if (f->nb_samples <= 0 || f->sample_rate <= 0 ||
	    f->sample_rate > 768000 ||
	    f->ch_layout.nb_channels <= 0 ||
	    f->ch_layout.nb_channels > 8)
		return;

	struct decoded_audio_frame af;
	memset(&af, 0, sizeof(af));

	int planes = av_sample_fmt_is_planar(f->format)
			     ? f->ch_layout.nb_channels
			     : 1;
	int bytes_per_sample = av_get_bytes_per_sample(f->format);
	if (bytes_per_sample <= 0 || planes <= 0 || planes > 8)
		return;

	for (int i = 0; i < planes && i < 8; i++) {
		if (!f->data[i])
			return;
		af.data[i] = f->data[i];
		size_t samples = (size_t)f->nb_samples;
		size_t bytes = (size_t)bytes_per_sample;
		size_t channels = av_sample_fmt_is_planar(f->format)
					  ? 1U
					  : (size_t)f->ch_layout.nb_channels;
		if (samples > SIZE_MAX / bytes ||
		    samples * bytes > SIZE_MAX / channels)
			return;
		af.data_size[i] = samples * bytes * channels;
	}

	af.frames = (uint32_t)f->nb_samples;
	af.sample_rate = (uint32_t)f->sample_rate;
	af.channels = (uint32_t)f->ch_layout.nb_channels;
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
	ctx->next_pts_ns = saturating_add_i64(af.pts_ns, duration);

	sd->audio_cb(sd->opaque, &af);
}

static bool receive_frames(struct stream_decoder *sd,
			   struct stream_decode_ctx *ctx)
{
	for (;;) {
		int ret = avcodec_receive_frame(ctx->decoder, ctx->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return true;
		if (ret < 0) {
			int64_t now = av_gettime_relative();
			if (now - ctx->last_error_log_us >= INT64_C(1000000)) {
				char error_text[AV_ERROR_MAX_STRING_SIZE];
				if (av_strerror(ret, error_text,
					       sizeof(error_text)) < 0)
					snprintf(error_text, sizeof(error_text),
						 "FFmpeg error %d", ret);
				blog(LOG_WARNING,
				     "[obs-smooth-media] %s decoder receive dropped a corrupt frame: %s (%d)",
				     ctx->audio ? "audio" : "video",
				     error_text, ret);
				ctx->last_error_log_us = now;
			}
			av_frame_unref(ctx->frame);
			/* A damaged live packet/frame is recoverable; allocation
			 * failure is not. Keep the transport and codec context so
			 * the next valid packet/keyframe can resume decoding. */
			return ret != AVERROR(ENOMEM);
		}

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
			if (av_frame_copy_props(ctx->sw_frame,
						ctx->frame) < 0) {
				av_frame_unref(ctx->sw_frame);
				av_frame_unref(ctx->frame);
				continue;
			}
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
}

static bool decode_packet(struct stream_decoder *sd,
			  struct stream_decode_ctx *ctx, AVPacket *pkt)
{
	int ret = avcodec_send_packet(ctx->decoder, pkt);
	if (ret == AVERROR(EAGAIN)) {
		if (!receive_frames(sd, ctx))
			return false;
		ret = avcodec_send_packet(ctx->decoder, pkt);
	}
	if (ret < 0 && ret != AVERROR_EOF) {
		int64_t now = av_gettime_relative();
		if (now - ctx->last_error_log_us >= INT64_C(1000000)) {
			char error_text[AV_ERROR_MAX_STRING_SIZE];
			if (av_strerror(ret, error_text,
				       sizeof(error_text)) < 0)
				snprintf(error_text, sizeof(error_text),
					 "FFmpeg error %d", ret);
			blog(LOG_WARNING,
			     "[obs-smooth-media] %s decoder rejected a corrupt packet: %s (%d)",
			     ctx->audio ? "audio" : "video",
			     error_text, ret);
			ctx->last_error_log_us = now;
		}
		return ret != AVERROR(ENOMEM);
	}

	return receive_frames(sd, ctx);
}

bool stream_decoder_read_next(struct stream_decoder *sd)
{
	if (!sd || !sd->fmt_ctx || !sd->packet ||
	    !os_atomic_load_bool(&sd->running) ||
	    os_atomic_load_bool(&sd->kill))
		return false;

	av_packet_unref(sd->packet);
	int ret = av_read_frame(sd->fmt_ctx, sd->packet);
	/* FFmpeg's librist protocol returns EAGAIN whenever its finite poll
	 * interval expires without a packet. That is an ordinary live-stream
	 * gap, not EOF: destroying the receiver here loses libRIST's peer and
	 * retransmission state, after which the same listener often cannot
	 * re-handshake. Other nonblocking protocols use EAGAIN the same way.
	 * Yield briefly so an immediately-nonblocking source cannot spin. */
	if (ret == AVERROR(EAGAIN)) {
		av_usleep(10000);
		return true;
	}
	if (ret < 0) {
		char error_text[AV_ERROR_MAX_STRING_SIZE];
		if (av_strerror(ret, error_text, sizeof(error_text)) < 0)
			snprintf(error_text, sizeof(error_text),
				 "FFmpeg error %d", ret);
		const char *protocol = "stream";
		if (sd->url) {
			if (strncmp(sd->url, "rist", 4) == 0)
				protocol = "RIST";
			else if (strncmp(sd->url, "srt", 3) == 0)
				protocol = "SRT";
			else if (strncmp(sd->url, "rtmp", 4) == 0)
				protocol = "RTMP";
		}
		/* Never log the URL: live URLs commonly contain credentials. */
		blog(LOG_WARNING,
		     "[obs-smooth-media] %s read failed: %s (%d)",
		     protocol, error_text, ret);
		if (ret == AVERROR_EOF) {
			bool ok = true;
			if (sd->has_video)
				ok = decode_packet(sd, &sd->video, NULL) && ok;
			if (sd->has_audio)
				ok = decode_packet(sd, &sd->audio, NULL) && ok;
			(void)ok;
		}
		os_atomic_set_bool(&sd->running, false);
		if (sd->stop_cb)
			sd->stop_cb(sd->opaque);
		return false;
	}

	/* Route packet to the correct decoder */
	bool decoded = true;
	if (sd->has_video &&
	    sd->packet->stream_index == sd->video.stream->index) {
		decoded = decode_packet(sd, &sd->video, sd->packet);
	} else if (sd->has_audio &&
		   sd->packet->stream_index == sd->audio.stream->index) {
		decoded = decode_packet(sd, &sd->audio, sd->packet);
	}

	if (!decoded)
		os_atomic_set_bool(&sd->running, false);
	return decoded;
}

void stream_decoder_request_stop(struct stream_decoder *sd)
{
	if (sd)
		os_atomic_set_bool(&sd->kill, true);
}

bool stream_decoder_should_stop(const struct stream_decoder *sd)
{
	return sd ? os_atomic_load_bool(&sd->kill) : true;
}

void stream_decoder_global_cleanup(void)
{
	pthread_mutex_lock(&sd_init_mutex);
	if (sd_initialized) {
		avformat_network_deinit();
		sd_initialized = false;
	}
	pthread_mutex_unlock(&sd_init_mutex);
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
