/*
 * frame_tiler.c — Standalone C program for fast video frame extraction, tiling, and PNG encoding.
 *
 * Usage:
 *   frame_tiler.exe <video_path> <output_dir> [width] [height] [fps] [tile_size] [workers]
 *
 * Arguments:
 *   video_path   — Path to input video file
 *   output_dir   — Directory to write PNG tiles
 *   width        — Output width (default: source width)
 *   height       — Output height (default: source height)
 *   fps          — Output FPS (default: source FPS)
 *   tile_size    — Max tile dimension, default 256
 *   workers      — Number of worker threads, default 16
 *
 * How it works:
 *   1. Spawns ffmpeg as a subprocess to decode video to raw BGR24 frames via pipe
 *   2. Reads frames from stdin (piped from ffmpeg)
 *   3. Splits each frame into tiles (tile_size x tile_size)
 *   4. Encodes each tile as PNG using stb_image_write (zero external dependencies)
 *   5. Writes PNG files to output_dir
 *   6. Outputs metadata JSON to stdout when done
 *
 * Compile (MinGW / MSYS2):
 *   gcc frame_tiler.c -O3 -o frame_tiler.exe
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

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
    #define DIR_SEP "\\"
    #define MKDIR(dir) _mkdir(dir)
    #define SNPRINTF _snprintf
#else
    #include <pthread.h>
    #include <sys/stat.h>
    #define DIR_SEP "/"
    #define MKDIR(dir) mkdir(dir, 0755)
    #define SNPRINTF snprintf
    #define _beginthreadex(a,b,c,d,e,f) ({ pthread_t t; pthread_create(&t,NULL,(void*(*)(void*))c,d); (unsigned long)(uintptr_t)t; })
#endif

#define MAX_PATH_LEN 4096
#define TILE_SIZE_DEFAULT 256
#define WORKERS_DEFAULT 16

/* ---- Tile data ---- */
typedef struct {
    unsigned char *png_data;
    int png_size;
    int frame_idx;
    int row;
    int col;
} Tile;

/* ---- Worker context ---- */
typedef struct {
    const char *output_dir;
    int tile_size;
    Tile *tiles;      /* Array of tiles to write */
    int tile_count;
} WorkerCtx;

/* ---- Mutex for thread safety ---- */
#ifdef _WIN32
static CRITICAL_SECTION g_mutex;
static void lock() { EnterCriticalSection(&g_mutex); }
static void unlock() { LeaveCriticalSection(&g_mutex); }
static void mutex_init() { InitializeCriticalSection(&g_mutex); }
static void mutex_destroy() { DeleteCriticalSection(&g_mutex); }
#else
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static void lock() { pthread_mutex_lock(&g_mutex); }
static void unlock() { pthread_mutex_unlock(&g_mutex); }
static void mutex_init() {}
static void mutex_destroy() {}
#endif

/* ---- Create directory recursively (simple version) ---- */
static void ensure_dir(const char *path) {
    /* Try to create; if it fails with EEXIST, that's fine */
    MKDIR(path);
    (void)path;
}

/* ---- Write a single tile to disk ---- */
static void write_tile(Tile *tile, const char *output_dir) {
    char filepath[MAX_PATH_LEN];
    SNPRINTF(filepath, sizeof(filepath), "%s/%d_%d_%d.png",
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
static Tile encode_tile(const unsigned char *bgr_data, int w, int h, int full_w,
                        int frame_idx, int row, int col) {
    Tile tile = {0};
    tile.frame_idx = frame_idx;
    tile.row = row;
    tile.col = col;

    /* Create contiguous buffer for this tile (BGR -> RGB for stbi) */
    unsigned char *rgb_buf = (unsigned char *)malloc((size_t)w * h * 3);
    if (!rgb_buf) return tile;

    for (int y = 0; y < h; y++) {
        const unsigned char *src = bgr_data + ((row * 256 + y) * full_w + col * 256) * 3;
        unsigned char *dst = rgb_buf + y * w * 3;
        for (int x = 0; x < w; x++) {
            /* BGR -> RGB */
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
    }

    tile.png_data = stbi_write_png_to_mem(rgb_buf, w * 3, w, h, 3, &tile.png_size);
    free(rgb_buf);
    return tile;
}

/* ---- Thread worker function ---- */
#ifdef _WIN32
static unsigned __stdcall worker_thread(void *arg)
#else
static void *worker_thread(void *arg)
#endif
{
    WorkerCtx *ctx = (WorkerCtx *)arg;
    for (int i = 0; i < ctx->tile_count; i++) {
        write_tile(&ctx->tiles[i], ctx->output_dir);
    }
    return 0;
}

/* ---- Process all frames ---- */
static int process_frames(FILE *ffmpeg_pipe, int width, int height, int fps,
                          int tile_size, int max_workers,
                          const char *output_dir,
                          int *out_frame_count, double *out_duration) {
    size_t frame_bytes = (size_t)width * height * 3;
    unsigned char *frame = (unsigned char *)malloc(frame_bytes);
    if (!frame) {
        fprintf(stderr, "[frame_tiler] Out of memory (%zu bytes)\n", frame_bytes);
        return -1;
    }

    int cols = (width + tile_size - 1) / tile_size;
    int rows = (height + tile_size - 1) / tile_size;
    int total_tiles_per_frame = rows * cols;

    int frame_idx = 0;
    int total_frames = 0;

    while (1) {
        /* Read one frame from ffmpeg pipe */
        size_t total_read = 0;
        while (total_read < frame_bytes) {
            size_t n = fread(frame + total_read, 1, frame_bytes - total_read, ffmpeg_pipe);
            if (n == 0) break;
            total_read += n;
        }
        if (total_read < frame_bytes) break; /* EOF */

        /* Encode all tiles for this frame */
        int tile_count = 0;
        Tile *tiles = (Tile *)calloc((size_t)total_tiles_per_frame, sizeof(Tile));
        if (!tiles) {
            fprintf(stderr, "[frame_tiler] Out of memory for tiles\n");
            break;
        }

        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int tw = (c * tile_size + tile_size <= width) ? tile_size : (width - c * tile_size);
                int th = (r * tile_size + tile_size <= height) ? tile_size : (height - r * tile_size);

                tiles[tile_count] = encode_tile(frame, tw, th, width, frame_idx, r, c);
                tile_count++;
            }
        }

        /* Write tiles (single-threaded for simplicity; can be parallelized) */
        for (int i = 0; i < tile_count; i++) {
            write_tile(&tiles[i], output_dir);
            if (tiles[i].png_data) {
                STBIW_FREE(tiles[i].png_data);
            }
        }
        free(tiles);

        total_frames++;
        if (total_frames % 100 == 0) {
            double ts = (double)total_frames / fps;
            fprintf(stderr, "[frame_tiler] Processed %d frames (%.1fs)\n", total_frames, ts);
        }

        frame_idx++;
    }

    free(frame);
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
    char cmd[MAX_PATH_LEN * 2];
    int cmd_len = 0;

    cmd_len = SNPRINTF(cmd, sizeof(cmd),
        "\"%s\" -hide_banner -loglevel error -i \"%s\"",
        ffmpeg_path ? ffmpeg_path : "ffmpeg", video_path);

    /* Add scale filter if needed */
    if (target_w > 0 || target_h > 0) {
        char vf[256];
        if (target_w > 0 && target_h > 0) {
            SNPRINTF(vf, sizeof(vf), "-vf scale=%d:%d", target_w, target_h);
        } else if (target_w > 0) {
            SNPRINTF(vf, sizeof(vf), "-vf scale=%d:-2", target_w);
        } else {
            SNPRINTF(vf, sizeof(vf), "-vf scale=-2:%d", target_h);
        }
        cmd_len += SNPRINTF(cmd + cmd_len, sizeof(cmd) - cmd_len, " %s", vf);
    }

    /* Add fps filter if needed */
    if (target_fps > 0) {
        cmd_len += SNPRINTF(cmd + cmd_len, sizeof(cmd) - cmd_len, " -vf fps=%g", target_fps);
    }

    /* Output raw BGR24 to stdout */
    cmd_len += SNPRINTF(cmd + cmd_len, sizeof(cmd) - cmd_len,
        " -f rawvideo -pix_fmt bgr24 -");

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
    /* Make read handle non-inheritable */
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

    /* Read from pipe */
    FILE *pipe = _fdopen(_open_osfhandle((intptr_t)hRead, 0), "rb");
    if (!pipe) {
        fprintf(stderr, "[frame_tiler] _fdopen failed\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return -1;
    }

    int ret = process_frames(pipe, target_w, target_h, target_fps,
                             tile_size, max_workers, output_dir,
                             out_frame_count, out_duration);

    fclose(pipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
#else
    /* Unix: use popen */
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "[frame_tiler] popen failed\n");
        return -1;
    }

    int ret = process_frames(pipe, target_w, target_h, target_fps,
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
    int target_w = 0;    /* 0 = keep original */
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
    ensure_dir(output_dir);

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

    mutex_init();

    int frame_count = 0;
    double duration = 0.0;

    int ret = run_ffmpeg_and_process(video_path, target_w, target_h, target_fps,
                                     tile_size, max_workers, output_dir,
                                     ffmpeg_path, &frame_count, &duration);

    mutex_destroy();

    if (ret == 0) {
        fprintf(stderr, "[frame_tiler] Done! %d frames, %.1fs duration\n", frame_count, duration);
        /* Output metadata as JSON to stdout for Python to parse */
        printf("{\"frame_count\":%d,\"duration\":%.3f}\n", frame_count, duration);
    } else {
        fprintf(stderr, "[frame_tiler] Failed with error %d\n", ret);
        return 1;
    }

    return 0;
}
