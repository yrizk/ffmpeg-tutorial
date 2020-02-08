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

}
