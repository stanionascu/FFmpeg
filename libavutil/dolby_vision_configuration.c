/**
 * Copyright (c) 2019 Stanislav Ionascu <stanislav.ionascu@gmail.com>
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

#include "dolby_vision_configuration.h"
#include "avstring.h"
#include "mem.h"

AVDolbyVisionConfiguration *av_dolby_vision_configuration_alloc(size_t* size)
{
    AVDolbyVisionConfiguration *dv_config = av_mallocz(sizeof(AVDolbyVisionConfiguration));
    if (!dv_config)
        return NULL;

    if (size)
        * size = sizeof(*dv_config);

    return dv_config;
}

int av_dolby_vision_configuration_parse(AVDolbyVisionConfiguration *dv,
    uint8_t *profile_data, size_t profile_data_size)
{
    uint16_t dv_level_data;
    if (profile_data_size < 4 || dv == NULL)
        return AVERROR_INVALIDDATA;

    dv->dv_version_major = profile_data[0];
    dv->dv_version_minor = profile_data[1];

    dv_level_data = (profile_data[2] << 8 | profile_data[3]);

    dv->dv_profile = (dv_level_data >> 9) & 0xFF;
    dv->dv_level = ((dv_level_data & 0xFF) >> 3) & 0x1F;
    dv->rpu_present = dv_level_data & 0x4;
    dv->el_present = dv_level_data & 0x2;
    dv->bl_present = dv_level_data & 0x1;

    return 0;
}

const char *av_dolby_vision_get_codec_type_str(AVDolbyVisionConfiguration *dv) {
    if (!dv)
        return NULL;

    switch (dv->dv_profile) {
    case 4:
    case 5:
    case 7:
        return "dvhe";
    case 8:
        return "hev1";
    case 9:
        return "avc3";
    }
    return NULL;
}

char *av_dolby_vision_get_codec_str(AVDolbyVisionConfiguration *dv)
{
    return av_asprintf("%s.%02d.%02d",
        av_dolby_vision_get_codec_type_str(dv), dv->dv_profile, dv->dv_level);
}
