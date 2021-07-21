/*
 * Shm video output driver
 * Copyright (c) 2021 Ricardo Villalba <ricardo@smplayer.info>
 * Copyright (c) 2005 Nicolas Plourde <nicolasplourde@gmail.com>
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Based on vo_corevideo.m by Nicolas Plourde <nicolas.plourde@gmail.com> */

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

//MPLAYER
#include "config.h"
#include "fastmemcpy.h"
#include "video_out.h"
#define NO_DRAW_SLICE
#define NO_DRAW_FRAME
#include "video_out_internal.h"
#include "aspect.h"
#include "mp_msg.h"
#include "m_option.h"
#include "mp_fifo.h"
#include "sub/sub.h"
#include "subopt-helper.h"

#include "input/input.h"
#include "input/mouse.h"

#include "osdep/keycodes.h"

// Shared memory
#define DEFAULT_BUFFER_NAME "mplayer"
static char * buffer_name;
static int shm_fd = 0;

// Image
static unsigned char * image_data;

static uint32_t image_width;
static uint32_t image_height;
static uint32_t image_bytes;
static uint32_t image_stride;
static uint32_t image_format;
static uint32_t frame_count = 0;
static uint32_t buffer_size = 0;
static uint32_t video_buffer_size = 0;

struct header_t {
    uint32_t header_size;
    uint32_t video_buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
    uint32_t stride[3];
    uint32_t planes;
    uint32_t format;
    uint32_t frame_count;
    uint32_t busy;
    float fps;
    // MPV
    int32_t rotate;
    int32_t colorspace;
    int32_t colorspace_levels;
    int32_t colorspace_primaries;
    int32_t colorspace_gamma;
    int32_t colorspace_light;
    float colorspace_sig_peak;
    int32_t chroma_location;
    uint32_t reserved[20];
} * header;

static vo_info_t info =
{
    "Shared Memory",
    "shm",
    "Ricardo Villalba <ricardo@smplayer.info>",
    ""
};

LIBVO_EXTERN(shm)

static void draw_alpha(int x0, int y0, int w, int h, unsigned char *src, unsigned char *srca, int stride)
{
    unsigned char *dst = image_data + image_bytes * (y0 * image_width + x0);
    vo_draw_alpha_func draw = vo_get_draw_alpha(image_format);
    if (!draw) return;
    draw(w,h,src,srca,stride,dst,image_stride);
}

static void free_buffers(void)
{
    if (munmap(header, buffer_size) == -1) {
        //mp_msg(MSGT_VO, MSGL_FATAL, "[vo_shm] uninit: munmap failed. Error: %s\n", strerror(errno));
    }

    if (shm_unlink(buffer_name) == -1) {
        //mp_msg(MSGT_VO, MSGL_FATAL, "[vo_shm] uninit: shm_unlink failed. Error: %s\n", strerror(errno));
    }
}

static void update_screen_info_shared_buffer(void)
{
}

#if 0
static void print_mpi(mp_image_t * mpi) {
    mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] mpi: %d %d planes: %d bpp: %d\n", mpi->width, mpi->height, mpi->num_planes, mpi->bpp);
    mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] mpi: chroma_width: %d chroma_height: %d\n", mpi->chroma_width, mpi->chroma_height);
    for (int n = 0; n < mpi->num_planes; n++) {
        mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] mpi: stride plane %d: %d\n", n, mpi->stride[n]);
    }
}

static int calculate_buffer_size(mp_image_t * mpi) {
    int size = 0;
    if (mpi->flags&MP_IMGFLAG_PLANAR) {
        size = (mpi->stride[0] * mpi->h) +
               (mpi->stride[1] * mpi->chroma_height) +
               (mpi->stride[2] * mpi->chroma_height);
        //mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] size: %d\n", size);
    } else {
        size = mpi->stride[0] * mpi->height;
    }
    return size;
}
#endif

static int config(uint32_t width, uint32_t height, uint32_t d_width, uint32_t d_height, uint32_t flags, char *title, uint32_t format)
{
    free_buffers();

    image_width = width;
    image_height = height;
    image_format = format;

#if 0
    mp_image_t * tmpi = alloc_mpi(width, height, format);
    video_buffer_size = calculate_buffer_size(tmpi);
    print_mpi(tmpi);
    mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] buffer size: %d\n", video_buffer_size);

    image_stride = tmpi->stride[0];
    image_bytes = tmpi->bpp / 8;
    //image_bytes = tmpi->stride[0] / tmpi->width;

    free_mp_image(tmpi);
#else
    switch (image_format)
    {
        case IMGFMT_RGB24:
            image_bytes = 3;
            break;
        case IMGFMT_RGB16:
            image_bytes = 2;
            break;
        case IMGFMT_I420:
            image_bytes = 1;
            break;
        case IMGFMT_YUY2:
        case IMGFMT_UYVY:
            image_bytes = 2;
            break;
        default:
            image_bytes = 3;
    }
    image_stride = width * image_bytes;
    video_buffer_size = image_stride * height;
    if (image_format == IMGFMT_I420) {
        video_buffer_size = width * height * 2;
    }
#endif

    mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] w: %d h: %d format: %d\n", image_width, image_height, image_format);
    mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] stride: %d bytes: %d\n", image_stride, image_bytes);
    mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] video buffer size: %d\n", video_buffer_size);

    mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] writing output to a shared buffer "
            "named \"%s\"\n", buffer_name);

    // Create shared memory
    shm_fd = shm_open(buffer_name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (shm_fd == -1)
    {
        mp_msg(MSGT_VO, MSGL_FATAL,
               "[vo_shm] failed to open shared memory. Error: %s\n", strerror(errno));
        return 1;
    }

    buffer_size = sizeof(header) + video_buffer_size;

    if (ftruncate(shm_fd, buffer_size) == -1)
    {
        mp_msg(MSGT_VO, MSGL_FATAL,
               "[vo_shm] failed to size shared memory, possibly already in use. Error: %s\n", strerror(errno));
        close(shm_fd);
        shm_unlink(buffer_name);
        return 1;
    }

    header = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (header == MAP_FAILED)
    {
        mp_msg(MSGT_VO, MSGL_FATAL,
               "[vo_shm] failed to map shared memory. Error: %s\n", strerror(errno));
        shm_unlink(buffer_name);
        return 1;
    }

    header->header_size = sizeof(struct header_t);
    header->video_buffer_size = video_buffer_size;

    image_data = (unsigned char*) header + header->header_size;
    //mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] header: %p image_data: %p\n", header, image_data);

    header->rotate = header->colorspace = header->colorspace_levels = header->colorspace_primaries =
    header->colorspace_gamma = header->colorspace_light = header->colorspace_sig_peak = header->chroma_location = 0;

    return 0;
}

static void check_events(void)
{
}

static void draw_osd(void)
{
    vo_draw_text(image_width, image_height, draw_alpha);
}

static void flip_page(void)
{
}

static uint32_t draw_image(mp_image_t *mpi)
{
    header->width = image_width;
    header->height = image_height;
    header->bytes = image_bytes;
    header->stride[0] = image_stride;
    if (mpi->flags&MP_IMGFLAG_PLANAR) {
        header->stride[1] = header->stride[2] = image_width / 2;
    } else {
        header->stride[1] = header->stride[2] = 0;
    }
    header->planes = mpi->num_planes;
    header->format = image_format;
    header->frame_count = frame_count++;
    header->fps = vo_fps;

    //mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] w: %d h: %d stride: %d\n", mpi->width, mpi->height, mpi->stride[0]);

    if (!(mpi->flags & MP_IMGFLAG_DIRECT)) {
        header->busy = 1;
        if (mpi->flags&MP_IMGFLAG_PLANAR) {
            unsigned char * ptr = image_data;
            int size = image_stride * image_height;
            memcpy_pic(ptr, mpi->planes[0], image_width, image_height, image_stride, mpi->stride[0]);
            ptr += size;
            size = (image_width * image_height) / 2;
            memcpy_pic(ptr, mpi->planes[1], image_width / 2, image_height / 2, image_width / 2, mpi->stride[1]);
            ptr += size;
            memcpy_pic(ptr, mpi->planes[2], image_width / 2, image_height / 2, image_width / 2, mpi->stride[2]);
        } else {
            memcpy_pic(image_data, mpi->planes[0], image_width * image_bytes, image_height, image_stride, mpi->stride[0]);
        }
        header->busy = 0;
    }

    return VO_TRUE;
}

static int query_format(uint32_t format)
{
    const int supportflags = VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW | VFCAP_OSD | VFCAP_HWSCALE_UP | VFCAP_HWSCALE_DOWN | VFCAP_ACCEPT_STRIDE | VOCAP_NOSLICES;

    switch(format)
    {
        case IMGFMT_I420:
            return supportflags;
        case IMGFMT_YUY2:
            return supportflags;
        case IMGFMT_UYVY:
            return supportflags;
        case IMGFMT_RGB24:
            return supportflags;
        case IMGFMT_RGB16:
            return supportflags;
    }
    return 0;
}

static int get_image(mp_image_t *mpi)
{
    return VO_FALSE;
}

static void uninit(void)
{
    free_buffers();

    free(buffer_name);
    buffer_name = NULL;
}

static const opt_t subopts[] = {
{"buffer_name",  OPT_ARG_MSTRZ, &buffer_name, NULL},
{NULL}
};

static int preinit(const char *arg)
{

    // Set defaults
    buffer_name = NULL;

    if (subopt_parse(arg, subopts) != 0) {
        mp_msg(MSGT_VO, MSGL_FATAL,
                "\n-vo shm command line help:\n"
                "Example: mplayer -vo shm:buffer_name=mybuff\n"
                "\nOptions:\n"
                "  buffer_name=<name>\n"
                "    Name of the shared buffer created with shm_open().\n"
                "    If not set it will use 'mplayer' as name.\n"
                "\n" );
        return -1;
    }

    if (!buffer_name)
        buffer_name = strdup(DEFAULT_BUFFER_NAME);

    return 0;
}

static int control(uint32_t request, void *data)
{
    switch (request)
    {
        case VOCTRL_DRAW_IMAGE: return draw_image(data);
        case VOCTRL_QUERY_FORMAT: return query_format(*(uint32_t*)data);
    }
    return VO_NOTIMPL;
}

