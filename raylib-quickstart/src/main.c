/*
Magnetometer 3D plotter
Reads whitespace-separated samples from resources/data/magnetometer_data.txt
and renders the magnetic field vector, a fading trail of past samples, and a
reference sphere at the mean |B| radius.

File format (one sample per line, 7 whitespace-separated numbers):
    rawX rawY rawZ gaussX gaussY gaussZ tempC

Only columns 4, 5, 6 (gaussX/Y/Z) and column 7 (tempC) are used for rendering.
Columns 1-3 are kept for lossless round-tripping of the source data.

Based on raylib-quickstart by Jeffery Myers (CC0 1.0).
*/

#include "raylib.h"
#include "resource_dir.h"   /* SearchAndSetResourceDir: chdir helper from quickstart */

#include <stdio.h>          /* fopen, fgets, sscanf, fprintf */
#include <stdlib.h>         /* (nothing used here yet, but harmless) */
#include <string.h>         /* memmove */
#include <math.h>           /* sqrtf, sinf, cosf, PI (from raylib) */

/* ------------------------------------------------------------------ */
/*  Tunable constants                                                 */
/* ------------------------------------------------------------------ */

/* How many past samples to remember for the trail.
 * The trail buffer is a ring; once it fills, the oldest sample is dropped. */
#define MAX_TRAIL      2000

/* World-units per gauss. A typical Earth field is ~0.5 gauss; multiplying
 * by 5 makes the vector span ~2.5 world units, which is comfortably visible
 * against the default grid (which is 10 units across). */
#define SCALE          5.0f

/* Playback rate: how many samples per second of wall-clock time get consumed.
 * Independent of the render rate (60 fps via SetTargetFPS below). */
#define PLAYBACK_HZ    20.0f

/* Fallback |B| used for the reference sphere until we have enough samples to
 * compute a meaningful running mean. Earth's field is roughly 0.25-0.65 gauss
 * depending on location; 0.92 matches the sample data in this project. */
#define DEFAULT_REF_B  0.92f

/* Reference-sphere mesh density. More rings/slices = smoother sphere but more
 * line segments per frame. 8x12 is a good balance for a single reference
 * shell. */
#define SPHERE_RINGS   8
#define SPHERE_SLICES  12

/* ------------------------------------------------------------------ */
/*  Types                                                             */
/* ------------------------------------------------------------------ */

/* Three floats, our minimal 3D vector. We use raylib's Vector3 for anything
 * that interacts with raylib's API, but Vec3 for pure data storage. Mixing
 * them would be fine too; keeping them separate makes the data layer obvious. */
typedef struct { 
	float x;
	float y; 
	float z; } Vec3;

/* ------------------------------------------------------------------ */
/*  Playback state (module-static, so no globals across files)        */
/* ------------------------------------------------------------------ */

/* Ring buffer of past samples, used to draw the fading trail. */
static Vec3  trail[MAX_TRAIL];
static int   trailCount = 0;

/* Most recent sample read from the file. Starts at origin so the first frame
 * (before any sample has been read) draws something sensible. */
static Vec3  current = {0};
static float tempC   = 0.0f;

/* Running mean of |B|, used to size the reference sphere to the data rather
 * than to a hardcoded value. Uses Welford's incremental-mean formula:
 *   mean += (x - mean) / n
 * This is numerically more stable than summing-and-dividing, and lets us
 * update in O(1) without storing all samples. */
static float meanMag  = 0.0f;
static int   magCount = 0;

/* ------------------------------------------------------------------ */
/*  Parsing                                                           */
/* ------------------------------------------------------------------ */

/*
 * Try to parse one line of the form:
 *     rawX rawY rawZ gaussX gaussY gaussZ tempC
 * into the output Vec3 and temp. Returns 1 on success, 0 on failure.
 *
 * sscanf returns the number of successful conversions. We demand all seven.
 * Lines that don't parse (blank lines, comments, the header row if present)
 * will produce fewer than 7 conversions and be rejected — that's how we skip
 * them without special-casing.
 */
static int parseLine(const char *line, Vec3 *out, float *t) {
    float rx, ry, rz;
    return sscanf(line, "%f %f %f %f %f %f %f",
                  &rx, &ry, &rz,
                  &out->x, &out->y, &out->z, t) == 7;
}

/*
 * Read lines from 'fp' until one parses successfully or we hit EOF.
 * Returns 1 if a sample was produced, 0 at EOF.
 *
 * The loop body is deliberately minimal: we don't know how many garbage
 * lines might precede the next valid one, so we just keep going until we
 * find a good one. This makes the reader robust against headers, trailing
 * whitespace, and re-encoding artifacts.
 */
static int readSample(FILE *fp, Vec3 *out, float *t) {
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (parseLine(line, out, t)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Playback helpers                                                  */
/* ------------------------------------------------------------------ */

/*
 * Append a sample to the trail.
 *
 * While the buffer isn't full, samples are stored in order. Once full, we
 * shift everything left by one and put the new sample at the end. This is
 * O(n) per sample, but n = MAX_TRAIL = 2000 floats * 3 = 24 KB memmove,
 * which is trivial at 20 Hz. A proper ring buffer with head/tail indices
 * would be O(1) and is the right answer at 100k+ samples; not worth the
 * added index arithmetic here.
 */
static void pushTrail(Vec3 v) {
    if (trailCount < MAX_TRAIL) {
        trail[trailCount++] = v;
    } else {
        /* Shift left by one slot. memmove is required rather than memcpy
         * because source and destination overlap. */
        memmove(trail, trail + 1, (MAX_TRAIL - 1) * sizeof(Vec3));
        trail[MAX_TRAIL - 1] = v;
    }
}

/*
 * Update the running mean of |B|.
 *
 * Welford's incremental formula: mean += (x - mean) / n. Avoids accumulating
 * a running sum, which loses precision over long runs.
 */
static void updateMeanMag(Vec3 v) {
    float m = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    magCount++;
    meanMag += (m - meanMag) / (float)magCount;
}

/* ------------------------------------------------------------------ */
/*  Geometry drawing                                                  */
/* ------------------------------------------------------------------ */

/*
 * Draw a wireframe sphere centered at c with radius r.
 *
 * Two families of curves:
 *   - Latitude rings: horizontal circles at different Y values
 *   - Longitude meridians: vertical half-circles connecting the poles
 *
 * Both are drawn as polylines of SEG segments. SEG=64 is enough that the
 * curves look smooth at any zoom level; more would just cost frame time.
 *
 * Don't use raylib's DrawCircle3D because it can't be offset along the
 * circle's own axis (needed for latitude rings at y != 0), and because we
 * want to hand-tune which rings/meridians get drawn.
 */
static void drawRefSphere(Vector3 c, float r, int rings, int slices, Color col) {
    const int SEG = 64;

    /* Latitude rings: phi goes from 0 (north pole) to PI (south pole).
     * Skip i=0 and i=rings to avoid degenerate rings at the poles
     * (radius zero → all points collapse to a single pixel). */
    for (int i = 1; i < rings; i++) {
        float phi = PI * (float)i / (float)rings;
        float y   = cosf(phi) * r;      /* height of this ring above equator */
        float rr  = sinf(phi) * r;      /* radius of this ring */
        Vector3 prev = {0};
        for (int s = 0; s <= SEG; s++) {
            float t = 2.0f * PI * (float)s / (float)SEG;
            Vector3 p = { c.x + rr * cosf(t), c.y + y, c.z + rr * sinf(t) };
            /* Draw a segment from previous point; skip on first iteration
             * because prev hasn't been initialized meaningfully. */
            if (s > 0) DrawLine3D(prev, p, col);
            prev = p;
        }
    }

    /* Longitude meridians: theta rotates the meridian plane around Y.
     * Each meridian is a half-circle from +Y pole to -Y pole. */
    for (int j = 0; j < slices; j++) {
        float theta = PI * (float)j / (float)slices;
        float ct = cosf(theta), st = sinf(theta);
        Vector3 prev = {0};
        for (int s = 0; s <= SEG; s++) {
            float phi = PI * (float)s / (float)SEG;
            float y  =  cosf(phi) * r;
            float rr =  sinf(phi) * r;
            /* The meridian lies in the plane spanned by (ct, 0, st) and Y. */
            Vector3 p = { c.x + rr * ct, c.y + y, c.z + rr * st };
            if (s > 0) DrawLine3D(prev, p, col);
            prev = p;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

int main(void) {
    /* Request vsync (caps framerate to display refresh) and high-DPI
     * awareness (Windows won't blur the window on high-resolution monitors).
     * Must be called before InitWindow. */
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);

    /* Create the OS window and an OpenGL context. Must be called before
     * any raylib drawing function. */
    InitWindow(1100, 750, "Magnetometer 3D");

    /* Change the process's working directory to the resources/ folder,
     * using a helper from the quickstart template. It locates the executable
     * and walks up the tree looking for a folder named "resources".
     * After this call, relative paths like "magnetometer_data.txt" resolve
     * to resources/magnetometer_data.txt. */
    SearchAndSetResourceDir("resources");

    /* Open the data file for reading. Because of the chdir above, this path
     * is relative to resources/. */
    FILE *fp = fopen("magnetometer_data.txt", "r");
    if (!fp) {
        fprintf(stderr, "cannot open magnetometer_data.txt (CWD should be resources/)\n");
        CloseWindow();
        return 1;
    }

    /* Cap render loop at 60 fps. Playback is time-based (see simTimer below)
     * so framerate doesn't affect how fast samples are consumed. */
    SetTargetFPS(60);

    /* Camera setup. Camera3D is raylib's standard perspective camera.
     * - position: where the eye is
     * - target:   what it looks at (origin — the center of our vector space)
     * - up:       which direction is "up" (Y-axis, standard for 3D views)
     * - fovy:     vertical field of view in degrees
     * - projection: PERSPECTIVE for natural depth, ORTHOGRAPHIC for diagrams */
    Camera3D cam = {
        .position   = { 6.0f, 4.5f, 6.0f },
        .target     = { 0.0f, 0.0f, 0.0f },
        .up         = { 0.0f, 1.0f, 0.0f },
        .fovy       = 45.0f,
        .projection = CAMERA_PERSPECTIVE,
    };

    /* Playback clock. stepDt is the wall-clock interval between samples:
     * 1/20 second at PLAYBACK_HZ=20. simTimer accumulates frame time and
     * drains it in stepDt-sized chunks, so playback speed is independent
     * of render rate. */
    const float stepDt = 1.0f / PLAYBACK_HZ;
    float simTimer = 0.0f;
    int   eof      = 0;   /* becomes 1 once the file is exhausted */

	/* 
	* True reference field for Toronto, ON (~0.53 Total Gauss)
	* Mapping to Raylib: 
	*   X_raylib = North (~0.189G)
	*   Y_raylib = Up (-Down Component = -0.494G)
	*   Z_raylib = East (~-0.031G due to -9.5° West Declination)
	*/
	static Vec3 earthRef = { 0.1890f, -0.4941f, -0.0316f }; 
    /* ---------------- main loop ---------------- */
    /* WindowShouldClose() returns true when the user clicks the close
     * button or presses ESC. */
    while (!WindowShouldClose()) {

        /* ---------- UPDATE ---------- */

        /* Advance playback. Consume as many samples as the elapsed time
         * permits — usually 0 or 1 per frame at 60 fps / 20 Hz playback,
         * but the while loop handles slower frames correctly. */
        if (!eof) {
            simTimer += GetFrameTime();    /* seconds since last frame */
            while (simTimer >= stepDt && !eof) {
                simTimer -= stepDt;
                Vec3 v; float t;
                if (readSample(fp, &v, &t)) {
                    current = v;
                    tempC   = t;
                    pushTrail(v);
                    updateMeanMag(v);
                } else {
                    eof = 1;   /* stop advancing once the file runs out */
                }
            }
        }

        /* Camera update. In current raylib, the mode is passed here rather
         * than via a separate SetCameraMode call. CAMERA_ORBITAL gives us
         * left-drag orbit, scroll zoom, right-drag pan for free. */
        UpdateCamera(&cam, CAMERA_ORBITAL);

        /* ---------- DRAW ---------- */

        BeginDrawing();
        ClearBackground((Color){18, 18, 28, 255});   /* dark blue-gray */

        BeginMode3D(cam);

        /* Static reference frame: a grid on the XZ plane, 10 units across,
         * with 1-unit spacing. */
        DrawGrid(10, 1.0f);

        /* Reference sphere at the running mean |B| radius. Using meanMag once
         * we have samples means the sphere self-calibrates to the data. */
        float R = (meanMag > 0.0f ? meanMag : DEFAULT_REF_B) * SCALE;
        drawRefSphere((Vector3){0,0,0}, R, SPHERE_RINGS, SPHERE_SLICES,
                      (Color){90, 90, 140, 90});   /* low alpha = ghostly */

        /* World axes for orientation. Red = X, green = Y, blue = Z.
         * Length 2 world units so they don't compete visually with the
         * data's ~4.6-unit radius. */
        DrawLine3D((Vector3){0,0,0}, (Vector3){2,0,0}, (Color){224, 85, 85, 255});
        DrawLine3D((Vector3){0,0,0}, (Vector3){0,2,0}, (Color){85, 224, 85, 255});
        DrawLine3D((Vector3){0,0,0}, (Vector3){0,0,2}, (Color){85, 136, 255, 255});

        /* Trail: a polyline connecting past samples in order. Alpha fades
         * with age so older parts of the trail recede visually. The alpha
         * formula maps i∈[1,trailCount] to alpha∈[40,240]. */
        for (int i = 1; i < trailCount; i++) {
            Vector3 a = { trail[i-1].x*SCALE, trail[i-1].y*SCALE, trail[i-1].z*SCALE };
            Vector3 b = { trail[i  ].x*SCALE, trail[i  ].y*SCALE, trail[i  ].z*SCALE };
            unsigned char alpha = (unsigned char)(40 + 200 * i / trailCount);
            DrawLine3D(a, b, (Color){255, 200, 60, alpha});
        }

        /* Current field vector: a yellow line from origin to the tip, an
         * orange sphere at the tip to make its position obvious, and a small
         * white sphere at the origin so the vector's base is visible. */
        Vector3 tip = { current.x*SCALE, current.y*SCALE, current.z*SCALE };
        DrawLine3D((Vector3){0,0,0}, tip, (Color){255, 220, 80, 255});
        DrawSphere(tip, 0.08f, (Color){255, 120, 40, 255});
        DrawSphere((Vector3){0,0,0}, 0.05f, RAYWHITE);

		DrawLine3D((Vector3){0,0,0}, 
		(Vector3){ earthRef.x * SCALE, earthRef.y * SCALE, earthRef.z * SCALE }, 
		(Color){ 0, 200, 200, 255 }); // Cyan

		// Optional: Add a small sphere at the tip for visibility
		DrawSphere((Vector3){ earthRef.x * SCALE, earthRef.y * SCALE, earthRef.z * SCALE }, 
				0.06f, (Color){ 0, 200, 200, 180 });

        EndMode3D();

        /* ---------- HUD ---------- */
        /* 2D overlay drawn after EndMode3D, so coordinates are screen pixels
         * (origin top-left, +Y down) not world units. */
        float mag = sqrtf(current.x*current.x + current.y*current.y + current.z*current.z);
        DrawText(TextFormat("gauss   X=%.4f  Y=%.4f  Z=%.4f", current.x, current.y, current.z), 10, 10, 20, RAYWHITE);
        DrawText(TextFormat("|B|     %.4f gauss", mag),     10, 35, 20, RAYWHITE);
        DrawText(TextFormat("mean|B| %.4f gauss", meanMag), 10, 60, 20, RAYWHITE);
        DrawText(TextFormat("temp    %.2f C",     tempC),   10, 85, 20, RAYWHITE);
        DrawText(TextFormat("samples %d%s", trailCount, eof ? "  [EOF]" : ""), 10, 110, 20, LIGHTGRAY);
        DrawText("mouse: orbit   wheel: zoom   right-drag: pan",
                 10, 720, 18, (Color){150, 150, 170, 255});

        EndDrawing();
    }

    /* ---------------- cleanup ---------------- */
    /* Free OS resources. Order matters for raylib: close the file, then the
     * window (which tears down the GL context). */
    fclose(fp);
    CloseWindow();
    return 0;
}