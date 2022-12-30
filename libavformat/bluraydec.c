/*
 * BluRay demuxer
 * Copyright (c) 2022 Stan Ionascu <stan at ionascu dot xyz>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <libbluray/bluray.h>
#include "libavformat/avformat.h"
#include "libavformat/demux.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"

#define BLOCK_SIZE 2048
#define LBA_TO_BYTES(LBA) (((int64_t)(LBA))*BLOCK_SIZE)
#define TIME_BASE_HZ 90000

typedef struct {
    const AVClass *class;

    AVIOContext *pb;
    BLURAY *bd;
    int title;
    int min_title_length;

    /* slave demuxer */
    AVFormatContext *mpegts_ctx;
    const AVInputFormat *mpegts;

    /* slave demuxer buffer */
    uint8_t *buffer;
    size_t buffer_size;
} BlurayDemuxContext;

static int bluray_read_blocks(void *opaque, void *buf, int lba, int num_blocks)
{
    AVIOContext *pb = opaque;
    int64_t offset = LBA_TO_BYTES(lba);
    int read_bytes;
    int read_blocks = -1;

    /* move stream-pointer */
    if (avio_seek(pb, offset, SEEK_SET) >= 0) {
        int bytes_to_read = LBA_TO_BYTES(num_blocks);
        if ((read_bytes = avio_read(pb, buf, bytes_to_read)) >= 0) {
            read_blocks = read_bytes / BLOCK_SIZE;
        } else {
            av_log(pb, AV_LOG_ERROR, "failed to read %d bytes at offset %ld: %d\n", bytes_to_read, offset, read_bytes);
        }
    } else {
        av_log(pb, AV_LOG_ERROR, "failed to seek to %ld\n", offset);
    }
    return read_blocks;
}

static int bluray_read_bd_packet(void *opaque, uint8_t *buf, int buf_size)
{
    BlurayDemuxContext *ctx = opaque;
    int len;

    if (!ctx || !ctx->bd) {
        return AVERROR(EFAULT);
    }

    len = bd_read(ctx->bd, buf, buf_size);

    return len == 0 ? AVERROR_EOF : len;
}

static int64_t bluray_seek_bd(void *opaque, int64_t offset, int whence)
{
    BlurayDemuxContext *ctx = opaque;

    if (!ctx || !ctx->bd) {
        return AVERROR(EFAULT);
    }

    switch (whence) {
    case SEEK_SET:
    case SEEK_CUR:
    case SEEK_END:
        return bd_seek(ctx->bd, offset);

    case AVSEEK_SIZE:
        return bd_get_title_size(ctx->bd);
    }

    av_log(ctx, AV_LOG_ERROR, "Unsupported whence operation %d\n", whence);
    return AVERROR(EINVAL);
}

static int bluray_read_probe(const AVProbeData *p)
{
    /* skip if it's bluray protocol */
    if (av_stristr(p->filename, "BLURAY:") != NULL) {
        return 0;
    }
    /* TODO: probe for udf / bdmv / bdav folder? */
    return AVPROBE_SCORE_EXTENSION;
}

static BLURAY_STREAM_INFO *bluray_find_stream_info_by_pid(BLURAY_STREAM_INFO **bd_streams,
                                                          int bd_streams_count,
                                                          uint16_t pid)
{
    int i;
    for (i = 0; i < bd_streams_count; i++) {
        if (bd_streams[i]->pid == pid) {
            return bd_streams[i];
        }
    }
    return NULL;
}

static int bluray_read_header(AVFormatContext *s)
{
    BlurayDemuxContext *ctx = s->priv_data;
    const BLURAY_DISC_INFO *disc_info;
    BLURAY_TITLE_INFO *info;
    BLURAY_CLIP_INFO *clip;
    BLURAY_STREAM_INFO **bd_streams;
    int bd_streams_count = 0;
    uint32_t num_title_idx;
    uint64_t pls_max_duration = 0;
    int longest_title, main_title;
    int strm_idx;
    int ret = 0;
    int i;

    if (!(ctx->mpegts_ctx = avformat_alloc_context())) {
        return AVERROR(ENOMEM);
    }

    /* bluray is always mpegts based */
    if ((ctx->mpegts = av_find_input_format("mpegts")) == NULL) {
        avformat_free_context(ctx->mpegts_ctx);
        return AVERROR_DEMUXER_NOT_FOUND;
    }

    /* open BD stream */
    ctx->bd = bd_init();
    if (bd_open_stream(ctx->bd, s->pb, &bluray_read_blocks) == 0) {
        av_log(s, AV_LOG_ERROR, "bd_open_stream(%s) failed\n", s->url);
        return AVERROR(EIO);
    }

    /* get general disc info */
    disc_info = bd_get_disc_info(ctx->bd);
    if (disc_info->disc_name) {
        av_log(s, AV_LOG_INFO, "opening bluray disc: %s\n", disc_info->disc_name);
        av_dict_set(&s->metadata, "title", disc_info->disc_name, 0);
    }

    num_title_idx = bd_get_titles(ctx->bd, TITLES_RELEVANT, ctx->min_title_length);
    av_log(s, AV_LOG_INFO, "%d usable titles\n", num_title_idx);
    if (num_title_idx < 1) {
        return AVERROR(EIO);
    }

    main_title = bd_get_main_title(ctx->bd);
    av_log(s, AV_LOG_INFO, "main title is assumed to be: %d\n", main_title);

    /* output all titles and find longest title/playlist for selection */
    for (i = 0; i < num_title_idx; i++) {
        info = bd_get_title_info(ctx->bd, i, 0);
        av_log(s, AV_LOG_INFO, "title %d: %05d.mpls (%d:%02d:%02d) with %d chapter(s)\n",
                info->idx,
                info->playlist,
                ((int)(info->duration / TIME_BASE_HZ) / 3600),
                ((int)(info->duration / TIME_BASE_HZ) % 3600) / 60,
                ((int)(info->duration / TIME_BASE_HZ) % 60),
                info->chapter_count);

        if (pls_max_duration < info->duration) {
            longest_title = info->idx;
            pls_max_duration = info->duration;
        }

        bd_free_title_info(info);
    }

    /* select title */
    if (ctx->title < 0) {
        /* pick main else longest title(s) */
        if (main_title >= 0) {
            ctx->title = main_title;
        } else {
            ctx->title = longest_title;
        }
    }

    /* select playlist */
    if (bd_select_title(ctx->bd, ctx->title) <= 0) {
        av_log(s, AV_LOG_ERROR, "bd_select_title(%d) failed\n", ctx->title);
        return AVERROR(EIO);
    }

    info = bd_get_title_info(ctx->bd, ctx->title, 0);
    av_log(s, AV_LOG_INFO, "selected title: %d (%05d.mpls)\n",
            ctx->title,
            info->playlist);

    if (bd_select_playlist(ctx->bd, info->playlist) <= 0) {
        av_log(s, AV_LOG_ERROR, "bd_select_playlist(%d) failed\n", ctx->title);
        return AVERROR(EIO);
    }

    s->duration = av_rescale(info->duration, AV_TIME_BASE, TIME_BASE_HZ);

    /* add chapters */
    for (i = 0; i < info->chapter_count; i++) {
        const BLURAY_TITLE_CHAPTER *bd_chap = &info->chapters[i];
        if (avpriv_new_chapter(s, bd_chap->idx,
                            (AVRational) { 1, TIME_BASE_HZ},
                            bd_chap->start,
                            bd_chap->start + bd_chap->duration, NULL) == NULL) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }

    /* read clip streams if possible */
    if (info->clip_count > 0) {
        strm_idx = 0;
        clip = &info->clips[0];
        bd_streams_count = clip->audio_stream_count + clip->sec_audio_stream_count
                            + clip->pg_stream_count;
        /* bd stream pid will match the mpegts stream pid */
        bd_streams = av_malloc_array(bd_streams_count, sizeof(intptr_t));
        if (bd_streams == NULL) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        for (i = 0; i < clip->audio_stream_count; i++) {
            bd_streams[strm_idx++] = &clip->audio_streams[i];
        }
        for (i = 0; i < clip->sec_audio_stream_count; i++) {
            bd_streams[strm_idx++] = &clip->sec_audio_streams[i];
        }
        for (i = 0; i < clip->pg_stream_count; i++) {
            bd_streams[strm_idx++] = &clip->pg_streams[i];
        }
    } else {
        bd_streams_count = 0;
    }

    /* initialize custom-IO to read data from bluray */
    ctx->buffer_size = LBA_TO_BYTES(16);
    if (!(ctx->buffer = av_malloc(ctx->buffer_size))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (!(ctx->mpegts_ctx->pb = avio_alloc_context(ctx->buffer,
            ctx->buffer_size, 0, ctx, &bluray_read_bd_packet, NULL, &bluray_seek_bd))) {
        av_free(ctx->buffer);
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avformat_open_input(&ctx->mpegts_ctx, NULL, ctx->mpegts, NULL);
    if (ret < 0) {
        goto end;
    }

    /* copy mpegts programs into main demuxer context */
    for (i = 0; i < ctx->mpegts_ctx->nb_programs; i++) {
        AVProgram *ts_p = ctx->mpegts_ctx->programs[i];
        AVProgram *p;
        if((p = av_new_program(s, ts_p->id)) == NULL) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        p->program_num = ts_p->program_num;
        p->nb_stream_indexes = ts_p->nb_stream_indexes;
        p->stream_index = av_malloc_array(ts_p->nb_stream_indexes, sizeof(*ts_p->stream_index));
        memcpy(p->stream_index, ts_p->stream_index, sizeof(*ts_p->stream_index)*ts_p->nb_stream_indexes);
        p->start_time = ts_p->start_time;
    }

    /* add additional bluray metadata to mpegts discovered streams */
    for (i = 0; i < ctx->mpegts_ctx->nb_streams; i++) {
        AVStream *st;
        BLURAY_STREAM_INFO *bd_st;
        AVStream *ts_st = ctx->mpegts_ctx->streams[i];
        if ((st = avformat_new_stream(s, NULL)) == NULL) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        avcodec_parameters_copy(st->codecpar, ts_st->codecpar);
        st->id = ts_st->id;
        st->index = ts_st->index;
        st->time_base = ts_st->time_base;
        st->start_time = ts_st->start_time;
        bd_st = bluray_find_stream_info_by_pid(bd_streams, bd_streams_count, st->id);
        if (bd_st != NULL) {
            av_dict_set(&st->metadata, "language", bd_st->lang, 0);
        }
    }

end:
    av_free(bd_streams);
    bd_free_title_info(info);
    return ret;
}

static int bluray_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    BlurayDemuxContext *ctx = s->priv_data;

    /* just use the mpegts to demux the stream */
    int ret = av_read_frame(ctx->mpegts_ctx, pkt);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to get a packet from: %s\n", s->url);
        return ret;
    }

    return ret;
}

static int bluray_read_close(AVFormatContext *s)
{
    BlurayDemuxContext *ctx = s->priv_data;
    avformat_close_input(&ctx->mpegts_ctx);
    bd_close(ctx->bd);
    return 0;
}

#define OFFSET(x) offsetof(BlurayDemuxContext, x)
static const AVOption options[] = {
    { "title", "", OFFSET(title), AV_OPT_TYPE_INT, { .i64=-1 }, -1,  99999, AV_OPT_FLAG_DECODING_PARAM },
    { "min-title-length", "", OFFSET(min_title_length), AV_OPT_TYPE_INT, { .i64=180 }, 180,  99999, AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass bluray_class = {
    .class_name     = "Blu-ray Disc Audio-Video",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
};

const AVInputFormat ff_bluray_demuxer = {
    .name           = "bluray",
    .long_name      = NULL_IF_CONFIG_SMALL("Blu-ray Disc Audio-Video"),
    .priv_data_size = sizeof(BlurayDemuxContext),
    .extensions     = "bdmv,iso",
    .read_probe     = bluray_read_probe,
    .read_header    = bluray_read_header,
    .read_packet    = bluray_read_packet,
    .read_close     = bluray_read_close,
    .priv_class     = &bluray_class,
    .mime_type      = "application/x-iso9660-image",
};
