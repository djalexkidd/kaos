#include "kaos.h"

#include <stdlib.h>
#include <string.h>

enum {
    CHANGE_RECORD = 0x2472a, CHANGE_RECORD_BYTES = 12, CHANGE_RECORD_COUNT = 33,
    DANCE_COLORS = 0x2840a, DANCE_COLORS_COUNT = 7,
    NOISE_COUNT = 20, NOISE_INSTRUMENT = 15, DANCE_COUNT = 8, TITLES_COUNT = 4,
    LOGO_TICKS = 12, DANCE_STEP_TICKS = 6, SPEECH_TICKS = 40,
    DOTS_TICKS = 14, TITLE_TICKS = 13, RASTER_TICKS = 40, BAR_TICKS = 24
};

typedef struct {
    bool looping;
    bool audible;
} NoiseVoice;

typedef struct {
    Frame frame;
    const uint8_t *style, *first_style;
    unsigned style_count, line_changes;
    unsigned flash_index, title_index, points_index, noise_index, flash_color;
    unsigned bar_age[4];
    NoiseVoice noise[4];
    bool next_hit_is_title;
} Sequence;

static void change_lines(Sequence *seq)
{
    seq->frame.line_style = seq->style_count++;
    if (++seq->line_changes == 10) {
        seq->line_changes = 0;
        seq->frame.tile_mode = (seq->frame.tile_mode + 1) % 3;
    }
    memset(seq->frame.line_phase, 0, sizeof(seq->frame.line_phase));
    seq->style += CHANGE_RECORD_BYTES;
    if (seq->style_count == CHANGE_RECORD_COUNT ||
        be32(seq->style) == UINT32_C(0x12345678)) {
        seq->style = seq->first_style;
        seq->style_count = 0;
    }
}

static void set_scene(Sequence *seq, Scene scene)
{
    seq->frame.scene = scene;
    seq->frame.age = 0;
}

static void resume_lines(Sequence *seq)
{
    set_scene(seq, LINES);
    memset(seq->bar_age, 0, sizeof(seq->bar_age));
    change_lines(seq);
}

static int channel_with(const uint8_t commands[4], unsigned instrument, bool masked)
{
    for (int ch = 0; ch < 4; ++ch) {
        unsigned command = masked ? commands[ch] >> 4 : commands[ch];
        if (command == instrument)
            return ch;
    }
    return -1;
}

static void hit(Sequence *seq)
{
    memset(seq->bar_age, 0, sizeof(seq->bar_age));
    if (seq->next_hit_is_title) {
        set_scene(seq, TITLE);
        seq->frame.image = seq->title_index++ % TITLES_COUNT;
    } else {
        set_scene(seq, DOTS);
        seq->frame.image = seq->points_index;
        seq->points_index = (seq->points_index + 1) % 3;
        seq->frame.tint = seq->flash_color++ % 4;
    }
    seq->next_hit_is_title = !seq->next_hit_is_title;
}

static bool noise_audible(NoiseVoice voices[4], const uint8_t *notes)
{
    bool audible = false;
    for (unsigned ch = 0; ch < 4; ++ch) {
        const uint8_t *note = notes + ch * 4;
        NoiseVoice *voice = &voices[ch];
        unsigned instrument = (note[0] & 0xf0) | (note[2] >> 4);

        /* DD-TVNOISE loops until the next note on its channel. Cxx changes
           the volume of that loop, including on rows with no new note. */
        if ((note[0] & 0x0f) || note[1]) {
            voice->looping = instrument == NOISE_INSTRUMENT;
            voice->audible = voice->looping;
        }
        if ((note[2] & 0x0f) == 0x0c)
            voice->audible = note[3] != 0;
        audible |= voice->looping && voice->audible;
    }
    return audible;
}

static void trigger(Sequence *seq, const uint8_t *notes,
                    const uint8_t commands[4], unsigned order)
{
    if (noise_audible(seq->noise, notes)) {
        if (seq->frame.scene != NOISE) {
            set_scene(seq, NOISE);
            memset(seq->bar_age, 0, sizeof(seq->bar_age));
        }
        return;
    }
    if (seq->frame.scene == NOISE)
        resume_lines(seq);
    if (seq->frame.scene == DOTS || seq->frame.scene == TITLE ||
        seq->frame.scene == LOGO || seq->frame.scene == BABY)
        return;
    if (seq->frame.scene != LINES) {
        if (channel_with(commands, 9, true) >= 0)
            hit(seq);
        return;
    }

    if (channel_with(commands, 0x70, false) >= 0) {
        set_scene(seq, LOGO);
    } else if (channel_with(commands, 0x80, false) >= 0) {
        set_scene(seq, BABY);
    } else if (channel_with(commands, 0x90, false) >= 0) {
        hit(seq);
    } else if (channel_with(commands, 0xa0, false) >= 0) {
        set_scene(seq, SPEECH);
    } else if (order != 3 && channel_with(commands, 0x60, false) >= 0) {
        set_scene(seq, DANCER);
    } else if (channel_with(commands, 1, true) >= 0 ||
               channel_with(commands, 14, true) >= 0) {
        change_lines(seq);
    } else if (channel_with(commands, 2, true) >= 0) {
        set_scene(seq, RASTER);
    }

    if (seq->frame.scene == LOGO || seq->frame.scene == BABY ||
        seq->frame.scene == DANCER)
        seq->frame.tint = seq->flash_index++ % DANCE_COLORS_COUNT;
}

static void advance(Sequence *seq, const uint8_t commands[4], bool row_start)
{
    Frame *frame = &seq->frame;
    switch (frame->scene) {
    case LINES:
        for (unsigned i = 0; i < 4; ++i)
            frame->line_phase[i] = (frame->line_phase[i] + seq->style[i]) & 255;
        break;
    case NOISE:
        frame->image = seq->noise_index++ % NOISE_COUNT;
        break;
    case DANCER:
        frame->image = frame->age / DANCE_STEP_TICKS;
        break;
    case SPEECH: {
        unsigned energy = 0;
        for (unsigned i = 0; i < 4; ++i)
            energy += commands[i] >> 4;
        frame->speech_phase[0] = (frame->speech_phase[0] + 4) & 511;
        frame->speech_phase[1] = (frame->speech_phase[1] + 3) & 511;
        frame->speech_phase[2] = (frame->speech_phase[2] + energy / 16) & 511;
        break;
    }
    default:
        break;
    }

    for (unsigned ch = 0; ch < 4; ++ch) {
        if (row_start && commands[ch] && frame->scene == RASTER)
            seq->bar_age[ch] = BAR_TICKS;
        frame->bar[ch] = (float)seq->bar_age[ch] / BAR_TICKS;
        if (seq->bar_age[ch])
            --seq->bar_age[ch];
    }
}

static unsigned scene_duration(Scene scene)
{
    switch (scene) {
    case LOGO: case BABY: return LOGO_TICKS;
    case DANCER: return DANCE_STEP_TICKS * DANCE_COUNT;
    case SPEECH: return SPEECH_TICKS;
    case DOTS: return DOTS_TICKS;
    case TITLE: return TITLE_TICKS;
    case RASTER: return RASTER_TICKS;
    default: return 0;
    }
}

bool timeline_build(Timeline *timeline, const Assets *assets)
{
    const uint8_t *mod = assets->module;
    const uint8_t *orders = mod + 952;
    const uint8_t *patterns = mod + 1084;
    unsigned song_length = mod[950];
    for (unsigned i = 0; i < song_length; ++i) {
        if (orders[i] > 20)
            return false;
    }
    timeline->count = (size_t)song_length * MUSIC_ROWS * PAL_TICKS_PER_ROW;
    timeline->frames = calloc(timeline->count, sizeof(*timeline->frames));
    if (!timeline->frames)
        return false;

    Sequence seq = {.first_style = amiga(assets, CHANGE_RECORD)};
    seq.style = seq.first_style;
    change_lines(&seq);
    for (size_t i = 0; i < timeline->count; ++i) {
        unsigned music_row = (unsigned)i / PAL_TICKS_PER_ROW;
        unsigned order = music_row / MUSIC_ROWS;
        unsigned row = music_row % MUSIC_ROWS;
        bool row_start = i % PAL_TICKS_PER_ROW == 0;
        const uint8_t *notes = patterns + orders[order] * 1024 + row * 16;
        const uint8_t commands[4] = {notes[2], notes[6], notes[10], notes[14]};

        unsigned duration = scene_duration(seq.frame.scene);
        if (duration && seq.frame.age >= duration)
            resume_lines(&seq);
        if (row_start)
            trigger(&seq, notes, commands, order);
        timeline->frames[i] = seq.frame;
        advance(&seq, commands, row_start);
        /* Include timer resets on the first tick of the tracker row. */
        memcpy(timeline->frames[i].bar, seq.frame.bar, sizeof(seq.frame.bar));
        if (seq.frame.scene == NOISE || seq.frame.scene == DANCER)
            timeline->frames[i].image = seq.frame.image;
        ++seq.frame.age;
    }
    return true;
}
