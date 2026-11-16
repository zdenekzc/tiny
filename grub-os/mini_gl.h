#pragma once
#include <stdint.h>

void gl_init(int w, int h, uint8_t *framebuffer);
void gl_clear(uint8_t color);
void gl_clear_depth(void);

void gl_matrix_identity(void);
void gl_matrix_rotate(float ax, float ay, float az);
void gl_matrix_translate(float x, float y, float z);

void gl_begin(void);
void gl_vertex3f(float x, float y, float z);
void gl_end(void);
