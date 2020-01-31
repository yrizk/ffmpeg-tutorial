// tutorial03.c
// A pedagogical video player that will stream through every video frame as fast as it can
// and play audio (out of sync).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard, 
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101, SDL 1.2.15
// on GCC 4.7.2 in Debian February 2015
//
// Use
//
// gcc -o tutorial03 tutorial03.c -lavformat -lavcodec -lswscale -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed, 
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial03 myvideofile.mpg
//
// to play the stream on your screen.

#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue {
  AVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  int size;
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;

// global quit flag
int quit = 0;


//---- Forward Declaration of methods ----
void packet_queue_init(PacketQueue *queue);

int packet_queue_put(PacketQueue *queue, AVPacket *packet);

static int packet_queue_get(PacketQueue* queue, AVPacket *packet, int block);

void audio_callback(void *userdata, Uint8* stream, int len);

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t* audio_buf, int buf_size);

static int audio_resampling(AVCodecContext *audio_decode_ctx, 
                            AVFrame *audio_decode_frame,
                            enum AVSampleFormat out_sample_fmt, 
                            int out_channels, int out_sample_rate, 
                            uint8_t *out_buf);
void err(char *msg);
void err_d(char *msg, const char* err_extra);

/** 
 * Entry Point for the binary.
 * @param argc command line arguments counter
 * @param argv command line arguments
 * 
 * @return execution exit code.
 */
int main(int argc, char *argv[]) {
  if (argc != 2)
    err("Please supply a video file to play");

  char *filename[80];
  sscanf(argv[1], "%s", &filename);

  /*
   * Initialize the nondeprecated SDL functionality.
   */
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
    err_d("Couldn't initialize SDL %d", SDL_GetError());

  // demuxers read a media file and split it into packets
  AVFormatContext *v_format_ctx = NULL;
  if (avformat_open_input(&v_format_ctx, &filename, NULL, NULL) < 0)
    err("Couldn't open the video file.");

  // print detailed stats about the file (using the newly made context)
  av_dump_format(v_format_ctx, 0, &filename, 0);

  int video_stream_idx = -1;
  int audio_stream_idx = -1;
  int i;

  for (i = 0; i < v_format_ctx->nb_streams; i++) {
    if (v_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx == -1)
      video_stream_idx = i;
    if (v_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx == -1)
      audio_stream_idx = i;
  }

  if (video_stream_idx == -1 || audio_stream_idx == -1)
    err("Couldn't find a video or audio stream.");

  AVCodec *a_codec = NULL;
  a_codec = avcodec_find_decoder(v_format_ctx->streams[audio_stream_idx]->codecpar->codec_ic);
  if (!a_codec)
    err("couldn't find the decoder! The codec is unsupported.");

  AVCodecContext *a_codec_ctx = NULL;
  a_codec_ctx = avcodec_alloc_context3(a_codec);

  if (avcodec_parameters_to_context(a_codec_ctx, v_format_ctx->streams[audio_stream_idx]->codecpar) != 0)
    err("Couldn't copy the audio codec.");

  SDL_AudioSpec requested_specs, actual_specs;

  // set audio settings from the the codec
  requested_specs.freq = a_codec_ctx->sample_rate;
  requested_specs.format = AUDIO_S16SYS;
  requested_specs.channels = a_codec_ctx->channels;
  requested_specs.silence = 0;
  requested_specs.samples = SDL_AUDIO_BUFFER_SIZE;
  requested_specs.userdata = a_codec_ctx;

  // unsigned 32 bit int representing the audio device id
  SDL_AudioDeviceID audio_device_id = NULL;

  // See [1] below for details.
  audio_device_id = SDL_OpenAudioDevice(NULL, 
                                          0,
                                          &requested_specs, 
                                          &actual_specs, 
                                          SDL_AUDIO_ALLOW_FORMAT_CHANGE);

  if (audio_device_id == 0)
    err_d("Failed to open audio device", SDL_GetError());

  if (avcodec_open2(a_codec_ctx, a_codec, NULL) < 0)
    err("Couldn't open the audio codec.");

  packet_queue_init(&audioq);

  SDL_PauseAudioDevice(audio_device_id, 0);

  AVCodec* v_codec = NULL;
  v_codec = avcodec_find_decoder(v_format_ctx->streams[video_stream_idx]->codecpar->codec_id);
  if (!v_codec)
    err("Unsupported video codec!");

  // use the codec to get the video codec context
  AVCodecContext *v_codec_ctx = NULL;
  v_codec_ctx = avcodec_alloc_context3(v_codec);
  if (avcodec_parameters_to_context(v_codec_ctx, v_format_ctx->streams[video_stream_idx]->codecpar) != 0)
    err("Couldn't copy the video codec context");

  if (avcodec_open2(v_codec_ctx, v_codec, NULL) < 0)
    err("Couldn't open the video codec");

  AVFrame *v_frame = av_frame_alloc();
  if (!v_frame)
    err("Couldn't allocate an AVFrame for video");

  // Create a SDL window 
  SDL_Window *screen = SDL_CreateWindow(
      "My Video Player", 
      SDL_WINDOWPOS_UNDEFINED,
      SDL_WINDOWPOS_UNDEFINED, 
      v_codec_ctx->width,
      v_codec_ctx->height,
      SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);

  if (!screen)
    err("Couldn't create the SDL Window");

  SDL_GL_SetSwapInterval(1);

  // create a 2d rendering context for the window
  SDL_Renderer *renderer = NULL;
  renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);

  // create a texture for a rendering context
  SDL_Texture *texture = NULL;
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, v_codec_ctx->width, v_codec_ctx->height);


  AVPacket *v_packet = av_packet_alloc();
  if (!v_packet)
    err("allocation for a AVPacket failed");

  // set up our SWSContext to convert the image to YUV420
  struct SwsContext *sws_ctx = NULL;
  sws_ctx = sws_getContext(
      v_codec_ctx->width, 
      v_codec_ctx->height,
      v_codec_ctx->pix_fmt,
      v_codec_ctx->width,
      v_codec_ctx->height,
      AV_PIX_FMT_YUV420P,
      SWS_BILINEAR,
      NULL,
      NULL,
      NULL
  );
  int n_bytes;
  n_bytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 
                                     v_codec_ctx->width,
                                     v_codec_ctx->height, 32);

  // allocate image buffer
  uint8_t* buffer = NULL;
  buffer = (uint8_t*) av_malloc(n_bytes * sizeof(uint8_t));

  // alloc the AVFrame later user to contain the scaled frame
  AVFrame *pict = av_frame_alloc();

  av_image_fill_arrays(
      pict->data,
      pict->linesize,
      buffer,
      AV_PIX_FMT_YUV420P,
      v_codec_ctx->width,
      v_codec_ctx->height,
      32
  );

  SDL_Event event;

  int frame_idx = 0;

  while(av_read_frame(v_format_ctx, v_packet) >= 0) {
    if (v_packet->stream_index == video_stream_idx) {
      // give the decoder raw uncompressed data in an AVPacket
      if (avcodec_send_packet(v_codec_ctx, v_packet) < 0)
        err("Error sending the packet back for decoding");
      // get the decodec output data from the decoder
      int ret
      while (ret >= 0) {
        ret = avcodec_receive_frame(v_codec_ctx, v_frame)
        // check if the whole frame was decoded
        // see [3] for details
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
          break;
        else if (ret < 0)
          err("Something bad happened with decoding");
        sws_scale(
            sws_ctx,
            (uint8_t const * const *) v_frame->data,
            v_frame->linesize,
            0,
            v_codec_ctx->height,
            pict->data,
            pict->linesize
        );

        printf(
              "Frame %c (%d) pts %d dts %d key_frame %d [coded_picture_number %d, display_picture_number %d, %dx%d]\n"
              av_get_pciture_type_char(v_frame->pict_type),
              v_codec_ctx->frame_number,
              v_frame->pts,
              v_frame->pkt_dts,
              v_frame->key_frame,
              v_frame->coded_picture_number,
              v_frame->display_picture_number,
              v_codec_ctx->width,
              v_codec_ctx->height
            );
        SDL_Rect rect;
        rect.x = 0;
        rect.y = 0;
        rect.w = v_codec_ctx->width;
        rect.h = v_codec_ctx->height;

        SDL_UpdateYUVTexture(
            texture,
            &rect,
            pict->data[0],
            pict->linesize[0],
            pict->data[1],
            pict->linesize[1],
            pict->data[2],
            pict->linesize[2]
        );

        SDL_RenderClear(renderer);

        SDL_RenderCopy(renderer, texture, NULL, NULL);

        SDL_RenderPresent(renderer);

      }
    }
    else if (v_packet->stream_index == audio_stream_idx)
      packet_queue_put(&audioq, v_packet);

    else 
      av_packet_unref(v_packet);
    SDL_PollEvent(&event);
    switch(event.type) {
      case SDL_QUIT:
        printf("Quitting...\n");
        SDL_Quit();
        quit = 1;
        break;
      default:
        break;
    }
    if (quit)
      break;
  }
  av_packet_unref(v_packet);

  av_frame_free(&pict);
  av_free(pict);

  av_frame_free(&v_frame);
  av_free(v_frame);

  avcodec_close(v_codec_ctx);
  avcodec_close(a_codec_ctx);

  avformat_close_input(v_format_ctx);
  
  return 0;
}

void err(char *msg) {
  printf("ERROR: %s \n", msg);
  exit(1);
}

void err_d(char *msg, const char* err_extra) {
  printf("Error: %s . Failure Code: %s \n", msg, err_extra);
  exit(1);
}

void packet_queue_init(PacketQueue *queue) {
  memset(queue, 0, sizeof(PacketQueue));
  queue->mutex = SDL_CreateMutex();
  if (!queue->mutex)
    err("Could not create mutex");
  q->cond = SDL_CreateCond();
  if (!queue->cond)
    err("Couldn't create conditional variable. ");
}

/**
 * Puts the given AVPacket into the queue.
 * @param queue the queue to insert into 
 * @param pkt the AVPacket to be inserted
 * @return 0 on success non-zero otherwise.
 */
int packet_queue_add(PacketQueue *queue, AVPacket *pkt) {
  //see [4] for details
  AVPacket *av_packet_list;
  av_packet_list = av_malloc(sizeof(AVPacketList));

  if (!av_packet_list)
    err("failed to allocate memory for the AVPacketList");

  av_packet_list->pkt = *pkt;
  av_packet_list->next = NULL;
  SDL_LockMutex(queue->mutex);
  if (!queue->last_pkt)
    queue->last_pkt = av_packet_list;
  else
    queue->last_pkt->next = av_packet_list;

  queue->last_pkt = av_packet_list;
  queue->nb_packets++;
  queue->size += av_packet_list->pkt.size;
  SDL_CondSignal(queue->cond);
  SDL_UnlockMutex(queue->mutex);
  return 0;
}


/*
 * [1] SDL_OpenAudioDevice
 * Use this function to open a specific audio device
 * Returns a valid (positive) device id on success and 0 on failure with
 * SDL_GetError() containing more information.
 *
 * Compare this to SDL_OpenAudio; which always acts on device id 1 (and
 * therefore returns 1 on success). Therefore, SDL_OpenAudioDevice will 
 * never return 1 to remain compatible with this function.
 *
 * Arguments:
 *  device[char *]: utf-8 string representing the device id requested. 
 *                  NULL (which is what this code does) will direct the most reasonable
 *                  default (which is also what SDL_OpenAudio does)
 *
 *  iscapture[int]: 0 false, true otherwise. this opens the audio device for
 *                  recording, not playback. (what about both?!)
 *
 *  desired[SDL_AudioSpec]: SDL_AudioSpec specs that are requested.
 *
 *  obtained[SDL_AudioSpec]: the one you get.
 *
 *  allowed_changes[int]: 0 is a fine value. but this is actually a bunch of 
 *                        flags that are OR'd together.
 *
 *  The device starts out ready but paused. SDL_PauseAudioDevice() to start it.
 *
 * [2] SDL_PauseAudioDevice
 * Plays / Pauses the audio callback processing for a given device id. 
 * Newly-opened audio devices start in the paused state, so you call this
 * function with pause_on=0 after opening the specified audio device 
 * to start playing sound. the allows you to safely intialize data for the audio
 * callback function. 
 * Pausing state does not stack, which means that if you pause multiple times
 * and then play, the device will play.
 *
 *
 *
 */
