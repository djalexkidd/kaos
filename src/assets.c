#include "kaos.h"

#include <stdlib.h>
#include <string.h>

#include "embedded_assets.h"

int16_t signed_word(const uint8_t *p)
{
    int value = be16(p);
    return (int16_t)(value < 32768 ? value : value - 65536);
}

bool assets_load(Assets *assets)
{
    uint8_t *part = calloc(embedded_part_size, 1);
    if (!part)
        return false;

    const uint8_t *source = embedded_graphics;
    for (size_t i = 0; i < sizeof embedded_ranges / sizeof *embedded_ranges; ++i) {
        size_t length = embedded_ranges[i].length;
        memcpy(part + embedded_ranges[i].offset, source, length);
        source += length;
    }
    *assets = (Assets){
        .part = part, .part_size = embedded_part_size,
        .module = embedded_music, .module_size = sizeof embedded_music
    };
    return true;
}

void assets_free(Assets *assets)
{
    free(assets->part);
    *assets = (Assets){0};
}
