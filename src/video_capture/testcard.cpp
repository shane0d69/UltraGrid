/**
 * @file   video_capture/testcard.cpp
 * @author Colin Perkins <csp@csperkins.org
 * @author Alvaro Saurin <saurin@dcs.gla.ac.uk>
 * @author Martin Benes     <martinbenesh@gmail.com>
 * @author Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 * @author Petr Holub       <hopet@ics.muni.cz>
 * @author Milos Liska      <xliska@fi.muni.cz>
 * @author Jiri Matela      <matela@ics.muni.cz>
 * @author Dalibor Matura   <255899@mail.muni.cz>
 * @author Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 */
/*
 * Copyright (c) 2005-2006 University of Glasgow
 * Copyright (c) 2005-2022 CESNET z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *      This product includes software developed by the University of Southern
 *      California Information Sciences Institute. This product also includes
 *      software developed by CESNET z.s.p.o.
 *
 * 4. Neither the name of the University, Institute, CESNET nor the names of
 *    its contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @file
 * @todo
 * Do the rendering in 16 bits
 */

#include "config.h"
#include "config_unix.h"
#include "config_win32.h"

#include "debug.h"
#include "host.h"
#include "lib_common.h"
#include "tv.h"
#include "video.h"
#include "video_capture.h"
#include "song1.h"
#include "utils/color_out.h"
#include "utils/misc.h"
#include "utils/ring_buffer.h"
#include "utils/vf_split.h"
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#ifdef HAVE_LIBSDL_MIXER
#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#else
#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>
#endif // defined HAVE_SDL2
#endif /* HAVE_LIBSDL_MIXER */
#include <vector>
#include "audio/types.h"
#include "utils/video_pattern_generator.hpp"
#include "video_capture/testcard_common.h"

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_BPS 2
#define BUFFER_SEC 1
constexpr int AUDIO_BUFFER_SIZE(int ch_count) { return AUDIO_SAMPLE_RATE * AUDIO_BPS * ch_count * BUFFER_SEC; }
#define MOD_NAME "[testcard] "
constexpr video_desc default_format = { 1920, 1080, UYVY, 25.0, INTERLACED_MERGED, 1 };

using rang::fg;
using rang::style;
using namespace std;

struct testcard_state {
        std::chrono::steady_clock::time_point last_frame_time;
        int pan;
        char *data {nullptr};
        std::chrono::steady_clock::time_point t0;
        struct video_frame *frame{nullptr};
        int frame_linesize;
        struct video_frame *tiled;

        struct audio_frame audio;
        char **tiles_data;
        int tiles_cnt_horizontal;
        int tiles_cnt_vertical;

        vector <char> audio_data;
        struct ring_buffer *midi_buf{};
        enum class grab_audio_t {
                NONE,
                ANY,
                MIDI,
                SINE,
        } grab_audio = grab_audio_t::NONE;

        bool still_image = false;
        string pattern{"bars"};
};

#if defined HAVE_LIBSDL_MIXER && ! defined HAVE_MACOSX
static void midi_audio_callback(int chan, void *stream, int len, void *udata)
{
        UNUSED(chan);
        struct testcard_state *s = (struct testcard_state *) udata;

        ring_buffer_write(s->midi_buf, static_cast<const char *>(stream), len);
}
#endif

static auto configure_sdl_mixer_audio(struct testcard_state *s) {
#if defined HAVE_LIBSDL_MIXER && ! defined HAVE_MACOSX
        SDL_Init(SDL_INIT_AUDIO);

        if( Mix_OpenAudio( AUDIO_SAMPLE_RATE, AUDIO_S16LSB,
                                s->audio.ch_count, 4096 ) == -1 ) {
                fprintf(stderr,"[testcard] error initalizing sound\n");
                return false;
        }
#ifdef _WIN32
        const char *filename = tmpnam(nullptr);
        FILE *f = fopen(filename, "wb");
#else
        char filename[] = P_tmpdir "/uv.midiXXXXXX";
        int fd = mkstemp(filename);
        FILE *f = fd == -1 ? nullptr : fdopen(fd, "wb");
#endif
        if (f == nullptr) {
                perror("fopen midi");
                return false;
        }
        size_t nwritten = fwrite(song1, sizeof song1, 1, f);
        fclose(f);
        if (nwritten != 1) {
                unlink(filename);
                return false;
        }
        Mix_Music *music = Mix_LoadMUS(filename);
        unlink(filename);
        if (music == nullptr) {
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "error loading MIDI: %s\n", Mix_GetError());
                return false;
        }

        s->midi_buf = ring_buffer_init(AUDIO_BUFFER_SIZE(s->audio.ch_count) /* 1 sec */);

        // register grab as a postmix processor
        if (!Mix_RegisterEffect(MIX_CHANNEL_POST, midi_audio_callback, nullptr, s)) {
                printf("[testcard] Mix_RegisterEffect: %s\n", Mix_GetError());
                return false;
        }

        if(Mix_PlayMusic(music,-1)==-1){
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "error playing MIDI: %s\n", Mix_GetError());
                return false;
        }
        Mix_Volume(-1, 0);

        cout << MOD_NAME << "Initialized MIDI\n";

        return true;
#else
        UNUSED(s);
        return false;
#endif
}

static void configure_fallback_audio(struct testcard_state *s) {
        static_assert(AUDIO_BPS == sizeof(int16_t), "Only 2-byte audio is supported for testcard audio at the moment");
        const int frequency = 1000;
        const double scale = 0.1;

        for (int i = 0; i < AUDIO_BUFFER_SIZE(s->audio.ch_count) / AUDIO_BPS; i += 1) {
                *(reinterpret_cast<int16_t*>(&s->audio_data[i * AUDIO_BPS])) = round(sin((static_cast<double>(i) / (static_cast<double>(AUDIO_SAMPLE_RATE) / frequency)) * M_PI * 2. ) * ((1LL << (AUDIO_BPS * 8)) / 2 - 1) * scale);
        }
}

static auto configure_audio(struct testcard_state *s)
{
        s->audio.bps = AUDIO_BPS;
        s->audio.ch_count = audio_capture_channels > 0 ? audio_capture_channels : DEFAULT_AUDIO_CAPTURE_CHANNELS;
        s->audio.sample_rate = AUDIO_SAMPLE_RATE;
        s->audio.max_size = AUDIO_BUFFER_SIZE(s->audio.ch_count);
        s->audio_data.resize(s->audio.max_size);
        s->audio.data = s->audio_data.data();

        if (s->grab_audio != testcard_state::grab_audio_t::SINE) {
                if (configure_sdl_mixer_audio(s)) {
                        s->grab_audio = testcard_state::grab_audio_t::MIDI;
                        return true;
                }
                if (s->grab_audio == testcard_state::grab_audio_t::MIDI) {
                        return false;
                }
        }

        LOG(LOG_LEVEL_WARNING) << MOD_NAME "SDL-mixer missing, running on Mac or other problem - using fallback audio.\n";
        configure_fallback_audio(s);
        s->grab_audio = testcard_state::grab_audio_t::SINE;

        return true;
}


static int configure_tiling(struct testcard_state *s, const char *fmt)
{
        char *tmp, *token, *saveptr = NULL;
        int tile_cnt;
        int x;

        int grid_w, grid_h;

        if(fmt[1] != '=') return 1;

        tmp = strdup(&fmt[2]);
        token = strtok_r(tmp, "x", &saveptr);
        grid_w = atoi(token);
        token = strtok_r(NULL, "x", &saveptr);
        grid_h = atoi(token);
        free(tmp);

        s->tiled = vf_alloc(grid_w * grid_h);
        s->tiles_cnt_horizontal = grid_w;
        s->tiles_cnt_vertical = grid_h;
        s->tiled->color_spec = s->frame->color_spec;
        s->tiled->fps = s->frame->fps;
        s->tiled->interlacing = s->frame->interlacing;

        tile_cnt = grid_w *
                grid_h;
        assert(tile_cnt >= 1);

        s->tiles_data = (char **) malloc(tile_cnt *
                        sizeof(char *));
        /* split only horizontally!!!!!! */
        vf_split(s->tiled, s->frame, grid_w,
                        1, 1 /*prealloc*/);
        /* for each row, make the tile data correct.
         * .data pointers of same row point to same block,
         * but different row */
        for(x = 0; x < grid_w; ++x) {
                int y;

                s->tiles_data[x] = s->tiled->tiles[x].data;

                s->tiled->tiles[x].width = s->frame->tiles[0].width/ grid_w;
                s->tiled->tiles[x].height = s->frame->tiles[0].height / grid_h;
                s->tiled->tiles[x].data_len = s->frame->tiles[0].data_len / (grid_w * grid_h);

                s->tiled->tiles[x].data =
                        s->tiles_data[x] = (char *) realloc(s->tiled->tiles[x].data,
                                        s->tiled->tiles[x].data_len * grid_h * 2);


                memcpy(s->tiled->tiles[x].data + s->tiled->tiles[x].data_len  * grid_h,
                                s->tiled->tiles[x].data, s->tiled->tiles[x].data_len * grid_h);
                /* recopy tiles vertically */
                for(y = 1; y < grid_h; ++y) {
                        memcpy(&s->tiled->tiles[y * grid_w + x],
                                        &s->tiled->tiles[x], sizeof(struct tile));
                        /* make the pointers correct */
                        s->tiles_data[y * grid_w + x] =
                                s->tiles_data[x] +
                                y * s->tiled->tiles[x].height *
                                vc_get_linesize(s->tiled->tiles[x].width, s->tiled->color_spec);

                        s->tiled->tiles[y * grid_w + x].data =
                                s->tiles_data[x] +
                                y * s->tiled->tiles[x].height *
                                vc_get_linesize(s->tiled->tiles[x].width, s->tiled->color_spec);
                }
        }

        return 0;
}

static bool parse_fps(const char *fps, struct video_desc *desc) {
        char *endptr = nullptr;
        desc->fps = strtod(fps, &endptr);
        desc->interlacing = PROGRESSIVE;
        if (strlen(endptr) != 0) { // optional interlacing suffix
                desc->interlacing = get_interlacing_from_suffix(endptr);
                if (desc->interlacing != PROGRESSIVE &&
                                desc->interlacing != SEGMENTED_FRAME &&
                                desc->interlacing != INTERLACED_MERGED) { // tff or bff
                        log_msg(LOG_LEVEL_ERROR, "Unsuppored interlacing format: %s!\n", endptr);
                        return false;
                }
                if (desc->interlacing == INTERLACED_MERGED) {
                        desc->fps /= 2;
                }
        }
        return true;
}

static auto parse_format(char **fmt, char **save_ptr) {
        struct video_desc desc{};
        desc.tile_count = 1;
        char *tmp = strtok_r(*fmt, ":", save_ptr);
        if (!tmp) {
                LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Missing width!\n";
                return video_desc{};
        }
        desc.width = max<long long>(strtol(tmp, nullptr, 0), 0);

        if ((tmp = strtok_r(nullptr, ":", save_ptr)) == nullptr) {
                LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Missing height!\n";
                return video_desc{};
        }
        desc.height = max<long long>(strtol(tmp, nullptr, 0), 0);

        if (desc.width * desc.height == 0) {
                fprintf(stderr, "Wrong dimensions for testcard.\n");
                return video_desc{};
        }

        if ((tmp = strtok_r(nullptr, ":", save_ptr)) == nullptr) {
                LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Missing FPS!\n";
                return video_desc{};
        }
        if (!parse_fps(tmp, &desc)) {
                return video_desc{};
        }

        if ((tmp = strtok_r(nullptr, ":", save_ptr)) == nullptr) {
                LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Missing pixel format!\n";
                return video_desc{};
        }
        desc.color_spec = get_codec_from_name(tmp);
        if (desc.color_spec == VIDEO_CODEC_NONE) {
                LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Unknown codec '" << tmp << "'\n";
                return video_desc{};
        }
        if (!testcard_has_conversion(desc.color_spec)) {
                LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Unsupported codec '" << tmp << "'\n";
                return video_desc{};
        }

        *fmt = nullptr;
        return desc;
}

static bool testcard_load_from_file(const char *filename, long data_len, char *data) {
        bool ret = true;
        FILE *in = fopen(filename, "r");
        if (in == nullptr) {
                LOG(LOG_LEVEL_WARNING) << MOD_NAME << "fopen: " << ug_strerror(errno) << "\n";
                return false;
        }
        fseek(in, 0L, SEEK_END);
        long filesize = ftell(in);
        if (filesize == -1) {
                LOG(LOG_LEVEL_WARNING) << MOD_NAME << "ftell: " << ug_strerror(errno) << "\n";
                filesize = data_len;
        }
        fseek(in, 0L, SEEK_SET);

        do {
                if (data_len != filesize) {
                        int level = data_len < filesize ? LOG_LEVEL_WARNING : LOG_LEVEL_ERROR;
                        LOG(level) << MOD_NAME  << "Wrong file size for selected "
                                "resolution and codec. File size " << filesize << ", "
                                "computed size " << data_len << "\n";
                        filesize = data_len;
                        if (level == LOG_LEVEL_ERROR) {
                                ret = false; break;
                        }
                }

                if (fread(data, filesize, 1, in) != 1) {
                        log_msg(LOG_LEVEL_ERROR, "Cannot read file %s\n", filename);
                        ret = false; break;
                }
        } while (false);

        fclose(in);
        return ret;
}

static int vidcap_testcard_init(struct vidcap_params *params, void **state)
{
        struct testcard_state *s = nullptr;
        char *filename = nullptr;
        const char *strip_fmt = NULL;
        char *save_ptr = NULL;
        int ret = VIDCAP_INIT_FAIL;
        char *tmp;

        if (vidcap_params_get_fmt(params) == NULL || strcmp(vidcap_params_get_fmt(params), "help") == 0) {
                printf("testcard options:\n");
                cout << BOLD(RED("\t-t testcard") << ":<width>:<height>:<fps>:<codec>[:filename=<filename>][:p][:s=<X>x<Y>][:i|:sf][:still][:pattern=<pattern>][:apattern=sine|midi] | -t testcard:help\n");
                cout << "where\n";
                cout << BOLD("\t<filename>") << " - use file named filename instead of default bars\n";
                cout << BOLD("\tp") << " - pan with frame\n";
                cout << BOLD("\ts") << " - split the frames into XxY separate tiles\n";
                cout << BOLD("\ti|sf") << " - send as interlaced or segmented frame (if none of those is set, progressive is assumed)\n";
                cout << BOLD("\tstill") << " - send still image\n";
                cout << BOLD("\tpattern") << " - pattern to use, use \"" << BOLD("pattern=help") << "\" for options\n";
                cout << BOLD("\tapattern") << " - audio pattern to use - \"sine\" or an included \"midi\"\n";
                cout << "\n";
                cout << "alternative format syntax:\n";
                cout << BOLD("\t-t testcard[:size=<width>x<height>][:fps=<fps>[:codec=<codec>][...]\n");
                cout << "\n";
                testcard_show_codec_help("testcard", false);
                cout << BOLD("Note:") << " only certain codec and generator combinations produce full-depth samples (not up-sampled 8-bit), use " << BOLD("pattern=help") << " for details.\n";
                return VIDCAP_INIT_NOERR;
        }

        s = new testcard_state();
        if (!s)
                return VIDCAP_INIT_FAIL;

        char *fmt = strdup(vidcap_params_get_fmt(params));
        char *ptr = fmt;

        struct video_desc desc = [&]{ return strlen(ptr) == 0 || !isdigit(ptr[0]) ? default_format : parse_format(&ptr, &save_ptr);}();
        if (!desc) {
                goto error;
        }

        tmp = strtok_r(ptr, ":", &save_ptr);
        while (tmp) {
                if (strcmp(tmp, "p") == 0) {
                        s->pan = 48;
                } else if (strncmp(tmp, "filename=", strlen("filename=")) == 0) {
                        filename = tmp + strlen("filename=");
                } else if (strncmp(tmp, "s=", 2) == 0) {
                        strip_fmt = tmp;
                } else if (strcmp(tmp, "i") == 0) {
                        s->frame->interlacing = INTERLACED_MERGED;
                        log_msg(LOG_LEVEL_WARNING, "[testcard] Deprecated 'i' option. Use format testcard:1920:1080:50i:UYVY instead!\n");
                } else if (strcmp(tmp, "sf") == 0) {
                        s->frame->interlacing = SEGMENTED_FRAME;
                        log_msg(LOG_LEVEL_WARNING, "[testcard] Deprecated 'sf' option. Use format testcard:1920:1080:25sf:UYVY instead!\n");
                } else if (strcmp(tmp, "still") == 0) {
                        s->still_image = true;
                } else if (strncmp(tmp, "pattern=", strlen("pattern=")) == 0) {
                        const char *pattern = tmp + strlen("pattern=");
                        s->pattern = pattern;
                } else if (strstr(tmp, "apattern=") == tmp) {
                        s->grab_audio = strcasecmp(tmp + strlen("apattern="), "sine") == 0 ? testcard_state::grab_audio_t::SINE : testcard_state::grab_audio_t::MIDI;
                } else if (strstr(tmp, "codec=") == tmp) {
                        desc.color_spec = get_codec_from_name(strchr(tmp, '=') + 1);
                } else if (strstr(tmp, "size=") == tmp && strchr(tmp, 'x') != nullptr) {
                        desc.width = stoi(strchr(tmp, '=') + 1);
                        desc.height = stoi(strchr(tmp, 'x') + 1);
                } else if (strstr(tmp, "fps=") == tmp) {
                        if (!parse_fps(strchr(tmp, '=') + 1, &desc)) {
                                goto error;
                        }
                } else {
                        fprintf(stderr, "[testcard] Unknown option: %s\n", tmp);
                        goto error;
                }
                tmp = strtok_r(NULL, ":", &save_ptr);
        }

        if (desc.color_spec == VIDEO_CODEC_NONE || desc.width <= 0 || desc.height <= 0 || desc.fps <= 0.0) {
                LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Wrong video format: " << desc << "\n";
                goto error;
        }

        s->frame = vf_alloc_desc(desc);
        vf_get_tile(s->frame, 0)->data = static_cast<char *>(malloc(s->frame->tiles[0].data_len * 2));
        s->frame_linesize = vc_get_linesize(desc.width, desc.color_spec);

        if (filename) {
                if (!testcard_load_from_file(filename, s->frame->tiles[0].data_len, s->frame->tiles[0].data)) {
                        goto error;
                }
        } else {
                auto data = video_pattern_generate(s->pattern.c_str(), s->frame->tiles[0].width, s->frame->tiles[0].height, s->frame->color_spec);
                if (!data) {
                        ret = s->pattern == "help" ? VIDCAP_INIT_NOERR : VIDCAP_INIT_FAIL;
                        goto error;
                }

                memcpy(vf_get_tile(s->frame, 0)->data, data.get(), s->frame->tiles[0].data_len);
        }

        // duplicate the image to allow scrolling
        memcpy(vf_get_tile(s->frame, 0)->data + vf_get_tile(s->frame, 0)->data_len, vf_get_tile(s->frame, 0)->data, vf_get_tile(s->frame, 0)->data_len);

        if (!s->still_image && codec_is_planar(s->frame->color_spec)) {
                log_msg(LOG_LEVEL_WARNING, MOD_NAME "Planar pixel format '%s', using still picture.\n", get_codec_name(s->frame->color_spec));
                s->still_image = true;
        }

        s->last_frame_time = std::chrono::steady_clock::now();

        LOG(LOG_LEVEL_INFO) << MOD_NAME << "capture set to " << desc << ", bpp "
                << get_bpp(s->frame->color_spec) << ", pattern: " << s->pattern
                << ", audio " << (s->grab_audio == testcard_state::grab_audio_t::NONE ? "off" : "on") << "\n";

        if(strip_fmt != NULL) {
                if(configure_tiling(s, strip_fmt) != 0) {
                        goto error;
                }
        }

        if(vidcap_params_get_flags(params) & VIDCAP_FLAG_AUDIO_EMBEDDED) {
                if (s->grab_audio == testcard_state::grab_audio_t::NONE) {
                        s->grab_audio = testcard_state::grab_audio_t::ANY;
                }
                if (!configure_audio(s)) {
                        LOG(LOG_LEVEL_ERROR) << "Cannot initialize audio!\n";
                        goto error;
                }
        } else {
                s->grab_audio = testcard_state::grab_audio_t::NONE;
        }

        free(fmt);

        s->data = s->frame->tiles[0].data;

        *state = s;
        return VIDCAP_INIT_OK;

error:
        free(fmt);
        free(s->data);
        vf_free(s->frame);
        delete s;
        return ret;
}

static void vidcap_testcard_done(void *state)
{
        struct testcard_state *s = (struct testcard_state *) state;
        free(s->data);
        if (s->tiled) {
                int i;
                for (i = 0; i < s->tiles_cnt_horizontal; ++i) {
                        free(s->tiles_data[i]);
                }
                vf_free(s->tiled);
        }
        vf_free(s->frame);
        ring_buffer_destroy(s->midi_buf);
        delete s;
}

static struct video_frame *vidcap_testcard_grab(void *arg, struct audio_frame **audio)
{
        struct testcard_state *state;
        state = (struct testcard_state *)arg;

        std::chrono::steady_clock::time_point curr_time =
                std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::duration<double>>(curr_time - state->last_frame_time).count() <
                        1.0 / state->frame->fps) {
                return NULL;
        }

        state->last_frame_time = curr_time;

        if (state->grab_audio != testcard_state::grab_audio_t::NONE) {
                if (state->grab_audio == testcard_state::grab_audio_t::MIDI) {
                        state->audio.data_len = ring_buffer_read(state->midi_buf, state->audio.data, state->audio.max_size);
                } else if (state->grab_audio == testcard_state::grab_audio_t::SINE) {
                        state->audio.data_len = state->audio.ch_count * state->audio.bps * AUDIO_SAMPLE_RATE / state->frame->fps;
                        state->audio.data += state->audio.data_len;
                        if (state->audio.data + state->audio.data_len > state->audio_data.data() + AUDIO_BUFFER_SIZE(state->audio.ch_count)) {
                                state->audio.data = state->audio_data.data();
                        }
                } else {
                        abort();
                }
                if(state->audio.data_len > 0)
                        *audio = &state->audio;
                else
                        *audio = NULL;
        } else {
                *audio = NULL;
        }

        if(!state->still_image) {
                vf_get_tile(state->frame, 0)->data += state->frame_linesize + state->pan;
        }
        if (vf_get_tile(state->frame, 0)->data > state->data + state->frame->tiles[0].data_len) {
                vf_get_tile(state->frame, 0)->data = state->data;
        }

        if (state->tiled) {
                /* update tile data instead */
                int i;
                int count = state->tiled->tile_count;

                for (i = 0; i < count; ++i) {
                        /* shift - for semantics of vars refer to configure_tiling*/
                        state->tiled->tiles[i].data += vc_get_linesize(
                                        state->tiled->tiles[i].width, state->tiled->color_spec);
                        /* if out of data, move to beginning
                         * keep in mind that we have two "pictures" for
                         * every tile stored sequentially */
                        if(state->tiled->tiles[i].data >= state->tiles_data[i] +
                                        state->tiled->tiles[i].data_len * state->tiles_cnt_vertical) {
                                state->tiled->tiles[i].data = state->tiles_data[i];
                        }
                }

                return state->tiled;
        }
        return state->frame;
}

static struct vidcap_type *vidcap_testcard_probe(bool verbose, void (**deleter)(void *))
{
        struct vidcap_type *vt;
        *deleter = free;

        vt = (struct vidcap_type *) calloc(1, sizeof(struct vidcap_type));
        if (vt == NULL) {
                return NULL;
        }

        vt->name = "testcard";
        vt->description = "Video testcard";

        if (!verbose) {
                return vt;
        }

        vt->card_count = 1;
        vt->cards = (struct device_info *) calloc(vt->card_count, sizeof(struct device_info));
        snprintf(vt->cards[0].name, sizeof vt->cards[0].name, "Testing signal");

        struct {
                int width;
                int height;
        } sizes[] = {
                {1280, 720},
                {1920, 1080},
                {3840, 2160},
        };
        int framerates[] = {24, 30, 60};
        const char * const pix_fmts[] = {"UYVY", "RGB"};

        snprintf(vt->cards[0].modes[0].name,
                        sizeof vt->cards[0].modes[0].name, "Default");
        snprintf(vt->cards[0].modes[0].id,
                        sizeof vt->cards[0].modes[0].id,
                        "{\"width\":\"\", "
                        "\"height\":\"\", "
                        "\"format\":\"\", "
                        "\"fps\":\"\"}");

        int i = 1;
        for(const auto &pix_fmt : pix_fmts){
                for(const auto &size : sizes){
                        for(const auto &fps : framerates){
                                snprintf(vt->cards[0].modes[i].name,
                                                sizeof vt->cards[0].name,
                                                "%dx%d@%d %s",
                                                size.width, size.height,
                                                fps, pix_fmt);
                                snprintf(vt->cards[0].modes[i].id,
                                                sizeof vt->cards[0].modes[0].id,
                                                "{\"width\":\"%d\", "
                                                "\"height\":\"%d\", "
                                                "\"format\":\"%s\", "
                                                "\"fps\":\"%d\"}",
                                                size.width, size.height,
                                                pix_fmt, fps);
                                i++;
                        }
                }
        }
        return vt;
}

static const struct video_capture_info vidcap_testcard_info = {
        vidcap_testcard_probe,
        vidcap_testcard_init,
        vidcap_testcard_done,
        vidcap_testcard_grab,
        true
};

REGISTER_MODULE(testcard, &vidcap_testcard_info, LIBRARY_CLASS_VIDEO_CAPTURE, VIDEO_CAPTURE_ABI_VERSION);

/* vim: set expandtab sw=8: */
