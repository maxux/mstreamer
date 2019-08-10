#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#define debug(...) printf(__VA_ARGS__)

//
// data structures
//
typedef struct buffer_t {
    struct v4l2_buffer info;
    void *buffer;
    size_t length;

} buffer_t;

typedef struct stats_t {
    size_t frames;
    size_t timeframes;
    time_t begintime;
    time_t lastupdate;

} stats_t;

typedef struct stream_t {
    int fd;

    // global settings
    int width;
    int height;
    int fps;

    // v4l2 parameters
    struct v4l2_capability cap;
    struct v4l2_format format;
    struct v4l2_streamparm parm;
    struct v4l2_requestbuffers bufrequest;
    struct v4l2_buffer bufferinfo;
    int buftype;

    // buffers
    buffer_t *buffers;
    size_t bufsize;

    // sdl parameters
    SDL_Window *window;
    SDL_RWops *rwops;
    SDL_Surface *image;
    SDL_Renderer *render;
    SDL_Texture *texture;

    // statistics
    stats_t stats;

} stream_t;

int global_running = 1;

//
// helpers
//
void diep(char *str) {
    perror(str);
    exit(EXIT_FAILURE);
}

//
// signal management
//
int signal_intercept(int signal, void (*function)(int)) {
    struct sigaction sig;
    int ret;

    sigemptyset(&sig.sa_mask);
    sig.sa_handler = function;
    sig.sa_flags = 0;

    if((ret = sigaction(signal, &sig, NULL)) == -1)
        diep("sigaction");

    return ret;
}

void sighandler(int signal) {
    printf("\n");

    if(signal == SIGINT) {
        printf("[+] signal: SIGINT received, stopping\n");
        global_running = 0;
    }
}

//
// video 4 linux 2
//
int v4l2_initialize(stream_t *stream) {
    //
    // capabilities checking
    //
    debug("[+] v4l2: querying capacities\n");
    if(ioctl(stream->fd, VIDIOC_QUERYCAP, &stream->cap) < 0)
        diep("VIDIOC_QUERYCAP");

    debug("[+] v4l2: capacities checking\n");
    if(!(stream->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "The device doesn't support requested capture format\n");
        exit(EXIT_FAILURE);
    }

    //
    // set video format
    //
    stream->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    stream->format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    stream->format.fmt.pix.width = stream->width;
    stream->format.fmt.pix.height = stream->height;

    debug("[+] v4l2: setting video format\n");
    if(ioctl(stream->fd, VIDIOC_S_FMT, &stream->format) < 0)
        diep("VIDIOC_S_FMT");

    //
    // set video framerate
    //
    stream->parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    debug("[+] v4l2: requesting parameters\n");
    if(ioctl(stream->fd, VIDIOC_G_PARM, &stream->parm) < 0)
        diep("VIDIOC_G_PARM");

    stream->parm.parm.capture.timeperframe.numerator = stream->fps;
    stream->parm.parm.capture.timeperframe.denominator = 1;

    debug("[+] v4l2: setting video parameters\n");
    if(ioctl(stream->fd, VIDIOC_S_PARM, &stream->parm) < 0)
        diep("VIDIOC_S_PARM");

    //
    // video buffer
    //
    stream->bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    stream->bufrequest.memory = V4L2_MEMORY_MMAP;
    stream->bufrequest.count = 8;

    debug("[+] v4l2: requesting buffers\n");
    if(ioctl(stream->fd, VIDIOC_REQBUFS, &stream->bufrequest) < 0)
        diep("VIDIOC_REQBUFS");

    stream->bufsize = stream->bufrequest.count;
    debug("[+] v4l2: buffers validated: %d\n", stream->bufrequest.count);

    debug("[+] v4l2: initializing buffers\n");
    if(!(stream->buffers = calloc(sizeof(buffer_t), stream->bufsize)))
        diep("calloc");

    stream->bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    stream->bufferinfo.memory = V4L2_MEMORY_MMAP;

    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_SHARED;

    for(size_t i = 0; i < stream->bufrequest.count; i++) {
        stream->buffers[i].info = stream->bufferinfo;
        stream->buffers[i].info.index = i;

        if(ioctl(stream->fd, VIDIOC_QUERYBUF, &stream->buffers[i].info) < 0)
            diep("VIDIOC_QUERYBUF");

        off_t offset = stream->buffers[i].info.m.offset;
        stream->buffers[i].length = stream->buffers[i].info.length;

        //
        // mmap buffer
        //
        stream->buffers[i].buffer = mmap(NULL, stream->buffers[i].length, prot, flags, stream->fd, offset);
        if(stream->buffers[i].buffer == MAP_FAILED)
            diep("mmap");

        memset(stream->buffers[i].buffer, 0x00, stream->buffers[i].length);

        //
        // initialize streaming
        //
        if(ioctl(stream->fd, VIDIOC_STREAMON, &stream->buffers[i].info.type) < 0)
            diep("VIDIOC_STREAMON");

        // request a frame on that buffer
        if(ioctl(stream->fd, VIDIOC_QBUF, &stream->buffers[i].info) < 0)
            diep("VIDIOC_QBUF");
    }

    return 0;
}

//
// window management
//
int sdl_initialize(stream_t *stream) {
    debug("[+] window: initializing display\n");
    stream->window = SDL_CreateWindow(
        "Video Streaming",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        stream->width, stream->height,
        SDL_WINDOW_SHOWN
    );

    SDL_UpdateWindowSurface(stream->window);

    return 0;
}

//
// statistics updater
//
void statistics_update_frame(stream_t *stream) {
    stream->stats.frames += 1;
    stream->stats.timeframes += 1;

    time_t elapsed = time(NULL) - stream->stats.begintime;

    if(stream->stats.lastupdate != time(NULL)) {
        size_t fps = stream->stats.timeframes;
        printf("[+] stream: running: %lu sec, frames: %lu, fps: %lu\n", elapsed, stream->stats.frames, fps);

        stream->stats.timeframes = 0;
        stream->stats.lastupdate = time(NULL);
    }
}

//
// main streaming loop
//
int streaming(stream_t *stream) {
    for(size_t i = 0; i < stream->bufsize; i++) {
        // dequeue the buffer
        if(ioctl(stream->fd, VIDIOC_DQBUF, &stream->buffers[i].info) < 0) {
            if(errno != EINTR)
                diep("VIDIOC_QBUF");

            return 1;
        }

        stream->rwops = SDL_RWFromConstMem(stream->buffers[i].buffer, stream->buffers[i].length);
        stream->image = IMG_LoadJPG_RW(stream->rwops);

        stream->render = SDL_CreateRenderer(stream->window, -1, 0);
        stream->texture = SDL_CreateTextureFromSurface(stream->render, stream->image);

        SDL_RenderCopy(stream->render, stream->texture, NULL, NULL);
        SDL_RenderPresent(stream->render);

        SDL_UpdateWindowSurface(stream->window);

        SDL_DestroyTexture(stream->texture);
        SDL_DestroyRenderer(stream->render);
        SDL_FreeSurface(stream->image);
        SDL_FreeRW(stream->rwops);

        // request next frame on that buffer
        if(ioctl(stream->fd, VIDIOC_QBUF, &stream->buffers[i].info) < 0)
            diep("VIDIOC_QBUF");

        statistics_update_frame(stream);
    }

    return 0;
}

//
// destructors
//
void sdl_cleanup(stream_t *stream) {
    debug("[+] display: closing window\n");
    SDL_DestroyWindow(stream->window);
    SDL_Quit();
}

void v4l2_cleanup(stream_t *stream) {
    debug("[+] v4l2: stopping streaming\n");
    if(ioctl(stream->fd, VIDIOC_STREAMOFF, &stream->buftype) < 0)
        diep("VIDIOC_STREAMOFF");

    debug("[+] v4l2: cleaning buffers\n");
    for(size_t i = 0; i < stream->bufsize; i++)
        munmap(stream->buffers[i].buffer, stream->buffers[i].length);

    free(stream->buffers);
}

//
// let's go
//
int main(void) {
    stream_t stream;

    memset(&stream, 0x00, sizeof(stream_t));

    stream.stats.begintime = time(NULL);
    stream.stats.lastupdate = stream.stats.begintime;

    stream.width = 1280;
    stream.height = 720;

    debug("[+] v4l2: opening video device\n");
    if((stream.fd = open("/dev/video0", O_RDWR)) < 0) {
        perror("open");
        exit(1);
    }

    v4l2_initialize(&stream);
    sdl_initialize(&stream);

    signal_intercept(SIGINT, sighandler);

    while(global_running)
        streaming(&stream);

    sdl_cleanup(&stream);;
    v4l2_cleanup(&stream);

    debug("[+] v4l2: closing video device\n");
    close(stream.fd);

    return 0;
}
