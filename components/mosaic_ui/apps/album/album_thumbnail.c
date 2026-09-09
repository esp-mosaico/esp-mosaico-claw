/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "album_thumbnail.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#else
#include <stdio.h>
#include <setjmp.h>
#include <jpeglib.h>
#endif

#define THUMB_SIDE 150U
#define DECODE_MAX_BYTES (4U * 1024U * 1024U)

static unsigned decode_shift(unsigned width, unsigned height)
{
    unsigned shift = 0;
    while (shift < 3 &&
           (((width >> (shift + 1)) >= THUMB_SIDE &&
             (height >> (shift + 1)) >= THUMB_SIDE) ||
            (width >> shift) > 2048 || (height >> shift) > 2048)) {
        ++shift;
    }
    return shift;
}

#ifdef ESP_PLATFORM
static uint8_t *decode_rgb(const uint8_t *data, size_t size, unsigned *width, unsigned *height,
                           unsigned *source_width, unsigned *source_height)
{
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t decoder = NULL;
    jpeg_dec_header_info_t info;
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)data, .inbuf_len = (int)size};
    uint8_t *pixels = NULL;
    if (size > INT_MAX || jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) {
        return NULL;
    }
    if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK) {
        goto done;
    }
    *width = *source_width = info.width;
    *height = *source_height = info.height;
    unsigned shift = decode_shift(*width, *height);
    /* The decoder's scaled dimensions must be multiples of eight. Preserve
     * the original dimensions for unscaled and very small JPEGs. */
    if (shift && (*width >> shift) >= 8 && (*height >> shift) >= 8) {
        unsigned divisor = 1U << shift;
        /* Round upward: rounding below ceil(source / 8) would exceed the
         * decoder's maximum reduction for imported, unaligned dimensions. */
        config.scale.width = (((*width + divisor - 1) >> shift) + 7) & ~7U;
        config.scale.height = (((*height + divisor - 1) >> shift) + 7) & ~7U;
        jpeg_dec_close(decoder);
        decoder = NULL;
        if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) {
            return NULL;
        }
        io = (jpeg_dec_io_t){.inbuf = (uint8_t *)data, .inbuf_len = (int)size};
        if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK) {
            goto done;
        }
        *width = config.scale.width;
        *height = config.scale.height;
    }
    int bytes = 0;
    if (!*width || !*height ||
        jpeg_dec_get_outbuf_len(decoder, &bytes) != JPEG_ERR_OK ||
        bytes <= 0 || bytes > DECODE_MAX_BYTES ||
        (size_t)*width * *height * 3 > (size_t)bytes) {
        goto done;
    }
    pixels = heap_caps_aligned_alloc(16, (size_t)bytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels) {
        io.outbuf = pixels;
        if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) {
            free(pixels);
            pixels = NULL;
        }
    }
done:
    jpeg_dec_close(decoder);
    return pixels;
}
#else
typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} thumbnail_jpeg_error_t;

static void jpeg_failed(j_common_ptr decoder)
{
    thumbnail_jpeg_error_t *error = (thumbnail_jpeg_error_t *)decoder->err;
    longjmp(error->jump, 1);
}

static uint8_t *decode_rgb(const uint8_t *data, size_t size, unsigned *width, unsigned *height,
                           unsigned *source_width, unsigned *source_height)
{
    struct jpeg_decompress_struct decoder = {0};
    thumbnail_jpeg_error_t error;
    uint8_t *volatile pixels = NULL;
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_failed;
    if (setjmp(error.jump)) {
        free(pixels);
        jpeg_destroy_decompress(&decoder);
        return NULL;
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, data, size);
    jpeg_read_header(&decoder, TRUE);
    *source_width = decoder.image_width;
    *source_height = decoder.image_height;
    decoder.scale_num = 1;
    decoder.scale_denom = 1U << decode_shift(decoder.image_width, decoder.image_height);
    decoder.out_color_space = JCS_RGB;
    jpeg_calc_output_dimensions(&decoder);
    if ((uint64_t)decoder.output_width * decoder.output_height * 3 > DECODE_MAX_BYTES) {
        jpeg_destroy_decompress(&decoder);
        return NULL;
    }
    jpeg_start_decompress(&decoder);
    *width = decoder.output_width;
    *height = decoder.output_height;
    pixels = malloc((size_t)*width * *height * 3);
    if (pixels) {
        while (decoder.output_scanline < decoder.output_height) {
            JSAMPROW row = pixels + (size_t)decoder.output_scanline * *width * 3;
            jpeg_read_scanlines(&decoder, &row, 1);
        }
        jpeg_finish_decompress(&decoder);
    }
    jpeg_destroy_decompress(&decoder);
    return pixels;
}
#endif

uint8_t *album_thumbnail_create(const uint8_t *jpeg, size_t size, size_t *out_size)
{
    *out_size = 0;
    unsigned width = 0, height = 0;
    unsigned source_width = 0, source_height = 0;
    uint8_t *pixels = decode_rgb(jpeg, size, &width, &height, &source_width, &source_height);
    if (!pixels) {
        return NULL;
    }
    /* A bounded QOI encoder using RGB and RUN operations. No second full
     * image or encoder context is needed; GSP decodes only 150x150 pixels. */
    uint8_t *qoi = malloc(14 + THUMB_SIDE * THUMB_SIDE * 4 + 8);
    if (!qoi) {
        free(pixels);
        return NULL;
    }
    const uint8_t header[14] = {'q', 'o', 'i', 'f', 0, 0, 0, THUMB_SIDE,
                               0, 0, 0, THUMB_SIDE, 3, 0};
    memcpy(qoi, header, sizeof(header));
    size_t at = sizeof(header);
    /* Map the square in original-image coordinates. Decoder dimensions may
     * round independently to MCU/scale alignment; do not distort that ratio. */
    unsigned source_crop = source_width < source_height ? source_width : source_height;
    unsigned crop_width = (uint64_t)width * source_crop / source_width;
    unsigned crop_height = (uint64_t)height * source_crop / source_height;
    if (!crop_width) {
        crop_width = 1;
    }
    if (!crop_height) {
        crop_height = 1;
    }
    unsigned left = (width - crop_width) / 2, top = (height - crop_height) / 2;
    uint8_t previous[3] = {0};
    unsigned run = 0;
    for (unsigned y = 0; y < THUMB_SIDE; ++y) {
        for (unsigned x = 0; x < THUMB_SIDE; ++x) {
            unsigned sx = left + (2 * x + 1) * crop_width / (2 * THUMB_SIDE);
            unsigned sy = top + (2 * y + 1) * crop_height / (2 * THUMB_SIDE);
            const uint8_t *rgb = pixels + ((size_t)sy * width + sx) * 3;
            if (memcmp(previous, rgb, 3) == 0) {
                if (++run == 62) {
                    qoi[at++] = 0xC0 | (run - 1);
                    run = 0;
                }
            } else {
                if (run) {
                    qoi[at++] = 0xC0 | (run - 1);
                    run = 0;
                }
                qoi[at++] = 0xFE;
                memcpy(qoi + at, rgb, 3);
                at += 3;
                memcpy(previous, rgb, 3);
            }
        }
    }
    if (run) {
        qoi[at++] = 0xC0 | (run - 1);
    }
    memset(qoi + at, 0, 8);
    qoi[at + 7] = 1;
    *out_size = at + 8;
    free(pixels);
    uint8_t *compact = realloc(qoi, *out_size);
    if (!compact) {
        free(qoi);
        *out_size = 0;
    }
    return compact;
}
