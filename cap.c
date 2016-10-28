/* -*- c -*-
    Captures video from a V4L2 device and compresses it to a h264 bitstream
    using the S805 hardware codec.

    Copyright (C) 2016 P2PTech Inc. <http://p2ptech.org>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>             /* getopt_long() */

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>
#include "vpcodec_1_0.h"


#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer {
    void   *start;
    size_t  length;
};

static char            *dev_name;
static int              fd = -1;
struct buffer          *buffers;
static unsigned int     n_buffers;
static int              force_format;
static int              frame_count = -1;
static vl_codec_handle_t codec_handle = NULL;
static char            *encode_src_buffer = NULL;
static char            *encode_dst_buffer = NULL;
static int              encode_buffer_size = -1;
static int              frame_rate = -1;
static struct v4l2_format cap_fmt;

static void errno_exit(const char *s)
{
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg)
{
    int r;

    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static void uyvy_to_nv12(const uint16_t *src, char *dst)
{
    // convert UYVY to NV12
    int srcStride = cap_fmt.fmt.pix.width;
    int dstStride = cap_fmt.fmt.pix.width;
    int dstVUOffset = cap_fmt.fmt.pix.width * cap_fmt.fmt.pix.height;

    for (int y = 0; y < cap_fmt.fmt.pix.height; y++)
    {
        for (int x = 0; x < cap_fmt.fmt.pix.width; x += 2)
        {
            int srcIndex = y * srcStride + x;
            uint16_t uy = src[srcIndex];
            uint16_t vy = src[srcIndex + 1];

            int dstIndex = y * dstStride + (x);
            dst[dstIndex] = uy >> 8;
            dst[dstIndex + 1] = vy >> 8;

            if (y % 2 == 0)
            {
                int dstVUIndex = (y >> 1) * dstStride + (x);
                dst[dstVUOffset + dstVUIndex] = vy & 0xff;
                dst[dstVUOffset + dstVUIndex + 1] = uy & 0xff;
            }
        }
    }
}

static void yuyv_to_nv12(const uint16_t *src, char *dst)
{
    // convert YUYV to NV12
    int srcStride = cap_fmt.fmt.pix.width;
    int dstStride = cap_fmt.fmt.pix.width;
    int dstVUOffset = cap_fmt.fmt.pix.width * cap_fmt.fmt.pix.height;

    for (int y = 0; y < cap_fmt.fmt.pix.height; y++)
    {
        for (int x = 0; x < cap_fmt.fmt.pix.width; x += 2)
        {
            int srcIndex = y * srcStride + x;
            uint16_t yu = src[srcIndex];
            uint16_t yv = src[srcIndex + 1];

            int dstIndex = y * dstStride + (x);
            dst[dstIndex] = yu & 0xff;
            dst[dstIndex + 1] = yv & 0xff;

            if (y % 2 == 0)
            {
                int dstVUIndex = (y >> 1) * dstStride + (x);
                dst[dstVUOffset + dstVUIndex] = yv >> 8;
                dst[dstVUOffset + dstVUIndex + 1] = yu >> 8;
            }
        }
    }
}


static void process_image(const void *p, int size)
{
    int bytes_encoded;
    char *out = encode_dst_buffer;

    if (size == cap_fmt.fmt.pix.sizeimage)
    {
        //convert to nv12
        switch(cap_fmt.fmt.pix.pixelformat)
        {
        case V4L2_PIX_FMT_YUYV:
            yuyv_to_nv12((const uint16_t*)p, encode_src_buffer);
            break;

        case V4L2_PIX_FMT_UYVY:
            uyvy_to_nv12((const uint16_t*)p, encode_src_buffer);
            break;

        default:
            fprintf(stderr, "Unsupported pixel format: 0x%x\n", cap_fmt.fmt.pix.pixelformat);
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        fprintf(stderr, "bad frame size %d != %d", size, cap_fmt.fmt.pix.sizeimage);
    }

    bytes_encoded = vl_video_encoder_encode(codec_handle, FRAME_TYPE_AUTO, encode_src_buffer, encode_buffer_size, &out);

    if (bytes_encoded <= 0)
    {
        fprintf(stderr, "Unable to encode frame\n");
        exit(EXIT_FAILURE);
    }

    if(write(1, encode_dst_buffer, bytes_encoded) < 0)
    {
        errno_exit("write");
    }

    //fflush(stderr);
    //fprintf(stderr, ".");
}

static int read_frame(void)
{
    struct v4l2_buffer buf;

    CLEAR(buf);

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
        switch (errno) {
        case EAGAIN:
            return 0;

        case EIO:
            /* Could ignore EIO, see spec. */

            /* fall through */

        default:
            errno_exit("VIDIOC_DQBUF");
        }
    }

    assert(buf.index < n_buffers);

    process_image(buffers[buf.index].start, buf.bytesused);

    if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
        errno_exit("VIDIOC_QBUF");

    return 1;
}

static void mainloop(void)
{
    while (frame_count != 0) {
        for (;;) {
            fd_set fds;
            struct timeval tv;
            int r;

            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            /* Timeout. */
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            r = select(fd + 1, &fds, NULL, NULL, &tv);

            if (-1 == r) {
                if (EINTR == errno)
                    continue;
                errno_exit("select");
            }

            if (0 == r) {
                fprintf(stderr, "select timeout\n");
                exit(EXIT_FAILURE);
            }

            if (read_frame())
                break;
            /* EAGAIN - continue select loop. */
        }
        if (frame_count > 0) frame_count -= 1;
    }
}

static void stop_capturing(void)
{
    enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
        errno_exit("VIDIOC_STREAMOFF");
}


static void start_capturing(void)
{
    unsigned int i;
    enum v4l2_buf_type type;

    for (i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;

        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF");
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
        errno_exit("VIDIOC_STREAMON");

}

static void uninit_device(void)
{
    unsigned int i;
    for (i = 0; i < n_buffers; ++i)
        if (-1 == munmap(buffers[i].start, buffers[i].length))
            errno_exit("munmap");

    free(buffers);
}

static void init_codec(void)
{
    int bit_rate = 1000000;
    int gop = 10;

    codec_handle = vl_video_encoder_init(CODEC_ID_H264,
                                         cap_fmt.fmt.pix.width,
                                         cap_fmt.fmt.pix.height,
                                         frame_rate,
                                         bit_rate,
                                         gop,
                                         IMG_FMT_NV12);
    if ( NULL == codec_handle ){
        fprintf(stderr, "Unable to init codec\n");
        exit(EXIT_FAILURE);
    }

    encode_buffer_size = cap_fmt.fmt.pix.width * cap_fmt.fmt.pix.height * 4;
    encode_src_buffer = (char*)malloc(encode_buffer_size);
    encode_dst_buffer = (char*)malloc(encode_buffer_size);
}

static void init_mmap(void)
{
    struct v4l2_requestbuffers req;

    CLEAR(req);

    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s does not support "
                    "memory mapping\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffer memory on %s\n",
                dev_name);
        exit(EXIT_FAILURE);
    }

    buffers = (buffer*)calloc(req.count, sizeof(*buffers));

    if (!buffers) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }

    for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
        struct v4l2_buffer buf;

        CLEAR(buf);

        buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory      = V4L2_MEMORY_MMAP;
        buf.index       = n_buffers;

        if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
            errno_exit("VIDIOC_QUERYBUF");

        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start =
            mmap(NULL /* start anywhere */,
                 buf.length,
                 PROT_READ | PROT_WRITE /* required */,
                 MAP_SHARED /* recommended */,
                 fd, buf.m.offset);

        if (MAP_FAILED == buffers[n_buffers].start)
            errno_exit("mmap");
    }
}


static void init_device(void)
{
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    struct v4l2_streamparm stream_parm;
    unsigned int min;

    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s is no V4L2 device\n",
                    dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_QUERYCAP");
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s is no video capture device\n",
                dev_name);
        exit(EXIT_FAILURE);
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s does not support streaming i/o\n",
                dev_name);
        exit(EXIT_FAILURE);
    }

    /* Select video input, video standard and tune here. */


    CLEAR(cropcap);

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */

        if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
            switch (errno) {
            case EINVAL:
                /* Cropping not supported. */
                break;
            default:
                /* Errors ignored. */
                break;
            }
        }
    } else {
        /* Errors ignored. */
    }


    CLEAR(cap_fmt);
    cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (force_format) {
        cap_fmt.fmt.pix.width       = 720;
        cap_fmt.fmt.pix.height      = 576;
        cap_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        cap_fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

        if (-1 == xioctl(fd, VIDIOC_S_FMT, &cap_fmt))
            errno_exit("VIDIOC_S_FMT");

        CLEAR(stream_parm);
        stream_parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        stream_parm.parm.capture.timeperframe.numerator = 1;
        stream_parm.parm.capture.timeperframe.denominator = 25;

        if (-1 == xioctl(fd, VIDIOC_S_PARM, &stream_parm)) {
            errno_exit("VIDIOC_S_PARM");
        }

        /* Note VIDIOC_S_FMT may change width and height. */
    } else {
        /* Preserve original settings as set by v4l2-ctl for example */
        if (-1 == xioctl(fd, VIDIOC_G_FMT, &cap_fmt))
            errno_exit("VIDIOC_G_FMT");
    }

    CLEAR(stream_parm);
    stream_parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (-1 == xioctl(fd, VIDIOC_G_PARM, &stream_parm)) {
        errno_exit("VIDIOC_G_PARM");
    }

    frame_rate = stream_parm.parm.capture.timeperframe.denominator / stream_parm.parm.capture.timeperframe.numerator;
    fprintf(stderr, "Capture %dx%d %d fps\n",  cap_fmt.fmt.pix.width,  cap_fmt.fmt.pix.height, frame_rate);

    /* Buggy driver paranoia. */
    min = cap_fmt.fmt.pix.width * 2;
    if (cap_fmt.fmt.pix.bytesperline < min)
        cap_fmt.fmt.pix.bytesperline = min;
    min = cap_fmt.fmt.pix.bytesperline * cap_fmt.fmt.pix.height;
    if (cap_fmt.fmt.pix.sizeimage < min)
        cap_fmt.fmt.pix.sizeimage = min;

    init_mmap();
}

static void close_device(void)
{
    if (-1 == close(fd))
        errno_exit("close");

    fd = -1;
}

static void open_device(void)
{
    struct stat st;

    if (-1 == stat(dev_name, &st)) {
        fprintf(stderr, "Cannot identify '%s': %d, %s\n",
                dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "%s is no device\n", dev_name);
        exit(EXIT_FAILURE);
    }

    fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

    if (-1 == fd) {
        fprintf(stderr, "Cannot open '%s': %d, %s\n",
                dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static void usage(FILE *fp, int argc, char **argv)
{
        fprintf(fp,
                 "Usage: %s [options]\n\n"
                 "Version 1.0\n"
                 "Options:\n"
                 "-d | --device name   Video device name [%s]\n"
                 "-h | --help          Print this message\n"
                 "-f | --format        Force format to 720x576 YUYV (PAL)\n"
                 "-c | --count         Number of frames to grab (default infinite)\n"
                 "",
                 argv[0], dev_name, frame_count);
}

static const char short_options[] = "d:hfc:";

static const struct option
long_options[] = {
    { "device", required_argument, NULL, 'd' },
    { "help",   no_argument,       NULL, 'h' },
    { "format", no_argument,       NULL, 'f' },
    { "count",  required_argument, NULL, 'c' },
    { 0, 0, 0, 0 }
};

int main(int argc, char **argv)
{
    dev_name = "/dev/video0";

    for (;;) {
        int idx;
        int c;

        c = getopt_long(argc, argv,
                        short_options, long_options, &idx);

        if (-1 == c)
            break;

        switch (c) {
        case 0: /* getopt_long() flag */
            break;

        case 'd':
            dev_name = optarg;
            break;

        case 'h':
            usage(stdout, argc, argv);
            exit(EXIT_SUCCESS);

        case 'f':
            force_format++;
            break;

        case 'c':
            errno = 0;
            frame_count = strtol(optarg, NULL, 0);
            if (errno)
                errno_exit(optarg);
            break;

        default:
            usage(stderr, argc, argv);
            exit(EXIT_FAILURE);
        }
    }

    open_device();
    init_device();
    init_codec();
    start_capturing();
    mainloop();
    stop_capturing();
    uninit_device();
    close_device();
    //fprintf(stderr, "\n");
    return 0;
}
