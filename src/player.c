// Shmees Media Center: playback (player.h).
//
// Threads: the read thread opens the file, reads packets, decodes the audio itself (it's cheap) and queues the video
// packets; the video thread decodes them through FFmpeg's frame threads, so slow video never starves the sound; the
// audio thread feeds the kernel's audio driver from a ring of samples; the UI thread takes the frame due now. One
// mutex guards the shared state and one condition wakes whoever waits on it.
//
// The clock: while audio plays it follows the samples the driver has taken; between them, and whenever the audio has
// nothing to give, it runs on with the wall clock, so a stall in one never stops the other.

#include "player.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/adler32.h>
#include <libavutil/channel_layout.h>
#include <libavutil/cpu.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>

#include "gfx.h"
#include "log.h"
#include "threads.h"
#include "xsdk/audio.h"

enum {
	QUEUE = 8,
	// Video packets read ahead: at most this many, and fewer once the sound is well ahead.
	PACKETS = 512,
	PACKETS_ENOUGH = 48,
	CHANNELS = 6,
	RATE = 48000,
	RING = RATE * 2,
	// The kernel plays a frame this many frames after it's queued.
	AHEAD_FRAMES = 8,
	// Every hardware thread: 2 to 5 (cores 1 and 2), then 1 and 0 (core 0), where the UI and the reading threads run
	// raised, so they still go first when they wake. On the console 5 threads decoded 1080p 20% faster than 4.
	DECODE_THREADS = 6,
	PRIME_FRAMES = 3,
	MAX_LOG_LINES = 60,
};

static const uint8_t DECODE_HW[DECODE_THREADS] = { 2, 3, 4, 5, 1, 0 };

// What the decoder gives up, step by step, when it can't keep up (player.h, PlayerStatus.level).
static const char *const LEVEL_NAMES[] = { "full quality", "no deblocking on B-frames", "no deblocking",
	"no deblocking, skipping B-frames" };
static const double LATENCY_S = (double)(AHEAD_FRAMES * XSDK_AUDIO_FRAME_SAMPLES) / RATE;
// How far audio may drift from the clock before samples are skipped or held back.
static const double AUDIO_SLACK_S = 0.08;

typedef struct {
	AVFrame *frame;
	double pts;
} Queued;

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool made;
	bool active;
	bool quit;
	PlayerState state;
	char path[300];
	double start_s;
	char error[160];
	// The decode thread's.
	pthread_t decode_thread;
	bool decode_made;
	AVFormatContext *fmt;
	AVCodecContext *vdec;
	AVCodecContext *adec;
	SwrContext *swr;
	int vstream;
	int astream;
	AVRational vtb;
	AVRational atb;
	double frame_duration;
	float *convert[CHANNELS];
	int convert_capacity;
	// Requests from the UI.
	bool seek_requested;
	double seek_to;
	bool paused;
	float volume;
	// Video packets waiting for the video thread, and a count the read thread bumps at each seek, so the video thread
	// knows to flush its decoder and drop what it was making.
	AVPacket *packets[PACKETS];
	int packet_head;
	int packet_count;
	bool packets_end;
	uint32_t serial;
	pthread_t video_thread;
	bool video_made;
	// Decoded video waiting to be shown.
	Queued queue[QUEUE];
	int queue_head;
	int queue_count;
	// Decoded audio, planar, at 48 kHz.
	float *ring[CHANNELS];
	uint64_t written;
	uint64_t read;
	double ring_base;
	bool ring_based;
	bool eof_demux;
	bool eof_video;
	bool eof_audio;
	// Frames before this are decoded but not shown: after a seek, from the keyframe before the target.
	double skip_until;
	bool first_shown;
	// The clock.
	double clock_pts;
	double clock_time;
	bool clock_running;
	// The audio thread.
	pthread_t audio_thread;
	bool audio_made;
	bool audio_quit;
	bool audio_out;
	// The UI's.
	GfxVideo video;
	double shown_pts;
	int frame_w;
	int frame_h;
	AVRational sar;
	// Status.
	double duration;
	bool has_video;
	bool has_audio;
	double fps;
	char video_codec[32];
	char audio_codec[32];
	uint32_t decoded;
	uint32_t shown;
	uint32_t dropped;
	uint32_t underruns;
	double window_start;
	uint32_t window_frames;
	float decode_fps;
	int level;
	double level_changed;
	double ahead_since;
	int threads;
	double last_stats_log;
	int log_lines;
} P = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void set_level(AVCodecContext *dec, int level);

// FFmpeg's AltiVec code: off unless the app turns it on (player_set_vector). The flags are read as each decoder
// opens, so they're set just before.
static bool vector_on;

static void apply_cpu_flags(bool vector)
{
	av_force_cpu_flags(vector ? -1 : 0);
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void wait_locked(double seconds)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	const long ns = (long)(seconds * 1e9);
	ts.tv_nsec += ns % 1000000000L;
	ts.tv_sec += ns / 1000000000L + ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;
	pthread_cond_timedwait(&P.cond, &P.lock, &ts);
}

static double clock_locked(void)
{
	if (!P.clock_running)
		return P.clock_pts;
	return P.clock_pts + (now_s() - P.clock_time);
}

static void set_clock_locked(double pts, bool running)
{
	P.clock_pts = pts;
	P.clock_time = now_s();
	P.clock_running = running;
}

static void fail_locked(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vsnprintf(P.error, sizeof P.error, format, ap);
	va_end(ap);
	P.state = PLAYER_FAILED;
	smc_log("player: %s", P.error);
	pthread_cond_broadcast(&P.cond);
}

static void ffmpeg_log(void *avcl, int level, const char *format, va_list ap)
{
	(void)avcl;
	if (level > AV_LOG_WARNING || P.log_lines >= MAX_LOG_LINES)
		return;
	P.log_lines++;
	char line[256];
	vsnprintf(line, sizeof line, format, ap);
	const size_t n = strlen(line);
	if (n && line[n - 1] == '\n')
		line[n - 1] = 0;
	smc_log("ffmpeg: %s", line);
}

// --------------------------------------------------------------------------------------------------------------------
// opening

static AVCodecContext *open_decoder(AVStream *stream, bool video, char *name, size_t name_size, int threads)
{
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	snprintf(name, name_size, "%s", avcodec_get_name(stream->codecpar->codec_id));
	if (!codec) {
		smc_log("player: no %s decoder for %s in this build", video ? "video" : "audio", name);
		return NULL;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return NULL;
	if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
		avcodec_free_context(&ctx);
		return NULL;
	}
	ctx->pkt_timebase = stream->time_base;
	if (video) {
		ctx->thread_count = threads;
		ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
		// Speedups that may bend the standard slightly where nobody would see it.
		ctx->flags2 |= AV_CODEC_FLAG2_FAST;
		threads_place_round(DECODE_HW, (uint32_t)threads);
	} else {
		ctx->thread_count = 1;
	}
	const int error = avcodec_open2(ctx, codec, NULL);
	if (video)
		threads_place_none();
	if (error < 0) {
		smc_log("player: opening the %s decoder failed: %s", name, av_err2str(error));
		avcodec_free_context(&ctx);
		return NULL;
	}
	return ctx;
}

static bool open_audio_convert(void)
{
	const int channels = P.adec->ch_layout.nb_channels;
	AVChannelLayout out;
	if (channels >= 6)
		out = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1;
	else
		out = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
	AVChannelLayout in;
	if (av_channel_layout_check(&P.adec->ch_layout) && P.adec->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_copy(&in, &P.adec->ch_layout);
	else
		av_channel_layout_default(&in, channels > 0 ? channels : 2);
	int error = swr_alloc_set_opts2(
		&P.swr, &out, AV_SAMPLE_FMT_FLTP, RATE, &in, P.adec->sample_fmt, P.adec->sample_rate, 0, NULL);
	av_channel_layout_uninit(&in);
	if (error >= 0)
		error = swr_init(P.swr);
	if (error < 0) {
		smc_log("player: the audio converter failed: %s", av_err2str(error));
		swr_free(&P.swr);
		return false;
	}
	return true;
}

static bool open_file(void)
{
	char url[320];
	// "hdd:/Movies/x.mp4" would read as a protocol called hdd: the file protocol takes the rest as the path.
	snprintf(url, sizeof url, "file:%s", P.path);
	int error = avformat_open_input(&P.fmt, url, NULL, NULL);
	if (error < 0) {
		pthread_mutex_lock(&P.lock);
		fail_locked("Can't open the file: %s", av_err2str(error));
		pthread_mutex_unlock(&P.lock);
		return false;
	}
	error = avformat_find_stream_info(P.fmt, NULL);
	if (error < 0)
		smc_log("player: stream info: %s", av_err2str(error));
	P.vstream = av_find_best_stream(P.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	P.astream = av_find_best_stream(P.fmt, AVMEDIA_TYPE_AUDIO, -1, P.vstream, NULL, 0);
	char vname[32] = "", aname[32] = "";
	if (P.vstream >= 0) {
		AVStream *s = P.fmt->streams[P.vstream];
		P.vdec = open_decoder(s, true, vname, sizeof vname, DECODE_THREADS);
		P.vtb = s->time_base;
		const AVRational rate = s->avg_frame_rate.num ? s->avg_frame_rate : s->r_frame_rate;
		P.fps = rate.num && rate.den ? av_q2d(rate) : 0.0;
		P.frame_duration = P.fps > 1.0 ? 1.0 / P.fps : 1.0 / 30.0;
		P.sar = s->sample_aspect_ratio.num ? s->sample_aspect_ratio : s->codecpar->sample_aspect_ratio;
	}
	if (P.astream >= 0) {
		AVStream *s = P.fmt->streams[P.astream];
		P.adec = open_decoder(s, false, aname, sizeof aname, 1);
		P.atb = s->time_base;
		if (P.adec && !open_audio_convert())
			avcodec_free_context(&P.adec);
	}
	pthread_mutex_lock(&P.lock);
	snprintf(P.video_codec, sizeof P.video_codec, "%s", vname);
	snprintf(P.audio_codec, sizeof P.audio_codec, "%s", aname);
	P.has_video = P.vdec != NULL;
	P.has_audio = P.adec != NULL;
	P.duration = P.fmt->duration > 0 ? (double)P.fmt->duration / AV_TIME_BASE : 0.0;
	P.threads = P.vdec ? P.vdec->thread_count : 0;
	if (!P.has_video && !P.has_audio) {
		if (P.vstream >= 0)
			fail_locked("This build can't decode %s video yet", vname);
		else
			fail_locked("No video or audio this build can play");
		pthread_mutex_unlock(&P.lock);
		return false;
	}
	if (P.vstream >= 0 && !P.has_video) {
		fail_locked("This build can't decode %s video yet", vname);
		pthread_mutex_unlock(&P.lock);
		return false;
	}
	// Bigger than 720p: the console can't decode H.264 at full quality in time (0.68-0.85x at 1080p30 on
	// 2026-11-18), so deblocking starts off and the ladder takes over from there.
	if (P.vdec && P.vdec->width * P.vdec->height > 1280 * 720 * 13 / 10 && P.vdec->codec_id == AV_CODEC_ID_H264) {
		P.level = 2;
		set_level(P.vdec, P.level);
	}
	P.state = PLAYER_BUFFERING;
	pthread_mutex_unlock(&P.lock);
	smc_log("player: %s: %s %dx%d %.3f fps, %s %d Hz %d ch, %.0f s, %d decode threads", P.path, vname,
		P.vdec ? P.vdec->width : 0, P.vdec ? P.vdec->height : 0, P.fps, aname, P.adec ? P.adec->sample_rate : 0,
		P.adec ? P.adec->ch_layout.nb_channels : 0, P.duration, P.threads);
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// decoding

static double frame_pts(const AVFrame *frame, AVRational tb, double fallback)
{
	const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
	if (ts == AV_NOPTS_VALUE)
		return fallback;
	double pts = (double)ts * av_q2d(tb);
	if (P.fmt->start_time != AV_NOPTS_VALUE)
		pts -= (double)P.fmt->start_time / AV_TIME_BASE;
	return pts;
}

// Quality steps: deblocking skipped on frames nothing refers to, then on all, then those frames not decoded at all.
static void set_level(AVCodecContext *dec, int level)
{
	dec->skip_loop_filter = level >= 2 ? AVDISCARD_ALL : level == 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
	dec->skip_frame = level >= 3 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}

// The video thread's: waits for room in the frame queue; false if a seek or close came first.
static bool push_frame(AVFrame *frame, double pts, uint32_t serial)
{
	pthread_mutex_lock(&P.lock);
	while (P.queue_count == QUEUE && !P.quit && P.serial == serial && P.state != PLAYER_FAILED)
		wait_locked(0.02);
	if (P.quit || P.serial != serial) {
		pthread_mutex_unlock(&P.lock);
		return false;
	}
	Queued *q = &P.queue[(P.queue_head + P.queue_count) % QUEUE];
	av_frame_move_ref(q->frame, frame);
	q->pts = pts;
	P.queue_count++;
	P.decoded++;
	P.window_frames++;
	const double now = now_s();
	if (now - P.window_start >= 1.0) {
		P.decode_fps = (float)(P.window_frames / (now - P.window_start));
		P.window_start = now;
		P.window_frames = 0;
	}
	// Behind: the oldest frame waiting is already late, so the decoder gives up a step of quality (set_level), no more
	// often than every 1.5 s. Well ahead for 6 s, it takes one back.
	const double clock = clock_locked();
	const bool behind = P.clock_running && P.queue[P.queue_head].pts < clock - 0.25;
	const bool ahead = P.clock_running && P.queue_count >= QUEUE / 2 && P.queue[P.queue_head].pts > clock + 0.05;
	if (!ahead)
		P.ahead_since = now;
	int change = 0;
	if (behind && P.level < 3 && now - P.level_changed > 1.5)
		change = 1;
	else if (ahead && P.level > 0 && now - P.ahead_since > 6.0 && now - P.level_changed > 6.0)
		change = -1;
	if (change) {
		P.level += change;
		P.level_changed = now;
		P.ahead_since = now;
	}
	const int level = P.level;
	pthread_cond_broadcast(&P.cond);
	pthread_mutex_unlock(&P.lock);
	if (change) {
		set_level(P.vdec, level);
		smc_log("player: video %s at %.1f s: %s", change > 0 ? "behind" : "ahead", clock, LEVEL_NAMES[level]);
	}
	return true;
}

// Decodes one packet, or with NULL drains the decoder; false once the decoder has given its last frame.
static bool decode_video(const AVPacket *packet, AVFrame *frame, uint32_t serial)
{
	int sent = avcodec_send_packet(P.vdec, packet);
	while (true) {
		const int r = avcodec_receive_frame(P.vdec, frame);
		if (r == AVERROR(EAGAIN)) {
			// The decoder was full when the packet came: it takes it now there's room.
			if (sent == AVERROR(EAGAIN) && packet) {
				sent = avcodec_send_packet(P.vdec, packet);
				continue;
			}
			return true;
		}
		if (r == AVERROR_EOF)
			return false;
		if (r < 0) {
			smc_log("player: video decode: %s", av_err2str(r));
			return true;
		}
		if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
			const char *name = av_get_pix_fmt_name((enum AVPixelFormat)frame->format);
			av_frame_unref(frame);
			pthread_mutex_lock(&P.lock);
			fail_locked("%s pictures aren't supported yet (8-bit 4:2:0 only)", name ? name : "These");
			pthread_mutex_unlock(&P.lock);
			return false;
		}
		const double pts = frame_pts(frame, P.vtb, P.shown_pts + P.frame_duration);
		if (pts < P.skip_until - P.frame_duration * 0.5) {
			av_frame_unref(frame);
			continue;
		}
		if (!push_frame(frame, pts, serial)) {
			av_frame_unref(frame);
			return true;
		}
	}
}

static void *video_main(void *argument)
{
	(void)argument;
	AVFrame *frame = av_frame_alloc();
	AVPacket *packet = av_packet_alloc();
	uint32_t serial = 0;
	bool drained = false;
	pthread_mutex_lock(&P.lock);
	serial = P.serial;
	while (!P.quit && P.state != PLAYER_FAILED) {
		if (P.serial != serial) {
			// A seek: what the decoder holds is from before it.
			serial = P.serial;
			drained = false;
			pthread_mutex_unlock(&P.lock);
			avcodec_flush_buffers(P.vdec);
			pthread_mutex_lock(&P.lock);
			continue;
		}
		if (P.packet_count) {
			av_packet_move_ref(packet, P.packets[P.packet_head]);
			P.packet_head = (P.packet_head + 1) % PACKETS;
			P.packet_count--;
			pthread_cond_broadcast(&P.cond);
			pthread_mutex_unlock(&P.lock);
			decode_video(packet, frame, serial);
			av_packet_unref(packet);
			pthread_mutex_lock(&P.lock);
			continue;
		}
		if (P.packets_end && !drained) {
			pthread_mutex_unlock(&P.lock);
			while (decode_video(NULL, frame, serial)) {
			}
			pthread_mutex_lock(&P.lock);
			drained = true;
			if (P.serial == serial) {
				P.eof_video = true;
				pthread_cond_broadcast(&P.cond);
			}
			continue;
		}
		wait_locked(0.02);
	}
	pthread_mutex_unlock(&P.lock);
	av_packet_free(&packet);
	av_frame_free(&frame);
	return NULL;
}

static void push_audio(float **planes, int samples, double pts)
{
	pthread_mutex_lock(&P.lock);
	if (!P.ring_based) {
		// The ring's first sample after opening or a seek: samples before the target are dropped.
		int skip = 0;
		if (pts < P.skip_until)
			skip = (int)((P.skip_until - pts) * RATE);
		if (skip >= samples) {
			pthread_mutex_unlock(&P.lock);
			return;
		}
		P.ring_base = pts + (double)skip / RATE;
		P.ring_based = true;
		P.written = P.read = 0;
		for (int c = 0; c < CHANNELS; c++)
			if (planes[c])
				planes[c] += skip;
		samples -= skip;
	}
	int done = 0;
	const uint32_t serial = P.serial;
	while (done < samples) {
		while (P.written - P.read >= RING && !P.quit && P.serial == serial && P.state != PLAYER_FAILED)
			wait_locked(0.02);
		if (P.quit || P.serial != serial)
			break;
		const uint64_t space = RING - (P.written - P.read);
		int n = samples - done;
		if ((uint64_t)n > space)
			n = (int)space;
		for (int i = 0; i < n; i++) {
			const uint32_t at = (uint32_t)((P.written + (uint64_t)i) % RING);
			for (int c = 0; c < CHANNELS; c++)
				P.ring[c][at] = planes[c] ? planes[c][done + i] : 0.0f;
		}
		P.written += (uint64_t)n;
		done += n;
	}
	pthread_mutex_unlock(&P.lock);
}

static void decode_audio(const AVPacket *packet, AVFrame *frame)
{
	if (avcodec_send_packet(P.adec, packet) < 0 && packet)
		return;
	while (true) {
		const int r = avcodec_receive_frame(P.adec, frame);
		if (r == AVERROR(EAGAIN))
			break;
		if (r == AVERROR_EOF) {
			pthread_mutex_lock(&P.lock);
			P.eof_audio = true;
			pthread_cond_broadcast(&P.cond);
			pthread_mutex_unlock(&P.lock);
			return;
		}
		if (r < 0)
			break;
		const double pts = frame_pts(frame, P.atb, P.ring_base + (double)P.written / RATE);
		const int out = swr_get_out_samples(P.swr, frame->nb_samples);
		if (out > P.convert_capacity) {
			for (int c = 0; c < CHANNELS; c++) {
				av_free(P.convert[c]);
				P.convert[c] = av_malloc((size_t)out * sizeof(float));
			}
			P.convert_capacity = out;
		}
		// 5.1 fills all six of the driver's channels in its order; stereo fills front left and right.
		const int out_channels = P.adec->ch_layout.nb_channels >= 6 ? 6 : 2;
		uint8_t *outs[CHANNELS];
		for (int c = 0; c < CHANNELS; c++)
			outs[c] = (uint8_t *)P.convert[c];
		const int got = swr_convert(P.swr, outs, out, (const uint8_t **)frame->extended_data, frame->nb_samples);
		av_frame_unref(frame);
		if (got <= 0)
			continue;
		float *planes[CHANNELS] = { 0 };
		for (int c = 0; c < out_channels; c++)
			planes[c] = P.convert[c];
		push_audio(planes, got, pts);
	}
}

static void clear_queues_locked(void)
{
	while (P.queue_count) {
		av_frame_unref(P.queue[P.queue_head].frame);
		P.queue_head = (P.queue_head + 1) % QUEUE;
		P.queue_count--;
	}
	P.queue_head = 0;
	while (P.packet_count) {
		av_packet_unref(P.packets[P.packet_head]);
		P.packet_head = (P.packet_head + 1) % PACKETS;
		P.packet_count--;
	}
	P.packet_head = 0;
}

static void do_seek(double target)
{
	pthread_mutex_lock(&P.lock);
	P.serial++;
	clear_queues_locked();
	P.packets_end = false;
	P.written = P.read = 0;
	P.ring_based = false;
	P.eof_demux = P.eof_video = P.eof_audio = false;
	P.skip_until = target;
	P.first_shown = false;
	P.state = PLAYER_BUFFERING;
	set_clock_locked(target, false);
	pthread_cond_broadcast(&P.cond);
	pthread_mutex_unlock(&P.lock);
	int64_t ts = (int64_t)(target * AV_TIME_BASE);
	if (P.fmt->start_time != AV_NOPTS_VALUE)
		ts += P.fmt->start_time;
	const int error = avformat_seek_file(P.fmt, -1, INT64_MIN, ts, ts, 0);
	if (error < 0)
		smc_log("player: seeking to %.1f s: %s", target, av_err2str(error));
	if (P.adec) {
		avcodec_flush_buffers(P.adec);
		swr_init(P.swr);
	}
}

static bool start_thread(pthread_t *thread, void *(*entry)(void *), size_t stack);

static void *decode_main(void *argument)
{
	(void)argument;
	if (!open_file())
		return NULL;
	if (P.vdec)
		P.video_made = start_thread(&P.video_thread, video_main, 256 * 1024);
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	if (P.start_s > 1.0)
		do_seek(P.start_s);
	pthread_mutex_lock(&P.lock);
	P.window_start = now_s();
	while (!P.quit && P.state != PLAYER_FAILED) {
		if (P.seek_requested) {
			const double target = P.seek_to;
			P.seek_requested = false;
			pthread_mutex_unlock(&P.lock);
			do_seek(target);
			pthread_mutex_lock(&P.lock);
			continue;
		}
		// Read ahead far enough, then wait: the packet queue full, or the sound well ahead with video packets in hand.
		const double audio_ahead = (double)(P.written - P.read) / RATE;
		const bool packets_full = P.packet_count >= PACKETS - 1;
		const bool enough = P.has_audio ? audio_ahead > 1.0 && (!P.has_video || P.packet_count >= PACKETS_ENOUGH)
										: P.packet_count >= PACKETS_ENOUGH * 2;
		if (P.eof_demux || packets_full || enough) {
			wait_locked(0.02);
			continue;
		}
		pthread_mutex_unlock(&P.lock);
		const int r = av_read_frame(P.fmt, packet);
		if (r < 0) {
			if (r != AVERROR_EOF)
				smc_log("player: reading: %s", av_err2str(r));
			if (P.adec)
				decode_audio(NULL, frame);
			pthread_mutex_lock(&P.lock);
			P.eof_demux = true;
			P.packets_end = true;
			if (!P.vdec)
				P.eof_video = true;
			if (!P.adec)
				P.eof_audio = true;
			pthread_cond_broadcast(&P.cond);
			continue;
		}
		if (packet->stream_index == P.vstream && P.vdec) {
			pthread_mutex_lock(&P.lock);
			if (!P.seek_requested) {
				av_packet_move_ref(P.packets[(P.packet_head + P.packet_count) % PACKETS], packet);
				P.packet_count++;
				pthread_cond_broadcast(&P.cond);
			}
			pthread_mutex_unlock(&P.lock);
		} else if (packet->stream_index == P.astream && P.adec) {
			decode_audio(packet, frame);
		}
		av_packet_unref(packet);
		pthread_mutex_lock(&P.lock);
	}
	pthread_mutex_unlock(&P.lock);
	av_frame_free(&frame);
	av_packet_free(&packet);
	return NULL;
}

// --------------------------------------------------------------------------------------------------------------------
// audio out

static void *audio_main(void *argument)
{
	(void)argument;
	NTSTATUS status = xsdk_audio_open(AHEAD_FRAMES);
	if (!NT_SUCCESS(status)) {
		smc_log("player: opening audio failed: 0x%08X", (unsigned)status);
		return NULL;
	}
	pthread_mutex_lock(&P.lock);
	P.audio_out = true;
	pthread_mutex_unlock(&P.lock);
	while (true) {
		float *out = NULL;
		status = xsdk_audio_next_frame(100, &out);
		pthread_mutex_lock(&P.lock);
		const bool quit = P.audio_quit;
		pthread_mutex_unlock(&P.lock);
		if (quit)
			break;
		if (!NT_SUCCESS(status) || !out)
			continue;
		pthread_mutex_lock(&P.lock);
		int n = 0;
		if (P.state == PLAYER_PLAYING && P.ring_based) {
			// The samples queued now are heard LATENCY_S from now.
			const double due = clock_locked() + LATENCY_S;
			double head = P.ring_base + (double)P.read / RATE;
			uint64_t available = P.written - P.read;
			if (head < due - AUDIO_SLACK_S && available) {
				// Behind the picture (after a stall): drop what's late.
				uint64_t skip = (uint64_t)((due - head) * RATE);
				if (skip > available)
					skip = available;
				P.read += skip;
				available -= skip;
				head = P.ring_base + (double)P.read / RATE;
			}
			if (head <= due + AUDIO_SLACK_S && available) {
				n = available < XSDK_AUDIO_FRAME_SAMPLES ? (int)available : XSDK_AUDIO_FRAME_SAMPLES;
				const float v = P.volume;
				for (int c = 0; c < CHANNELS; c++) {
					float *dst = out + c * XSDK_AUDIO_FRAME_SAMPLES;
					for (int i = 0; i < n; i++)
						dst[i] = P.ring[c][(P.read + (uint64_t)i) % RING] * v;
				}
				P.read += (uint64_t)n;
				// The clock follows what's heard.
				set_clock_locked(head - LATENCY_S + (double)n / RATE, true);
				pthread_cond_broadcast(&P.cond);
			}
			if (n < XSDK_AUDIO_FRAME_SAMPLES && !P.eof_audio && head <= due + AUDIO_SLACK_S)
				P.underruns++;
		}
		for (int c = 0; c < CHANNELS; c++)
			memset(out + c * XSDK_AUDIO_FRAME_SAMPLES + n, 0, (size_t)(XSDK_AUDIO_FRAME_SAMPLES - n) * sizeof(float));
		pthread_mutex_unlock(&P.lock);
		xsdk_audio_queue_frame();
	}
	xsdk_audio_close();
	return NULL;
}

// --------------------------------------------------------------------------------------------------------------------
// the UI's side

static bool start_thread(pthread_t *thread, void *(*entry)(void *), size_t stack)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stack);
	const int error = pthread_create(thread, &attr, entry, NULL);
	pthread_attr_destroy(&attr);
	if (error) {
		smc_log("player: can't start a thread: %d", error);
		return false;
	}
	threads_place(*thread, HW_IO);
	threads_set_priority(*thread, 1);
	return true;
}

static void free_all(void)
{
	clear_queues_locked();
	for (int i = 0; i < QUEUE; i++)
		av_frame_free(&P.queue[i].frame);
	for (int i = 0; i < PACKETS; i++)
		av_packet_free(&P.packets[i]);
	for (int c = 0; c < CHANNELS; c++) {
		free(P.ring[c]);
		P.ring[c] = NULL;
		av_free(P.convert[c]);
		P.convert[c] = NULL;
	}
	P.convert_capacity = 0;
	avcodec_free_context(&P.vdec);
	avcodec_free_context(&P.adec);
	swr_free(&P.swr);
	avformat_close_input(&P.fmt);
	gfx_video_free(&P.video);
}

void player_set_vector(bool on)
{
	vector_on = on;
	smc_log("player: vector unit %s", on ? "on" : "off");
}

bool player_vector(void)
{
	return vector_on;
}

bool player_open(const char *path, double start_s, uint32_t volume_percent)
{
	player_close();
	apply_cpu_flags(vector_on);
	av_log_set_callback(ffmpeg_log);
	pthread_mutex_lock(&P.lock);
	P.active = true;
	P.quit = false;
	P.state = PLAYER_OPENING;
	snprintf(P.path, sizeof P.path, "%s", path);
	P.start_s = start_s;
	P.error[0] = 0;
	P.paused = false;
	P.seek_requested = false;
	P.volume = (float)volume_percent / 100.0f;
	P.queue_head = P.queue_count = 0;
	P.packet_head = P.packet_count = 0;
	P.packets_end = false;
	P.serial = 0;
	P.video_made = false;
	P.written = P.read = 0;
	P.ring_based = false;
	P.eof_demux = P.eof_video = P.eof_audio = false;
	P.skip_until = start_s > 1.0 ? start_s : 0.0;
	P.first_shown = false;
	P.shown_pts = 0.0;
	P.frame_w = P.frame_h = 0;
	P.decoded = P.shown = P.dropped = P.underruns = 0;
	P.decode_fps = 0.0f;
	P.level = 0;
	P.level_changed = P.ahead_since = 0.0;
	P.has_video = P.has_audio = false;
	P.duration = 0.0;
	P.log_lines = 0;
	P.video_codec[0] = P.audio_codec[0] = 0;
	P.audio_quit = false;
	P.audio_out = false;
	set_clock_locked(P.skip_until, false);
	bool ok = true;
	for (int i = 0; i < QUEUE && ok; i++)
		ok = (P.queue[i].frame = av_frame_alloc()) != NULL;
	for (int i = 0; i < PACKETS && ok; i++)
		ok = (P.packets[i] = av_packet_alloc()) != NULL;
	for (int c = 0; c < CHANNELS && ok; c++)
		ok = (P.ring[c] = calloc(RING, sizeof(float))) != NULL;
	if (!ok)
		fail_locked("Out of memory");
	pthread_mutex_unlock(&P.lock);
	if (!ok)
		return false;
	P.decode_made = start_thread(&P.decode_thread, decode_main, 512 * 1024);
	P.audio_made = start_thread(&P.audio_thread, audio_main, 64 * 1024);
	if (!P.decode_made) {
		pthread_mutex_lock(&P.lock);
		fail_locked("Couldn't start decoding");
		pthread_mutex_unlock(&P.lock);
		return false;
	}
	smc_log("player: opening %s at %.1f s", path, start_s);
	return true;
}

void player_close(void)
{
	if (!P.active)
		return;
	pthread_mutex_lock(&P.lock);
	P.quit = true;
	P.audio_quit = true;
	pthread_cond_broadcast(&P.cond);
	pthread_mutex_unlock(&P.lock);
	if (P.decode_made)
		pthread_join(P.decode_thread, NULL);
	if (P.video_made)
		pthread_join(P.video_thread, NULL);
	if (P.audio_made)
		pthread_join(P.audio_thread, NULL);
	P.decode_made = P.audio_made = P.video_made = false;
	pthread_mutex_lock(&P.lock);
	smc_log("player: closed: %u decoded, %u shown, %u dropped, %u audio underruns", (unsigned)P.decoded,
		(unsigned)P.shown, (unsigned)P.dropped, (unsigned)P.underruns);
	free_all();
	P.state = PLAYER_IDLE;
	P.active = false;
	pthread_mutex_unlock(&P.lock);
}

void player_set_paused(bool paused)
{
	pthread_mutex_lock(&P.lock);
	if (paused != P.paused) {
		P.paused = paused;
		if (P.state == PLAYER_PLAYING && paused) {
			set_clock_locked(clock_locked(), false);
			P.state = PLAYER_PAUSED;
		} else if (P.state == PLAYER_PAUSED && !paused) {
			set_clock_locked(P.clock_pts, true);
			P.state = PLAYER_PLAYING;
		}
	}
	pthread_mutex_unlock(&P.lock);
}

void player_seek(double position_s)
{
	pthread_mutex_lock(&P.lock);
	if (P.duration > 0.0 && position_s > P.duration - 1.0)
		position_s = P.duration - 1.0;
	if (position_s < 0.0)
		position_s = 0.0;
	P.seek_requested = true;
	P.seek_to = position_s;
	smc_log("player: seek to %.1f s", position_s);
	// The bar moves at once; the picture follows when the decoder gets there.
	set_clock_locked(position_s, false);
	if (P.state == PLAYER_ENDED || P.state == PLAYER_PLAYING || P.state == PLAYER_PAUSED)
		P.state = PLAYER_BUFFERING;
	pthread_cond_broadcast(&P.cond);
	pthread_mutex_unlock(&P.lock);
}

void player_set_volume(uint32_t volume_percent)
{
	pthread_mutex_lock(&P.lock);
	P.volume = (float)volume_percent / 100.0f;
	pthread_mutex_unlock(&P.lock);
}

void player_status(PlayerStatus *s)
{
	memset(s, 0, sizeof *s);
	pthread_mutex_lock(&P.lock);
	s->state = P.state;
	double position = P.state == PLAYER_BUFFERING || !P.first_shown ? P.clock_pts : clock_locked();
	if (P.duration > 0.0 && position > P.duration)
		position = P.duration;
	s->position = position < 0.0 ? 0.0 : position;
	s->duration = P.duration;
	s->has_video = P.has_video;
	s->has_audio = P.has_audio;
	s->width = P.frame_w;
	s->height = P.frame_h;
	s->fps = P.fps;
	snprintf(s->video_codec, sizeof s->video_codec, "%s", P.video_codec);
	snprintf(s->audio_codec, sizeof s->audio_codec, "%s", P.audio_codec);
	snprintf(s->error, sizeof s->error, "%s", P.error);
	s->decoded = P.decoded;
	s->shown = P.shown;
	s->dropped = P.dropped;
	s->audio_underruns = P.underruns;
	s->decode_fps = P.decode_fps;
	s->queued_frames = (uint32_t)P.queue_count;
	s->audio_buffered_s = (float)(P.written - P.read) / RATE;
	s->threads = P.threads;
	s->level = P.level;
	snprintf(s->level_name, sizeof s->level_name, "%s", LEVEL_NAMES[P.level]);
	pthread_mutex_unlock(&P.lock);
}

void player_update(void)
{
	if (!P.active)
		return;
	AVFrame *take = NULL;
	double take_pts = 0.0;
	pthread_mutex_lock(&P.lock);
	if (P.state == PLAYER_BUFFERING) {
		const double buffered = (double)(P.written - P.read) / RATE;
		const bool video_ready = !P.has_video || P.queue_count >= PRIME_FRAMES || P.eof_video || P.queue_count == QUEUE;
		const bool audio_ready = !P.has_audio || buffered >= 0.25 || P.eof_audio || P.written - P.read >= RING;
		if (video_ready && audio_ready && !P.seek_requested) {
			double start = P.skip_until;
			if (P.has_video && P.queue_count)
				start = P.queue[P.queue_head].pts;
			else if (P.has_audio && P.ring_based)
				start = P.ring_base;
			set_clock_locked(start, !P.paused);
			P.state = P.paused ? PLAYER_PAUSED : PLAYER_PLAYING;
		}
	}
	if (P.has_video && P.state != PLAYER_FAILED && P.state != PLAYER_OPENING) {
		const double now = clock_locked();
		while (P.queue_count) {
			Queued *head = &P.queue[P.queue_head];
			const bool due = head->pts <= now + 0.008 || !P.first_shown;
			if (!due)
				break;
			const bool next_due = P.queue_count >= 2 && P.queue[(P.queue_head + 1) % QUEUE].pts <= now &&
								  P.first_shown && P.clock_running;
			take = av_frame_alloc();
			if (!take)
				break;
			av_frame_move_ref(take, head->frame);
			take_pts = head->pts;
			P.queue_head = (P.queue_head + 1) % QUEUE;
			P.queue_count--;
			pthread_cond_broadcast(&P.cond);
			if (next_due) {
				// A later frame is due too: this one is too late to show.
				av_frame_free(&take);
				P.dropped++;
				continue;
			}
			break;
		}
	}
	if (P.state == PLAYER_PLAYING && P.eof_demux && P.eof_video && P.eof_audio && P.queue_count == 0 &&
		(!P.has_audio || P.written == P.read || !P.audio_out) && !take) {
		P.state = PLAYER_ENDED;
		smc_log("player: ended at %.1f s", clock_locked());
	}
	const double now = now_s();
	if (P.state == PLAYER_PLAYING && now - P.last_stats_log >= 10.0) {
		P.last_stats_log = now;
		smc_log("player: at %.1f s: decoding %.1f fps, %u shown, %u dropped, queue %d, audio %.2f s, %u underruns%s%s",
			clock_locked(), (double)P.decode_fps, (unsigned)P.shown, (unsigned)P.dropped, P.queue_count,
			(double)(P.written - P.read) / RATE, (unsigned)P.underruns, P.level ? ", " : "",
			P.level ? LEVEL_NAMES[P.level] : "");
	}
	pthread_mutex_unlock(&P.lock);
	if (!take)
		return;
	// The textures follow the frame's size; a change (rare) rebuilds them.
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((enum AVPixelFormat)take->format);
	const int cw = AV_CEIL_RSHIFT(take->width, desc ? desc->log2_chroma_w : 1);
	const int ch = AV_CEIL_RSHIFT(take->height, desc ? desc->log2_chroma_h : 1);
	if (!P.video.valid || (int)P.video.width != take->width || (int)P.video.height != take->height) {
		gfx_video_free(&P.video);
		if (!gfx_video_create(&P.video, (uint32_t)take->width, (uint32_t)take->height, (uint32_t)cw, (uint32_t)ch))
			smc_log("player: no textures for %dx%d video", take->width, take->height);
	}
	const bool bt709 = take->colorspace == AVCOL_SPC_BT709 ||
					   (take->colorspace == AVCOL_SPC_UNSPECIFIED && take->height >= 600);
	const bool full = take->color_range == AVCOL_RANGE_JPEG || take->format == AV_PIX_FMT_YUVJ420P;
	const uint8_t *planes[3] = { take->data[0], take->data[1], take->data[2] };
	gfx_video_upload(&P.video, planes, take->linesize, bt709, full);
	pthread_mutex_lock(&P.lock);
	P.shown_pts = take_pts;
	P.first_shown = true;
	P.frame_w = take->width;
	P.frame_h = take->height;
	if (take->sample_aspect_ratio.num)
		P.sar = take->sample_aspect_ratio;
	P.shown++;
	pthread_mutex_unlock(&P.lock);
	av_frame_free(&take);
}

bool player_draw(float x, float y, float w, float h)
{
	if (!P.video.valid || !P.video.ready || !P.frame_w || !P.frame_h)
		return false;
	double aspect = (double)P.frame_w / P.frame_h;
	if (P.sar.num > 0 && P.sar.den > 0)
		aspect *= av_q2d(P.sar);
	float dw = w;
	float dh = (float)(w / aspect);
	if (dh > h) {
		dh = h;
		dw = (float)(h * aspect);
	}
	gfx_video_draw(&P.video, x + (w - dw) * 0.5f, y + (h - dh) * 0.5f, dw, dh, 0xFFFFFFFF);
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// the benchmark

static struct {
	pthread_mutex_t lock;
	PlayerBenchmark result;
	char path[300];
	double seconds;
	int threads;
	int level;
	bool vector;
	pthread_t thread;
	bool made;
} B = { .lock = PTHREAD_MUTEX_INITIALIZER };

// A checksum of a decoded picture's three planes, row by row: equal checksums, the same picture.
static uint32_t frame_checksum(const AVFrame *frame)
{
	unsigned long sum = 1;
	for (int p = 0; p < 3 && frame->data[p]; p++) {
		const int w = p ? AV_CEIL_RSHIFT(frame->width, 1) : frame->width;
		const int h = p ? AV_CEIL_RSHIFT(frame->height, 1) : frame->height;
		for (int y = 0; y < h; y++)
			sum = av_adler32_update(sum, frame->data[p] + (ptrdiff_t)y * frame->linesize[p], (size_t)w);
	}
	return (uint32_t)sum;
}

static void *bench_main(void *argument)
{
	(void)argument;
	AVFormatContext *fmt = NULL;
	AVCodecContext *dec = NULL;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	char url[320];
	snprintf(url, sizeof url, "file:%s", B.path);
	char error[128] = "";
	uint32_t frames = 0;
	double elapsed = 0.0;
	int stream = -1;
	int r = avformat_open_input(&fmt, url, NULL, NULL);
	if (r < 0)
		snprintf(error, sizeof error, "open: %s", av_err2str(r));
	if (!error[0]) {
		avformat_find_stream_info(fmt, NULL);
		stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
		if (stream < 0)
			snprintf(error, sizeof error, "no video stream");
	}
	if (!error[0]) {
		char name[32];
		apply_cpu_flags(B.vector);
		dec = open_decoder(fmt->streams[stream], true, name, sizeof name, B.threads);
		apply_cpu_flags(vector_on);
		if (dec)
			set_level(dec, B.level);
		pthread_mutex_lock(&B.lock);
		snprintf(B.result.codec, sizeof B.result.codec, "%s", name);
		B.result.width = fmt->streams[stream]->codecpar->width;
		B.result.height = fmt->streams[stream]->codecpar->height;
		const AVRational rate = fmt->streams[stream]->avg_frame_rate;
		B.result.file_fps = rate.num && rate.den ? av_q2d(rate) : 0.0;
		pthread_mutex_unlock(&B.lock);
		if (!dec)
			snprintf(error, sizeof error, "no decoder for %s", name);
	}
	if (!error[0]) {
		const double start = now_s();
		bool eof = false;
		while (!eof && (elapsed = now_s() - start) < B.seconds) {
			r = av_read_frame(fmt, packet);
			if (r < 0) {
				avcodec_send_packet(dec, NULL);
				eof = true;
			} else if (packet->stream_index == stream) {
				avcodec_send_packet(dec, packet);
			}
			av_packet_unref(packet);
			while (avcodec_receive_frame(dec, frame) == 0) {
				if (frames < PLAYER_BENCH_CHECKSUMS)
					B.result.checksums[frames] = frame_checksum(frame);
				frames++;
				av_frame_unref(frame);
			}
			pthread_mutex_lock(&B.lock);
			B.result.frames = frames;
			B.result.seconds = elapsed;
			pthread_mutex_unlock(&B.lock);
		}
		elapsed = now_s() - start;
	}
	avcodec_free_context(&dec);
	avformat_close_input(&fmt);
	av_frame_free(&frame);
	av_packet_free(&packet);
	pthread_mutex_lock(&B.lock);
	B.result.frames = frames;
	B.result.checksum_count = frames < PLAYER_BENCH_CHECKSUMS ? frames : PLAYER_BENCH_CHECKSUMS;
	B.result.seconds = elapsed;
	B.result.fps = elapsed > 0.0 ? frames / elapsed : 0.0;
	snprintf(B.result.error, sizeof B.result.error, "%s", error);
	B.result.running = false;
	B.result.done = true;
	smc_log("benchmark: %s [%s%s]: %s %dx%d (%.3f fps file): %u frames in %.2f s = %.2f fps, %.2fx real time%s%s",
		B.result.file, B.result.label, B.vector ? ", vector unit" : "", B.result.codec, B.result.width, B.result.height, B.result.file_fps, (unsigned)frames, elapsed,
		B.result.fps, B.result.file_fps > 0.0 ? B.result.fps / B.result.file_fps : 0.0, error[0] ? ": " : "", error);
	pthread_mutex_unlock(&B.lock);
	return NULL;
}

bool player_benchmark_start(const char *path, double seconds, int threads, int level, bool vector, const char *label)
{
	pthread_mutex_lock(&B.lock);
	const bool running = B.result.running;
	pthread_mutex_unlock(&B.lock);
	if (running)
		return false;
	if (B.made) {
		pthread_join(B.thread, NULL);
		B.made = false;
	}
	av_log_set_callback(ffmpeg_log);
	P.log_lines = 0;
	memset(&B.result, 0, sizeof B.result);
	snprintf(B.path, sizeof B.path, "%s", path);
	const char *slash = strrchr(path, '/');
	snprintf(B.result.file, sizeof B.result.file, "%s", slash ? slash + 1 : path);
	B.seconds = seconds;
	B.threads = threads;
	B.level = level;
	B.vector = vector;
	snprintf(B.result.label, sizeof B.result.label, "%s", label);
	B.result.threads = threads;
	B.result.level = level;
	B.result.vector = vector;
	B.result.running = true;
	B.made = start_thread(&B.thread, bench_main, 512 * 1024);
	if (!B.made)
		B.result.running = false;
	return B.made;
}

void player_benchmark_result(PlayerBenchmark *result)
{
	pthread_mutex_lock(&B.lock);
	*result = B.result;
	pthread_mutex_unlock(&B.lock);
}

int player_decode_threads(void)
{
	return DECODE_THREADS;
}
