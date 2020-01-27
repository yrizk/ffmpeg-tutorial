#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <stdio.h>

#if LIBAVCODE_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_free avcodec_free_frame
#endif

// ppm files simply lays out rgb values sequentially.
void SaveFrame(AVFrame* pFrame, int width, int height, int iFrame) {
  FILE *pFile;
  char szFilename[32];
  int y; 

  // open 
  sprintf(szFilename, "frame%d.ppm", iFrame);
  pFile = fopen(szFilename, "wb");
  if (pFile == NULL)
    return 
  fprintf(pFile, "P6\n%d %d\n255\n", width, height);
  
  // write the pixel data
  for (y = 0; y < height; y++)
    fwrite(pFrame->data[0] + y * pFrame->linesize[0], 1, width * 3, pFile);
  //close 
  fclose(pFile);
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printf("please provide a movie file\n");
    return -1;
  }

  // initialize the ffmpeg library.
  av_register_all();

  AVFormatContext *pFormatCtx = NULL;

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
  AVCodecContext *pCodecCtxOrig = NULL;
  AVCodecContext *pCodecCtx = NULL;

  // Find the 1st video stream
  int videoStream = -1;
  for (i = 0; i < pFormatCtx->nb_streams; i++) {
    if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
      videoStream = i;
      break;
    }
  }

  if (videoStream == -1) 
    return -1; // :( didn't find a video stream

  // get a pointer to the codec context for the video stream
  pCodecCtxOrig = pFormatCtx->streams[videoStream]->codec;

  AVCodec *pCodec = NULL;
  // find the right decoder for this video stream
  pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
  if (pCodec == NULL) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  // Copy the context (after allocating memory for it)
  pCodecCtx = avcodec_alloc_context3(pCodec);
  if (avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
    fprintf(stderr, "Couldn't copy codec context\n");
    return -1;
  }
  if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
    return -1; // couldn't open the codec

  AVFrame *pFrame = NULL;
  AVFrame *pFrameRGB = NULL;

  // Allocate an AVFrame struct
  pFrame = av_frame_alloc();
  pFrameRGB = av_frame_alloc();


  if(pFrameRGB == NULL)
    return -1;
  uint8_t *buffer = NULL;
  int num_bytes;
  num_bytes = avpicture_get_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
  buffer = (uint8_t*) av_malloc(num_bytes * sizeof(uint8_t));
  // assign correct parts of the buffer to image planes in pFrameRGB
  // AVFrame is a superset of AVPicture, which is how this cast is safe.
  avpicture_fill((AVPicture*) pFrameRGB, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
  /*
   * READING THE DATA
   */
  struct SwsContext* sws_ctx = NULL;
  int frame_finished;
  AVPacket packet;
  // initialize sws context for software scaling
  sws_ctx = sws_getContext(pCodecCtx->width, 
      pCodecCtx->height,
      pCodecCtx->pix_fmt,
      pCodecCtx->width,
      pCodecCtx->height,
      AV_PIX_FMT_RGB24,
      SWS_BILINEAR,
      NULL,
      NULL,
      NULL);
  i = 0;
  while (av_read_frame(pFormatCtx, &packet) >= 0){
    // is this a packet from the video stream?
    if (packet.stream_index  == videoStream) {
      // decode the video frame
      avcodec_decode_video2(pCodecCtx, pFrame, &frame_finished, &packet);
      // did we get a full frame?
      if (frame_finished) {
        sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);
        if (++i <=5)
          SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
      }
    }
    // free the packet that was allocated by av_read_frame
    av_free_packet(&packet);
  }

  //free the rgb image
  av_free(buffer);
  av_free(pFrameRGB);

  // free the yuv frame
  av_free(pFrame);

  // close the codecs
  avcodec_close(pCodecCtx);
  avcodec_close(pCodecCtxOrig);

  // close the video file
  avformat_close_input(&pFormatCtx);
  return 0;
}
