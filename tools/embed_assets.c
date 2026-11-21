#include "kaos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ADF_SIZE = 80 * 2 * 11 * 512,
    KAOS_OFFSET = 44 * 2 * 11 * 512,
    KAOS_PACKED_SIZE = 0x2eb94,
    KAOS_SIZE = 0x53336,
    EMBEDDED_MODULE = 0x2c236,
    MODULE_SIZE = 150474
};

static uint8_t *read_file(const char *path, size_t size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return NULL;
    }
    uint8_t *data = malloc(size);
    if (!data || fread(data, 1, size, file) != size || fgetc(file) != EOF ||
        ferror(file)) {
        fprintf(stderr, "%s : taille ou lecture incorrecte\n", path);
        free(data);
        data = NULL;
    }
    fclose(file);
    return data;
}

typedef struct {
    const uint8_t *start, *cursor;
    uint32_t word;
    bool failed;
} BitStream;

/* The disk loader reads words backwards and uses a high sentinel bit. */
static unsigned bits(BitStream *stream, unsigned count)
{
    unsigned result = 0;
    while (count--) {
        unsigned bit = stream->word & 1;
        stream->word >>= 1;
        if (!stream->word) {
            if (stream->cursor == stream->start) {
                stream->failed = true;
                return 0;
            }
            stream->cursor -= 4;
            uint32_t refill = be32(stream->cursor);
            bit = refill & 1;
            stream->word = (refill >> 1) | UINT32_C(0x80000000);
        }
        result = (result << 1) | bit;
    }
    return result;
}

static bool unpack(const uint8_t *block, uint8_t *output)
{
    if (be32(block) != KAOS_PACKED_SIZE || be32(block + 4) != KAOS_SIZE)
        return false;

    const uint8_t *start = block + 12;
    uint32_t checksum = be32(block + 8);
    for (size_t i = 0; i < KAOS_PACKED_SIZE; i += 4)
        checksum ^= be32(start + i);
    if (checksum)
        return false;

    BitStream stream = {start, start + KAOS_PACKED_SIZE - 4, 0, false};
    stream.word = be32(stream.cursor);
    size_t remaining = KAOS_SIZE;
    while (remaining) {
        unsigned count, distance = 0;
        bool literal = false;
        if (!bits(&stream, 1)) {
            literal = !bits(&stream, 1);
            count = literal ? 1 + bits(&stream, 3) : 2;
            if (!literal)
                distance = bits(&stream, 8);
        } else {
            unsigned code = bits(&stream, 2);
            literal = code == 3;
            if (literal)
                count = 9 + bits(&stream, 8);
            else if (code == 2) {
                count = 1 + bits(&stream, 8);
                distance = bits(&stream, 12);
            } else {
                count = 3 + code;
                distance = bits(&stream, 9 + code);
            }
        }
        if (stream.failed || count > remaining ||
            (!literal && (!distance || distance > KAOS_SIZE - remaining)))
            return false;
        while (count--) {
            --remaining;
            output[remaining] = literal ? (uint8_t)bits(&stream, 8)
                                        : output[remaining + distance];
        }
        if (stream.failed)
            return false;
    }
    return stream.cursor == stream.start;
}

typedef struct {
    const uint8_t *part;
    uint8_t required[EMBEDDED_MODULE];
} Graphics;

static bool mark(Graphics *graphics, unsigned address, size_t length)
{
    if (address < AMIGA_BASE || length > EMBEDDED_MODULE ||
        address - AMIGA_BASE > EMBEDDED_MODULE - length)
        return false;
    memset(graphics->required + (address - AMIGA_BASE), 1, length);
    return true;
}

static bool mark_pictures(Graphics *graphics, unsigned table, unsigned count,
                          unsigned step, size_t length)
{
    if (!mark(graphics, table, (count - 1) * step + 4))
        return false;
    const uint8_t *pointers = graphics->part + (table - AMIGA_BASE);
    for (unsigned i = 0; i < count; ++i) {
        if (!mark(graphics, be32(pointers + i * step), length))
            return false;
    }
    return true;
}

static bool mark_points(Graphics *graphics)
{
    const unsigned table = 0x255a0;
    if (!mark(graphics, table, 3 * 4))
        return false;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned address = be32(graphics->part + (table - AMIGA_BASE) + i * 4);
        if (address < AMIGA_BASE || address - AMIGA_BASE > EMBEDDED_MODULE - 0x800)
            return false;
        const uint8_t *points = graphics->part + (address - AMIGA_BASE);
        size_t length = 0;
        while (length < 0x800 && be32(points + length) != UINT32_C(0x12345678))
            length += 4;
        if (length == 0x800 || !mark(graphics, address, length + 4))
            return false;
    }
    return true;
}

static bool mark_graphics(Graphics *graphics)
{
    return mark(graphics, 0x2472a, 33 * 12) && /* Ribbon styles and palettes. */
           mark(graphics, 0x25170, 20 * 2) &&  /* Copper color ramp. */
           mark(graphics, 0x25498, 4 * 6) &&   /* Point drawing palettes. */
           mark_points(graphics) &&
           mark(graphics, 0x26db4, 40) &&      /* Two bitplanes of the point stamp. */
           mark(graphics, 0x27b4e, 22 * 8) &&  /* Wireframe vertices. */
           mark(graphics, 0x27c06, 2 * 512 * 2) && /* Sine and cosine. */
           mark(graphics, 0x2840a, 7 * 2) &&   /* Dancer, logo and baby colors. */
           mark(graphics, 0x2b706, 128 * 256) && /* Ribbon wave table. */
           mark(graphics, 0x45ab6, 40 * 256) && /* KAOS logo. */
           mark(graphics, 0x482b6, 40 * 256) && /* Baby. */
           mark_pictures(graphics, 0x28426, 8, 4, 16 * 165) && /* Dancer. */
           mark_pictures(graphics, 0x28b56, 4, 4, 40 * 140) && /* Title cards. */
           mark_pictures(graphics, 0x28b6e, 5, 16, 12760 + 44 * 278); /* TV noise. */
}

static void write_byte(uint8_t byte, size_t index)
{
    if (index % 16 == 0)
        fputs("\n   ", stdout);
    printf(" 0x%02x,", (unsigned)byte);
}

static bool emit(const uint8_t *part, const uint8_t *music)
{
    static Graphics graphics;
    graphics.part = part;
    if (!mark_graphics(&graphics)) {
        fputs("Pointeur graphique KAOS hors limites\n", stderr);
        return false;
    }

    printf("/* Generated by embed-assets. */\n"
           "enum { embedded_part_size = %u };\n"
           "static const uint8_t embedded_graphics[] = {", (unsigned)EMBEDDED_MODULE);
    size_t index = 0;
    for (size_t i = 0; i < sizeof graphics.required; ++i) {
        if (graphics.required[i])
            write_byte(part[i], index++);
    }

    fputs("\n};\nstatic const struct { size_t offset, length; } embedded_ranges[] = {\n", stdout);
    for (size_t i = 0; i < sizeof graphics.required;) {
        if (!graphics.required[i]) {
            ++i;
            continue;
        }
        size_t start = i++;
        while (i < sizeof graphics.required && graphics.required[i])
            ++i;
        printf("    {0x%zx, %zu},\n", start, i - start);
    }

    fputs("};\nstatic const uint8_t embedded_music[] = {", stdout);
    for (size_t i = 0; i < MODULE_SIZE; ++i)
        write_byte(music[i], i);
    fputs("\n};\n", stdout);
    return fflush(stdout) == 0 && !ferror(stdout);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fputs("Usage : embed-assets ADF MOD\n", stderr);
        return EXIT_FAILURE;
    }

    uint8_t *disk = read_file(argv[1], ADF_SIZE);
    uint8_t *part = malloc(KAOS_SIZE);
    uint8_t *music = NULL;
    bool ok = false;
    if (!disk || !part)
        goto done;

    const uint8_t *original_music = part + EMBEDDED_MODULE;
    if (!unpack(disk + KAOS_OFFSET, part) || memcmp(original_music, "kaosmodule", 10) ||
        memcmp(original_music + 1080, "M.K.", 4) || original_music[950] != 19) {
        fprintf(stderr, "%s : bloc KAOS de l’ADF non reconnu\n", argv[1]);
        goto done;
    }

    music = read_file(argv[2], MODULE_SIZE);
    if (!music || memcmp(music, original_music, MODULE_SIZE)) {
        fprintf(stderr, "%s : le module doit correspondre à celui de KAOS\n", argv[2]);
        goto done;
    }
    ok = emit(part, music);

done:
    free(music);
    free(part);
    free(disk);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
