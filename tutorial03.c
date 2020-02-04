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
  a_codec = avcodec_find_decoder(v_format_ctx->streams[audio_stream_idx]->codecpar->codec_id);
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
              av_get_picture_type_char(v_frame->pict_type),
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

// this is understood: I get this.
void packet_queue_init(PacketQueue *q) {
  memset(q,0,sizeof(PacketQueue));
  q->mutex = SDL_CreateMutex();
  q->cond = SDL_CreateCond();
}

/**
 * I also now understand this function.
 * Puts the given AVPacket into the queue.
 * @param queue the queue to insert into 
 * @param pkt the AVPacket to be inserted
 * @return 0 on success non-zero otherwise.
 */
int packet_queue_put(PacketQueue *queue, AVPacket *pkt) {
  // allocate memory for the counter.
  AVPacketList* to_add = av_malloc(sizeof(AVPacketList)); 
  if (!current)
    return -1;
  to_add->pkt = *pkt;
  // since this is a q, we insert at the end of the list and therefore NULL next
  to_add->next = NULL;

  // now, it is time to add to_add to the q. we need to run through the q 
  SDL_LockMutex(queue->mutex);

  if (!queue->last_pkt)
    // last pkt is null means the first one is null so the whole queue is empty
    queue->first_pkt = to_add;
  else
    queue->last_pkt->next = to_add; // add to the end

  queue->last_pkt = to_add;

  queue->nb_packets++;

  queue->size += to_add->pkt.size;

  SDL_CondSignal(q->cond);
  SDL_UnlockMutex(q->mutex);
  return 0;
}

/**
 * I rewrote this too. I'm not sure about the queue clearing stuff.
 * Pops an AVPacket out of the PacketQueue
 * @param q the PacketQueue to extract from
 * @param pkt the first AVPacket extracted from the queue
 * @param block 0 to avoid waiting for an AVPacket to be inserted (if the queue
 * is empty) and nonzero otherwise.
 * 
 * @return < 0 if the quit flag was set, 0 if the queue was empty and 1 if its 
 *         not empty and a packet was extracted.
 */

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
  int ret = 0;
  AVPacketList* item; 

  SDL_LockMutex(q->mutex);
  for (;;) {
    if (quit) {
      ret = -1;
      break;
    }
    item = q->first_pkt;
    if (item) {
      // let's update the list (removing the first item) if its there.
      if (item->next) {
        // update the first_pkt location to the next one.
        q->first_pkt->next = q->first_pkt->next->next;
      }
      q->first_pkt = item->next;
      q->nb_packets--;
      q->size -= item->pkt.size;
      *pkt = item->pkt;
      av_free(item);
      ret = 1;
      break;
    }
    else if (!block) {
      // not blocking
      ret = 0;
      break;
    } else {
      // unlock mutex wait for cond signal, then lock the mutex again
      SDL_CondWait(q->cond, q->mutex);
    }
  }
  SDL_UnlockMutex(q->mutex);
  return ret;
}

/*
 * Pull in data from audio_decode_frame(), store the result in an intermediary 
 * buffer, attempt to write as many bytes as the amount defined by len to stream
 * and get more data if we dont have enough or save it for the next run if we
 * have some left over.
 *
 * @param userdata the pointer stuffed into SDL_AudioSpec
 * @param stream the buffer we will be writing audio data to
 * @param len the size of that buffer (must write extra later on)
 *
 */
void audio_callback(void *userdata, Uint8 *stream, int total_bytes_to_write) {
  AVFormatContext *a_codec_ctx = (AVFormatContext *) userdata;

  int bytes_to_write = 0;
  int size_decoded_data = 0;

  static uint8_t buf[MAX_AUDIO_FRAME_SIZE * 3 / 2];
  static unsigned int offset = 0;
  static unsigned int size = 0;

  while (total_bytes_to_write > 0)  {
    if (quit) 
      return;
    if (offset >= size) {
      // request more data
      size_decoded_data = audio_decode_frame(a_codec_ctx, buf, sizeof(buf));
      if (size_decoded_data < 0) {
        size_decoded_data = 1024;
        memset(buf, 0, size_decoded_data);
        printf("audio_decode_frame failed this time around.\n");
      } else {
        size = size_decoded_data;
      }
      offset = 0;
    }
    bytes_to_write = size - offset;
    if (bytes_to_write > total_bytes_to_write)
      bytes_to_write = total_bytes_to_write;

    memcpy(stream, (uint8_t*) buf + offset, bytes_to_write);
    
    total_bytes_to_write -= bytes_to_write;
    stream += bytes_to_write;
    offset += bytes_to_write;

  }

}

/*
 * Get a packet from the queue if it is available. Decode the extracted packet. 
 * once we have a farme, resample it and copy it into the buffer, making sure 
 * that data_size is smaller than the audio buffer.
 *
 * @param a_codec_ctx: the audio AVCodecContext used for decoding
 * @param audio_buf: the audio buffer that we will be writing into.
 * @param buf_size: the size of hte audio buffer, 1.5x larger the one ffmpeg 
 *                  gives us.
 *
 * @return: 0 if all went well -1 in case quit or an error.
 */

int audio_decode_frame(AVCodecContext *a_codec_ctx, uint8_t* audio_buf, int buf_size) {
  AVPacket* av_packet = av_packet_alloc();
  static uint8_t* pkt_data = NULL;
  static int pkt_size = 0;

  int current_len = 0;
  int resampled_data_size = 0;
  AVFrame* av_frame = av_frame_alloc();
  if (!av_frame)
    err("av frame failed to allocate");
  for(;;) {
    while(pkt_size > 0) {
      int got_frame = 0;
      int ret = avcodec_receive_frame(a_codec_ctx, av_frame);
      if (ret == 0)
        got_frame = 1;
      if (ret == AVERROR(EAGAIN))
        ret = 0;
      if (ret == 0)
        ret = avcodec_send_packet(a_codec_ctx, av_packet);
      if (ret == AVERROR(EAGAIN))
        ret = 0;
      if (ret < 0)
        err("avcodec_receive_Frame error");
      else
        current_len = av_packet->size;

      pkt_data += current_len;
      pkt_size -= current_len;
      resampled_data_size = 0;
    

      if(got_frame) {
        // move on to sampling
        resampled_data_size = audio_resampling(
            a_codec_ctx,
            av_frame,
            AV_SAMPLE_FMT_S16,
            a_codec_ctx->channels,
            a_codec_ctx->sample_rate,
            audio_buf);
        assert(resampled_data_size <= buf_size);
      }
      if (data_size <= 0) 
        continue;
      return resampled_data_size;
    }
    if (av_packet->data)
      av_packet_unref(av_packet);
    if(packet_queue_get(&audioq, av_packet, 1) < 0)
      return -1;
    pkt_data = pkt->data;
    pkt_size = pkt->size;
  }

  return 0;

}
/*
 * Resample the audio data retreived using ffmpeg before playing it. 
 *
 * @param audio_decode_ctx: context from the original AVFormatContext
 * @param decoded_audio_frame: decoded audio frame
 * @param out_sample_fmt: audio output sample format 
 * @param out_channels:  audio output channel
 * @param out_sample_rate
 * @param out_buf audio output buffer.
 *
 * @return the size of the resampled audio data.
 */
static int audio_resampling(
            AVFormatContext *audio_decode_ctx, 
            AVFrame  *decoded_audio_frame, 
            enum AVSampleFormat out_sample_fmt,
            int out_channels,
            int out_sample_rate,
            uint8_t* out_buf
    ) {
  if (quit)
    return -1;

  SwrContext *swr_context = NULL;
  int ret = 0;
  int64_t in_channel_layout = audio_decode_ctx->channel_layout;
  int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
  int out_nb_channels = 0;
  int out_linesize = 0;
  int in_nb_samples = 0;
  int out_nb_samples = 0;
  int max_out_nb_samples = 0;
  uint8_t** resampled_data = NULL;
  int resampled_data_size = 0;
  
  swr_ctx = swr_alloc();

  if (!swr_ctx) {
    err("swr_alloc failure");
  }
  in_channel_layout = (audio_decode_ctx->channels == av_get_channel_layout_nb_channels(audio_decode_ctx->channel)) ? 
    audio_decode_ctx->channel_layout : av_get_default_channel_layout(audio_decode_ctx->channels);
  
  // check input audio channels were retreived correctly
  if (in_channel_layout <= 0)
    err("in_channel_layout error");
  if (out_channels == 1)
    out_channel_layout = AV_CH_LAYOUT_MONO;
  if (out_channels == 2)
    out_channel_layout = AV_CH_LAYOUT_STEREO;
  else
    out_channel_layout = AV_CH_LAYOUT_SURROUND;

  // retreive number of audio samples (per channel)
  in_nb_samples = decoded_audio_frame->nb_samples;
  if (in_nb_samples <= 0) {
    err("in_nb_samples error");
  }

  av_opt_set_int(
      swr_ctx, 
      "in_channel_layout",
      in_channel_layout,
      0);

  av_opt_set_int(
      swr_ctx,
      "in_sample_rate",
      audio_decode_ctx->sample_rate,
      0);

  av_opt_set_sample_fmt(
      swr_ctx, 
      "in_sample_fmt",
      audio_decode_ctx->sample_fmt,
      0);

  av_opt_set_sample_fmt(
      swr_ctx, 
      "out_channel_layout",
      out_channel_layout,
      0);
  
  av_opt_set_sample_fmt(
      swr_ctx, 
      "out_sample_rate",
      out_sample_rate,
      0);

  av_opt_set_sample_fmt(
      swr_ctx, 
      "out_sample_fmt",
      out_sample_fmt,
      0);

  if (swr_init(swr_ctx) < 0)
    err("failed to initialize the resampling context.");

  max_out_nb_samples = out_nb_samples = av_rescale_rnd(in_nb_samples,
                                                  out_sample_rate,
                                                  audio_decode_ctx->sample_rate,
                                                  AV_ROUND_UP);

  if (max_out_nb_samples <= 0)
    err("av_rescale_rnd error");

  out_nb_channels = av_get_channel_layout_nb_channels(out_channel_layout);

  ret = av_samples_alloc_array_and_samples(
      &resampled_data,
      &out_linesize,
      out_nb_channels,
      out_nb_samples,
      out_sample_fmt,
      0);

  if (ret < 0)
    err("av_samples_alloc_and_samples() error: Couldn't allocate destination samples");

  out_nb_samples = av_rescale_rnd(
                    swr_get_delay(swr_ctx, audio_decode_ctx->sample_rate) + in_nb_samples,
                    out_sample_rate,
                    audio_decode_ctx->sample_rate,
                    AV_ROUND_UP);
  if (out_nb_samples <= 0)
    err("av_rescale_rnd error");

  if (out_nb_samples > max_out_nb_samples) {
    av_free(resampled_data[0]);

    ret = av_samples_alloc(
            resampled_data,
            &out_linesize,
            out_nb_channels,
            out_nb_samples,
            out_sample_fmt,
            1);

    if (ret < 0)
      err("av_samples_alloc failed");

    max_out_nb_samples = out_nb_samples;
  }
  if (swr_ctx) {
    ret = swr_convert(
            swr_ctx, 
            resampled_data,
            out_nb_samples,
            (const uint8_t **) decoded_audio_frame->data,
            decoded_audio_frame->nb_samples
        );
    if (ret < 0)
      err("swr_convert error");

    resampled_data_size = av_samples_get_buffer_size(
                            &out_linesize,
                            out_nb_channels,
                            ret, 
                            out_sample_fmt,
                            1);

    if (resampled_data_size < 0) 
      err("av_samples_get_buffer_size error");
  } else {
    err("swr_ctx null error");
  }
  memcpy(out_buf, resampled_data[0], resampled_data_size);

  // free memory
  if (resampled_data)
    av_freep(&resampled_data[0]);
  if (swr_ctx)
    av_free(&swr_ctx);

  return resampled_data_size;
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
 * [3] avcodec_recieve_frame
 * for decoding, call this method. on success, it will return an AVFrame
 * containing uncompressed audio / video data
 *
 * Repeat this call until it returns AVERROR(EAGAIN) or an error. this return
 * value means that new input data is required to return new output. So in this
 * case, we continue to send it more input. note that a frame / packet in may
 * not result in just the same of decoded information out.
 *
 * [4]
 * this is a hack -- the packet memory allocation stuff is broken. 
 * As docmented av_dup_packet is broken by design (?) and av_packet_ref matches
 * the AVFrame ref-counted API and can safely used instead.
 *
 * [5]
 * avcodec_decode_audio4() is deprecated in ffmpeg 3.1
 *
 * Now that this function is deprecated and replaced by 2 calls (receive frame
 * and send packet), this could be optimized into seperate routines and seperate
 * threads.
 *
 * Also now it always consumes a whole buffer some codec in the caller might be
 * optimizable. 
 *
 */
