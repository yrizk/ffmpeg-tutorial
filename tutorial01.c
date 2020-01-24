#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <stdio.h>

#if LIBAVCODE_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

int main(int argc, char* argv[]) {
  // initialize the ffmpeg library.
  av_register_all();

  AVFormatContext* pFormatCtx = NULL;

  /* open video file
   * Args:
   *  - AVFormatContext* 
   *  - argv[1] filename to open
   *  - file format: NULL to autodetect
   *  - format options: NULL to autodetect
   */
  if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
    return -1; // Couldn't open file :(

  // Retreive Stream information and populate it into pFormatCtx
  if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
    return -1; // could not find stream information

  // dumps information about the file on stdout
  av_dump_format(pFormatCtx, 0, argv[1], 0);

  int i; 
  AVCodecContext* pCodecCtxOrig = NULL;
  AVCodexContext* pCodecCtx = NULL;

  // Find the 1st video stream
  videoStream = -1;
  for (int i = 0; i < pFormatContext->nb_streams; i++) {
    if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
      videoStream = i;
      break;
    }
  }

  if (videoStream == -1) 
    return -1; // :( didn't find a video stream

  // get a pointer to the codec context for the video stream
  pCodecCtx = pFormatCtx->streams[videoStream]->codec;

  AVCodec* pCodec = NULL;
  // find the right decoder for this video stream
  pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
  if (pCodec == NULL) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  // Copy the context
  pCodecCtx = avcodec_alloc_context3(pCodec);
  if (avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
    fprintf(stderr, "Couldn't copy codec context\n");
    return -1;
  }
  if (avcodec_open2(pCodecCtx, pCodec) < 0)
    return -1; // couldn't open the codec

  AVFrame* pFrame = NULL;
  pFrame = av_frame_alloc();

  // Allocate an AVFrame struct
  pFrameRGB = av_frame_alloc();
  if(pFrameRGB == NULL)
    return -1;
}
