#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdlib.h>

#include <exception>
#include <vector>
#include <cstring>

#include "../codec_c1/vpcodec_1_0.h"


std::vector<char> read_file(char *name)
{
    // Load the NV12 test data
    int fd = open(name, O_RDONLY);
    if (fd < 0)
    {
        fprintf(stderr, "open failed.\n");
        throw std::exception();
    }

    off_t length = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    std::vector<char> data(length);
    ssize_t cnt = read(fd, &data[0], length);

    close(fd);

    if (cnt != length)
    {
        fprintf(stderr, "read failed.\n");
        throw std::exception();
    }
    return data;
}

int main()
{

    std::vector<char> data1 = read_file("640x480.1.yuv");
    std::vector<char> data2 = read_file("640x480.2.yuv");

    // Create an output file
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    int fdOut = open("test.h264", O_CREAT| O_TRUNC | O_WRONLY, mode);
    if (fdOut < 0)
    {
        fprintf(stderr, "open test.h24 failed\n");
        throw std::exception();
    }


    // Initialize the encoder
    vl_codec_id_t codec_id = CODEC_ID_H264;
    int width = 640;
    int height = 480;
    int frame_rate = 30;
    int bit_rate = 1000000;
    int gop = 10;

    vl_img_format_t img_format = IMG_FMT_NV12;

    vl_codec_handle_t handle = vl_video_encoder_init(codec_id, width, height, frame_rate, bit_rate, gop, img_format);
    fprintf(stderr, "handle = %ld\n", handle);


    // Encode the video frames
    const int BUFFER_SIZE = 1024 * 32 * 10;
    char buffer[BUFFER_SIZE];

    for (int i = 0; i < 30; ++i)
    {
        vl_frame_type_t type = FRAME_TYPE_AUTO;

        // Switch source images
        char* in = (rand() % 2) ? &data1[0] : &data2[0];

        int in_size = BUFFER_SIZE;
        char* out = buffer;

        int outCnt = vl_video_encoder_encode(handle, type, in, in_size, &out);
        fprintf(stderr, "vl_video_encoder_encode = %d\n", outCnt);

        if (outCnt > 0)
        {
            write(fdOut, buffer, outCnt);
        }
    }


    // Close the decoder
    vl_video_encoder_destory(handle);

    // Close the output file
    close(fdOut);

    return 0;
}
