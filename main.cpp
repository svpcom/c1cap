/*
*
* Copyright (C) 2016 OtherCrashOverride@users.noreply.github.com.
* All rights reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2, as
* published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*
*/


// Developer notes:
// g++ -g main.cpp -o c2cap -l vpcodec
// ffmpeg -framerate 60 -i test.h264 -vcodec copy test.mp4
// sudo apt install libjpeg-turbo8-dev
// echo 1 | sudo tee /sys/class/graphics/fb0/blank

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <cstdlib> //atoi

#include <exception>
#include <vector>
#include <cstring>

#include "../c2_vpcodec/include/vpcodec_1_0.h"

#include <linux/videodev2.h> // V4L
#include <sys/mman.h>	// mmap

// The headers are not aware C++ exists
extern "C"
{
	//#include <amcodec/codec.h>
#include <codec.h>
}
// Codec parameter flags
//    size_t is used to make it 
//    64bit safe for use on Odroid C2
const size_t EXTERNAL_PTS = 0x01;
const size_t SYNC_OUTSIDE = 0x02;
const size_t USE_IDR_FRAMERATE = 0x04;
const size_t UCODE_IP_ONLY_PARAM = 0x08;
const size_t MAX_REFER_BUF = 0x10;
const size_t ERROR_RECOVERY_MODE_IN = 0x20;

#include <turbojpeg.h>
#include <memory>


#include "Exception.h"
#include "Stopwatch.h"
#include "Timer.h"
#include "Mutex.h"

// Ion video header from drivers\staging\android\uapi\ion.h
#include "ion.h"
#include "meson_ion.h"
#include "ge2d.h"
#include "ge2d_cmd.h"


const char* DEFAULT_DEVICE = "/dev/video0";
const char* DEFAULT_OUTPUT = "default.h264";
const int BUFFER_COUNT = 4;

const int DEFAULT_WIDTH = 640;
const int DEFAULT_HEIGHT = 480;
const int DEFAULT_FRAME_RATE = 30;
const int DEFAULT_BITRATE = 1000000 * 5;



const size_t MJpegDhtLength = 0x1A4;
unsigned char MJpegDht[MJpegDhtLength] = {
	/* JPEG DHT Segment for YCrCb omitted from MJPG data */
	0xFF,0xC4,0x01,0xA2,
	0x00,0x00,0x01,0x05,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x01,0x00,0x03,0x01,0x01,0x01,0x01,
	0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
	0x08,0x09,0x0A,0x0B,0x10,0x00,0x02,0x01,0x03,0x03,0x02,0x04,0x03,0x05,0x05,0x04,0x04,0x00,
	0x00,0x01,0x7D,0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,
	0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,0x24,
	0x33,0x62,0x72,0x82,0x09,0x0A,0x16,0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,0x29,0x2A,0x34,
	0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,0x56,
	0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,
	0x79,0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,
	0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,
	0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,
	0xDA,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,
	0xF8,0xF9,0xFA,0x11,0x00,0x02,0x01,0x02,0x04,0x04,0x03,0x04,0x07,0x05,0x04,0x04,0x00,0x01,
	0x02,0x77,0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
	0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xA1,0xB1,0xC1,0x09,0x23,0x33,0x52,0xF0,0x15,0x62,
	0x72,0xD1,0x0A,0x16,0x24,0x34,0xE1,0x25,0xF1,0x17,0x18,0x19,0x1A,0x26,0x27,0x28,0x29,0x2A,
	0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,0x56,
	0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,
	0x79,0x7A,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,
	0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,
	0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,
	0xD9,0xDA,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,
	0xF9,0xFA
};


struct BufferMapping
{
	void* Start;
	size_t Length;
};


struct option longopts[] = {
	{ "device",			required_argument,	NULL,	'd' },
	{ "output",			required_argument,	NULL,	'o' },
	{ "width",			required_argument,	NULL,	'w' },
	{ "height",			required_argument,	NULL,	'h' },
	{ "fps",			required_argument,	NULL,	'f' },
	{ "bitrate",		required_argument,	NULL,	'b' },
	{ "pixformat",		required_argument,	NULL,	'p' },
	{ 0, 0, 0, 0 }
};

//class Exception : public std::exception
//{
//public:
//	Exception(const char* message)
//		: std::exception()
//	{
//		fprintf(stderr, "%s\n", message);
//	}
//
//};


codec_para_t codecContext;
void OpenCodec(int width, int height, int fps)
{
	//fps *= 2;

	// Initialize the codec
	codecContext = { 0 };


	codecContext.stream_type = STREAM_TYPE_ES_VIDEO;
	codecContext.video_type = VFORMAT_MJPEG;
	codecContext.has_video = 1;
	codecContext.noblock = 0;
	codecContext.am_sysinfo.format = VIDEO_DEC_FORMAT_MJPEG;
	codecContext.am_sysinfo.width = width;
	codecContext.am_sysinfo.height = height;
	codecContext.am_sysinfo.rate = (96000.0 / fps);	
	codecContext.am_sysinfo.param = (void*)( SYNC_OUTSIDE); //EXTERNAL_PTS |


	int api = codec_init(&codecContext);
	if (api != 0)
	{
		throw Exception("codec_init failed.");		
	}
}

void WriteCodecData(unsigned char* data, int dataLength)
{
	int offset = 0;
	while (offset < dataLength)
	{
		int count = codec_write(&codecContext, data + offset, dataLength - offset);
		if (count > 0)
		{
			offset += count;
		}
	}
}


//timeval startTime;
//timeval endTime;
//
//void ResetTime()
//{
//	gettimeofday(&startTime, NULL);
//	endTime = startTime;
//}
//
//float GetTime()
//{
//	gettimeofday(&endTime, NULL);
//	float seconds = (endTime.tv_sec - startTime.tv_sec);
//	float milliseconds = (float(endTime.tv_usec - startTime.tv_usec)) / 1000000.0f;
//
//	startTime = endTime;
//
//	return seconds + milliseconds;
//}

//float ConvertTimeval(timeval* value)
//{
//	if (value == nullptr)
//		throw Exception("value is null.");
//
//	float seconds = value->tv_sec;
//	float milliseconds = ((float)value->tv_usec) / 1000000.0f;
//
//	return seconds + milliseconds;
//}


vl_codec_handle_t handle;
int encoderFileDescriptor = -1;
unsigned char* encodeNV12Buffer = nullptr;
char* encodeBitstreamBuffer = nullptr;
size_t encodeBitstreamBufferLength = 0;
Mutex encodeMutex;
unsigned char* mjpegData = nullptr;
size_t mjpegDataLength = 0;
bool needsMJpegDht = true;
double timeStamp = 0;
double frameRate = 0;

void EncodeFrame()
{
	encodeMutex.Lock();

	// Encode the video frames
	vl_frame_type_t type = FRAME_TYPE_AUTO;
	char* in = (char*)encodeNV12Buffer;
	int in_size = encodeBitstreamBufferLength;
	char* out = encodeBitstreamBuffer;
	int outCnt = vl_video_encoder_encode(handle, type, in, in_size, &out);
	//printf("vl_video_encoder_encode = %d\n", outCnt);

	encodeMutex.Unlock();

	if (outCnt > 0)
	{
		ssize_t writeCount = write(encoderFileDescriptor, encodeBitstreamBuffer, outCnt);
		if (writeCount < 0)
		{
			throw Exception("write failed.");
		}
	}


	//unsigned int pts = timeStamp * 96000;
	//codec_set_pcrscr(&codecContext, (int)pts);


	timeStamp += frameRate;
	//fprintf(stderr, "timeStamp=%f\n", timeStamp);
}

void EncodeFrameHardware()
{
	encodeMutex.Lock();

	// Encode the video frames
	vl_frame_type_t type = FRAME_TYPE_AUTO;
	char* in = (char*)encodeNV12Buffer;
	int in_size = encodeBitstreamBufferLength;
	char* out = encodeBitstreamBuffer;
	int outCnt = vl_video_encoder_encode(handle, type, in, in_size, &out);
	//printf("vl_video_encoder_encode = %d\n", outCnt);


	// Hardware
	unsigned int pts = timeStamp * 96000;
	//if (codec_checkin_pts(&codecContext, pts))
	//{
	//	printf("codec_checkin_pts failed\n");
	//}

	if (needsMJpegDht)
	{
		// Find the start of scan (SOS)
		unsigned char* sos = mjpegData;
		while (sos < mjpegData + mjpegDataLength - 1)
		{
			if (sos[0] == 0xff && sos[1] == 0xda)
				break;

			++sos;
		}

		// Send everthing up to SOS
		int headerLength = sos - mjpegData;
		WriteCodecData(mjpegData, headerLength);

		// Send DHT
		WriteCodecData(MJpegDht, MJpegDhtLength);

		// Send remaining data
		WriteCodecData(sos, mjpegDataLength - headerLength);

		//printf("dataLength=%lu, found SOS @ %d\n", dataLength, headerLength);
	}
	else
	{
		WriteCodecData(mjpegData, (int)mjpegDataLength);
	}

	encodeMutex.Unlock();

	if (outCnt > 0)
	{
		ssize_t writeCount = write(encoderFileDescriptor, encodeBitstreamBuffer, outCnt);
		if (writeCount < 0)
		{
			throw Exception("write failed.");
		}
	}


	codec_set_pcrscr(&codecContext, (int)pts);


	timeStamp += frameRate;
	fprintf(stderr, "timeStamp=%f\n", timeStamp);
}


struct IonBuffer
{
	ion_user_handle_t Handle;
	int ExportHandle;
	size_t Length;
	unsigned long PhysicalAddress;
};

IonBuffer IonAllocate(int ion_fd, size_t bufferSize)
{
	int io;
	IonBuffer result; 

	// Allocate a buffer
	ion_allocation_data allocation_data = { 0 };
	allocation_data.len = bufferSize;
	allocation_data.heap_id_mask = ION_HEAP_CARVEOUT_MASK;
	allocation_data.flags = ION_FLAG_CACHED;

	io = ioctl(ion_fd, ION_IOC_ALLOC, &allocation_data);
	if (io != 0)
	{
		throw Exception("ION_IOC_ALLOC failed.");
	}

	printf("ion handle=%d\n", allocation_data.handle);


	// Map/share the buffer
	ion_fd_data ionData = { 0 };
	ionData.handle = allocation_data.handle;

	io = ioctl(ion_fd, ION_IOC_SHARE, &ionData);
	if (io != 0)
	{
		throw Exception("ION_IOC_SHARE failed.");
	}

	printf("ion map=%d\n", ionData.fd);


	// Get the physical address for the buffer
	meson_phys_data physData = { 0 };
	physData.handle = ionData.fd;

	ion_custom_data ionCustomData = { 0 };
	ionCustomData.cmd = ION_IOC_MESON_PHYS_ADDR;
	ionCustomData.arg = (long unsigned int)&physData;

	io = ioctl(ion_fd, ION_IOC_CUSTOM, &ionCustomData);
	if (io != 0)
	{
		//throw Exception("ION_IOC_CUSTOM failed.");
		printf("ION_IOC_CUSTOM failed (%d).", io);
	}


	result.Handle = allocation_data.handle;
	result.ExportHandle = ionData.fd;
	result.Length = allocation_data.len;
	result.PhysicalAddress = physData.phys_addr;

	printf("ion phys_addr=%lu\n", result.PhysicalAddress);


	//ion_handle_data ionHandleData = { 0 };
	//ionHandleData.handle = allocation_data.handle;

	//io = ioctl(ion_fd, ION_IOC_FREE, &ionHandleData);
	//if (io != 0)
	//{
	//	throw Exception("ION_IOC_FREE failed.");
	//}

	return result;
}


enum class PictureFormat
{
	Unknown = 0,
	Yuyv = V4L2_PIX_FMT_YUYV,
	MJpeg = V4L2_PIX_FMT_MJPEG
};



Stopwatch sw;
Timer timer;

int ion_fd = -1;
IonBuffer YuvSource = { 0 };
IonBuffer YuvDestination = { 0 };


int main(int argc, char** argv)
{
	int io;


	// options
	const char* device = DEFAULT_DEVICE;
	const char* output = DEFAULT_OUTPUT;
	int width = DEFAULT_WIDTH;
	int height = DEFAULT_HEIGHT;
	int fps = DEFAULT_FRAME_RATE;
	int bitrate = DEFAULT_BITRATE;
	PictureFormat pixformat = PictureFormat::Yuyv;

	int c;
	while ((c = getopt_long(argc, argv, "d:o:w:h:f:b:p:", longopts, NULL)) != -1)
	{
		switch (c)
		{
			case 'd':			
				device = optarg;			
				break;

			case 'o':
				output = optarg;
				break;

			case 'w':
				width = atoi(optarg);
				break;

			case 'h':
				height = atoi(optarg);
				break;

			case 'f':
				fps = atoi(optarg);
				break;

			case 'b':
				bitrate = atoi(optarg);
				break;

			case 'p':
				if (strcmp(optarg, "yuyv") == 0)
				{
					pixformat = PictureFormat::Yuyv;
				}
				else if (strcmp(optarg, "mjpeg") == 0)
				{
					pixformat = PictureFormat::MJpeg;
				}
				else
				{
					throw Exception("Unknown pixformat.");
				}
				break;

			default:
				throw Exception("Unknown option.");
		}
	}


	int captureDev = open(device, O_RDWR);
	if (captureDev < 0)
	{
		throw Exception("capture device open failed.");
	}


	v4l2_capability caps = { 0 };
	io = ioctl(captureDev, VIDIOC_QUERYCAP, &caps);
	if (io < 0)
	{
		throw Exception("VIDIOC_QUERYCAP failed.");
	}

	printf("card = %s\n", (char*)caps.card);
	printf("\tbus_info = %s\n", (char*)caps.bus_info);
	printf("\tdriver = %s\n", (char*)caps.driver);

	if (!caps.capabilities & V4L2_CAP_VIDEO_CAPTURE)
	{
		throw Exception("V4L2_CAP_VIDEO_CAPTURE not supported.");
	}
	else
	{
		fprintf(stderr, "V4L2_CAP_VIDEO_CAPTURE supported.\n");
	}



	v4l2_fmtdesc formatDesc = { 0 };
	formatDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	
	fprintf(stderr, "Supported formats:\n");
	while (true)
	{
		io = ioctl(captureDev, VIDIOC_ENUM_FMT, &formatDesc);
		if (io < 0)
		{
			//printf("VIDIOC_ENUM_FMT failed.\n");
			break;
		}
		
		fprintf(stderr, "\tdescription = %s, pixelformat=0x%x\n", formatDesc.description, formatDesc.pixelformat);

		
		v4l2_frmsizeenum formatSize = { 0 };
		formatSize.pixel_format = formatDesc.pixelformat;

		while (true)
		{
			io = ioctl(captureDev, VIDIOC_ENUM_FRAMESIZES, &formatSize);
			if (io < 0)
			{
				//printf("VIDIOC_ENUM_FRAMESIZES failed.\n");
				break;
			}

			fprintf(stderr, "\t\twidth = %d, height = %d\n", formatSize.discrete.width, formatSize.discrete.height);


			v4l2_frmivalenum frameInterval = { 0 };
			frameInterval.pixel_format = formatSize.pixel_format;
			frameInterval.width = formatSize.discrete.width;
			frameInterval.height = formatSize.discrete.height;

			while (true)
			{
				io = ioctl(captureDev, VIDIOC_ENUM_FRAMEINTERVALS, &frameInterval);
				if (io < 0)
				{
					//printf("VIDIOC_ENUM_FRAMEINTERVALS failed.\n");
					break;
				}

				fprintf(stderr, "\t\t\tnumerator = %d, denominator = %d\n", frameInterval.discrete.numerator, frameInterval.discrete.denominator);
				++frameInterval.index;
			}


			++formatSize.index;
		}

		++formatDesc.index;
	}

	
	// Apply capture settings
	v4l2_format format = { 0 };
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = width;
	format.fmt.pix.height = height;
	//format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	//format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	format.fmt.pix.pixelformat = (__u32)pixformat;
	format.fmt.pix.field = V4L2_FIELD_ANY; //V4L2_FIELD_INTERLACED; //V4L2_FIELD_ANY; V4L2_FIELD_ALTERNATE

	io = ioctl(captureDev, VIDIOC_S_FMT, &format);
	if (io < 0)
	{
		throw Exception("VIDIOC_S_FMT failed.");
	}

	fprintf(stderr, "v4l2_format: width=%d, height=%d, pixelformat=0x%x\n",
		format.fmt.pix.width, format.fmt.pix.height, format.fmt.pix.pixelformat);

	// Readback device selected settings
	width = format.fmt.pix.width;
	height = format.fmt.pix.height;
	pixformat = (PictureFormat)format.fmt.pix.pixelformat;


	v4l2_streamparm streamParm = { 0 };
	streamParm.type = format.type;
	streamParm.parm.capture.timeperframe.numerator = 1;
	streamParm.parm.capture.timeperframe.denominator = fps;

	io = ioctl(captureDev, VIDIOC_S_PARM, &streamParm);
	if (io < 0)
	{
		throw Exception("VIDIOC_S_PARM failed.");
	}

	fprintf(stderr, "capture.timeperframe: numerator=%d, denominator=%d\n",
		streamParm.parm.capture.timeperframe.numerator,
		streamParm.parm.capture.timeperframe.denominator);

	// Note: Video is encoded at the requested framerate whether
	// the device produces it or not.  Therefore, there is no
	// need to read back teh fps.


	// Request buffers
	v4l2_requestbuffers requestBuffers = { 0 };
	requestBuffers.count = BUFFER_COUNT;
	requestBuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	requestBuffers.memory = V4L2_MEMORY_MMAP;

	io = ioctl(captureDev, VIDIOC_REQBUFS, &requestBuffers);
	if (io < 0)
	{
		throw Exception("VIDIOC_REQBUFS failed.");
	}

	
	// Map buffers
	BufferMapping bufferMappings[requestBuffers.count] = { 0 };
	for (int i = 0; i < requestBuffers.count; ++i)
	{
		v4l2_buffer buffer = { 0 };
		buffer.type = requestBuffers.type;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = i;

		io = ioctl(captureDev, VIDIOC_QUERYBUF, &buffer);
		if (io < 0)
		{
			throw Exception("VIDIOC_QUERYBUF failed.");
		}

		bufferMappings[i].Length = buffer.length;
		bufferMappings[i].Start = mmap(NULL, buffer.length,
			PROT_READ | PROT_WRITE, /* recommended */
			MAP_SHARED,             /* recommended */
			captureDev, buffer.m.offset);
	}


	// Queue buffers
	for (int i = 0; i < requestBuffers.count; ++i)
	{
		v4l2_buffer buffer = { 0 };
		buffer.index = i;
		buffer.type = requestBuffers.type;
		buffer.memory = requestBuffers.memory;

		io = ioctl(captureDev, VIDIOC_QBUF, &buffer);
		if (io < 0)
		{
			throw Exception("VIDIOC_QBUF failed.");
		}
	}


	// Create MJPEG codec
	//OpenCodec(width, height, fps);


	// Create an output file	
	int fdOut;
	
	if (std::strcmp(output, "-") == 0)
	{
		fdOut = 1; //stdout
	}
	else
	{
		mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
		
		fdOut = open(output, O_CREAT | O_TRUNC | O_WRONLY, mode);
		if (fdOut < 0)
		{
			throw Exception("open output failed.");
		}
	}

	encoderFileDescriptor = fdOut;


	
	// GE2D
	int ge2d_fd = open("/dev/ge2d", O_RDWR);
	if (ge2d_fd < 0)
	{
		throw Exception("open /dev/ge2d failed.");
	}



	// Ion
	ion_fd = open("/dev/ion", O_RDWR);
	if (ion_fd < 0)
	{
		throw Exception("open ion failed.");
	}


	YuvSource = IonAllocate(ion_fd, width * height * 4);
	
	void* yuvSourcePtr = mmap(NULL,
		YuvSource.Length,
		PROT_READ | PROT_WRITE,
		MAP_FILE | MAP_SHARED,
		YuvSource.ExportHandle,
		0);
	if (!yuvSourcePtr)
	{
		throw Exception("YuvSource mmap failed.");
	}


	YuvDestination = IonAllocate(ion_fd, width * height * 4);

	void* yuvDestinationPtr = mmap(NULL,
		YuvDestination.Length,
		PROT_READ | PROT_WRITE,
		MAP_FILE | MAP_SHARED,
		YuvDestination.ExportHandle,
		0);
	if (!yuvDestinationPtr)
	{
		throw Exception("YuvDestination mmap failed.");
	}




#if 1
	// Initialize the encoder
	vl_codec_id_t codec_id = CODEC_ID_H264;
	width = format.fmt.pix.width;
	height = format.fmt.pix.height;
	fps = (int)((double)streamParm.parm.capture.timeperframe.denominator /
				(double)streamParm.parm.capture.timeperframe.numerator);
	//int bit_rate = bitrate;
	int gop = 10;

	fprintf(stderr, "vl_video_encoder_init: width=%d, height=%d, fps=%d, bitrate=%d, gop=%d\n",
		width, height, fps, bitrate, gop);

	vl_img_format_t img_format = IMG_FMT_NV12;
	handle = vl_video_encoder_init(codec_id, width, height, fps, bitrate, gop, img_format);
	fprintf(stderr, "handle = %ld\n", handle);
#endif


	// Start streaming
	int bufferType = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	io = ioctl(captureDev, VIDIOC_STREAMON, &bufferType);
	if (io < 0)
	{
		throw Exception("VIDIOC_STREAMON failed.");
	}


	int nv12Size = format.fmt.pix.width * format.fmt.pix.height * 4;
	unsigned char* nv12 = (unsigned char*)yuvDestinationPtr; //new unsigned char[nv12Size];
	encodeNV12Buffer = nv12;
	fprintf(stderr, "nv12Size = %d\n", nv12Size);

	int ENCODE_BUFFER_SIZE = nv12Size; //1024 * 32;
	char* encodeBuffer = new char[ENCODE_BUFFER_SIZE];
	encodeBitstreamBuffer = encodeBuffer;
	//fprintf(stderr, "ENCODEC_BUFFER_SIZE = %d\n", ENCODEC_BUFFER_SIZE);


	// jpeg-turbo
	tjhandle jpegDecompressor = tjInitDecompress();
	int jpegWidth = 0;
	int jpegHeight = 0;
	int jpegSubsamp = 0;
	int jpegColorspace = 0;
	unsigned char* jpegYuv = nullptr;
	unsigned long jpegYuvSize = 0;


	bool isFirstFrame = true;
	/*bool needsMJpegDht = true;*/
	int frames = 0;
	float totalTime = 0;
	float lastTimestamp = 0;
	sw.Start(); //ResetTime();
	
	timer.Callback = EncodeFrame; //EncodeFrameHardware
	timer.SetInterval(1.0 / fps);
	//timer.Start();

	while (true)
	{
		// get buffer
		v4l2_buffer buffer = { 0 };
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;

		io = ioctl(captureDev, VIDIOC_DQBUF, &buffer);
		if (io < 0)
		{
			throw Exception("VIDIOC_DQBUF failed.");
		}


		//float timestamp = ConvertTimeval(&buffer.timestamp);
		//float elapsedTime = timestamp - lastTimestamp;

		//printf("Got a buffer: index=%d, timestamp=%f, elapsed=%f\n", buffer.index, timestamp, elapsedTime);

		/*
		__u32	type	Frame rate the timecodes are based on, see Table 3-6.
		__u32	flags	Timecode flags, see Table 3-7.
		__u8	frames	Frame count, 0 ... 23/24/29/49/59, depending on the type of timecode.
		__u8	seconds	Seconds count, 0 ... 59. This is a binary, not BCD number.
		__u8	minutes	Minutes count, 0 ... 59. This is a binary, not BCD number.
		__u8	hours	Hours count, 0 ... 29. This is a binary, not BCD number.
		__u8	userbits[4]	The "user group" bits from the timecode.
		*/
		//printf("\ttype=%d, flags=%d, frames=%d, seconds=%d, minutes=%d, hours=%d\n",
		//	buffer.timecode.type, buffer.timecode.flags, buffer.timecode.frames, buffer.timecode.seconds, buffer.timecode.minutes, buffer.timecode.hours);

		//lastTimestamp = timestamp;

		if (pixformat == PictureFormat::Yuyv)
		{
			if (isFirstFrame)
			{
				frameRate = timer.Interval();
				timer.Start();

				isFirstFrame = false;
			}


			// Process frame

#if 0
			//printf("Got a buffer: index = %d\n", buffer.index);
			unsigned short* data = (unsigned short*)bufferMappings[buffer.index].Start;

			// convert YUYV to NV12
			int srcStride = format.fmt.pix.width; // *sizeof(short);
			int dstStride = format.fmt.pix.width;
			int dstVUOffset = format.fmt.pix.width * format.fmt.pix.height;

			for (int y = 0; y < format.fmt.pix.height; ++y)
			{
				for (int x = 0; x < format.fmt.pix.width; x += 2)
				{
					int srcIndex = y * srcStride + x;
					//unsigned char l = data[srcIndex];
					unsigned short yu = data[srcIndex];
					unsigned short yv = data[srcIndex + 1];


					int dstIndex = y * dstStride + (x);
					nv12[dstIndex] = yu & 0xff;
					nv12[dstIndex + 1] = yv & 0xff;

					if (y % 2 == 0)
					{
						int dstVUIndex = (y >> 1) * dstStride + (x);
						nv12[dstVUOffset + dstVUIndex] = yv >> 8;
						nv12[dstVUOffset + dstVUIndex + 1] = yu >> 8;
					}
				}
			}
#else
			// Blit

			encodeMutex.Lock();


			unsigned char* data = (unsigned char*)bufferMappings[buffer.index].Start;
			size_t dataLength = buffer.bytesused;

#if 0
			memcpy((void*)yuvSourcePtr, data, dataLength);
#else
			unsigned char* dest = (unsigned char*)yuvSourcePtr;

			for (size_t i = 0; i < dataLength; i += 4)
			{
				dest[i] = data[i + 1];
				dest[i + 1] = data[i + 0];
				dest[i + 2] = data[i + 3];
				dest[i + 3] = data[i + 2];
			}
#endif

			//jpegYuv = (unsigned char*)yuvSourcePtr;


			{
				// Syncronize the source data

				ion_fd_data ionFdData = { 0 };
				ionFdData.fd = YuvSource.ExportHandle;

				io = ioctl(ion_fd, ION_IOC_SYNC, &ionFdData);
				if (io != 0)
				{
					throw Exception("ION_IOC_SYNC failed.");
				}
			}


			// Configure GE2D

			config_para_s config = { 0 };

			config.src_dst_type =ALLOC_ALLOC; //ALLOC_OSD0;
			config.alu_const_color = 0xffffffff;
			
			config.src_format = GE2D_LITTLE_ENDIAN | GE2D_FMT_S16_YUV422;
			config.src_planes[0].addr = YuvSource.PhysicalAddress;
			config.src_planes[0].w = width;
			config.src_planes[0].h = height;

			config.dst_format = GE2D_LITTLE_ENDIAN | GE2D_FORMAT_M24_NV21; //GE2D_FORMAT_S32_ARGB;
			config.dst_planes[0].addr = YuvDestination.PhysicalAddress;
			config.dst_planes[0].w = width;
			config.dst_planes[0].h = height;
			config.dst_planes[1].addr = config.dst_planes[0].addr + (format.fmt.pix.width * format.fmt.pix.height);
			config.dst_planes[1].w = width;
			config.dst_planes[1].h = height / 2;

			io = ioctl(ge2d_fd, GE2D_CONFIG, &config);
			if (io < 0)
			{
				throw Exception("GE2D_CONFIG failed");
			}


			// Perform the blit operation
			ge2d_para_s blitRect = { 0 };

			blitRect.src1_rect.x = 0;
			blitRect.src1_rect.y = 0;
			blitRect.src1_rect.w = width;
			blitRect.src1_rect.h = height;

			blitRect.dst_rect.x = 0;
			blitRect.dst_rect.y = 0;
			blitRect.dst_rect.w = width;
			blitRect.dst_rect.h = height;

			io = ioctl(ge2d_fd, GE2D_BLIT_NOALPHA, &blitRect);
			if (io < 0)
			{
				throw Exception("GE2D_BLIT_NOALPHA failed.");
			}

			//printf("GE2D Blit OK.\n");


			{
				// Syncronize the destination data
				ion_fd_data ionFdData = { 0 };
				ionFdData.fd = YuvDestination.ExportHandle;

				io = ioctl(ion_fd, ION_IOC_SYNC, &ionFdData);
				if (io != 0)
				{
					throw Exception("ION_IOC_SYNC failed.");
				}
			}

#endif

			encodeMutex.Unlock();


		}
		else if (pixformat == PictureFormat::MJpeg)
		{
			// MJPEG
			unsigned char* data = (unsigned char*)bufferMappings[buffer.index].Start;
			size_t dataLength = buffer.bytesused;

			mjpegData = data;
			mjpegDataLength = dataLength;

			//printf("dataLength=%lu\n", dataLength);

			if (isFirstFrame)
			{
				unsigned char* scan = data;
				while (scan < data + dataLength - 4)
				{
					if (scan[0] == MJpegDht[0] &&
						scan[1] == MJpegDht[1] &&
						scan[2] == MJpegDht[2] &&
						scan[3] == MJpegDht[3])
					{
						needsMJpegDht = false;
						break;
					}

					++scan;
				}

				isFirstFrame = false;

				fprintf(stderr, "needsMjpegDht = %d\n", needsMJpegDht);


				// jpeg-turbo
				int api = tjDecompressHeader3(jpegDecompressor,
					data,
					dataLength,
					&jpegWidth,
					&jpegHeight,
					&jpegSubsamp,
					&jpegColorspace);
				if (api != 0)
				{
					char* message = tjGetErrorStr();
					fprintf(stderr, "tjDecompressHeader3 failed (%s)\n", message);
				}
				else
				{
					fprintf(stderr, "jpegWidth=%d jpegHeight=%d jpegSubsamp=%d jpegColorspace=%d\n",
						jpegWidth, jpegHeight, jpegSubsamp, jpegColorspace);
				}

				// TODO: fail if not YUV422

				unsigned long jpegYuvSize = tjBufSizeYUV2(jpegWidth,
					1,
					jpegHeight,
					jpegSubsamp);

				//jpegYuv = new unsigned char[jpegYuvSize];
				jpegYuv = (unsigned char*)yuvSourcePtr;

				fprintf(stderr, "jpegYuv=%p jpegYuvSize=%lu\n",
					jpegYuv, jpegYuvSize);


				frameRate = timer.Interval();
				timer.Start();
			}


#if 0
			ssize_t writeCount = write(fdOut, data, buffer.bytesused);
			if (writeCount < 0)
			{
				throw Exception("write failed.");
			}
#endif


#if 0
			if (needsMJpegDht)
			{
				// Find the start of scan (SOS)
				unsigned char* sos = data;
				while (sos < data + dataLength - 1)
				{
					if (sos[0] == 0xff && sos[1] == 0xda)
						break;

					++sos;
				}

				// Send everthing up to SOS
				int headerLength = sos - data;
				WriteCodecData(data, headerLength);

				// Send DHT
				WriteCodecData(MJpegDht, MJpegDhtLength);

				// Send remaining data
				WriteCodecData(sos, dataLength - headerLength);

				//printf("dataLength=%lu, found SOS @ %d\n", dataLength, headerLength);
			}
			else
			{
				WriteCodecData(data, dataLength);
			}
#endif


			// jpeg-turbo
			int api = tjDecompressToYUV2(jpegDecompressor,
				data,
				dataLength,
				jpegYuv,
				jpegWidth,
				1,
				jpegHeight,
				TJFLAG_FASTDCT);	//TJFLAG_ACCURATEDCT
			if (api != 0)
			{
				char* message = tjGetErrorStr();
				fprintf(stderr, "tjDecompressToYUV2 failed (%s)\n", message);
			}
			else
			{
				encodeMutex.Lock();

#if 0
				// convert YUV422 to NV12
				int srcStride = format.fmt.pix.width;
				unsigned char* srcY = jpegYuv;
				unsigned char* srcU = srcY + (format.fmt.pix.width * format.fmt.pix.height);
				unsigned char* srcV = srcU + ((format.fmt.pix.width / 2) * format.fmt.pix.height);

				int dstStride = format.fmt.pix.width;
				int dstVUOffset = format.fmt.pix.width * format.fmt.pix.height;

				for (int y = 0; y < format.fmt.pix.height; ++y)
				{
					for (int x = 0; x < format.fmt.pix.width; x += 2)
					{
						int srcIndex = y * srcStride + x;
						int chromaIndex = y * (srcStride >> 1) + (x >> 1);

						//unsigned char l = data[srcIndex];
						unsigned short yu = srcY[srcIndex] | (srcU[chromaIndex] << 8);
						unsigned short yv = srcY[srcIndex + 1] | (srcV[chromaIndex] << 8);


						int dstIndex = y * dstStride + (x);
						nv12[dstIndex] = yu & 0xff;
						nv12[dstIndex + 1] = yv & 0xff;

						if (y % 2 == 0)
						{
							int dstVUIndex = (y >> 1) * dstStride + (x);
							nv12[dstVUOffset + dstVUIndex] = yv >> 8;
							nv12[dstVUOffset + dstVUIndex + 1] = yu >> 8;
						}
					}
				}

#else
				// Blit

				{
					// Syncronize the source data

					ion_fd_data ionFdData = { 0 };
					ionFdData.fd = YuvSource.ExportHandle;

					io = ioctl(ion_fd, ION_IOC_SYNC, &ionFdData);
					if (io != 0)
					{
						throw Exception("ION_IOC_SYNC failed.");
					}
				}


				// Configure GE2D

				config_para_s config = { 0 };

				config.src_dst_type = ALLOC_ALLOC; //ALLOC_OSD0;
				config.alu_const_color = 0xffffffff;
				//GE2D_FORMAT_S16_YUV422T, GE2D_FORMAT_S16_YUV422B kernel panics
				config.src_format = GE2D_LITTLE_ENDIAN | GE2D_FORMAT_M24_YUV422; //GE2D_LITTLE_ENDIAN | GE2D_FORMAT_S8_Y;
				config.dst_format = GE2D_LITTLE_ENDIAN | GE2D_FORMAT_M24_NV21; //GE2D_FORMAT_S32_ARGB;

				config.src_planes[0].addr = YuvSource.PhysicalAddress;
				config.src_planes[0].w = width;
				config.src_planes[0].h = height;
				config.src_planes[1].addr = config.src_planes[0].addr + (width * height);
				config.src_planes[1].w = width / 2;
				config.src_planes[1].h = height;
				config.src_planes[2].addr = config.src_planes[1].addr + ((width / 2) * height);
				config.src_planes[2].w = width / 2;
				config.src_planes[2].h = height;

				config.dst_planes[0].addr = YuvDestination.PhysicalAddress;
				config.dst_planes[0].w = width;
				config.dst_planes[0].h = height;
				config.dst_planes[1].addr = config.dst_planes[0].addr + (format.fmt.pix.width * format.fmt.pix.height);
				config.dst_planes[1].w = width;
				config.dst_planes[1].h = height / 2;

				io = ioctl(ge2d_fd, GE2D_CONFIG, &config);
				if (io < 0)
				{
					throw Exception("GE2D_CONFIG failed");
				}


				// Perform the blit operation
				ge2d_para_s blitRect = { 0 };

				blitRect.src1_rect.x = 0;
				blitRect.src1_rect.y = 0;
				blitRect.src1_rect.w = width;
				blitRect.src1_rect.h = height;

				blitRect.dst_rect.x = 0;
				blitRect.dst_rect.y = 0;
				blitRect.dst_rect.w = width;
				blitRect.dst_rect.h = height;

				io = ioctl(ge2d_fd, GE2D_BLIT_NOALPHA, &blitRect);
				if (io < 0)
				{
					throw Exception("GE2D_BLIT_NOALPHA failed.");
				}

				//printf("GE2D Blit OK.\n");


				{
					// Syncronize the destination data
					ion_fd_data ionFdData = { 0 };
					ionFdData.fd = YuvDestination.ExportHandle;

					io = ioctl(ion_fd, ION_IOC_SYNC, &ionFdData);
					if (io != 0)
					{
						throw Exception("ION_IOC_SYNC failed.");
					}
				}

#endif

				encodeMutex.Unlock();
			}
		}
		else
		{
			throw Exception("Unsupported PictureFromat.");
		}


		//Preview
		config_para_s config = { 0 };

		config.src_dst_type = ALLOC_OSD0;
		config.alu_const_color = 0xffffffff;

		config.src_format = GE2D_LITTLE_ENDIAN | GE2D_FORMAT_M24_NV21;
		config.src_planes[0].addr = YuvDestination.PhysicalAddress;
		config.src_planes[0].w = width;
		config.src_planes[0].h = height;
		config.src_planes[1].addr = config.src_planes[0].addr + (width * height);
		config.src_planes[1].w = width;
		config.src_planes[1].h = height / 2;

		config.dst_format = GE2D_FORMAT_S32_ARGB;

		io = ioctl(ge2d_fd, GE2D_CONFIG, &config);
		if (io < 0)
		{
			throw Exception("GE2D_CONFIG failed");
		}

		// Perform the blit operation
		ge2d_para_s blitRect = { 0 };

		blitRect.src1_rect.x = 0;
		blitRect.src1_rect.y = 0;
		blitRect.src1_rect.w = width;
		blitRect.src1_rect.h = height;

		blitRect.dst_rect.x = 0;
		blitRect.dst_rect.y = 0;
		blitRect.dst_rect.w = width;
		blitRect.dst_rect.h = height;

		// Note GE2D_STRETCHBLIT_NOALPHA is required to operate properly
		io = ioctl(ge2d_fd, GE2D_STRETCHBLIT_NOALPHA, &blitRect);
		if (io < 0)
		{
			throw Exception("GE2D_STRETCHBLIT_NOALPHA failed.");
		}

		//printf("GE2D Blit OK.\n");


		// return buffer
		io = ioctl(captureDev, VIDIOC_QBUF, &buffer);
		if (io < 0)
		{
			throw Exception("VIDIOC_QBUF failed.");
		}


		// Measure FPS
		++frames;
		totalTime += (float)sw.Elapsed(); //GetTime();
		
		sw.Reset();

		if (totalTime >= 1.0f)
		{
			int fps = (int)(frames / totalTime);
			fprintf(stderr, "FPS: %i\n", fps);

			frames = 0;
			totalTime = 0;
		}
	}


	return 0;
}
