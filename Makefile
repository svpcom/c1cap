CODEC_LIB:=../codec_c1
CXXFLAGS=-O2 -Wall -mfpu=neon -ftree-vectorize -ffast-math -I$(CODEC_LIB)/include

all:  c1cap test

run_test: test
	rm -f test.avi test.h264
	./test
	ffmpeg -framerate 30 -f h264 -i test.h264 -vcodec copy  test.avi

c1cap: cap.c $(CODEC_LIB)/libvpcodec.a
	$(CXX) $(CXXFLAGS) $^ -o $@

test: test.cpp $(CODEC_LIB)/libvpcodec.a
	$(CXX) $(CXXFLAGS) $^ -o $@

