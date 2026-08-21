// PNG decode, for the damage path only. Drawing a whole frame hands chrome's
// own bytes to the terminal untouched and never comes through here.
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wunused-function"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#include "../vendor/stb_image.h"

#include "web.h"

uint8_t *png_decode_rgb(const void *png, size_t n, int *w, int *h) {
    int comp = 0;
    if (n > (size_t)INT32_MAX) return NULL;
    return stbi_load_from_memory(png, (int)n, w, h, &comp, 3);
}

void png_decode_free(uint8_t *rgb) {
    stbi_image_free(rgb);
}
