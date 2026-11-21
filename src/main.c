#include <SDL.h>
#include <SDL_opengl.h>
#include <libopenmpt/libopenmpt.h>

#include "kaos.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int width, height;
    double start;
    bool fullscreen, uncapped, mute, benchmark, classic;
} Options;

typedef struct {
    SDL_AudioDeviceID device;
    openmpt_module *module;
    SDL_AudioSpec spec;
    uint64_t samples, callback_time;
    unsigned callback_frames;
    uint64_t frequency;
} Audio;

static void usage(FILE *out, const char *name)
{
    fprintf(out,
        "Usage : %s [--size LARGEURxHAUTEUR]\n"
        "             [--fullscreen] [--start SECONDES] [--uncapped] [--mute]\n"
        "             [--classic] [--benchmark]\n"
        "--classic affiche le cadrage PAL (384x278, 50 images/s).\n"
        "Échap quitte. F11 bascule le plein écran.\n", name);
}

static bool size_option(const char *text, int *width, int *height)
{
    char *end;
    long w = strtol(text, &end, 10);
    if (end == text || *end != 'x')
        return false;
    const char *second = end + 1;
    long h = strtol(second, &end, 10);
    if (end == second || *end || w < 320 || w > 8192 || h < 240 || h > 8192)
        return false;
    *width = (int)w;
    *height = (int)h;
    return true;
}

static bool start_option(const char *text, double *seconds)
{
    char *end;
    errno = 0;
    double value = strtod(text, &end);
    if (errno || end == text || *end || !isfinite(value) || value < 0)
        return false;
    *seconds = value;
    return true;
}

static bool options_read(Options *options, int argc, char **argv)
{
    *options = (Options){0};
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--fullscreen"))
            options->fullscreen = true;
        else if (!strcmp(arg, "--uncapped"))
            options->uncapped = true;
        else if (!strcmp(arg, "--mute"))
            options->mute = true;
        else if (!strcmp(arg, "--benchmark"))
            options->benchmark = true;
        else if (!strcmp(arg, "--classic"))
            options->classic = true;
        else if (!strcmp(arg, "--size") && i + 1 < argc) {
            if (!size_option(argv[++i], &options->width, &options->height))
                return false;
        } else if (!strcmp(arg, "--start") && i + 1 < argc) {
            if (!start_option(argv[++i], &options->start))
                return false;
        } else {
            return false;
        }
    }
    if (!options->width) {
        options->width = options->classic ? AMIGA_WIDTH : 1920;
        options->height = options->classic ? AMIGA_HEIGHT : 1080;
    }
    return true;
}

static void audio_callback(void *userdata, Uint8 *stream, int length)
{
    Audio *audio = userdata;
    unsigned frames = (unsigned)length / (2 * sizeof(int16_t));
    uint64_t now = SDL_GetPerformanceCounter();
    size_t rendered = openmpt_module_read_interleaved_stereo(
        audio->module, audio->spec.freq, frames, (int16_t *)stream);
    if (rendered < frames) {
        size_t bytes = rendered * 2 * sizeof(int16_t);
        memset(stream + bytes, 0, (size_t)length - bytes);
    }
    audio->callback_time = now;
    audio->callback_frames = frames;
    audio->samples += rendered;
}

static bool audio_open(Audio *audio, const Assets *assets, double start)
{
    const openmpt_module_initial_ctl controls[] = {
        {"seek.sync_samples", "1"}, {NULL, NULL}
    };
    int error = 0;
    const char *message = NULL;
    audio->module = openmpt_module_create_from_memory2(
        assets->module, assets->module_size, openmpt_log_func_silent, NULL,
        NULL, NULL, &error, &message, controls);
    if (!audio->module)
        fprintf(stderr, "Lecture du module (%d) : %s\n", error, message ? message : "");
    if (message)
        openmpt_free_string(message);
    if (!audio->module)
        return false;

    openmpt_module_set_repeat_count(audio->module, -1);
    openmpt_module_set_render_param(audio->module, OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH, 1);
    SDL_AudioSpec wanted = {.freq = 48000, .format = AUDIO_S16SYS,
                            .channels = 2, .samples = 512,
                            .callback = audio_callback, .userdata = audio};
    audio->device = SDL_OpenAudioDevice(NULL, 0, &wanted, &audio->spec,
                                        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!audio->device) {
        fprintf(stderr, "Audio SDL : %s\n", SDL_GetError());
        return false;
    }

    /* libopenmpt seeks by tracker row. Decode up to the requested sample. */
    const double row_seconds = (double)PAL_TICKS_PER_ROW / PAL_HZ;
    double row = floor(start / row_seconds) * row_seconds;
    double position = openmpt_module_set_position_seconds(audio->module, fmax(0.0, row - 0.000001));
    audio->samples = (uint64_t)llround(position * audio->spec.freq);
    uint64_t target = (uint64_t)llround(start * audio->spec.freq);
    int16_t discard[512 * 2];
    while (audio->samples < target) {
        uint64_t remaining = target - audio->samples;
        size_t block = remaining < 512 ? (size_t)remaining : 512;
        size_t rendered = openmpt_module_read_interleaved_stereo(
            audio->module, audio->spec.freq, block, discard);
        if (!rendered) {
            fputs("Position de lecture KAOS inaccessible\n", stderr);
            return false;
        }
        audio->samples += rendered;
    }
    audio->frequency = SDL_GetPerformanceFrequency();
    return true;
}

static double audio_position(const Audio *audio)
{
    SDL_LockAudioDevice(audio->device);
    uint64_t last = audio->callback_time;
    unsigned frames = audio->callback_frames;
    uint64_t samples = audio->samples;
    SDL_UnlockAudioDevice(audio->device);

    double elapsed = last ? (double)(SDL_GetPerformanceCounter() - last) / audio->frequency : 0;
    double period = (double)frames / audio->spec.freq;
    return (double)samples / audio->spec.freq - period + fmin(elapsed, period);
}

static bool sync_window(SDL_Window *window, int *width, int *height)
{
    SDL_GL_GetDrawableSize(window, width, height);
    if (*width <= 0 || *height <= 0)
        return true;
    if (!renderer_resize(*width, *height)) {
        fprintf(stderr, "Création du tampon OpenGL impossible (%dx%d)\n", *width, *height);
        return false;
    }
    return true;
}

static void render_at(const Timeline *timeline, double tick)
{
    tick = fmod(tick, (double)timeline->count);
    size_t index = (size_t)tick;
    const Frame *next = index + 1 < timeline->count ? timeline->frames + index + 1 : NULL;
    renderer_draw(timeline->frames + index, next, (float)(tick - index));
}

static bool play(SDL_Window *window, const Timeline *timeline, const Audio *audio,
                 const Options *options, bool vsync)
{
    uint64_t frequency = SDL_GetPerformanceFrequency(), start = SDL_GetPerformanceCounter();
    bool fullscreen = options->fullscreen;
    uint64_t frame_period = frequency / PAL_HZ;
    uint64_t next_frame = start;
    if (audio->device)
        SDL_PauseAudioDevice(audio->device, 0);

    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                return true;
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.sym == SDLK_ESCAPE)
                    return true;
                if (event.key.keysym.sym == SDLK_F11) {
                    if (SDL_SetWindowFullscreen(window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) == 0)
                        fullscreen = !fullscreen;
                    else
                        fprintf(stderr, "Plein écran : %s\n", SDL_GetError());
                }
            }
        }

        int width, height;
        if (!sync_window(window, &width, &height))
            return false;
        if (width <= 0 || height <= 0) {
            SDL_Delay(16);
            continue;
        }
        uint64_t now = SDL_GetPerformanceCounter();
        if (options->classic) {
            /* Keep the display cadence independent of the audio device clock. */
            if (now < next_frame) {
                double remaining_ms = (double)(next_frame - now) * 1000.0 / frequency;
                SDL_Delay((Uint32)fmax(1.0, remaining_ms - 1.0));
                continue;
            }
            next_frame += ((now - next_frame) / frame_period + 1) * frame_period;
        }
        double time = audio->device ? audio_position(audio)
            : options->start + (double)(now - start) / frequency;
        double tick = time * PAL_HZ;
        render_at(timeline, options->classic ? floor(tick) : tick);
        SDL_GL_SwapWindow(window);
        if (!options->classic && !vsync && !options->uncapped)
            SDL_Delay(1);
    }
}

static bool benchmark(SDL_Window *window, const Timeline *timeline,
                      const Options *options)
{
    int width, height;
    if (!sync_window(window, &width, &height) || width <= 0 || height <= 0)
        return false;
    const size_t frames = 1200;
    const double step = (double)timeline->count / frames;
    double first_tick = options->start * PAL_HZ;
    render_at(timeline, options->classic ? floor(first_tick) : first_tick);
    glFinish();

    uint64_t start = SDL_GetPerformanceCounter();
    for (size_t i = 0; i < frames; ++i) {
        double tick = first_tick + i * step;
        render_at(timeline, options->classic ? floor(tick) : tick);
        glFinish();
        SDL_GL_SwapWindow(window);
    }
    double seconds = (double)(SDL_GetPerformanceCounter() - start) /
                     SDL_GetPerformanceFrequency();
    printf("OpenGL %s\n", (const char *)glGetString(GL_RENDERER));
    printf("%dx%d, %zu images, %.2f s, %.1f FPS\n", width, height, frames,
           seconds, frames / seconds);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        fprintf(stderr, "Erreur OpenGL 0x%x\n", error);
    return error == GL_NO_ERROR;
}

int main(int argc, char **argv)
{
    Options options;
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (!options_read(&options, argc, argv)) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    Assets assets = {0};
    Timeline timeline = {0};
    Audio audio = {0};
    SDL_Window *window = NULL;
    SDL_GLContext context = NULL;
    bool success = false;

    if (!assets_load(&assets)) {
        fputs("Chargement des assets intégrés impossible\n", stderr);
        goto done;
    }
    if (!timeline_build(&timeline, &assets)) {
        fputs("Construction de la chronologie KAOS impossible\n", stderr);
        goto done;
    }
    double duration = (double)timeline.count / PAL_HZ;
    if (options.start >= duration) {
        fprintf(stderr, "--start doit être inférieur à %.2f secondes\n", duration);
        goto done;
    }

    bool sound = !options.mute && !options.benchmark;
    if (SDL_Init(SDL_INIT_VIDEO | (sound ? SDL_INIT_AUDIO : 0)) != 0) {
        fprintf(stderr, "Initialisation SDL : %s\n", SDL_GetError());
        goto done;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    if (!options.classic)
        flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    if (options.benchmark)
        flags |= SDL_WINDOW_HIDDEN;
    if (options.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window = SDL_CreateWindow("KAOS · Budbrain", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, options.width, options.height, flags);
    if (!window || !(context = SDL_GL_CreateContext(window))) {
        fprintf(stderr, "Fenêtre OpenGL 3.3 : %s\n", SDL_GetError());
        goto done;
    }

    bool vsync = !options.classic && !options.uncapped && !options.benchmark &&
                 SDL_GL_SetSwapInterval(1) == 0;
    if (!vsync)
        SDL_GL_SetSwapInterval(0);
    if (!renderer_init(&assets, options.classic)) {
        fputs("Initialisation des textures KAOS impossible\n", stderr);
        goto done;
    }
    if (sound && !audio_open(&audio, &assets, options.start))
        goto done;
    success = options.benchmark ? benchmark(window, &timeline, &options)
                                : play(window, &timeline, &audio, &options, vsync);

done:
    if (audio.device)
        SDL_CloseAudioDevice(audio.device);
    if (audio.module)
        openmpt_module_destroy(audio.module);
    if (context) {
        renderer_free();
        SDL_GL_DeleteContext(context);
    }
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
    free(timeline.frames);
    assets_free(&assets);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
