#include "mini_gl.h"
#include <string.h>
#include <math.h>

typedef struct { float x,y,z; } vec3;

static int g_w, g_h;
static uint8_t *g_fb;
static float *g_zbuf;

static vec3 mat_r(vec3 v, float ax, float ay, float az) {
    float cx=cosf(ax), sx=sinf(ax);
    float cy=cosf(ay), sy=sinf(ay);
    float cz=cosf(az), sz=sinf(az);

    float y = v.y*cx - v.z*sx;
    float z = v.y*sx + v.z*cx;
    v.y=y; v.z=z;

    float x = v.x*cy + v.z*sy;
    z = -v.x*sy + v.z*cy;
    v.x=x; v.z=z;

    x = v.x*cz - v.y*sz;
    y = v.x*sz + v.y*cz;
    v.x=x; v.y=y;
    return v;
}

static vec3 g_rot = {0,0,0};
static vec3 g_trans = {0,0,0};

void gl_init(int w, int h, uint8_t *framebuffer) {
    g_w = w; g_h = h; g_fb = framebuffer;
    g_zbuf = (float*)0x100000; // pick some free RAM
}

void gl_clear(uint8_t color) {
    memset(g_fb, color, g_w*g_h);
}

void gl_clear_depth(void) {
    for (int i=0;i<g_w*g_h;i++) g_zbuf[i] = 1e9f;
}

void gl_matrix_identity(void) {
    g_rot.x = g_rot.y = g_rot.z = 0;
    g_trans.x = g_trans.y = g_trans.z = 0;
}

void gl_matrix_rotate(float ax, float ay, float az) {
    g_rot.x += ax; g_rot.y += ay; g_rot.z += az;
}

void gl_matrix_translate(float x, float y, float z) {
    g_trans.x += x; g_trans.y += y; g_trans.z += z;
}

static void putpixel_z(int x, int y, float z, uint8_t c) {
    if (x<0||x>=g_w||y<0||y>=g_h) return;
    int idx = y*g_w + x;
    if (z < g_zbuf[idx]) {
        g_zbuf[idx] = z;
        g_fb[idx] = c;
    }
}

static void project(vec3 v, int *x, int *y, float *z) {
    v = mat_r(v, g_rot.x, g_rot.y, g_rot.z);
    v.x += g_trans.x;
    v.y += g_trans.y;
    v.z += g_trans.z + 4.0f;
    float f = 200.0f / v.z;
    *x = (int)(v.x*f + g_w/2);
    *y = (int)(v.y*f + g_h/2);
    *z = v.z;
}

/* triangle rasterizer: flat color, simple barycentric */
static void draw_triangle(vec3 a, vec3 b, vec3 c, uint8_t col) {
    int x0,y0,x1,y1,x2,y2;
    float z0,z1,z2;
    project(a,&x0,&y0,&z0);
    project(b,&x1,&y1,&z1);
    project(c,&x2,&y2,&z2);

    int minx = x0<x1?(x0<x2?x0:x2):(x1<x2?x1:x2);
    int maxx = x0>x1?(x0>x2?x0:x2):(x1>x2?x1:x2);
    int miny = y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2);
    int maxy = y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2);

    float denom = (float)((y1 - y2)*(x0 - x2) + (x2 - x1)*(y0 - y2));
    if (denom == 0) return;

    for (int y=miny; y<=maxy; y++) {
        for (int x=minx; x<=maxx; x++) {
            float w0 = (float)((y1 - y2)*(x - x2) + (x2 - x1)*(y - y2)) / denom;
            float w1 = (float)((y2 - y0)*(x - x2) + (x0 - x2)*(y - y2)) / denom;
            float w2 = 1.0f - w0 - w1;
            if (w0>=0 && w1>=0 && w2>=0) {
                float z = w0*z0 + w1*z1 + w2*z2;
                putpixel_z(x,y,z,col);
            }
        }
    }
}

/* GL-style API */

static vec3 vbuf[3];
static int vcount = 0;

void gl_begin(void) {
    vcount = 0;
}

void gl_vertex3f(float x, float y, float z) {
    if (vcount < 3) {
        vbuf[vcount].x = x;
        vbuf[vcount].y = y;
        vbuf[vcount].z = z;
        vcount++;
    }
}

void gl_end(void) {
    if (vcount == 3) {
        draw_triangle(vbuf[0], vbuf[1], vbuf[2], 15);
    }
}
