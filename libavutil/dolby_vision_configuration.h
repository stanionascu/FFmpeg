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

#ifndef AVUTIL_DOLBY_VISION_CONFIGURATION_H
#define AVUTIL_DOLBY_VISION_CONFIGURATION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * The Dolby Vision decoder configuration record provides the configuration
 * information that is required to initialize the Dolby Vision decoder.
 * dolby-vision-bitstreams-within-the-iso-base-media-file-format-v2.0.pdf
 * Section 3.1
 */
typedef struct AVDolbyVisionConfiguration {
    /**
     * The major version number of the Dolby Vision specification that the
     * stream complies with
     */
    uint8_t dv_version_major;

    /**
     * The minor version number of the Dolby Vision specification that the
     * stream complies with.
     */
    uint8_t dv_version_minor;

    /**
     * The Dolby Vision profile.
     */
    uint8_t dv_profile;

    /**
     * The Dolby Vision level.
     */
    uint8_t dv_level;

    /**
     * Indicates that the track contains an RPU substream.
     */
    bool rpu_present;

    /**
     * Indicates that the track contains an EL substream.
     */
    bool el_present;

    /**
     * Indicates that the track contains an BL substream.
     */
    bool bl_present;
} AVDolbyVisionConfiguration;

AVDolbyVisionConfiguration *av_dolby_vision_configuration_alloc(size_t *size);
/**
 * Parses the dolby vision configuration packet into the structure.
 */
int av_dolby_vision_configuration_parse(AVDolbyVisionConfiguration *dv,
    uint8_t *profile_data, size_t profile_data_size);

/**
 * Returns the codec type used for the configuration.
 * For profiles 4-7 - dvhe, 8 - hev1, 9 - avc3
 */
const char *av_dolby_vision_get_codec_type_str(AVDolbyVisionConfiguration *dv);
/**
 * Returns the codec bitstream profile string, e.g. dvhe.05.09
 */
char *av_dolby_vision_get_codec_str(AVDolbyVisionConfiguration *dv);

#endif /* AVUTIL_DOLBY_VISION_CONFIGURATION_H */
