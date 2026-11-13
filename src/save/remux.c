/* rr_remux: join OBS split-file MKV segments into one MP4 by stream copy.
 *
 * Shipped implementation of the rolling-replay plugin's save step.
 * It does NOT use the concat demuxer. Timestamps are regenerated per stream:
 *   - video: OBS output is constant frame rate, so every pts is snapped to the
 *     frame grid, and dts is rebuilt from decode order (MKV stores no dts);
 *   - audio: every packet is exactly frame_size samples, so timestamps are a
 *     running sample counter.
 * Each stream is therefore continuous across segment joins, with no drift.
 *
 * Why this is safe: OBS's ffmpeg_muxer writes every packet of every stream to
 * exactly one split file, in order, and restarts each stream's timestamps at 0
 * in every file (see ts_offset_update() in obs-ffmpeg-mux.c). So counting
 * packets is enough to rebuild the original timeline exactly.
 *
 * Tested with libavformat 60 (FFmpeg 6.1) and libavformat 62 (FFmpeg 8.1.2,
 * the version OBS 33.0.1 ships), on segments recorded by libobs 30.0.2 with
 * x264 (B-frames), SVT-AV1 and AAC, at 60 and 59.94 fps. HEVC was tested on
 * synthetic segments only. See docs/wp1-findings.md ("In-process rr_remux
 * results") and docs/STATUS.md for the results.
 *
 * On failure the partly written output file is left in place; the caller
 * deletes it. Build the test CLI with -DRR_REMUX_MAIN.
 *
 * License: GPL-2.0-or-later.
 */
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RR_MAX_STREAMS 8 /* 1 video + up to 6 audio, with headroom */

struct rr_remux_params {
	const char *const *segments; /* absolute paths, oldest first, all closed */
	int num_segments;
	const char *out_path; /* final .mp4 path; caller handles .part + rename */
	int fps_num, fps_den; /* OBS video fps at record time */
	double inpoint_sec;   /* seconds into segment 0 to start at; 0 = from its start */
	/* optional: return non-zero from cancel() to abort */
	int (*cancel)(void *ud);
	void (*progress)(void *ud, int segment_index, int num_segments);
	void *ud;
};

struct rr_remux_result {
	int64_t video_frames;
	int64_t audio_packets[RR_MAX_STREAMS];
	double duration_sec; /* video_frames / fps */
	int reorder_delay;   /* frames */
	int grid_warnings;   /* packets whose pts was not on the expected grid */
	int skipped_tail;    /* 1 if the last segment was an unreadable stub and was left out */
};

static void set_err(char *err, size_t errlen, const char *what, int code)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
	if (code)
		av_strerror(code, buf, sizeof(buf));
	snprintf(err, errlen, "%s%s%s", what, code ? ": " : "", buf);
}

/* Frame index of a video timestamp on the fps grid. */
static int64_t grid_index(int64_t ts, AVRational in_tb, AVRational frame_tb)
{
	return av_rescale_q_rnd(ts, in_tb, frame_tb, AV_ROUND_NEAR_INF);
}

/* One pass over a segment's video packets: reorder delay and the cut point. */
static int prescan_first(const char *path, int vidx, AVRational frame_tb, double inpoint_sec, int *delay,
			 int64_t *cut_frame)
{
	AVFormatContext *in = NULL;
	int ret = avformat_open_input(&in, path, NULL, NULL);
	if (ret < 0)
		return ret;
	AVPacket *pkt = av_packet_alloc();
	if (!pkt) {
		avformat_close_input(&in);
		return AVERROR(ENOMEM);
	}
	int64_t j = 0, best_key = 0, first_pts = AV_NOPTS_VALUE;
	int d = 0;
	int64_t want = (int64_t)floor(inpoint_sec * frame_tb.den / frame_tb.num + 1e-6);
	while ((ret = av_read_frame(in, pkt)) >= 0) {
		if (pkt->stream_index == vidx && pkt->pts != AV_NOPTS_VALUE) {
			if (first_pts == AV_NOPTS_VALUE)
				first_pts = pkt->pts;
			int64_t p = grid_index(pkt->pts - first_pts, in->streams[vidx]->time_base, frame_tb);
			if (j - p > d)
				d = (int)(j - p);
			if ((pkt->flags & AV_PKT_FLAG_KEY) && p <= want && p > best_key)
				best_key = p;
			j++;
		}
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);
	avformat_close_input(&in);
	*delay = d;
	*cut_frame = best_key;
	return ret == AVERROR_EOF ? 0 : ret;
}

int rr_remux(const struct rr_remux_params *p, struct rr_remux_result *res, char *err, size_t errlen)
{
	AVFormatContext *out = NULL, *in = NULL;
	AVPacket *pkt = NULL;
	int ret = 0, nb = 0, vidx = -1;
	int frame_size[RR_MAX_STREAMS] = {0};
	int sample_rate[RR_MAX_STREAMS] = {0};
	enum AVCodecID codec[RR_MAX_STREAMS];
	int64_t base[RR_MAX_STREAMS] = {0};  /* video: frames; audio: samples */
	int64_t count[RR_MAX_STREAMS] = {0}; /* packets seen in the current segment */
	int64_t first_pts[RR_MAX_STREAMS];   /* first pts of each stream in the current segment */
	AVRational frame_tb = av_make_q(p->fps_den, p->fps_num);
	int delay = 0;
	int64_t cut_frame = 0;
	int started_video = 0;

	memset(res, 0, sizeof(*res));
	if (p->num_segments < 1 || p->fps_num <= 0 || p->fps_den <= 0) {
		set_err(err, errlen, "bad parameters", 0);
		return AVERROR(EINVAL);
	}

	pkt = av_packet_alloc();
	if (!pkt)
		return AVERROR(ENOMEM);

	for (int s = 0; s < p->num_segments; s++) {
		if (p->cancel && p->cancel(p->ud)) {
			ret = AVERROR_EXIT;
			set_err(err, errlen, "cancelled", 0);
			goto fail;
		}
		if ((ret = avformat_open_input(&in, p->segments[s], NULL, NULL)) < 0) {
			/* OBS can leave an unreadable stub as the very last file when the
			 * output stops. Only that one may be left out; a missing middle
			 * segment would put a hole in the replay. */
			if (s > 0 && s == p->num_segments - 1) {
				res->skipped_tail = 1;
				ret = 0;
				break;
			}
			set_err(err, errlen, "open segment", ret);
			goto fail;
		}

		if (s == 0) {
			/* Only the first segment is probed; it fills video_delay,
			 * frame_size and colour information. */
			if ((ret = avformat_find_stream_info(in, NULL)) < 0) {
				set_err(err, errlen, "probe segment", ret);
				goto fail;
			}
			nb = (int)in->nb_streams;
			if (nb < 1 || nb > RR_MAX_STREAMS) {
				ret = AVERROR_INVALIDDATA;
				set_err(err, errlen, "unexpected stream count", 0);
				goto fail;
			}
			if ((ret = avformat_alloc_output_context2(&out, NULL, "mp4", p->out_path)) < 0) {
				set_err(err, errlen, "alloc output", ret);
				goto fail;
			}
			for (int i = 0; i < nb; i++) {
				AVStream *is = in->streams[i];
				AVStream *os = avformat_new_stream(out, NULL);
				if (!os) {
					ret = AVERROR(ENOMEM);
					goto fail;
				}
				if ((ret = avcodec_parameters_copy(os->codecpar, is->codecpar)) < 0)
					goto fail;
				os->codecpar->codec_tag = 0;
				codec[i] = is->codecpar->codec_id;
				if (is->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
					if (vidx >= 0) {
						ret = AVERROR_INVALIDDATA;
						set_err(err, errlen, "more than one video stream", 0);
						goto fail;
					}
					vidx = i;
					if (codec[i] == AV_CODEC_ID_HEVC)
						os->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
					os->time_base = frame_tb;
					os->avg_frame_rate = av_make_q(p->fps_num, p->fps_den);
					os->r_frame_rate = os->avg_frame_rate;
					delay = is->codecpar->video_delay;
				} else if (is->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
					sample_rate[i] = is->codecpar->sample_rate;
					frame_size[i] = is->codecpar->frame_size;
					if (frame_size[i] <= 0)
						frame_size[i] = codec[i] == AV_CODEC_ID_OPUS ? 960 : 1024;
					if (sample_rate[i] <= 0) {
						ret = AVERROR_INVALIDDATA;
						set_err(err, errlen, "audio stream without sample rate", 0);
						goto fail;
					}
					os->time_base = av_make_q(1, sample_rate[i]);
				} else {
					ret = AVERROR_INVALIDDATA;
					set_err(err, errlen, "unexpected stream type", 0);
					goto fail;
				}
			}
			if (vidx < 0) {
				ret = AVERROR_INVALIDDATA;
				set_err(err, errlen, "no video stream", 0);
				goto fail;
			}

			int scan_delay = 0;
			if ((ret = prescan_first(p->segments[0], vidx, frame_tb, p->inpoint_sec, &scan_delay,
						 &cut_frame)) < 0) {
				set_err(err, errlen, "prescan", ret);
				goto fail;
			}
			if (scan_delay > delay)
				delay = scan_delay;
			res->reorder_delay = delay;

			/* Start the output at the cut keyframe. Audio is shifted by the
			 * same amount of time, so A/V sync inside the segment is kept. */
			base[vidx] = -cut_frame;
			for (int i = 0; i < nb; i++) {
				if (i == vidx)
					continue;
				base[i] = -av_rescale_q_rnd(cut_frame, frame_tb, av_make_q(1, sample_rate[i]),
							    AV_ROUND_NEAR_INF);
			}

			if ((ret = avio_open(&out->pb, p->out_path, AVIO_FLAG_WRITE)) < 0) {
				set_err(err, errlen, "open output", ret);
				goto fail;
			}
			if ((ret = avformat_write_header(out, NULL)) < 0) {
				set_err(err, errlen, "write header", ret);
				goto fail;
			}
		} else {
			if ((int)in->nb_streams != nb) {
				ret = AVERROR_INVALIDDATA;
				set_err(err, errlen, "segment stream count changed", 0);
				goto fail;
			}
			for (int i = 0; i < nb; i++) {
				if (in->streams[i]->codecpar->codec_id != codec[i]) {
					ret = AVERROR_INVALIDDATA;
					set_err(err, errlen, "segment codec changed", 0);
					goto fail;
				}
			}
		}

		memset(count, 0, sizeof(count));
		for (int i = 0; i < RR_MAX_STREAMS; i++)
			first_pts[i] = AV_NOPTS_VALUE;
		while ((ret = av_read_frame(in, pkt)) >= 0) {
			int i = pkt->stream_index;
			if (i < 0 || i >= nb) {
				av_packet_unref(pkt);
				continue;
			}
			AVStream *is = in->streams[i];
			AVStream *os = out->streams[i];
			int64_t k = count[i]++;
			int64_t opts, odts, odur;

			if (i == vidx) {
				if (pkt->pts == AV_NOPTS_VALUE) {
					ret = AVERROR_INVALIDDATA;
					set_err(err, errlen, "video packet without pts", 0);
					av_packet_unref(pkt);
					goto fail;
				}
				/* OBS starts every segment's video at pts 0. Measuring from the
				 * segment's first packet keeps this correct for any input. */
				if (first_pts[i] == AV_NOPTS_VALUE)
					first_pts[i] = pkt->pts;
				int64_t rel = pkt->pts - first_pts[i];
				int64_t g = grid_index(rel, is->time_base, frame_tb);
				int64_t back = av_rescale_q(g, frame_tb, is->time_base);
				if (llabs(back - rel) > 1)
					res->grid_warnings++;
				opts = base[i] + g;
				odts = base[i] + k - delay;
				odur = 1;
				if (!started_video) {
					/* drop everything before the cut keyframe */
					if (!(pkt->flags & AV_PKT_FLAG_KEY) || g < cut_frame) {
						av_packet_unref(pkt);
						continue;
					}
					started_video = 1;
				}
				if (opts < 0) {
					/* a picture shown before the cut keyframe (open GOP); OBS
					 * encoders do not produce these, but never write one */
					av_packet_unref(pkt);
					continue;
				}
				if (odts > opts) {
					/* reorder delay was underestimated; keep the file valid */
					res->grid_warnings++;
					opts = odts;
				}
				res->video_frames++;
			} else {
				opts = odts = base[i] + k * frame_size[i];
				odur = frame_size[i];
				if (pkt->pts != AV_NOPTS_VALUE) {
					if (first_pts[i] == AV_NOPTS_VALUE)
						first_pts[i] = pkt->pts;
					int64_t expect = av_rescale_q(k * frame_size[i], av_make_q(1, sample_rate[i]),
								      is->time_base);
					if (llabs(expect - (pkt->pts - first_pts[i])) > 2)
						res->grid_warnings++;
				}
				if (opts < 0) { /* audio before the cut point */
					av_packet_unref(pkt);
					continue;
				}
				res->audio_packets[i]++;
			}

			pkt->pts =
				av_rescale_q(opts, i == vidx ? frame_tb : av_make_q(1, sample_rate[i]), os->time_base);
			pkt->dts =
				av_rescale_q(odts, i == vidx ? frame_tb : av_make_q(1, sample_rate[i]), os->time_base);
			pkt->duration =
				av_rescale_q(odur, i == vidx ? frame_tb : av_make_q(1, sample_rate[i]), os->time_base);
			pkt->pos = -1;
			ret = av_interleaved_write_frame(out, pkt); /* takes ownership of the packet data */
			if (ret < 0) {
				set_err(err, errlen, "write packet", ret);
				goto fail;
			}
		}
		if (ret != AVERROR_EOF) {
			set_err(err, errlen, "read segment", ret);
			goto fail;
		}
		ret = 0;

		/* Every packet of every stream is in exactly one segment, so each
		 * stream simply continues where it stopped. */
		for (int i = 0; i < nb; i++)
			base[i] += i == vidx ? count[i] : count[i] * frame_size[i];

		avformat_close_input(&in);
		if (p->progress)
			p->progress(p->ud, s + 1, p->num_segments);
	}

	if (!started_video) {
		ret = AVERROR_INVALIDDATA;
		set_err(err, errlen, "no video keyframe found", 0);
		goto fail;
	}
	if ((ret = av_write_trailer(out)) < 0) {
		set_err(err, errlen, "write trailer", ret);
		goto fail;
	}
	res->duration_sec = (double)res->video_frames * p->fps_den / p->fps_num;

fail:
	av_packet_free(&pkt);
	if (in)
		avformat_close_input(&in);
	if (out) {
		if (out->pb)
			avio_closep(&out->pb);
		avformat_free_context(out);
	}
	return ret;
}

#ifdef RR_REMUX_MAIN
/* remux_ref <out.mp4> <fps_num> <fps_den> <inpoint_sec> <seg1.mkv> [seg2.mkv ...] */
int main(int argc, char **argv)
{
	if (argc < 6) {
		fprintf(stderr, "usage: %s out.mp4 fps_num fps_den inpoint_sec seg...\n", argv[0]);
		return 2;
	}
	struct rr_remux_params p = {0};
	p.out_path = argv[1];
	p.fps_num = atoi(argv[2]);
	p.fps_den = atoi(argv[3]);
	p.inpoint_sec = atof(argv[4]);
	p.segments = (const char *const *)&argv[5];
	p.num_segments = argc - 5;
	struct rr_remux_result r;
	char err[256] = "";
	int ret = rr_remux(&p, &r, err, sizeof(err));
	if (ret < 0) {
		fprintf(stderr, "remux failed: %s\n", err);
		return 1;
	}
	printf("video_frames=%lld duration=%.6f delay=%d grid_warnings=%d skipped_tail=%d", (long long)r.video_frames,
	       r.duration_sec, r.reorder_delay, r.grid_warnings, r.skipped_tail);
	for (int i = 0; i < RR_MAX_STREAMS; i++)
		if (r.audio_packets[i])
			printf(" a%d=%lld", i, (long long)r.audio_packets[i]);
	printf("\n");
	return 0;
}
#endif
