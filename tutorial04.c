#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#ifdef __MINGW32__
#undef main
#endif

// SDL Audio buffer size in # of samples.
#define SDL_AUDIO_BUFFER_SIZE 1024

// maximum number of samples per channel in an audio frame
#define MAX_AUDIO_FRAME_SIZE 192000

// audio packets queue max size
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)

// video packets queue max size
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

// Custom SDL Event type to notify that the video frame is to be displayed
#define FF_REFRESH_EVENT (SDL_USEREVENT)

#define VIDEO_PICTURE_QUEUE_SIZE 1

typedef struct PacketQueue {
  AVPacketList *first_pkt;
  AVPacketList *last_pkt;
  int nb_packets;
  int size;
  SDL_mutex* mutex;
  SDL_cond* cond;
} PacketQueue;

// Qeuue Structure to store processed video frames
typedef struct VideoPicture {
  AVFrame *frame;
  int width;
  int height;
  int allocated;
} VideoPicture;

/*
 * Struct to hold format context, indices of audio / video stream,
 * corresponding AVStream objs, the audio / video codecs, the qs 
 * and buffers, quit flag, and filename
 */
typedef struct VideoState {
  // file io context
  AVFormatContext *file_format_ctx;

  // Audio Stream information
  int audio_stream_idx;
  AVStream* a_stream;
  AVCodecContext* a_codec_ctx;
  PacketQueue audioq;
  uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3 / 2)];
  unsigned int audio_buf_size;
  unsigned int audio_buf_offset;
  AVFrame audio_frame;
  AVPacket audio_pkt;
  uint8_t* audio_pkt_data;
  int audio_pkt_size;

  // Video stream information
  int video_stream_idx;
  AVStream* v_stream;
  AVCodecContext* v_codec_ctx;
  SDL_Texture* texture;
  SDL_Renderer* renderer;
  PacketQueue videoq;
  struct swsContext* sws_ctx;
  VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
  int pictq_size;
  int pictq_rindex;
  int pictq_windex;
  SDL_mutex* pictq_mutex;
  SDL_cond* pictq_cond;

  // Threads
  SDL_Thread* decode_tid;
  SDL_Thread* video_tid;

  // filename
  char filename[1024];

  // quit flag
  int quit;

} VideoState;

SDL_Window* screen;

SDL_mutex* screen_mutex;

VideoState global_video_state;

int decode_thread(void* arg);

int stream_component_open(
    VideoState* video_state,
    int stream_idx
);

void alloc_picture(void* userdata);

int queue_picture(
    VideoState* vs,
    AVFrame* v_frame);

int video_thread(void *args);

void video_refresh_timer(void* userdata);

static void schedule_refresh(
    VideoState* video_state,
    int delay);

static Uint32 sdl_refresh_timer_cb(
    Uint32 interval,
    void* opaque);

void video_display(VideoState* video_state);

void packet_queue_init(PacketQueue* q);

int packet_queue_put(PacketQueue* q, AVPacket* pkt);

static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block);

void audio_callback(void* userdata, Uint8 stream, int len);

int audio_decode_frame(VideoState* video_state, uint8_t* audio_buf, int buf_size);

static int audio_resampling(AVCodecContext* a_decode_ctx, AVFrame* audio_decode_frame, enum AVSampleFormat out_sample_fmt, int out_channels, int out_sample_rate, uint8_t* out_buf);

void err(char* msg) {
  printf("ERR: %s \n", msg);
  exit(-1);
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    err("Please supply movie to play");
  }
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
    err ("Couldn't initialize SDL");

  VideoState* vs = av_mallocz(sizeof(VideoState));

  av_strlcpy(vs->filename, argv[1], sizeof(vs->filename));

  vs->pictq_mutex = SDL_CreateMutex();
  vs->pictq_cond = SDL_CreateCond();

  // launch threads by pushing the custom FF_REFRESH_EVENT
  schedule_refresh(vs, 39);

  vs->decode_tid = SDL_CreateThread(decode_thread, "Decoding Thread", vs);

  if (!vs->decode_tid) {
    av_free(vs);
    err("decoding thread failed to start.");
  }

  SDL_Event event;
  for(;;) {
    SDL_WaitEvent(&event);
    switch(event.type) {
      case FF_QUIT_EVENT:
        break;
      case SDL_QUIT:
        vs->quit = 1;
        SDL_Quit();
        break;
      case FF_REFRESH_EVENT:
        video_refresh_timer(event.user.data1);
        break;
      default:
        break;
    }
    if (vs->quit)
      break;
  }
  return 0;
}

/**
 * Opens audio and video streams. 
 *  - Retreieve both codecs 
 *  - start a loop to read AVPackets from AVFormatContext
 *  - puts each type of packet into its appropriate queue
 */
int decode_thread(void* arg) {
  VideoState* vs = (VideoState*) arg;
  AVFormatContext* f_format_ctx = NULL;
  if (avformat_open_input(&f_format_ctx, vs->filename, NULL, NULL) < 0)
    err("Couldn't open the file.");

  AVPacket pkt1, *pkt = &pkt1;
  vs->video_stream_idx = -1;
  vs->audio_stream_idx = -1;

  global_video_state = vs;

  vs->f_format_ctx = f_format_ctx;
  
  if (avformat_find_stream_info(f_format_ctx, NULL) < 0)
    err("avformat_find_stream_info failed");

  av_dump_format(f_format_ctx, 0, vs->filename, 0);

  int i;
  int audio_stream_idx = -1; 
  int video_stream_idx = -1;
  for (i = 0; i < f_format_ctx->nb_streams; i++) {
    if (f_format_ctx->streams[i]->codecpar->codec_id == AVMEDIA_TYPE_VIDEO && video_stream_idx == -1)
      video_stream_idx = i;
    if (f_format_ctx->streams[i]->codecpar->codec_id == AVMEDIA_TYPE_AUDIO &&
        audio_stream_idx == -1)
      audio_stream_idx = i;
  }

  if (video_stream_idx == -1) {
    printf("couldn't find the video stream. \n");
    goto fail;
  } else {
    if (stream_component_open(vs, video_stream_idx) < 0) {
      printf("couldn't open the video codec. \n");
      goto fail;
    }
  }
  
  if (audio_stream_idx == -1) {
    printf("couldn't find the audio stream. \n");
    goto fail;
  } else {
    if (stream_component_open(vs, audio_stream_idx) < 0) {
      printf("couldn't open the audio codec. \n");
      goto fail;
    }
  }

  if (vs->video_stream_idx < 0 || vs->audio_stream_idx < 0) {
    printf("Couldn't open the codecs: %s. \n", vs->filename);
    goto fail;
  }
  // main decode loop: read packets and insert into correct queue
  for (;;) {
    if (vs->quit)
      break;
    if (vs->audioq.size > MAX_AUDIOQ_SIZE || vs->videoq.size > MAX_VIDEOQ_SIZE) {
      SDL_Delay(10);
      continue
    }
    if (av_read_frame(vs->file_format_ctx, pkt) < 0) {
      if (vs->file_format_ctx->pb->error == 0) {
        SDL_Delay(100);
        continue;
      }
    } else {
      break; // error with reading frame.
    }

    if (pkt->stream_index == vs->video_stream_idx)
      packet_queue_put(&vs->videoq, pkt);
    else if (pkt->stream_index == vs->audio_stream_idx)
      packet_queue_put(&vs->audioq, pkt);
    else 
      av_packet_unref(pkt); // free the packet
  }
  while (!vs->quit)
    SDL_Delay(100);
  fail: {
          if (1) {
            SDL_Event event;
            event.type = FF_QUIT_EVENT;
            event.user.data1 = vs;
            // push the event 
            SDL_PushEvent(&event);

            return -1;
          }
        };
        return 0;
  }

/*
 * Retreives the AVCodec and initializes the AVCodecContext for the given
 * AVStream index. for the AVMEDIA_TYPE_AUDIO, the codec type sets the preferred 
 * audio specs, opens the audio device and starts playing.
 * @param video_state the global variable reference to save info to 
 * @param stream_index the stream index for this specific AVFormatContext
 * @return < 0 if error, 0 otherwise.
 */
int stream_component_open(VideoState *vs, int stream_index) {
  AVFormatContext* ctx = vs->f_format_ctx;

  if (stream_index < 0 || stream_index > ctx->nb_streams)
    err("invalid stream index");
  AVCodec* codec = NULL;
  codec = avcodec_find_decoder(ctx->streams[streams_index]->codecpar->codec_id);
  if (codec == NULL) 
    err("unsupported codec");
  // get codec context
  AVCodecContext *codec_ctx = NULL;
  codec_ctx = avcodec_alloc_context3(codec);
  if(avcodec_parameters_to_context(codec_ctx, ctx->streams[streams_index]->codecpar) != 0)
    err("Couldn't copy the codec context.");
  if (codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
    SDL_AudioSpec wanted_specs, specs;

    wanted_specs.freq = codec_ctx->sample_rate;
    wanted_specs.format = AUDIO_S16SYS;
    wanted_specs.channels = codec_ctx->channels;
    wanted_specs.silence = 0;
    wanted_specs.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_specs.callback = audio_callback;
    wanted_specs.userdata = vs;

    if (SDL_OpenAudio(&wanted_specs, &specs) < 0)
      err("SDL_OpenAudio error.");
  }
  if(avcodec_open2(codec_ctx, codec, NULL) < 0)
    err("Unsupported codec");
  switch(codec_ctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      vs->audio_stream_idx = stream_index;
      vs->a_stream = codec_ctx->streams[stream_index];
      vs->a_codec_ctx = codec_ctx;
      vs->audio_buf_size = 0;
      vs->audio_buf_offset = 0;

      memset(&vs->audio_pkt, 0, sizeof(vs->audio_pkt));
      packet_queue_init(vs->audioq);
      SDL_PauseAudio(0);

      break;
    case AVMEDIA_TYPE_VIDEO:
      vs->video_stream_idx = stream_index;
      vs->v_stream = codec_ctx->streams[stream_index];
      vs->v_codec_ctx = codec_ctx;
      packet_queue_init(vs->videoq);
      vs->video_tid = SDL_CreateThread(video_tid, "Video Thread", vs);

      vs->sws_ctx = sws_getContext(vs->v_codec_ctx->width,
                                   vs->v_codec_ctx->height,
                                   vs->v_codec_ctx->pix_fmt,
                                   vs->v_codec_ctx->width,
                                   vs->v_codec_ctx->height,
                                   AV_PIX_FMT_YUV420P,
                                   SWS_BILINEAR,
                                   NULL,
                                   NULL,
                                   NULL);
      screen = SDL_CreateWindow(
          "My video player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
          codec_ctx->width/2,
          codec_ctx->height/2,
          SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
      if (!screen)
        err("Couldn't create the SDL screen.");

      SDL_GL_SetSwapInterval(1);
      screen_mutex = SDL_CreateMutex();
      vs->renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
      vs->texture = SDL_CreateTexture(vs->renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, vs->v_codec_ctx->width, vs->v_codec_ctx->height);
      break;
    default:
      break;
  }
  return 0;
}

/*
 * Allocates a new SDL Overlay for the VideoPicture struct referenced by the
 * global VideoState struct reference.
 * Also updates the remaining VideoPicture struct fields.
 */
void alloc_picture(void *userdata) {
  // retreive global VideoState reference
  VideoState *vs = (VideoState *) userdata;

  VideoPicture* vp = vs->pictq[vs->pictq_windex];

  if (vp->frame) {
    av_frame_free(&vp->frame);
    av_free(vp->frame);
  }
  SDL_LockMutex(screen_mutex);

  int num_bytes = av_image_get_buffer_size(
      AV_PIX_FMT_YUV420P,
      vs->v_codec_ctx->width,
      vs->v_codec_ctx->height,
      32);

  uint8_t* buffer = (uint8_t*) av_malloc(sizeof(uint8_t) * num_bytes);

  vp->frame = av_frame_alloc();
  if (!vp->frame)
    err("Couldn't allocate the frame.\n");

  av_image_fill_arrays(
      vp->frame->data,
      vp->frame->linesize,
      buffer,
      AV_PIX_FMT_YUV420P,
      vs->v_codec_ctx->width,
      vs->v_codec_ctx->height);

  SDL_UnlockMutex(screen_mutex);
  vp->width = vs->v_codec_ctx->width;
  vp->height = vs->v_codec_ctx->height;
  vp->allocated = 1;
}

/* 
 * Waits for space in the VideoPicture queue. Allocates a new SDL Overlay in
 * case it is not allocated or has a different width / height. converts the 
 * AVFrame to an AVPicture using specs supported by SDL and writes it in the
 * videoq. 
 */
int queue_picture(VideoState* vs, AVFrame* frame) {
  SDL_LockMutex(vs->pictq_mutex);
  while (vs->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !vs->quit)
    SDL_CondWait(vs->pictq_cond, vs->pictq_mutex);
  SDL_UnlockMutex(vs->pictq_mutex);
  if (vs->quit)
    return -1;
  VideoPicture *vp = vs->pictq[vs->pictq_windex];

  if (!vp->frame || vp->width != vs->v_codec_ctx->width || vp->height != vs->v_codec_ctx->height) {
    vp->allocated = 0;
    alloc_picture(vs);
    if(vs->quit)
      return -1;
  }
  if (vp->frame) {
    vp->frame->pict_type = frame->pict_type;
    vp->frame->pts = frame->pts;
    vp->frame->pkt_dts = frame->pkt_dts;
    vp->frame->key_frame = frame->key_frame;
    vp->frame->coded_picture_number = frame->coded_picture_number;
    vp->frame->display_picture_number = frame->display_picture_number;
    vp->frame->width = frame->width;
    vp->frame->height = frame->height;
  }
  sws_scale(
      vs->sws_ctx,
      (uint8_t const * const *) frame->data,
      frame->linesize,
      0,
      vs->v_codec_ctx->height,
      vp->frame->data,
      vp->frame->linesize);
  ++vs->pictq_windex;
  if(vs->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)
    vs->pictq_windex = 0;

  SDL_LockMutex(vs->pictq_mutex);
  vs->pictq_size++;
  SDL_UnlockMutex(vs->pictq_mutex);
}
return 0;
}

/**
 * Creates a thread that reads in packets from the video queue using
 * packet_queue_get(), decodes the video packets into a frame, and then calls 
 * the queue_picture() function to put the processed frame into the picture
 * queue.
 */
int video_thread(void* arg) {
  VideoState* vs = (VideoState*) arg;
  AVPacket* packet = av_packet_alloc();
  if (!packet)
    err("couldn't allocate packet");

  int frame_finished; 
  static AVFrame* frame = NULL;
  frame = av_frame_alloc();
  for (;;) {
    if (packet_queue_get(&vs->videoq, packet, 1) < 0)
      break;
    int ret = avcodec_send_packet(vs->v_codec_ctx, packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    else if (ret < 0)
      err("Error while decoding.");
    else 
      frame_finished = 1;
    if (frame_finished) {
      if (queue_picture(vs, frame) < 0)
        break; // queue is full.
    }
    av_packet_unref(packet);
  }
  av_frame_free(&frame);
  av_free(&frame);
  return 0;
}

/**
 * Pulls from the VideoPicture queue when there is something, sets timer for 
 * when the next video frame should be shown, calls video_display() to show it
 * on the screen, and then updates video state accordingly.
 * @param userdata SDL_UserEvent->data1 is an user defined data pointer.
 */
void video_refresh_timer(void* userdata) {
  VideoState* vs = (VideoState*) userdata;
  VideoPicture* vp; 
  if (vs->v_stream) {
    if (vs->pictq_size == 0)
      schedule_refresh(vs, 39);
    else {
      vp = &vs->pictq[vs->pictq_rindex];
      schedule_refresh(vs, 39);
      video_display(vs);
      if (++vs->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE)
        vs->pictq_rindex = 0;
      SDL_LockMutex(vs->pictq_mutex);
      vs->pictq_size--;
      SDL_CondSignal(vs->pictq_cond);
      SDL_UnlockMutex(vs->pictq_mutex); 
    }
  } else {
    schedule_refresh(vs, 39);
  }
}

static void schedule_refresh(VideoState* vs, int delay) {
  SDL_AddTimer(delay, sdl_refresh_timer_cb, vs);
}

/**
 * pushes the FF_REFRESH_EVENT to the queue. 
 * @param interval 
 * @param opaque
 */
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void* opaque) {
  SDL_Event event;
  event.type = FF_REFRESH_EVENT;
  event.user.data1 = opaque;

  SDL_PushEvent(&event);
  // returning 0 stops the timer.
  return 0;
}

void video_display(VideoState* vs) {
  VideoPicture* vp; 
  float aspect_ratio;
  int w,h,x,y;
  vp = &vs->pictq[vs->pictq_rindex];
  if (vp->frame) {
    if (vs->v_codec_ctx->sample_aspect_ratio.num == 0)
      aspect_ratio = 0;
    else 
      aspect_ratio = av_q2d(vs->v_codec_ctx->sample_aspect_ratio) * vs->v_codec_ctx->width / vs->v_codec_ctx->height;
    if (aspect_ratio <= 0.0)
      aspect_ratio = (float) vs->v_codec_ctx->width / (float) vs->v_codec_ctx->height;
    int screen_width, screen_height;
    SDL_GetWindowSize(screen &screen_width, &screen_height);
    h = screen_height;
    w = ((int) rint(h * aspect_ratio)) & -3;

    if (w > screen_width) {
      w = screen_width;
      h = ((int) rint(w / aspect_ratio)) & -3;
    }
    x = (screen_width - w);
    y = (screen_height - h);
    printf("Frame %c (%d) pts %d dts %d key_frame %d [codec_picture_number %d, display_picture_number %d, %dx%d]\n", 
        av_get_picture_type_char(vp->frame->pict_type),
        vs->v_codec_ctx->frame_number,
        vp->frame->pts,
        vp->frame->pkt_dts,
        vp->frame->key_frame,
        vp->frame->coded_picture_number,
        vp->frame->display_picture_number,
        vp->frame->width,
        vp->frame->height);
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = 2*w;
    rect.h = 2*h;
    SDL_LockMutex(screen_mutex);
    SDL_UpdateYUVTexture(
        vs->texture,
        &rect,
        vp->frame->data[0],
        vp->frame->linesize[0],
        vp->frame->data[1],
        vp->frame->linesize[1],
        vp->frame->data[2],
        vp->frame->linesize[2]);
    SDL_RenderClear(vs->renderer);
    SDL_RenderCopy(vs->renderer, vs->texture, NULL, NULL);
    SDL_RenderPresent(vs->renderer);
    SDL_UnlockMutex(screen_mutex);
  } else {
    SDL_Event ev; 
    ev.type = FF_QUIT_EVENT;
    ev.user.data1 = vs;
    SDL_PushEvent(&ev);
  }
}

void packet_queue_init(PacketQueue* q) {
  memset(q, 0, sizeof(PacketQueue));
  q->cond = SDL_CreateCond();
  q->mutex = SDL_CreateMutex();
  if (!q->mutex || !q->cond) 
    err("error initializing the mutex / conditional");
}

void packet_queue_put(PacketQueue* q, AVPacket* packet) {
  AVPacketList* pkt_list = NULL;
  pkt_list = av_malloc(sizeof(AVPacketList));
  if (!pkt_list) 
    err("err intializing pkt list");
  pkt_list->pkt = *packet;
  pkt_list->next = NULL;
  SDL_LockMutex(q->mutex);
  if (!q->last_pkt) {
    // no last packet, meaning empty q.
    q->first_pkt = pkt_list;
  } else {
    q->last_pkt->next = pkt_list; // append it to the end.
  }
  q->last_pkt = pkt_list;
  q->nb_packets++;
  q->size += pkt_list->pkt.size;
  SDL_CondSignal(q->cond);
  SDL_UnlockMutex(q->mutex);
  return 0;
}

void packet_queue_get(PacketQueue* q, AVPacket* packet, int block) {
  int ret; 
  AVPacketList* pkt_list;
  SDL_LockMutex(q->mutex);
  for (;;) {
    if (global_video_state->quit)  {
      ret = -1;
      break;
    }
    pkt_list = q->first_pkt;
    if (pkt_list) {
      q->first_pkt = pkt_list->next;
      if (!q->first_pkt) {
        q->last_pkt = NULL;
      }
      q->nb_packets--;
      q->size -= pkt_list->pkt.size;
      *packet = pkt_list->pkt;
      av_free(pkt_list);
      ret = 1;
      break;
    }
    else if (!block) {
      ret = 0;
      break;
    } else {
      SDL_CondWait(q->cond, q->mutex);
    }
  }
  SDL_UnlockMutex(q->mutex);
  return ret;
}

void audio_callback(void* userdata, Uint8* stream, int total_bytes) {
  VideoState* vs = (VideoState*) userdata;
  int current_bytes = -1;
  unsigned int decoded_audio_size = -1;

  while (total_bytes > 0) {
    if (global_video_state->quit)
      return;
    if (vs->audio_buf_offset >= vs->audio_buf_size) {
      decoded_audio_size = audio_decode_frame(vs, vs->audio_buf, sizeof(vs->audio_buf));
      if (decoded_audio_size < 0) {
        vs->audio_buf_size = 1024;
        memset(vs->audio_buf, 0, vs->audio_buf_size);
      } else {
        vs->audio_buf_size = decoded_audio_size;
      }
    }
    current_bytes = vs->audio_buf_size - vs->audio_buf_offset;
    if(current_bytes > total_bytes)
      current_bytes = total_bytes;
    memcpy(stream, (uint8_t*) vs->audio_buf + vs->audio_buf_offset, current_bytes);
    total_bytes -= current_bytes;
    stream += current_bytes;
    vs->audio_buf_offset += current_bytes;
  }
}

int audio_decode_frame(VideoState* vs, uint8_t* audio_buf, int len) {
  AVPacket* av_packet = av_packet_alloc();
  static uint8_t* audio_pkt_data = NULL;
  static int audio_pkt_size = 0;

  static AvFrame* av_frame = NULL;
  av_frame = av_frame_alloc();
  if (!av_frame) 
    err("av_frame misallocated");
  int curr_len = 0;
  int data_size = 0;
  for (;;) {
    if (vs->quit)
      return -1;
    while (audio_pkt_size > 0) {
      // write into audio_buf (the sink) 
      int got_frame = 0;
      int ret = avcodec_receive_frame(vs->a_codec_ctx, av_frame);
      if (ret == 0)
        got_frame = 1;
      if (ret == AVERROR(EAGAIN)) 
        ret = 0;
      if (ret == 0)
        ret = avcodec_send_packet(vs->a_codec_ctx, av_packet);
      if (ret == AVERROR(EAGAIN))
        ret = 0;
      else if (ret < 0)
        err("avcodec_receive_frame err");
      else
        curr_len = av_packet->size;
      if (curr_len < 0) {
        audio_pkt_size = 0;
        break;
      }
      audio_pkt_data += curr_len;
      audio_pkt_size -= curr_len;
      data_size = 0;
      if (got_frame) {
        data_size = audio_resampling(
                    vs->a_codec_ctx,
                    av_frame,
                    AV_SAMPLE_FMT_S16,
                    vs->a_codec_ctx->channels,
                    vs->a_codec_ctx->sample_rate,
                    audio_buf);
        assert(data_size <= buf_size);
      }
      if (data_size <= 0)
        continue;
      return data_size;
    }
    if (av_packet->data) {
      av_packet_unref(av_packet);
    }
    if (packet_queue_get(&vs->audioq, av_packet, 1) < 0)
      return -1;
    audio_pkt_data = av_packet->data;
    audio_pkt_size = av_packet->size;
  }
  return 0;
}
static int audio_resampling(
                      AVCodecContext* audio_decode_ctx,
                      AVFrame* av_frame,
                      enum AVSampleFormat out_sample_fmt,
                      int out_channels,
                      int out_sample_rate,
                      uint8_t* out_buf) {
  SwrContext* swr_ctx = NULL;
  int ret = 0;
  int64_t in_channel_layout = audio_decode_ctx->channel_layout;
  uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
  int out_nb_channels = 0;
  int out_linesize = 0;
  int in_nb_samples = 0;
  int64_t out_nb_samples = 0;
  int64_t max_out_nb_samples = 0;
  uint8_t** resampled_data = NULL;
  int resampled_data_size = 0;

  swr_ctx = swr_alloc();
  if (!swr_ctx)
    err("err alloc swr");
  in_channel_layout = (audio_decode_ctx->channels == av_get_channel_layout_nb_channels(audio_decode_ctx->channel_layout)) ? audio_decode_ctx->channel_layout : av_get_default_channel_layout(audio_decode_ctx->channels);
  if (in_channel_layout <= 0)
    err("in_channel_layout error");
  if (out_channels == 1)
    out_channel_layout = AV_CH_LAYOUT_MONO;
  if (out_channels == 2)
    out_channel_layout = AV_CH_LAYOUT_STEREO;
  else
    out_channel_layout = AV_CH_LAYOUT_SURROUND;

  in_nb_samples = av_frame->nb_samples;
  if (in_nb_samples <= 0)
    err("in_nb_samples error");

  av_opt_set_int(
      swr_ctx,
      "in_channel_layout",
      in_channel_layout,
      0);
  
  av_opt_set_int(
      swr_ctx,
      "in_sample_rate",
      in_sample_rate,
      0);

  av_opt_set_sample_fmt(
      swr_ctx, 
      "in_sample_fmt",
      audio_decode_ctx->sample_fmt,
      0);
  
}


