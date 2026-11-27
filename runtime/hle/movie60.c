// Kirby: 60 fps copies of the game's 30 fps movies, made in the background on the first launch from the player's own
// game files (FFmpeg's minterpolate: motion-interpolated in-betweens) into <data root>/cache/movie60/. movie.c plays
// a copy once it's there; until then the blended in-betweens. minterpolate runs on one core: each movie is cut into
// parts (half the cores) made at the same time at low priority, then joined with the original's audio.
// Each part reads half a second more (its last in-betweens need the next frame) and is cut to its exact length: parts
// a few frames short each put the picture ahead of the sound. LARECOMP_MOVIE60=0: none made or played.
#include "../rt.h"
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>  // (rmdir)
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define MINTERP "minterpolate=fps=60:mi_mode=mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1:scd=fdiff"

extern const char *rt_data_root(void);

typedef struct {
    const char *src, *out;
    double ss, len;  // seconds: where the part starts in the original, how long it is
    int frames;      // made (the join's offsets)
    bool ok;
} Part;

static void low_priority(void) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
}

// The best H.264 encoder this FFmpeg has (an LGPL build has no libx264: Media Foundation's), else MPEG-4 part 2
static const AVCodec *pick_encoder(enum AVPixelFormat *fmt) {
    static const char *names[] = {"libx264", "h264_mf", "mpeg4"};
    for (int i = 0; i < 3; i++) {
        const AVCodec *c = avcodec_find_encoder_by_name(names[i]);
        if (c) { *fmt = !strcmp(names[i], "h264_mf") ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P; return c; }
    }
    return NULL;
}

static int write_frame(AVCodecContext *enc, AVFrame *f, AVFormatContext *oc, AVStream *os, AVPacket *pkt) {
    int r = avcodec_send_frame(enc, f);
    while (r >= 0) {
        r = avcodec_receive_packet(enc, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
        if (r < 0) return r;
        av_packet_rescale_ts(pkt, enc->time_base, os->time_base);
        pkt->stream_index = 0;
        if ((r = av_interleaved_write_frame(oc, pkt)) < 0) return r;
    }
    return r == AVERROR_EOF ? 0 : r;
}

// One part: decode [ss, ss + len + 0.5) -> minterpolate -> the frames before len -> <out> (video only)
static void *part_thread(void *arg) {
    Part *p = arg;
    low_priority();
    AVFormatContext *ic = NULL, *oc = NULL;
    AVCodecContext *dec = NULL, *enc = NULL;
    AVFilterGraph *g = NULL;
    AVFrame *f = av_frame_alloc(), *ff = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc(), *opkt = av_packet_alloc();
    AVFilterContext *src = NULL, *sink = NULL;
    AVStream *os = NULL;
    int vs = -1;
    if (avformat_open_input(&ic, p->src, NULL, NULL) < 0 || avformat_find_stream_info(ic, NULL) < 0) goto done;
    const AVCodec *dc = NULL;
    if ((vs = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, &dc, 0)) < 0) goto done;
    AVStream *is = ic->streams[vs];
    dec = avcodec_alloc_context3(dc);
    avcodec_parameters_to_context(dec, is->codecpar);
    dec->pkt_timebase = is->time_base;
    if (avcodec_open2(dec, dc, NULL) < 0) goto done;

    enum AVPixelFormat efmt;
    const AVCodec *ec = pick_encoder(&efmt);
    if (!ec) goto done;
    {   // the filter graph: buffer -> minterpolate -> the encoder's format -> buffersink
        g = avfilter_graph_alloc();
        char args[256];
        snprintf(args, sizeof args, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1", dec->width,
                 dec->height, dec->pix_fmt, is->time_base.num, is->time_base.den);
        if (avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in", args, NULL, g) < 0 ||
            avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", NULL, NULL, g) < 0)
            goto done;
        AVFilterInOut *in = avfilter_inout_alloc(), *out = avfilter_inout_alloc();
        out->name = av_strdup("in"), out->filter_ctx = src;
        in->name = av_strdup("out"), in->filter_ctx = sink;
        char desc[256];
        snprintf(desc, sizeof desc, "%s,format=%s", MINTERP, av_get_pix_fmt_name(efmt));
        int r = avfilter_graph_parse_ptr(g, desc, &in, &out, NULL);
        avfilter_inout_free(&in), avfilter_inout_free(&out);
        if (r < 0 || avfilter_graph_config(g, NULL) < 0) goto done;
    }
    {   // the encoder (one thread: the other parts run beside it) and the part's file
        enc = avcodec_alloc_context3(ec);
        enc->width = dec->width, enc->height = dec->height, enc->pix_fmt = efmt;
        enc->time_base = av_buffersink_get_time_base(sink);
        enc->framerate = (AVRational){60, 1};
        enc->thread_count = 1;
        if (!strcmp(ec->name, "libx264")) av_opt_set(enc->priv_data, "crf", "16", 0), av_opt_set(enc->priv_data, "preset", "medium", 0);
        else if (!strcmp(ec->name, "mpeg4")) enc->flags |= AV_CODEC_FLAG_QSCALE, enc->global_quality = FF_QP2LAMBDA * 2;
        else enc->bit_rate = 30000000;
        avformat_alloc_output_context2(&oc, NULL, "mp4", p->out);
        if (!oc) goto done;
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(enc, ec, NULL) < 0) goto done;
        os = avformat_new_stream(oc, NULL);
        avcodec_parameters_from_context(os->codecpar, enc);
        os->time_base = enc->time_base;
        if (avio_open(&oc->pb, p->out, AVIO_FLAG_WRITE) < 0 || avformat_write_header(oc, NULL) < 0) goto done;
    }
    int64_t ss = (int64_t)(p->ss / av_q2d(is->time_base)), stop = (int64_t)((p->ss + p->len + 0.5) / av_q2d(is->time_base));
    int64_t keep = (int64_t)(p->len / av_q2d(enc->time_base) + 0.5);  // frames of 1/60 s before len
    if (p->ss > 0 && av_seek_frame(ic, vs, ss, AVSEEK_FLAG_BACKWARD) < 0) goto done;
    bool fed_all = false, closed = false;
    int err = 0;
    while (!err) {
        bool eof = false;
        if (!fed_all) {
            int r = av_read_frame(ic, pkt);
            if (r < 0) eof = true;
            else if (pkt->stream_index == vs) avcodec_send_packet(dec, pkt);  // (a bad packet: skipped)
            av_packet_unref(pkt);
            if (eof) avcodec_send_packet(dec, NULL);
        }
        while (!err) {  // decoded frames in range -> the graph (timestamps from the part's start)
            int r = avcodec_receive_frame(dec, f);
            if (r == AVERROR(EAGAIN)) break;
            if (r == AVERROR_EOF) { fed_all = true; break; }
            if (r < 0) { err = r; break; }
            int64_t t = f->best_effort_timestamp;
            if (t >= stop) fed_all = true;
            else if (t >= ss) {
                f->pts = t - ss;
                if (av_buffersrc_add_frame_flags(src, f, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) err = -1;
            }
            av_frame_unref(f);
            if (fed_all) break;
        }
        if (fed_all && !closed) (void)av_buffersrc_add_frame(src, NULL), closed = true;
        for (;;) {  // the graph's frames before len -> the encoder
            int r = av_buffersink_get_frame(sink, ff);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) { if (r == AVERROR_EOF) goto flush; break; }
            if (r < 0) { err = r; break; }
            if (ff->pts >= 0 && ff->pts < keep) {
                ff->pts = p->frames++;
                ff->pict_type = AV_PICTURE_TYPE_NONE;
                if (write_frame(enc, ff, oc, os, opkt) < 0) err = -1;
            }
            av_frame_unref(ff);
        }
    }
flush:
    if (!err && write_frame(enc, NULL, oc, os, opkt) >= 0 && av_write_trailer(oc) >= 0) p->ok = p->frames > 0;
done:
    if (oc) { if (oc->pb) avio_closep(&oc->pb); avformat_free_context(oc); }
    avfilter_graph_free(&g);
    avcodec_free_context(&enc), avcodec_free_context(&dec);
    avformat_close_input(&ic);
    av_frame_free(&f), av_frame_free(&ff), av_packet_free(&pkt), av_packet_free(&opkt);
    return NULL;
}

// The parts' video one after the other (offset by the frames before), and the original's audio as it is
static bool join(const char *src, Part *parts, int n, const char *out) {
    AVFormatContext *oc = NULL, *ac = NULL, *pc[64] = {0};
    bool ok = false;
    AVPacket *pkt = av_packet_alloc();
    avformat_alloc_output_context2(&oc, NULL, "mp4", out);
    if (!oc) goto done;
    for (int i = 0; i < n; i++)
        if (avformat_open_input(&pc[i], parts[i].out, NULL, NULL) < 0 || avformat_find_stream_info(pc[i], NULL) < 0) goto done;
    AVStream *vo = avformat_new_stream(oc, NULL);
    avcodec_parameters_copy(vo->codecpar, pc[0]->streams[0]->codecpar);
    vo->codecpar->codec_tag = 0;
    vo->time_base = (AVRational){1, 60};
    int as = -1;
    AVStream *ao = NULL;
    if (avformat_open_input(&ac, src, NULL, NULL) >= 0 && avformat_find_stream_info(ac, NULL) >= 0 &&
        (as = av_find_best_stream(ac, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0)) >= 0) {
        ao = avformat_new_stream(oc, NULL);
        avcodec_parameters_copy(ao->codecpar, ac->streams[as]->codecpar);
        ao->codecpar->codec_tag = 0;
        ao->time_base = ac->streams[as]->time_base;
    }
    AVDictionary *o = NULL;
    av_dict_set(&o, "movflags", "+faststart", 0);
    if (avio_open(&oc->pb, out, AVIO_FLAG_WRITE) < 0 || avformat_write_header(oc, &o) < 0) { av_dict_free(&o); goto done; }
    av_dict_free(&o);
    // video first, then audio: the muxer interleaves by time (faststart rewrites the index at the end)
    int64_t base = 0;  // frames before this part
    for (int i = 0; i < n; i++) {
        AVRational tb = pc[i]->streams[0]->time_base;
        int64_t off = av_rescale_q(base, (AVRational){1, 60}, tb);
        while (av_read_frame(pc[i], pkt) >= 0) {
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += off;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += off;
            av_packet_rescale_ts(pkt, tb, vo->time_base);
            pkt->stream_index = vo->index;
            pkt->pos = -1;
            if (av_interleaved_write_frame(oc, pkt) < 0) goto done;
        }
        base += parts[i].frames;
    }
    if (ao)
        while (av_read_frame(ac, pkt) >= 0) {
            if (pkt->stream_index != as) { av_packet_unref(pkt); continue; }
            av_packet_rescale_ts(pkt, ac->streams[as]->time_base, ao->time_base);
            pkt->stream_index = ao->index;
            pkt->pos = -1;
            if (av_interleaved_write_frame(oc, pkt) < 0) goto done;
        }
    ok = av_write_trailer(oc) >= 0;
done:
    if (oc) { if (oc->pb) avio_closep(&oc->pb); avformat_free_context(oc); }
    for (int i = 0; i < n; i++) avformat_close_input(&pc[i]);
    avformat_close_input(&ac);
    av_packet_free(&pkt);
    return ok;
}

static double duration_of(const char *path) {
    AVFormatContext *ic = NULL;
    double d = 0;
    if (avformat_open_input(&ic, path, NULL, NULL) >= 0 && avformat_find_stream_info(ic, NULL) >= 0 && ic->duration > 0)
        d = ic->duration / (double)AV_TIME_BASE;
    avformat_close_input(&ic);
    return d;
}

static bool make_one(const char *src, const char *dir, const char *name, int jobs) {
    char out[1100], tmp[1100];
    snprintf(out, sizeof out, "%s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s/%s.tmp", dir, name);
    double dur = duration_of(src);
    if (dur <= 0) return false;
    mkdir(tmp, 0755);
    Part parts[64];
    pthread_t th[64];
    char paths[64][1200];
    double part = dur / jobs;
    uint64_t t0 = SDL_GetTicks();
    LOG("[movie60] %s: %.1f s in %d parts, interpolating to 60 fps\n", name, dur, jobs);
    for (int i = 0; i < jobs; i++) {
        snprintf(paths[i], sizeof paths[i], "%s/p%d.mp4", tmp, i);
        parts[i] = (Part){src, paths[i], part * i, part, 0, false};
        pthread_create(&th[i], NULL, part_thread, &parts[i]);
    }
    bool ok = true;
    for (int i = 0; i < jobs; i++) pthread_join(th[i], NULL), ok &= parts[i].ok;
    char joined[1200];
    snprintf(joined, sizeof joined, "%s/joined.mp4", tmp);
    ok = ok && join(src, parts, jobs, joined) && rename(joined, out) == 0;  // (movie.c sees it only once whole)
    for (int i = 0; i < jobs; i++) remove(paths[i]);
    remove(joined);
    rmdir(tmp);
    LOG("[movie60] %s: %s in %.0f s\n", name, ok ? "done" : "FAILED", (SDL_GetTicks() - t0) / 1000.0);
    return ok;
}

static char g_src_dir[1024];
static void *movie60_thread(void *arg) {
    (void)arg;
    low_priority();
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/cache", rt_data_root());
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/cache/movie60", rt_data_root());
    mkdir(dir, 0755);
    int jobs = SDL_GetNumLogicalCPUCores() / 2;
    jobs = jobs < 1 ? 1 : jobs > 64 ? 64 : jobs;
    DIR *d = opendir(g_src_dir);
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".mp4")) continue;
        char src[2200], out[2200];
        snprintf(src, sizeof src, "%s/%s", g_src_dir, e->d_name);
        snprintf(out, sizeof out, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(out, &st) == 0) continue;
        make_one(src, dir, e->d_name, jobs);
    }
    closedir(d);
    return NULL;
}

// Once per run, from the first movie the game opens (runtime/hle/movie.c): the missing copies of <romfs>/mov/Product
void movie60_start(const char *romfs_dir) {
    static bool started;
    const char *e = getenv("LARECOMP_MOVIE60");
    if (started || (e && !atoi(e))) return;
    started = true;
    av_log_set_level(AV_LOG_ERROR);  // (libx264's statistics per part)
    snprintf(g_src_dir, sizeof g_src_dir, "%s/mov/Product", romfs_dir);
    pthread_t t;
    if (pthread_create(&t, NULL, movie60_thread, NULL) == 0) pthread_detach(t);
}
