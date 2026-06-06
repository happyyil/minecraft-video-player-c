/*
 * frame_tiler.c — Multithreaded video frame extraction, tiling, and PNG encoding.
 *
 * Usage:
 *   frame_tiler.exe <video_path> <output_dir> [width] [height] [fps] [tile_size] [workers] [ffmpeg_path]
 *
 * Arguments:
 *   video_path   — Path to input video file
 *   output_dir   — Directory to write PNG tiles
 *   width        — Output width (default: source width)
 *   height       — Output height (default: source height)
 *   fps          — Output FPS (default: source FPS)
 *   tile_size    — Max tile dimension, default 256
 *   workers      — Number of worker threads, default 16
 *   ffmpeg_path  — Path to ffmpeg executable (default: ffmpeg on PATH)
 *
 * Threading model:
 *   1 reader thread (main) reads raw RGB24 frames from ffmpeg pipe.
 *   N worker threads pop FrameTasks from a bounded queue, encode tiles to PNG,
 *   and write them to disk in parallel.
 *
 * Compile (MinGW / MSYS2):
 *   gcc frame_tiler.c -O3 -o frame_tiler.exe -lpthread
 *
 * Compile (MSVC):
 *   cl frame_tiler.c /O2 /o frame_tiler.exe
 *
 * Dependencies: None! stb_image_write.h is included as a single header.
 * Only needs ffmpeg.exe on PATH for video decoding.
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- Platform abstractions ---- */
#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
    #include <io.h>
    #include <direct.h>
    #define DIR_SEP "\\"
    #define MKDIR(dir) _mkdir(dir)
    #define SNPRINTF _snprintf

    typedef CRITICAL_SECTION mutex_t;
    typedef CONDITION_VARIABLE cond_t;
    typedef HANDLE thread_handle_t;

    static inline void mutex_init(mutex_t *m) { InitializeCriticalSection(m); }
    static inline void mutex_destroy(mutex_t *m) { DeleteCriticalSection(m); }
    static inline void mutex_lock(mutex_t *m) { EnterCriticalSection(m); }
    static inline void mutex_unlock(mutex_t *m) { LeaveCriticalSection(m); }

    static inline void cond_init(cond_t *c) { InitializeConditionVariable(c); }
    static inline void cond_wait(cond_t *c, mutex_t *m) { SleepConditionVariableCS(c, m, INFINITE); }
    static inline void cond_broadcast(cond_t *c) { WakeAllConditionVariable(c); }

    static inline thread_handle_t thread_create(void *(*func)(void*), void *arg) {
        return (thread_handle_t)_beginthreadex(NULL, 0, (unsigned(__stdcall*)(void*))func, arg, 0, NULL);
    }
    static inline void thread_wait(thread_handle_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
    #include <pthread.h>
    #include <sys/stat.h>
    #define DIR_SEP "/"
    #define MKDIR(dir) mkdir(dir, 0755)
    #define SNPRINTF snprintf

    typedef pthread_mutex_t mutex_t;
    typedef pthread_cond_t cond_t;
    typedef pthread_t thread_handle_t;

    static inline void mutex_init(mutex_t *m) { pthread_mutex_init(m, NULL); }
    static inline void mutex_destroy(mutex_t *m) { pthread_mutex_destroy(m); }
    static inline void mutex_lock(mutex_t *m) { pthread_mutex_lock(m); }
    static inline void mutex_unlock(mutex_t *m) { pthread_mutex_unlock(m); }

    static inline void cond_init(cond_t *c) { pthread_cond_init(c, NULL); }
    static inline void cond_wait(cond_t *c, mutex_t *m) { pthread_cond_wait(c, m); }
    static inline void cond_broadcast(cond_t *c) { pthread_cond_broadcast(c); }

    static inline thread_handle_t thread_create(void *(*func)(void*), void *arg) {
        pthread_t t;
        pthread_create(&t, NULL, func, arg);
        return t;
    }
    static inline void thread_wait(thread_handle_t t) { pthread_join(t, NULL); }
#endif

#define MAX_PATH_LEN 4096
#define TILE_SIZE_DEFAULT 256
#define WORKERS_DEFAULT 16

/* ---- Per-tile output data ---- */
typedef struct {
    unsigned char *png_data;
    int png_size;
    int frame_idx;
    int row;
    int col;
} Tile;

/* ---- Frame task (one full frame for a worker) ---- */
typedef struct {
    unsigned char *rgb_data;  /* malloc'd full frame buffer */
    int width;
    int height;
    int frame_idx;
} FrameTask;

/* ---- Bounded task queue ---- */
typedef struct {
    FrameTask *buf;
    int cap;
    int head;
    int tail;
    int count;
    mutex_t mutex;
    cond_t not_full;
    cond_t not_empty;
    int shutdown;
} TaskQueue;

static void queue_init(TaskQueue *q, int cap) {
    q->buf = (FrameTask *)malloc(sizeof(FrameTask) * cap);
    q->cap = cap;
    q->head = q->tail = q->count = 0;
    q->shutdown = 0;
    mutex_init(&q->mutex);
    cond_init(&q->not_full);
    cond_init(&q->not_empty);
}

static void queue_destroy(TaskQueue *q) {
    free(q->buf);
    mutex_destroy(&q->mutex);
}

static void queue_push(TaskQueue *q, FrameTask task) {
    mutex_lock(&q->mutex);
    while (q->count >= q->cap && !q->shutdown) {
        cond_wait(&q->not_full, &q->mutex);
    }
    q->buf[q->tail] = task;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    cond_broadcast(&q->not_empty);
    mutex_unlock(&q->mutex);
}

static int queue_pop(TaskQueue *q, FrameTask *out) {
    mutex_lock(&q->mutex);
    while (q->count == 0 && !q->shutdown) {
        cond_wait(&q->not_empty, &q->mutex);
    }
    if (q->count == 0) {
        mutex_unlock(&q->mutex);
        return 0; /* shutdown + empty */
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    cond_broadcast(&q->not_full);
    mutex_unlock(&q->mutex);
    return 1;
}

static void queue_shutdown(TaskQueue *q) {
    mutex_lock(&q->mutex);
    q->shutdown = 1;
    cond_broadcast(&q->not_empty);
    cond_broadcast(&q->not_full);
    mutex_unlock(&q->mutex);
}

/* ---- Write a single tile to disk ---- */
static void write_tile(Tile *tile, const char *output_dir) {
    char filepath[MAX_PATH_LEN];
    SNPRINTF(filepath, sizeof(filepath), "%s"DIR_SEP"%d_%d_%d.png",
             output_dir, tile->frame_idx, tile->row, tile->col);

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "[frame_tiler] Cannot write: %s\n", filepath);
        return;
    }
    fwrite(tile->png_data, 1, tile->png_size, f);
    fclose(f);
}

/* ---- Encode a tile as PNG ---- */
static Tile encode_tile(const unsigned char *rgb_data, int w, int h, int full_w, int full_h,
                        int frame_idx, int row, int col, int tile_size) {
    Tile tile = {0};
    tile.frame_idx = frame_idx;
    tile.row = row;
    tile.col = col;
    (void)full_h; /* unused */

    /* Create contiguous buffer for this tile */
    unsigned char *buf = (unsigned char *)malloc((size_t)w * h * 3);
    if (!buf) return tile;

    for (int y = 0; y < h; y++) {
        const unsigned char *src = rgb_data + ((row * tile_size + y) * full_w + col * tile_size) * 3;
        unsigned char *dst = buf + y * w * 3;
        memcpy(dst, src, (size_t)w * 3);
    }

    tile.png_data = stbi_write_png_to_mem(buf, w * 3, w, h, 3, &tile.png_size);
    free(buf);
    return tile;
}

/* ---- Worker thread context ---- */
typedef struct {
    TaskQueue *queue;
    const char *output_dir;
    int tile_size;
    int *processed_count; /* atomic-ish via mutex in main? No, use atomic if possible. Just use a plain int for now since we only read it at the end. */
} WorkerArgs;

static void *worker_thread(void *arg) {
    WorkerArgs *ctx = (WorkerArgs *)arg;
    TaskQueue *q = ctx->queue;
    const char *output_dir = ctx->output_dir;
    int tile_size = ctx->tile_size;

    FrameTask task;
    while (queue_pop(q, &task)) {
        int cols = (task.width + tile_size - 1) / tile_size;
        int rows = (task.height + tile_size - 1) / tile_size;

        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int tw = (c * tile_size + tile_size <= task.width) ? tile_size : (task.width - c * tile_size);
                int th = (r * tile_size + tile_size <= task.height) ? tile_size : (task.height - r * tile_size);

                Tile tile = encode_tile(task.rgb_data, tw, th, task.width, task.height,
                                        task.frame_idx, r, c, tile_size);
                write_tile(&tile, output_dir);
                if (tile.png_data) {
                    STBIW_FREE(tile.png_data);
                }
            }
        }

        free(task.rgb_data);
    }

    return NULL;
}

/* ---- Process all frames with worker threads ---- */
static int process_frames_mt(FILE *ffmpeg_pipe, int width, int height, int fps,
                              int tile_size, int max_workers,
                              const char *output_dir,
                              int *out_frame_count, double *out_duration) {
    size_t frame_bytes = (size_t)width * height * 3;
    int frame_idx = 0;
    int total_frames = 0;

    /* Bounded queue: capacity = max_workers * 2 to keep memory bounded */
    TaskQueue queue;
    queue_init(&queue, max_workers * 2 > 4 ? max_workers * 2 : 4);

    WorkerArgs wargs = {&queue, output_dir, tile_size, NULL};

    /* Spawn worker threads */
    thread_handle_t *threads = (thread_handle_t *)malloc(sizeof(thread_handle_t) * max_workers);
    for (int i = 0; i < max_workers; i++) {
        threads[i] = thread_create(worker_thread, &wargs);
    }

    /* Read frames from ffmpeg and push to queue */
    while (1) {
        unsigned char *frame = (unsigned char *)malloc(frame_bytes);
        if (!frame) {
            fprintf(stderr, "[frame_tiler] Out of memory for frame buffer\n");
            break;
        }

        size_t total_read = 0;
        while (total_read < frame_bytes) {
            size_t n = fread(frame + total_read, 1, frame_bytes - total_read, ffmpeg_pipe);
            if (n == 0) break;
            total_read += n;
        }
        if (total_read < frame_bytes) {
            free(frame);
            break; /* EOF */
        }

        FrameTask task = {frame, width, height, frame_idx};
        queue_push(&queue, task);

        total_frames++;
        if (total_frames % 100 == 0) {
            double ts = (double)total_frames / fps;
            fprintf(stderr, "[frame_tiler] Queued %d frames (%.1fs)\n", total_frames, ts);
        }
        frame_idx++;
    }

    /* Signal shutdown */
    queue_shutdown(&queue);

    /* Wait for all workers to finish */
    for (int i = 0; i < max_workers; i++) {
        thread_wait(threads[i]);
    }

    free(threads);
    queue_destroy(&queue);

    *out_frame_count = total_frames;
    *out_duration = (double)total_frames / fps;
    return 0;
}

/* ---- Run ffmpeg and pipe frames to us ---- */
static int run_ffmpeg_and_process(const char *video_path, int target_w, int target_h,
                                    double target_fps, int tile_size, int max_workers,
                                    const char *output_dir,
                                    const char *ffmpeg_path,
                                    int *out_frame_count, double *out_duration) {
    /* Build ffmpeg command */
    char cmd[MAX_PATH_LEN * 4];
    int cmd_len = 0;

    cmd_len = SNPRINTF(cmd, sizeof(cmd),
        "\"%s\" -hide_banner -loglevel error -i \"%s\"",
        ffmpeg_path ? ffmpeg_path : "ffmpeg", video_path);

    /* Build video filter string: scale and/or fps */
    char vf[MAX_PATH_LEN];
    int vf_len = 0;
    int has_vf = 0;

    if (target_w > 0 || target_h > 0) {
        if (target_w > 0 && target_h > 0) {
            vf_len = SNPRINTF(vf, sizeof(vf), "scale=%d:%d", target_w, target_h);
        } else if (target_w > 0) {
            vf_len = SNPRINTF(vf, sizeof(vf), "scale=%d:-2", target_w);
        } else {
            vf_len = SNPRINTF(vf, sizeof(vf), "scale=-2:%d", target_h);
        }
        has_vf = 1;
    }

    if (target_fps > 0) {
        if (has_vf) {
            vf_len += SNPRINTF(vf + vf_len, sizeof(vf) - vf_len, ",fps=%g", target_fps);
        } else {
            vf_len = SNPRINTF(vf, sizeof(vf), "fps=%g", target_fps);
            has_vf = 1;
        }
    }

    if (has_vf) {
        cmd_len += SNPRINTF(cmd + cmd_len, sizeof(cmd) - cmd_len, " -vf \"%s\"", vf);
    }

    /* Output raw RGB24 to stdout (no BGR->RGB conversion needed) */
    cmd_len += SNPRINTF(cmd + cmd_len, sizeof(cmd) - cmd_len,
        " -f rawvideo -pix_fmt rgb24 -");

    fprintf(stderr, "[frame_tiler] Running: %s\n", cmd);

#ifdef _WIN32
    /* Windows: use CreateProcess with pipes */
    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        fprintf(stderr, "[frame_tiler] CreatePipe failed\n");
        return -1;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[frame_tiler] CreateProcess failed: %lu\n", GetLastError());
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return -1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(hWrite);

    FILE *pipe = _fdopen(_open_osfhandle((intptr_t)hRead, 0), "rb");
    if (!pipe) {
        fprintf(stderr, "[frame_tiler] _fdopen failed\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return -1;
    }

    int ret = process_frames_mt(pipe, target_w, target_h, target_fps,
                                tile_size, max_workers, output_dir,
                                out_frame_count, out_duration);

    fclose(pipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
#else
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "[frame_tiler] popen failed\n");
        return -1;
    }

    int ret = process_frames_mt(pipe, target_w, target_h, target_fps,
                                tile_size, max_workers, output_dir,
                                out_frame_count, out_duration);

    pclose(pipe);
#endif

    return ret;
}

/* ---- Main ---- */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <video_path> <output_dir> [width] [height] [fps] [tile_size] [workers] [ffmpeg_path]\n", argv[0]);
        return 1;
    }

    const char *video_path = argv[1];
    const char *output_dir = argv[2];
    int target_w = 0;
    int target_h = 0;
    double target_fps = 0.0;
    int tile_size = TILE_SIZE_DEFAULT;
    int max_workers = WORKERS_DEFAULT;
    const char *ffmpeg_path = NULL;

    if (argc > 3) target_w = atoi(argv[3]);
    if (argc > 4) target_h = atoi(argv[4]);
    if (argc > 5) target_fps = atof(argv[5]);
    if (argc > 6) tile_size = atoi(argv[6]);
    if (argc > 7) max_workers = atoi(argv[7]);
    if (argc > 8) ffmpeg_path = argv[8];

    if (tile_size <= 0) tile_size = TILE_SIZE_DEFAULT;
    if (max_workers < 1) max_workers = 1;

    /* Create output directory */
    MKDIR(output_dir);

    fprintf(stderr, "[frame_tiler] Starting...\n");
    fprintf(stderr, "[frame_tiler] Video: %s\n", video_path);
    fprintf(stderr, "[frame_tiler] Output: %s\n", output_dir);
    if (target_w > 0 || target_h > 0) {
        fprintf(stderr, "[frame_tiler] Size: %dx%d\n", target_w, target_h);
    }
    if (target_fps > 0) {
        fprintf(stderr, "[frame_tiler] FPS: %g\n", target_fps);
    }
    fprintf(stderr, "[frame_tiler] Tile size: %d\n", tile_size);
    fprintf(stderr, "[frame_tiler] Workers: %d\n", max_workers);

    int frame_count = 0;
    double duration = 0.0;

    int ret = run_ffmpeg_and_process(video_path, target_w, target_h, target_fps,
                                     tile_size, max_workers, output_dir,
                                     ffmpeg_path, &frame_count, &duration);

    if (ret == 0) {
        fprintf(stderr, "[frame_tiler] Done! %d frames, %.1fs duration\n", frame_count, duration);
        printf("{\"frame_count\":%d,\"duration\":%.3f}\n", frame_count, duration);
    } else {
        fprintf(stderr, "[frame_tiler] Failed with error %d\n", ret);
        return 1;
    }

    return 0;
}
