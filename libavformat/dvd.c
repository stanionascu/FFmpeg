/*
 * DVD (libdvdread) protocol
 * Copyright (c) 2023 Stan Ionascu <stanislav.ionascu@gmail.com>
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

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/ifo_print.h>

#include "libavformat/avformat.h"
#include "libavformat/url.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"

#define DVD_PROTO_PREFIX "dvd:"
#define DVD_SECTOR_SIZE 2048

typedef struct {
    const AVClass *class;

    dvd_reader_t *dvd;
    dvd_logger_cb dvd_logger;

    int title_nr;
    int angle_nr;
    dvd_file_t *dvd_title_file;
    ifo_handle_t *vmg_ifo;
    ifo_handle_t *vts_ifo;
    pgc_t *current_pgc;
    int current_cell;

    int start_sector;
    int current_sector;
    int end_sector;

} DvdProtocolContext;

static int bcd2int(int bcd)
{
    return (((bcd & 0xf0) >> 4) * 10 + (bcd & 0x0f));
}

static void dvd_logger_ff(void *opaque, dvd_logger_level_t dvd_level, const char *fmt, va_list args)
{
    int level = AV_LOG_TRACE;
    if (dvd_level == DVD_LOGGER_LEVEL_ERROR) {
        level = AV_LOG_ERROR;
    } else if (dvd_level == DVD_LOGGER_LEVEL_WARN) {
        level = AV_LOG_WARNING;
    } else if (dvd_level == DVD_LOGGER_LEVEL_INFO) {
        level = AV_LOG_INFO;
    } else if (dvd_level == DVD_LOGGER_LEVEL_DEBUG) {
        level = AV_LOG_DEBUG;
    }
    av_vlog(opaque, level, fmt, args);
    /* add new line */
    av_log(opaque, level, "\n");
}

static int ff_dvd_get_title_set_length(ifo_handle_t *vts_ifo, tt_srpt_t *tt_srpt, int title_nr)
{
    /* reindex 1 => 0 */
    int vts_title_number = tt_srpt->title[title_nr].vts_ttn - 1;
    int pgc_number = vts_ifo->vts_ptt_srpt->title[vts_title_number].ptt[0].pgcn;
    dvd_time_t *playback_time = &vts_ifo->vts_pgcit->pgci_srp[pgc_number - 1].pgc->playback_time;

    return (bcd2int(playback_time->hour) * 3600 +
            bcd2int(playback_time->minute) * 60 +
            bcd2int(playback_time->second)) * 1000;
}

static void ff_dvd_set_program_chain_info(DvdProtocolContext *ctx, int title_nr, int ptt_nr)
{
    /* reindex 1 => 0 */
    int vts_title_number = ctx->vmg_ifo->tt_srpt->title[title_nr].vts_ttn - 1;
    ptt_info_t *ptt = &ctx->vts_ifo->vts_ptt_srpt->title[vts_title_number].ptt[ptt_nr];
    int pgc_id = ptt->pgcn;
    int pgn  = ptt->pgn;

    ctx->current_pgc = ctx->vts_ifo->vts_pgcit->pgci_srp[pgc_id - 1].pgc;
    ctx->current_cell = ctx->current_pgc->program_map[pgn - 1] - 1;

    /* change angle */
    if(ctx->current_pgc->cell_playback[ctx->current_cell].block_type == BLOCK_TYPE_ANGLE_BLOCK ) {
      ctx->current_cell += ctx->angle_nr;
    }

    ctx->current_sector = ctx->start_sector = ctx->current_pgc->cell_playback[ctx->current_cell].first_sector;
    ctx->end_sector = ctx->current_pgc->cell_playback[ctx->current_cell].last_sector;
}

static int ff_dvd_get_next_cell(DvdProtocolContext *ctx)
{
    int next_cell = ctx->current_cell;

    /* fast-forward until last-cell */
    if (ctx->current_pgc->cell_playback[next_cell].block_type == BLOCK_TYPE_ANGLE_BLOCK) {
        ctx->current_cell += ctx->angle_nr;
        while (next_cell < ctx->current_pgc->nr_of_cells &&
                ctx->current_pgc->cell_playback[next_cell].block_type != BLOCK_MODE_LAST_CELL) {
            next_cell++;
        }
    }

    /* take next cell */
    next_cell++;
    if (next_cell >= ctx->current_pgc->nr_of_cells) {
        return -1;
    }

    return next_cell;
}

static int dvd_url_close(URLContext *h)
{
    DvdProtocolContext *ctx = h->priv_data;
    ifoClose(ctx->vts_ifo);
    ifoClose(ctx->vmg_ifo);
    DVDCloseFile(ctx->dvd_title_file);
    DVDClose(ctx->dvd);
    return 0;
}

static int dvd_url_open(URLContext *h, const char *path, int flags)
{
    DvdProtocolContext *ctx = h->priv_data;
    const char *dvd_path = path;
    ifo_handle_t *vmg_ifo, *vts_ifo;
    tt_srpt_t *tt_srpt;
    int num_vts, num_titles;
    int longest_title_nr;
    int64_t longest_title_length_ms = 0;
    int vts_nr;
    int title_set_nr;
    char disc_volume_id[32];

    ctx->vmg_ifo = ctx->vts_ifo = NULL;
    ctx->dvd_logger.pf_log = &dvd_logger_ff;

    /* strip protocol from path */
    av_strstart(path, DVD_PROTO_PREFIX, &dvd_path);

    /* open dvd folder or disc */
    if ((ctx->dvd = DVDOpen2(h, &ctx->dvd_logger, dvd_path)) == NULL) {
        av_log(h, AV_LOG_ERROR, "DVDOpen(%s) failed\n", dvd_path);
        return AVERROR(EIO);
    }

    if (DVDUDFVolumeInfo(ctx->dvd, &disc_volume_id[0], 32, NULL, 0) == 0) {
        av_log(h, AV_LOG_INFO, "opened disc-volume-id: %s\n", disc_volume_id);
    }

    /* read toc */
    if ((vmg_ifo = ifoOpen(ctx->dvd, 0)) == NULL) {
        av_log(h, AV_LOG_ERROR, "ifoOpen(0) failed\n");
        return AVERROR(EIO);
    }

    /* read titles */
    num_vts = vmg_ifo->vts_atrt->nr_of_vtss;
    num_titles = vmg_ifo->tt_srpt->nr_of_srpts;
    av_log(h, AV_LOG_INFO, "there are %d usable titles\n", num_titles);

    /* get title-set info(s) */
    tt_srpt = vmg_ifo->tt_srpt;
    if (ctx->title_nr >= num_titles) {
        av_log(h, AV_LOG_ERROR, "invalid title id %d\n", ctx->title_nr);
        return AVERROR(EINVAL);
    }

    for (vts_nr = 1; vts_nr <= num_vts; vts_nr++) {
        int title_nr;
        for (title_nr = 0; title_nr < num_titles; title_nr++) {
            int title_length_ms;

            if (tt_srpt->title[title_nr].title_set_nr != vts_nr) {
                continue;
            }

            /* describe the title(s) info */
            if ((vts_ifo = ifoOpen(ctx->dvd, vts_nr)) == NULL) {
                av_log(h, AV_LOG_ERROR, "ifoOpen(%d) failed\n", title_nr);
                return AVERROR(ENOMEM);
            }

            /* skip if video title set or program chain info is missing */
            if (vts_ifo->vtsi_mat == NULL || vts_ifo->vts_pgcit == NULL) {
                ifoClose(vts_ifo);
                av_log(h, AV_LOG_TRACE, "skip title %d as no vts or pgc info is present\n",
                        title_nr);
                continue;
            }

            /* skip if vts_ttn is incorrect */
            if (tt_srpt->title[title_nr].vts_ttn < 1 ||
                tt_srpt->title[title_nr].vts_ttn > num_titles) {
                ifoClose(vts_ifo);
                av_log(h, AV_LOG_WARNING, "skip title %d as vts_ttn is out of bounds\n",
                        title_nr);
                continue;
            }

            title_length_ms = ff_dvd_get_title_set_length(vts_ifo, tt_srpt, title_nr);
            av_log(h, AV_LOG_INFO, "title %03d : (%d:%02d:%02d) and %d chapter(s)\n",
                title_nr,
                (title_length_ms / 3600000),
                (title_length_ms % 3600000) / 60000,
                (title_length_ms % 60000) / 1000,
                tt_srpt->title[title_nr].nr_of_ptts);

            if (longest_title_length_ms <= title_length_ms) {
                longest_title_nr = title_nr;
                longest_title_length_ms = title_length_ms;
            }

            ifoClose(vts_ifo);
        }
    }

    if (ctx->title_nr < 0) {
        ctx->title_nr = longest_title_nr;
    }

    /* check if title selection is valid */
    if (tt_srpt->title[ctx->title_nr].vts_ttn < 1 ||
        tt_srpt->title[ctx->title_nr].vts_ttn > num_titles) {
        av_log(h, AV_LOG_INFO, "selected title %d is not valid, vts_ttn is out of bounds\n",
                ctx->title_nr);
        ifoClose(vmg_ifo);
        return AVERROR(EIO);
    }

    av_log(h, AV_LOG_INFO, "selected title %d\n", ctx->title_nr);

    if (ctx->angle_nr > 0 && ctx->angle_nr >= tt_srpt->title[ctx->title_nr].nr_of_angles) {
        av_log(h, AV_LOG_ERROR, "incorrect angle selected %d out of %d angle(s)\n",
                ctx->angle_nr,
                tt_srpt->title[ctx->title_nr].nr_of_angles);
    }

    /* open request title_set info */
    title_set_nr = tt_srpt->title[ctx->title_nr].title_set_nr;
    if ((vts_ifo = ifoOpen(ctx->dvd, title_set_nr)) == NULL) {
        av_log(h, AV_LOG_INFO, "ifoOpen(%d) failed\n", title_set_nr);
        return AVERROR(ENOMEM);
    }

    /* open file */
    if ((ctx->dvd_title_file = DVDOpenFile(ctx->dvd, title_set_nr, DVD_READ_TITLE_VOBS)) == NULL) {
        av_log(h, AV_LOG_ERROR, "DVDOpenFile(%d) failed\n", title_set_nr);
        return AVERROR(EIO);
    }

    ctx->vmg_ifo = vmg_ifo;
    ctx->vts_ifo = vts_ifo;

    ff_dvd_set_program_chain_info(ctx, ctx->title_nr, 0);

    return 0;
}

static int dvd_url_read(URLContext *h, unsigned char *buf, int size)
{
    DvdProtocolContext *ctx = h->priv_data;
    ssize_t blocks_got;
    int num_blocks = size / DVD_SECTOR_SIZE;

    if (ctx->current_sector >= ctx->end_sector) {
        int next_cell = ff_dvd_get_next_cell(ctx);
        if (next_cell < 0) {
            return AVERROR_EOF;
        }

        ctx->current_cell = next_cell;
        ctx->start_sector = ctx->current_pgc->cell_playback[ctx->current_cell].first_sector;
        ctx->end_sector = ctx->current_pgc->cell_playback[ctx->current_cell].last_sector;
        ctx->current_sector = ctx->start_sector;
    }

    /* read all sectors that fit into buf */
    blocks_got = DVDReadBlocks(ctx->dvd_title_file, ctx->current_sector, num_blocks, buf);
    if (blocks_got <= 0) {
        av_log(h, AV_LOG_ERROR, "failed to DVDReadBlocks() %d blocks at offset %d\n",
                num_blocks, ctx->current_sector);
        return AVERROR(EIO);
    }
    ctx->current_sector += blocks_got;
    return blocks_got <= 0 ? AVERROR_EOF : blocks_got * DVD_SECTOR_SIZE;
}

#define OFFSET(x) offsetof(DvdProtocolContext, x)
static const AVOption options[] = {
    {"title", "", OFFSET(title_nr), AV_OPT_TYPE_INT, { .i64=-1 }, -1,  9999, AV_OPT_FLAG_DECODING_PARAM },
    {"angle", "", OFFSET(angle_nr), AV_OPT_TYPE_INT, { .i64=0 },   0,   256, AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};
static const AVClass dvd_context_class = {
    .class_name     = "dvd",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
};

const URLProtocol ff_dvd_protocol = {
    .name            = "dvd",
    .url_close       = dvd_url_close,
    .url_open        = dvd_url_open,
    .url_read        = dvd_url_read,
    .priv_data_size  = sizeof(DvdProtocolContext),
    .priv_data_class = &dvd_context_class,
};
