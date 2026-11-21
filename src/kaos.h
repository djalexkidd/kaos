#ifndef KAOS_H
#define KAOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { AMIGA_BASE = 0x24000, AMIGA_WIDTH = 384, AMIGA_HEIGHT = 278 };
enum { MUSIC_ROWS = 64, PAL_TICKS_PER_ROW = 6, PAL_HZ = 50 };

typedef struct {
    uint8_t *part;
    size_t part_size;
    const uint8_t *module;
    size_t module_size;
} Assets;

bool assets_load(Assets *assets);
void assets_free(Assets *assets);
static inline const uint8_t *amiga(const Assets *assets, unsigned address)
{
    return assets->part + (address - AMIGA_BASE);
}

static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)be16(p) << 16) | be16(p + 2);
}

int16_t signed_word(const uint8_t *p);

typedef enum { LINES, LOGO, BABY, NOISE, DANCER, SPEECH, DOTS, TITLE, RASTER } Scene;

typedef struct {
    Scene scene;
    unsigned age;
    unsigned line_style, tile_mode, line_phase[4];
    unsigned image, tint, speech_phase[3];
    float bar[4];
} Frame;

typedef struct {
    Frame *frames;
    size_t count;
} Timeline;

bool timeline_build(Timeline *timeline, const Assets *assets);

bool renderer_init(const Assets *assets, bool classic);
bool renderer_resize(int width, int height);
void renderer_draw(const Frame *frame, const Frame *next, float mix);
void renderer_free(void);

#endif
