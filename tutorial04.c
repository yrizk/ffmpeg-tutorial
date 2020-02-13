#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

// prevents SDL from overriding main 
#ifdef __MINGW32__
#undef main
#endif


#define SDL_AUDIO_BUFFER_SIZE 1024

#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)

#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define FF_REFRESH_EVENT (SDL_USEREVENT)

#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

// Queue structure used to store AVPackets
typedef struct PacketQueue {
    AVPacketList* first_pkt;
    AVPacketList* last_pkt;
    int nb_packets;
    int size; 
    SDL_mutex* mutex;
    SDL_cond* cond;
} PacketQueue;

// struct specifically for video frames
typedef struct VideoPicture {
    AVFrame* v_frame;
    int width;
    int height;
    int allocated;
} VideoPicture;

typedef struct VideoState {
    // mp4, avi, flv etc. this holds information about the # streams, their
    // encoding , etc.
    AVFormatContext* file_format_ctx;

    /* ------- Audio Section ----- */
    // within file_format_ctx
    int audio_stream_idx;
    // the whole audio stream from AVFormatContext*. in this case, its 
    // file_format_ctx
    AVStream* a_stream;
    // allocated space via avcodec_alloc_context3(codec), and just has
    // information about the encoding for this stream.
    AVCodecContext* a_ctx;
    // the queue to add audio frames to for synchronized playback
    PacketQueue* a_q;
    // the audio buffer used in the audio_callback
    uint8_t* a_buf[MAX_AUDIO_FRAME_SIZE * 3 / 2];
    // the following 2 variables are used in audio_callback
    unsigned int a_buf_size;
    unsigned int a_buf_offset;
    // the last frame that was decoded from the AVPacket (see below)
    AVFrame a_frame;
    // the last AVPacket that was pulled out
    AVPacket a_pkt; 
    uint8_t* a_pkt_data;
    // the size of the last AVPacket that we decoded
    int a_pkt_size;

    /* ------- Video Section ----- */
    // within file_format_ctx->streams array
    int video_stream_idx;
    // the whole video stream from AVFormatContext, (i.e file_format_ctx)
    AVStream* v_stream;
    AVCodecContext* v_ctx;
    SDL_Texture *texture;
    SDL_Renderer *renderer;
    // for resampling.
    struct swsContext* sws_ctx;
    VideoPicture* v_q;
    // the size of the video queue
    int pq_size;
    // tracking the index of the last read of v_q
    int pictq_rindex;
    // index of the last writing index of v_q
    int pictq_windex;
    SDL_mutex* pq_mutex;
    SDL_cond* pq_cond;

    SDL_Thread* decode_tid;
    SDL_Thread* video_tid;

    char* filename[1024];
    // a global flag to coordinate across all threads to quit.
    int quit;
} VideoState;

///////////////////////////////////////////////////////////////////////////////
// GLOBAL STATE
///////////////////////////////////////////////////////////////////////////////
SDL_Window* g_screen;
VideoState* g_vs;
SDL_mutex* g_screen_mutex;

///////////////////////////////////////////////////////////////////////////////
// forward decls of fns
///////////////////////////////////////////////////////////////////////////////

// does all gruntwork required for finding the streams, finding the decoders for
// each of the streams, and stuffing the AVPackets into the correct queue.
int stream_component_open(VideoState* vs, int stream_idx);
void alloc_picture(void* userdata);
// adds the passed in frame to the video q
int queue_picture(VideoState* vs, AVFrame* frame);
// the runner for video_tid
int video_thread(void* arg);
// the runner for the decoder tid
int decode_thread(void* arg);

void video_refresh_timer(void* userdata);

// calls video_refresh_timer every delay (or something like that)
static void schedule_refresh(VideoState* vs,int delay);

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void* opaque);

void video_display(VideoState* vs);

// for the audio q
void packet_queue_init(PacketQueue* q);

// here, pkt is produced
int packet_queue_get(PacketQueue* q, AVPacket* pkt);

// here, pkt is consumed.
int packet_queue_put(PacketQueue* q, AVPacket* pkt);

