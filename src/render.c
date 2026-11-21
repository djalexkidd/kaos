#define GL_GLEXT_PROTOTYPES
#include <SDL_opengl.h>

#include "kaos.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    SCREEN_X = 32, SCREEN_Y = 16,
    POINT_WIDTH = 5, POINT_HEIGHT = 5,
    LINE_TABLE = 0x2b706, CHANGE_RECORD = 0x2472a,
    DANCE_COLORS = 0x2840a, NOISE_PALETTE = 0x25170,
    MAX_VERTICES = 127 * 16 * 6
};

typedef struct {
    GLuint id;
    int width, height;
} Picture;

typedef struct { float x, y, red, green; } Vertex;
typedef struct { float x, y; } Point;

static struct {
    const Assets *assets;
    bool classic;
    GLuint mask_program, present_program, vbo, vao, fbo, mask;
    GLint ink_picture, ink_textured, palette, raster, bar, copper_bars;
    int width, height, view_x, view_y, view_width, view_height;
    int mask_width, mask_height;
    Picture logo, baby, dance[8], titles[4], noise[5], dot;
    Vertex vertices[MAX_VERTICES];
    size_t used;
} gpu;

static const char vertex_source[] =
    "#version 330 core\n"
    "layout(location=0) in vec2 point;\n"
    "layout(location=1) in vec2 value;\n"
    "out vec2 xy; out vec2 v;\n"
    "void main() {\n"
    "  xy=point; v=value;\n"
    "  gl_Position=vec4(point*vec2(2.0/384.0,-2.0/278.0)+vec2(-1,1),0,1);\n"
    "}\n";

static const char mask_source[] =
    "#version 330 core\n"
    "in vec2 v; out vec2 ink;\n"
    "uniform sampler2D picture; uniform bool textured;\n"
    "void main() { ink=textured ? texture(picture,v).rg : v; }\n";

static const char present_source[] =
    "#version 330 core\n"
    "in vec2 xy; in vec2 v; out vec4 pixel;\n"
    "uniform sampler2D mask; uniform vec3 colors[4];\n"
    "uniform vec4 bars; uniform vec3 ramp[20]; uniform bool copper_bars, classic;\n"
    "void main() {\n"
    "  vec2 bits=texture(mask,vec2(v.x,1-v.y)).rg;\n"
    "  int index=int(bits.r>0.5)+2*int(bits.g>0.5);\n"
    "  vec3 rgb=colors[index];\n"
    "  if (copper_bars && index==0) {\n"
    "    float x=classic ? floor(xy.x)+0.5 : xy.x;\n"
    "    int y=int(floor(xy.y))-82;\n"
    "    int ch=y/36; int line=y%36;\n"
    "    if (y>=0 && ch<4 && line<20) {\n"
    "      float lo=mix(107,11,bars[ch]);\n"
    "      float hi=mix(267,363,bars[ch]);\n"
    "      if (x>=lo && x<hi) {\n"
    "        vec3 band=ramp[line];\n"
    "        if (x>=lo+32 && x<hi-32) {\n"
    "          if (ch==0) band.gb=vec2(0);\n"
    "          if (ch==1) band.rb=vec2(0);\n"
    "          if (ch==2) band.rg=vec2(0);\n"
    "          if (ch==3) band.b=0;\n"
    "        }\n"
    "        rgb=band;\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  pixel=vec4(rgb,1);\n"
    "}\n";

static GLuint compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Compilation GLSL : %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint program(const char *fragment_source)
{
    GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
    GLuint result = 0;
    if (vertex && fragment) {
        result = glCreateProgram();
        glAttachShader(result, vertex);
        glAttachShader(result, fragment);
        glLinkProgram(result);
        GLint ok = 0;
        glGetProgramiv(result, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(result, sizeof(log), NULL, log);
            fprintf(stderr, "Édition de liens GLSL : %s\n", log);
            glDeleteProgram(result);
            result = 0;
        }
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return result;
}

static void nearest_texture(GLuint id)
{
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool in_part(unsigned address, size_t length)
{
    return address >= AMIGA_BASE && length <= gpu.assets->part_size &&
           address - AMIGA_BASE <= gpu.assets->part_size - length;
}

static bool load_picture(Picture *picture, unsigned address, unsigned plane2,
                         int width, int height, int stride)
{
    size_t length = (size_t)stride * height;
    if (!in_part(address, length) || (plane2 && !in_part(plane2, length)))
        return false;
    const uint8_t *first = amiga(gpu.assets, address);
    const uint8_t *second = plane2 ? amiga(gpu.assets, plane2) : NULL;
    uint8_t *pixels = malloc((size_t)width * height * 2);
    if (!pixels)
        return false;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int byte = y * stride + x / 8;
            unsigned bit = 0x80u >> (x & 7);
            size_t out = ((size_t)y * width + x) * 2;
            pixels[out] = (first[byte] & bit) ? 255 : 0;
            pixels[out + 1] = (second && (second[byte] & bit)) ? 255 : 0;
        }
    }

    picture->width = width;
    picture->height = height;
    glGenTextures(1, &picture->id);
    nearest_texture(picture->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width, height, 0,
                 GL_RG, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
    return glGetError() == GL_NO_ERROR;
}

static void color12(float color[3], unsigned bits)
{
    color[0] = ((bits >> 8) & 15) / 15.0f;
    color[1] = ((bits >> 4) & 15) / 15.0f;
    color[2] = (bits & 15) / 15.0f;
}

bool renderer_init(const Assets *assets, bool classic)
{
    gpu.assets = assets;
    gpu.classic = classic;
    gpu.mask_program = program(mask_source);
    gpu.present_program = program(present_source);
    if (!gpu.mask_program || !gpu.present_program)
        return false;

    glGenVertexArrays(1, &gpu.vao);
    glBindVertexArray(gpu.vao);
    glGenBuffers(1, &gpu.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (const void *)offsetof(Vertex, red));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    gpu.ink_picture = glGetUniformLocation(gpu.mask_program, "picture");
    gpu.ink_textured = glGetUniformLocation(gpu.mask_program, "textured");
    gpu.palette = glGetUniformLocation(gpu.present_program, "colors");
    gpu.raster = glGetUniformLocation(gpu.present_program, "ramp");
    gpu.bar = glGetUniformLocation(gpu.present_program, "bars");
    gpu.copper_bars = glGetUniformLocation(gpu.present_program, "copper_bars");
    glUseProgram(gpu.present_program);
    glUniform1i(glGetUniformLocation(gpu.present_program, "mask"), 0);
    glUniform1i(glGetUniformLocation(gpu.present_program, "classic"), classic);
    float ramp[20][3];
    for (unsigned i = 0; i < 20; ++i)
        color12(ramp[i], be16(amiga(assets, NOISE_PALETTE + i * 2)));
    glUniform3fv(gpu.raster, 20, &ramp[0][0]);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenFramebuffers(1, &gpu.fbo);
    glGenTextures(1, &gpu.mask);
    if (!load_picture(&gpu.logo, 0x45ab6, 0, 320, 256, 40) ||
        !load_picture(&gpu.baby, 0x482b6, 0, 320, 256, 40) ||
        !load_picture(&gpu.dot, 0x26db4, 0x26dc8,
                      POINT_WIDTH, POINT_HEIGHT, 4))
        return false;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned address = be32(amiga(assets, 0x28426 + i * 4));
        if (!load_picture(&gpu.dance[i], address, 0, 128, 165, 16))
            return false;
    }
    for (unsigned i = 0; i < 4; ++i) {
        unsigned address = be32(amiga(assets, 0x28b56 + i * 4));
        if (!load_picture(&gpu.titles[i], address, 0, 320, 140, 40))
            return false;
    }
    for (unsigned i = 0; i < 5; ++i) {
        unsigned address = be32(amiga(assets, 0x28b6e + i * 16));
        if (!load_picture(&gpu.noise[i], address, address + 12760, 352, 278, 44))
            return false;
    }
    return true;
}

bool renderer_resize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    if (width == gpu.width && height == gpu.height)
        return true;

    gpu.width = width;
    gpu.height = height;
    int aspect_width = gpu.classic ? AMIGA_WIDTH : 4;
    int aspect_height = gpu.classic ? AMIGA_HEIGHT : 3;
    gpu.view_width = width;
    gpu.view_height = height;
    if ((int64_t)width * aspect_height > (int64_t)height * aspect_width)
        gpu.view_width = (int)((int64_t)height * aspect_width / aspect_height);
    else
        gpu.view_height = (int)((int64_t)width * aspect_height / aspect_width);
    if (!gpu.view_width || !gpu.view_height)
        return false;
    gpu.view_x = (width - gpu.view_width) / 2;
    gpu.view_y = (height - gpu.view_height) / 2;

    int mask_width = gpu.classic ? AMIGA_WIDTH : gpu.view_width;
    int mask_height = gpu.classic ? AMIGA_HEIGHT : gpu.view_height;
    if (mask_width == gpu.mask_width && mask_height == gpu.mask_height)
        return true;
    nearest_texture(gpu.mask);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, mask_width, mask_height,
                 0, GL_RG, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, gpu.mask, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (ok) {
        gpu.mask_width = mask_width;
        gpu.mask_height = mask_height;
    }
    return ok;
}

static void quad_vertices(Vertex out[6], float x, float y, float w, float h)
{
    out[0] = (Vertex){x, y, 0, 0};
    out[1] = (Vertex){x + w, y, 1, 0};
    out[2] = (Vertex){x, y + h, 0, 1};
    out[3] = out[2];
    out[4] = out[1];
    out[5] = (Vertex){x + w, y + h, 1, 1};
}

static void quad(float x, float y, float w, float h)
{
    Vertex vertices[6];
    quad_vertices(vertices, x, y, w, h);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void picture(const Picture *image, float x, float y)
{
    glUniform1i(gpu.ink_textured, 1);
    glBindTexture(GL_TEXTURE_2D, image->id);
    quad(x, y, image->width, image->height);
}

static void line(Point a, Point b, unsigned ink)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float length = sqrtf(dx * dx + dy * dy);
    if (length < 0.01f || gpu.used + 6 > MAX_VERTICES)
        return;

    float nx = -0.56f * dy / length, ny = 0.56f * dx / length;
    float r = ink == 0, g = ink == 1;
    Vertex aa = {a.x + nx, a.y + ny, r, g}, ab = {a.x - nx, a.y - ny, r, g};
    Vertex ba = {b.x + nx, b.y + ny, r, g}, bb = {b.x - nx, b.y - ny, r, g};
    Vertex *out = gpu.vertices + gpu.used;
    out[0] = aa; out[1] = ab; out[2] = ba;
    out[3] = ba; out[4] = ab; out[5] = bb;
    gpu.used += 6;
}

static void draw_lines(void)
{
    glUniform1i(gpu.ink_textured, 0);
    glBufferData(GL_ARRAY_BUFFER, gpu.used * sizeof(Vertex),
                 gpu.vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)gpu.used);
    gpu.used = 0;
}

static uint8_t phase_byte(unsigned start, unsigned step, int point)
{
    return (uint8_t)(start + step * (unsigned)point);
}

/* The inner lookup selects a row in the original 128-by-256 wave table. */
static Point ribbon_point(const Frame *frame, unsigned next_step, int point)
{
    const uint8_t *wave = amiga(gpu.assets, LINE_TABLE);
    const uint8_t *style = amiga(gpu.assets, CHANGE_RECORD + frame->line_style * 12);
    unsigned phase[4];
    for (unsigned k = 0; k < 4; ++k) {
        unsigned initial = k == 1 ? 64 : 0;
        phase[k] = phase_byte(initial + frame->line_phase[k] + next_step * style[k],
                              style[4 + k], point);
    }
    unsigned x = wave[((wave[phase[2]] >> 1) << 8) + phase[0]];
    unsigned y = wave[((wave[phase[3]] >> 1) << 8) + phase[1]];
    unsigned shift = frame->tile_mode ? 3 - frame->tile_mode : 0;
    return (Point){(float)(x >> shift), (float)(y >> shift)};
}

static void ribbon(const Frame *frame, float mix)
{
    Point points[128];
    for (int i = 0; i < 128; ++i) {
        Point a = ribbon_point(frame, 0, i);
        Point b = ribbon_point(frame, 1, i);
        points[i] = (Point){a.x + (b.x - a.x) * mix, a.y + (b.y - a.y) * mix};
    }
    int stride = frame->tile_mode ? 2 : 1;
    int tile_count = frame->tile_mode == 1 ? 4 : frame->tile_mode == 2 ? 2 : 1;
    int tile_size = 256 / tile_count;
    for (int ty = 0; ty < tile_count; ++ty) {
        for (int tx = 0; tx < tile_count; ++tx) {
            float x = SCREEN_X + 24 + tx * tile_size;
            float y = SCREEN_Y + ty * tile_size;
            for (int i = 0; i < 128; i += 2 * stride) {
                Point a = {points[i].x + x, points[i].y + y};
                Point b = {points[i + 1].x + x, points[i + 1].y + y};
                line(a, b, (unsigned)(i / (2 * stride)) & 1);
            }
        }
    }
    draw_lines();
}

static float wave16(unsigned base, float phase)
{
    unsigned i = (unsigned)phase;
    float fraction = phase - i;
    const uint8_t *table = amiga(gpu.assets, base);
    float a = signed_word(table + (i & 511) * 2);
    float b = signed_word(table + ((i + 1) & 511) * 2);
    return (a + (b - a) * fraction) / 16384.0f;
}

/* Rotate the original KAOS vertices in X, Y and Z before the Amiga projection. */
static void speech(const Frame *frame, const Frame *next, float mix)
{
    float s[3], c[3];
    for (unsigned i = 0; i < 3; ++i) {
        unsigned delta = next ? (next->speech_phase[i] - frame->speech_phase[i]) & 511 : 0;
        float phase = frame->speech_phase[i] + delta * mix;
        s[i] = wave16(0x27c06, phase);
        c[i] = wave16(0x28006, phase);
    }

    Point prior = {0};
    const uint8_t *vertices = amiga(gpu.assets, 0x27b4e);
    for (const uint8_t *v = vertices; v < vertices + 22 * 8 && be16(v) != 555; v += 8) {
        float y = signed_word(v + 2), x = signed_word(v + 4), z = signed_word(v + 6);
        float x1 = x * c[0] - z * s[0], z1 = x * s[0] + z * c[0];
        float y2 = y * c[1] - z1 * s[1], z2 = y * s[1] + z1 * c[1];
        float x3 = y2 * c[2] - x1 * s[2], z3 = y2 * s[2] + x1 * c[2];
        float scale = 1.0f / (300.0f + z3);
        Point p = {
            SCREEN_X + 160.0f + 250.0f * x3 * scale,
            SCREEN_Y + 100.0f + (200.0f - z3) * (60.0f - z2) * scale * 0.5f
                - z2 * 0.5f
        };
        if (be16(v))
            line(prior, p, 0);
        prior = p;
    }
    draw_lines();
}

static void dots(const Frame *frame, float mix)
{
    const uint8_t *pointer = amiga(gpu.assets, 0x255a0 + frame->image * 4);
    unsigned address = be32(pointer);
    if (!in_part(address, 0x800))
        return;
    const uint8_t *start = amiga(gpu.assets, address);
    const uint8_t *end = start;
    while (end < start + 0x800 && be32(end) != UINT32_C(0x12345678))
        end += 4;

    size_t count = (size_t)(end - start) / 4;
    size_t shown = (size_t)(count * fminf(1.0f, (frame->age + mix + 1) / 5.0f));
    glUniform1i(gpu.ink_textured, 1);
    glBindTexture(GL_TEXTURE_2D, gpu.dot.id);
    for (size_t i = 0; i < shown && gpu.used + 6 <= MAX_VERTICES; ++i) {
        const uint8_t *p = start + i * 4;
        quad_vertices(gpu.vertices + gpu.used, SCREEN_X + be16(p + 2),
                      SCREEN_Y + be16(p), POINT_WIDTH, POINT_HEIGHT);
        gpu.used += 6;
    }
    glBufferData(GL_ARRAY_BUFFER, gpu.used * sizeof(Vertex), gpu.vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)gpu.used);
    gpu.used = 0;
}

void renderer_draw(const Frame *frame, const Frame *next, float mix)
{
    if (next && next->scene != frame->scene)
        next = NULL;
    glBindFramebuffer(GL_FRAMEBUFFER, gpu.fbo);
    glViewport(0, 0, gpu.mask_width, gpu.mask_height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(gpu.mask_program);
    glUniform1i(gpu.ink_picture, 0);
    glEnable(GL_BLEND);
    glBlendEquation(GL_MAX);

    unsigned colors[4] = {0, 0xfff, 0xfff, 0xfff};
    switch (frame->scene) {
    case LINES: {
        const uint8_t *style = amiga(gpu.assets, CHANGE_RECORD + frame->line_style * 12);
        colors[1] = be16(style + 8);
        colors[2] = be16(style + 10);
        ribbon(frame, next && next->line_style == frame->line_style ? mix : 0);
        break;
    }
    case LOGO: case BABY: case DANCER:
        colors[1] = be16(amiga(gpu.assets, DANCE_COLORS + frame->tint * 2));
        if (frame->scene == DANCER)
            picture(&gpu.dance[frame->image], SCREEN_X + 112, SCREEN_Y + 60);
        else
            picture(frame->scene == LOGO ? &gpu.logo : &gpu.baby, SCREEN_X, SCREEN_Y);
        break;
    case NOISE:
        colors[2] = 0xbbb;
        colors[3] = 0x666;
        picture(&gpu.noise[frame->image / 4], 16, 0);
        break;
    case SPEECH:
        colors[1] = be16(amiga(gpu.assets, CHANGE_RECORD + frame->line_style * 12 + 8));
        speech(frame, next, mix);
        break;
    case DOTS:
        for (unsigned i = 1; i < 4; ++i)
            colors[i] = be16(amiga(gpu.assets, 0x25498 + frame->tint * 6 + (i - 1) * 2));
        dots(frame, mix);
        break;
    case TITLE:
        picture(&gpu.titles[frame->image], SCREEN_X, SCREEN_Y + 70);
        break;
    case RASTER:
        break;
    }

    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, gpu.width, gpu.height);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(gpu.view_x, gpu.view_y, gpu.view_width, gpu.view_height);
    glUseProgram(gpu.present_program);
    float rgb[4][3], bars[4];
    for (unsigned i = 0; i < 4; ++i) {
        color12(rgb[i], colors[i]);
        bars[i] = frame->bar[i] + (next ? (next->bar[i] - frame->bar[i]) * mix : 0);
    }
    glUniform3fv(gpu.palette, 4, &rgb[0][0]);
    glUniform4fv(gpu.bar, 1, bars);
    glUniform1i(gpu.copper_bars, frame->scene == RASTER);
    glBindTexture(GL_TEXTURE_2D, gpu.mask);
    quad(0, 0, AMIGA_WIDTH, AMIGA_HEIGHT);
}

void renderer_free(void)
{
    glDeleteTextures(1, &gpu.logo.id);
    glDeleteTextures(1, &gpu.baby.id);
    glDeleteTextures(1, &gpu.dot.id);
    for (unsigned i = 0; i < 8; ++i)
        glDeleteTextures(1, &gpu.dance[i].id);
    for (unsigned i = 0; i < 4; ++i)
        glDeleteTextures(1, &gpu.titles[i].id);
    for (unsigned i = 0; i < 5; ++i)
        glDeleteTextures(1, &gpu.noise[i].id);
    glDeleteTextures(1, &gpu.mask);
    glDeleteFramebuffers(1, &gpu.fbo);
    glDeleteBuffers(1, &gpu.vbo);
    glDeleteVertexArrays(1, &gpu.vao);
    glDeleteProgram(gpu.mask_program);
    glDeleteProgram(gpu.present_program);
}
