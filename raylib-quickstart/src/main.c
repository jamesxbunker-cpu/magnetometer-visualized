/*
Magnetometer 3D plotter - live version
Reads whitespace-separated samples from a COM port and renders the magnetic
field vector, a fading trail of past samples, and a reference sphere at the
mean |B| radius.

Expected line format (same as the file version):
    rawX rawY rawZ gaussX gaussY gaussZ tempC
with CRLF or LF line endings.

Based on raylib-quickstart by Jeffery Myers (CC0 1.0).
*/

#include "raylib.h"
#include "resource_dir.h"    /* SearchAndSetResourceDir */
#include "comport.h"         /* <-- NEW: our library */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Tunable constants                                                 */
/* ------------------------------------------------------------------ */

#define MAX_TRAIL      2000
#define SCALE          5.0f
#define DEFAULT_REF_B  0.92f
#define SPHERE_RINGS   8
#define SPHERE_SLICES  12

/* COM port settings. Change via command line or hardcode here. */
#define DEFAULT_PORT   "COM3"
#define DEFAULT_BAUD   115200

/* Max bytes per comport_read call. The STM32 likely sends one line per
 * transmission (~60 bytes), but we allow up to 512 in case it batches. */
#define RX_CHUNK       512

/* Line buffer capacity. A single sample line is ~60-70 chars; 512 gives
 * plenty of headroom for malformed lines or slow accumulation. */
#define LINE_BUF       512

/* ------------------------------------------------------------------ */
/*  Types                                                             */
/* ------------------------------------------------------------------ */

typedef struct { float x, y, z; } Vec3;

/* ------------------------------------------------------------------ */
/*  Playback state                                                    */
/* ------------------------------------------------------------------ */

static Vec3  trail[MAX_TRAIL];
static int   trailCount = 0;
static Vec3  current = {0};
static float tempC   = 0.0f;
static float meanMag = 0.0f;
static int   magCount = 0;

/* ------------------------------------------------------------------ */
/*  Parsing                                                           */
/* ------------------------------------------------------------------ */

static int parseLine(const char *line, Vec3 *out, float *t) {
    float rx, ry, rz;
    return sscanf(line, "%f %f %f %f %f %f %f",
                  &rx, &ry, &rz,
                  &out->x, &out->y, &out->z, t) == 7;
}

/* ------------------------------------------------------------------ */
/*  Trail + running mean                                              */
/* ------------------------------------------------------------------ */

static void pushTrail(Vec3 v) {
    if (trailCount < MAX_TRAIL) {
        trail[trailCount++] = v;
    } else {
        memmove(trail, trail + 1, (MAX_TRAIL - 1) * sizeof(Vec3));
        trail[MAX_TRAIL - 1] = v;
    }
}

static void updateMeanMag(Vec3 v) {
    float m = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    magCount++;
    meanMag += (m - meanMag) / (float)magCount;
}

/* ------------------------------------------------------------------ */
/*  Line-buffered COM reader                                          */
/* ------------------------------------------------------------------ */

/*
 * The port gives us bytes, not lines. This struct accumulates bytes until
 * a newline shows up, then hands the caller a complete line.
 *
 * If the line buffer fills up without a newline (garbage, baud mismatch,
 * or a device sending binary), we discard the buffer and start over. That
 * keeps one bad stretch from wedging the parser forever.
 */
typedef struct {
    char  buf[LINE_BUF];
    int   len;
    int   discard;   /* 1 = we overflowed; skip bytes until next '\n' */
} line_reader_t;

static void lr_init(line_reader_t *lr) {
    lr->len = 0;
    lr->discard = 0;
}

/*
 * Feed bytes from the port. Whenever a complete line is available, copy it
 * into 'out' (null-terminated, newline stripped) and return 1.
 * On no complete line, return 0. Any leftover bytes stay buffered.
 *
 * Caller loops this until it returns 0, to drain all complete lines from
 * one read() call in case multiple arrived at once.
 */
static int lr_next(line_reader_t *lr, const unsigned char *chunk, int n,
                   int *consumed, char *out, size_t out_sz) {
    int i = *consumed;
    while (i < n) {
        unsigned char c = chunk[i++];

        if (c == '\n') {
            /* End of line. If we were discarding, reset and continue. */
            if (lr->discard) {
                lr->discard = 0;
                lr->len = 0;
                continue;
            }
            /* Copy out (strip trailing '\r' if present). */
            int copy = lr->len;
            if (copy > 0 && lr->buf[copy - 1] == '\r') copy--;
            if ((size_t)copy >= out_sz) copy = (int)out_sz - 1;
            memcpy(out, lr->buf, (size_t)copy);
            out[copy] = '\0';
            lr->len = 0;
            *consumed = i;
            return 1;
        }

        if (lr->discard) {
            /* Still skipping until newline. */
            continue;
        }

        if (lr->len < LINE_BUF - 1) {
            lr->buf[lr->len++] = (char)c;
        } else {
            /* Overflow: too long without a newline. Start discarding. */
            lr->discard = 1;
            lr->len = 0;
        }
    }
    *consumed = i;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Geometry drawing                                                  */
/* ------------------------------------------------------------------ */

static void drawRefSphere(Vector3 c, float r, int rings, int slices, Color col) {
    const int SEG = 64;

    for (int i = 1; i < rings; i++) {
        float phi = PI * (float)i / (float)rings;
        float y   = cosf(phi) * r;
        float rr  = sinf(phi) * r;
        Vector3 prev = {0};
        for (int s = 0; s <= SEG; s++) {
            float t = 2.0f * PI * (float)s / (float)SEG;
            Vector3 p = { c.x + rr * cosf(t), c.y + y, c.z + rr * sinf(t) };
            if (s > 0) DrawLine3D(prev, p, col);
            prev = p;
        }
    }

    for (int j = 0; j < slices; j++) {
        float theta = PI * (float)j / (float)slices;
        float ct = cosf(theta), st = sinf(theta);
        Vector3 prev = {0};
        for (int s = 0; s <= SEG; s++) {
            float phi = PI * (float)s / (float)SEG;
            float y  =  cosf(phi) * r;
            float rr =  sinf(phi) * r;
            Vector3 p = { c.x + rr * ct, c.y + y, c.z + rr * st };
            if (s > 0) DrawLine3D(prev, p, col);
            prev = p;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    /* Command line: program [port] [baud]. Defaults to COM3 @ 115200. */
    const char *port = (argc > 1) ? argv[1] : DEFAULT_PORT;
    unsigned    baud = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 10)
                                  : DEFAULT_BAUD;

    /* ------------------------------------------------------------------
     * Open the COM port FIRST, before the window. If the port doesn't open,
     * we want a clean error message, not a window flashing and disappearing.
     * ------------------------------------------------------------------ */
    comport_t *cp = comport_open(port, baud);
    if (!cp) {
        fprintf(stderr, "cannot open %s at %u baud\n", port, baud);
        return 1;
    }
    printf("Opened %s. Waiting for data...\n", comport_name(cp));

    /* ------------------------------------------------------------------
     * raylib setup (unchanged from the file version, minus the file open)
     * ------------------------------------------------------------------ */
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
    InitWindow(1100, 750, "Magnetometer 3D - live");

    /* Only needed if you still want to load shaders or other assets from
     * resources/. The data file is no longer read from disk. */
    SearchAndSetResourceDir("resources");

    SetTargetFPS(60);

    Camera3D cam = {
        .position   = { 6.0f, 4.5f, 6.0f },
        .target     = { 0.0f, 0.0f, 0.0f },
        .up         = { 0.0f, 1.0f, 0.0f },
        .fovy       = 45.0f,
        .projection = CAMERA_PERSPECTIVE,
    };

    static Vec3 earthRef = { 0.1890f, -0.4941f, -0.0316f };

    /* Line reader state, persists across frames. */
    line_reader_t lr;
    lr_init(&lr);

    /* Stats for the HUD. */
    unsigned long bytes_rx   = 0;
    unsigned long samples_rx = 0;
    unsigned long parse_fail = 0;

    /* ---------------- main loop ---------------- */
    while (!WindowShouldClose()) {

        /* ---------- UPDATE: drain the COM port ---------- */

        /*
         * Read from the port in a loop until it has nothing more for us.
         * `comport_read` with a small timeout (0 = non-blocking) returns
         * as soon as any bytes are available, or 0 immediately if nothing.
         *
         * The inner while handles the case where multiple samples arrive
         * in one chunk (e.g., the STM32 buffered them during a slow frame).
         */
        unsigned char chunk[RX_CHUNK];
        for (;;) {
            int n = comport_read(cp, chunk, sizeof(chunk), 0);
            if (n < 0) {
                fprintf(stderr, "COM port lost.\n");
                goto done;      /* exit the render loop */
            }
            if (n == 0) break;  /* nothing more right now */

            bytes_rx += (unsigned long)n;

            /* Feed the line reader. It may produce zero, one, or many
             * complete lines from this chunk. */
            int consumed = 0;
            char line[LINE_BUF];
            while (lr_next(&lr, chunk, n, &consumed, line, sizeof(line))) {
                Vec3 v; float t;
                if (parseLine(line, &v, &t)) {
                    current = v;
                    tempC   = t;
                    pushTrail(v);
                    updateMeanMag(v);
                    samples_rx++;
                } else {
                    parse_fail++;
                }
            }
        }

        /* Camera update */
        UpdateCamera(&cam, CAMERA_ORBITAL);

        /* ---------- DRAW ---------- */
        BeginDrawing();
        ClearBackground((Color){18, 18, 28, 255});

        BeginMode3D(cam);

        DrawGrid(10, 1.0f);

        float R = (meanMag > 0.0f ? meanMag : DEFAULT_REF_B) * SCALE;
        drawRefSphere((Vector3){0,0,0}, R, SPHERE_RINGS, SPHERE_SLICES,
                      (Color){90, 90, 140, 90});

        DrawLine3D((Vector3){0,0,0}, (Vector3){2,0,0}, (Color){224, 85, 85, 255});
        DrawLine3D((Vector3){0,0,0}, (Vector3){0,2,0}, (Color){85, 224, 85, 255});
        DrawLine3D((Vector3){0,0,0}, (Vector3){0,0,2}, (Color){85, 136, 255, 255});

        for (int i = 1; i < trailCount; i++) {
            Vector3 a = { trail[i-1].x*SCALE, trail[i-1].y*SCALE, trail[i-1].z*SCALE };
            Vector3 b = { trail[i  ].x*SCALE, trail[i  ].y*SCALE, trail[i  ].z*SCALE };
            unsigned char alpha = (unsigned char)(40 + 200 * i / trailCount);
            DrawLine3D(a, b, (Color){255, 200, 60, alpha});
        }

        Vector3 tip = { current.x*SCALE, current.y*SCALE, current.z*SCALE };
        DrawLine3D((Vector3){0,0,0}, tip, (Color){255, 220, 80, 255});
        DrawSphere(tip, 0.08f, (Color){255, 120, 40, 255});
        DrawSphere((Vector3){0,0,0}, 0.05f, RAYWHITE);

        DrawLine3D((Vector3){0,0,0},
            (Vector3){ earthRef.x * SCALE, earthRef.y * SCALE, earthRef.z * SCALE },
            (Color){ 0, 200, 200, 255 });
        DrawSphere((Vector3){ earthRef.x * SCALE, earthRef.y * SCALE, earthRef.z * SCALE },
                   0.06f, (Color){ 0, 200, 200, 180 });

        EndMode3D();

        /* ---------- HUD ---------- */
        float mag = sqrtf(current.x*current.x + current.y*current.y + current.z*current.z);
        DrawText(TextFormat("gauss   X=%.4f  Y=%.4f  Z=%.4f", current.x, current.y, current.z), 10, 10, 20, RAYWHITE);
        DrawText(TextFormat("|B|     %.4f gauss", mag),     10, 35, 20, RAYWHITE);
        DrawText(TextFormat("mean|B| %.4f gauss", meanMag), 10, 60, 20, RAYWHITE);
        DrawText(TextFormat("temp    %.2f C",     tempC),   10, 85, 20, RAYWHITE);
        DrawText(TextFormat("samples %d  (rx %lu, parse-fail %lu)",
                            trailCount, samples_rx, parse_fail),
                 10, 110, 20, LIGHTGRAY);
        DrawText(TextFormat("port %s  baud %u  bytes %lu",
                            comport_name(cp), baud, bytes_rx),
                 10, 135, 18, (Color){150, 170, 200, 255});
        DrawText("mouse: orbit   wheel: zoom   right-drag: pan",
                 10, 720, 18, (Color){150, 150, 170, 255});

        EndDrawing();
    }

done:
    /* ---------------- cleanup ---------------- */
    comport_close(cp);
    CloseWindow();
    return 0;
}