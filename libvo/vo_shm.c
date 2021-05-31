/*
 * CoreVideo video output driver
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

//shared memory
static int shared_buffer = 0;
#define DEFAULT_BUFFER_NAME "mplayer"
static char *buffer_name;
static int shm_fd = 0;

//Screen
static int screen_id = -1;

//image
static unsigned char *image_data;

static uint32_t image_width;
static uint32_t image_height;
static uint32_t image_bytes;
static uint32_t image_stride;
static uint32_t image_format;
static uint32_t frame_count = 0;
static uint32_t buffer_size = 0;

struct header_t {
	uint32_t header_size;
	uint32_t width;
	uint32_t height;
	uint32_t bytes;
	uint32_t stride;
	uint32_t format;
	uint32_t frame_count;
	uint32_t busy;
	unsigned char * image_buffer;
} * header;

static vo_info_t info =
{
	"Shared Memory",
	"shm",
	"Nicolas Plourde <nicolas.plourde@gmail.com>",
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

static void free_file_specific(void)
{
	if (munmap(header, buffer_size) == -1)
		mp_msg(MSGT_VO, MSGL_FATAL, "[vo_shm] uninit: munmap failed. Error: %s\n", strerror(errno));

	if (shm_unlink(buffer_name) == -1)
		mp_msg(MSGT_VO, MSGL_FATAL, "[vo_shm] uninit: shm_unlink failed. Error: %s\n", strerror(errno));
}

static void update_screen_info_shared_buffer(void)
{
}

static int config(uint32_t width, uint32_t height, uint32_t d_width, uint32_t d_height, uint32_t flags, char *title, uint32_t format)
{
	free_file_specific();

	//mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] image_format: %d\n", image_format);

	//misc mplayer setup
	image_width = width;
	image_height = height;
	switch (image_format)
	{
		case IMGFMT_RGB24:
			image_bytes = 3;
			break;
		case IMGFMT_RGB16:
			image_bytes = 2;
			break;
		case IMGFMT_ARGB:
		case IMGFMT_BGRA:
			image_bytes = 4;
			break;
		case IMGFMT_YUY2:
		case IMGFMT_UYVY:
			image_bytes = 2;
			break;
	}
	// should be aligned, but that would break the shared buffer
	image_stride = image_width * image_bytes;

	//mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] %d %d %d\n", image_width, image_height, image_stride);

	mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] writing output to a shared buffer "
			"named \"%s\"\n",buffer_name);

	// create shared memory
	shm_fd = shm_open(buffer_name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	if (shm_fd == -1)
	{
		mp_msg(MSGT_VO, MSGL_FATAL,
			   "[vo_shm] failed to open shared memory. Error: %s\n", strerror(errno));
		return 1;
	}

	buffer_size = sizeof(header) + (image_height * image_stride);

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
	image_data = (unsigned char*) &header->image_buffer;
	//mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] header: %p image_data: %p\n", header, image_data);
	header->header_size = sizeof(struct header_t);

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
	header->stride = image_stride;
	header->format = image_format;
	header->frame_count = frame_count++;

	if (!(mpi->flags & MP_IMGFLAG_DIRECT)) {
		header->busy = 1;
		memcpy_pic(image_data, mpi->planes[0], image_width*image_bytes, image_height, image_stride, mpi->stride[0]);
		header->busy = 0;
	}

	//mp_msg(MSGT_VO, MSGL_INFO, "[vo_shm] frame_count: %d\n", frame_count);

	return VO_TRUE;
}

static int query_format(uint32_t format)
{
    const int supportflags = VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW | VFCAP_OSD | VFCAP_HWSCALE_UP | VFCAP_HWSCALE_DOWN | VFCAP_ACCEPT_STRIDE | VOCAP_NOSLICES;
    image_format = format;

    switch(format)
	{
		/*
		case IMGFMT_YUY2:
			//pixelFormat = kYUVSPixelFormat;
			return supportflags;
		case IMGFMT_UYVY:
			//pixelFormat = k2vuyPixelFormat;
			return supportflags;
		case IMGFMT_YV12:
			return supportflags;
		*/
		case IMGFMT_RGB24:
			//pixelFormat = k24RGBPixelFormat;
			return supportflags;
		/*
		case IMGFMT_ARGB:
			//pixelFormat = k32ARGBPixelFormat;
			return supportflags;
		*/
		/*
		case IMGFMT_BGRA:
			//pixelFormat = k32BGRAPixelFormat;
			return supportflags;
		*/
		case IMGFMT_RGB16:
			//pixelFormat = k32BGRAPixelFormat;
			return supportflags;
    }
    return 0;

	/*
    const int supported_flags = VFCAP_CSP_SUPPORTED|VFCAP_CSP_SUPPORTED_BY_HW|VFCAP_ACCEPT_STRIDE;
    image_format = format;
    switch(format){
    case IMGFMT_RGB24:
        return  supported_flags;
    //case IMGFMT_RGBA:
    //    return supported_flags;
    }
	*/
    return 0;
}

static int get_image(mp_image_t *mpi)
{
    if (!(mpi->flags & (MP_IMGFLAG_ACCEPT_STRIDE | MP_IMGFLAG_ACCEPT_WIDTH)) ||
            (mpi->type != MP_IMGTYPE_TEMP && mpi->type != MP_IMGTYPE_STATIC))
        return VO_FALSE;

	// mpi should not be planar format here
	mpi->planes[0] = image_data;
	mpi->stride[0] = image_stride;
	mpi->flags |=  MP_IMGFLAG_DIRECT;
	mpi->flags &= ~MP_IMGFLAG_DRAW_CALLBACK;
	return VO_TRUE;
}

static void uninit(void)
{
    free_file_specific();

    free(buffer_name);
    buffer_name = NULL;
}

static const opt_t subopts[] = {
{"device_id",     OPT_ARG_INT,  &screen_id,     NULL},
{"shared_buffer", OPT_ARG_BOOL, &shared_buffer, NULL},
{"buffer_name",   OPT_ARG_MSTRZ,&buffer_name,   NULL},
{NULL}
};

static int preinit(const char *arg)
{

	// set defaults
	screen_id = -1;
	shared_buffer = 0;
	buffer_name = NULL;

	if (subopt_parse(arg, subopts) != 0) {
		mp_msg(MSGT_VO, MSGL_FATAL,
				"\n-vo shm command line help:\n"
				"Example: mplayer -vo shm:device_id=1:shared_buffer:buffer_name=mybuff\n"
				"\nOptions:\n"
				"  device_id=<0-...>\n"
				"    DEPRECATED, use -xineramascreen instead.\n"
				"    Set screen device ID for fullscreen.\n"
				"  shared_buffer\n"
				"    Write output to a shared memory buffer instead of displaying it.\n"
				"  buffer_name=<name>\n"
				"    Name of the shared buffer created with shm_open() as well as\n"
				"    the name of the NSConnection MPlayer will try to open.\n"
				"    Setting buffer_name implicitly enables shared_buffer.\n"
				"\n" );
		return -1;
	}

	if (!buffer_name)
		buffer_name = strdup(DEFAULT_BUFFER_NAME);
	else
		shared_buffer = 1;

    return 0;
}

static int control(uint32_t request, void *data)
{
	switch (request)
	{
		case VOCTRL_DRAW_IMAGE: return draw_image(data);
		case VOCTRL_QUERY_FORMAT: return query_format(*(uint32_t*)data);
		case VOCTRL_GET_IMAGE: return get_image(data);
		/*
		case VOCTRL_ONTOP: vo_ontop = !vo_ontop; if(!shared_buffer){ [mpGLView ontop]; } else { [mplayerosxProto ontop]; } return VO_TRUE;
		case VOCTRL_ROOTWIN: vo_rootwin = !vo_rootwin; [mpGLView rootwin]; return VO_TRUE;
		case VOCTRL_FULLSCREEN: vo_fs = !vo_fs; if(!shared_buffer){ [mpGLView fullscreen: NO]; } else { [mplayerosxProto toggleFullscreen]; } return VO_TRUE;
		case VOCTRL_GET_PANSCAN: return VO_TRUE;
		case VOCTRL_SET_PANSCAN: panscan_calc(); return VO_TRUE;
		case VOCTRL_UPDATE_SCREENINFO:
			if (shared_buffer)
				update_screen_info_shared_buffer();
			else
				[mpGLView update_screen_info];
			return VO_TRUE;
		*/
	}
	return VO_NOTIMPL;
}

