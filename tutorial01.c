#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <ffmpeg/swscale.h>

#include <stdio.h>

#if LIBAVCODE_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

int main(int argc, char* argv[]) {
  // initialize the ffmpeg library.
  av_register_all();

  AVFormatContext* pFormatCtx = NULL;

  // open video file
  if (avformat_open_input(&pFormatCtx, argv[1] /* filename */, NULL /* file format, set to autodetect */, 0 /* buffer size, set to autodetect */, NULL /*format options, set to autodetect*/) != 0)
    return -1; // Couldn't open file :(

  // Retreive Stream information and populate it into pFormatCtx
  if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
    return -1; // could not find stream information

  // dumps information about the file on stderr
  av_dump_format(pFormatCtx, 0, argv[1], 0);
}
