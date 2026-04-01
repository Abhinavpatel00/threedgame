#include "audio.h"

#include "fs.h"

#include "external/phyfs/src/physfs.h"
#include "external/soloud/include/soloud.h"
#include "external/soloud/include/soloud_file.h"
#include "external/soloud/include/soloud_wav.h"
#include "external/soloud/include/soloud_wavstream.h"
#include "external/soloud/include/soloud_bus.h"
#include "external/soloud/include/soloud_freeverbfilter.h"

#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

struct AudioClip
{
    bool             valid;
    bool             streaming;
    bool             loop;
    float            base_volume;
    SoLoud::Wav*      wav;
    SoLoud::WavStream* stream;
    SoLoud::File*     stream_file;
    char             path[256];
};

struct AudioEmitter
{
    bool        valid;
    AudioClipID clip;
    AudioBusID  bus;
    SoLoud::handle handle;
    float       position[3];
    float       velocity[3];
    float       min_distance;
    float       max_distance;
    float       rolloff;
    float       doppler;
    float       volume;
    float       occlusion;
    AudioAttenuationModel attenuation;
    bool        paused;
};

struct AudioBus
{
    bool           valid;
    SoLoud::Bus    bus;
    SoLoud::handle handle;
    char           name[32];
    float          volume;
    bool           reverb_enabled;
    SoLoud::FreeverbFilter reverb;
};

struct AudioSystem
{
    SoLoud::Soloud soloud;
    AudioConfig    config;
    AudioListener  listener;

    AudioClip*    clips;
    AudioEmitter* emitters;
    AudioBus*     buses;

    uint32_t clip_capacity;
    uint32_t emitter_capacity;
    uint32_t bus_capacity;

    AudioBusID master_bus;
    AudioBusID sfx_bus;
    AudioBusID music_bus;
    AudioBusID ui_bus;

    bool initialized;
};

class PhysFSFile : public SoLoud::File
{
public:
    explicit PhysFSFile(PHYSFS_File* file)
        : mFile(file)
        , mLength(file ? PHYSFS_fileLength(file) : 0)
    {
    }

    ~PhysFSFile() override
    {
        if(mFile)
            PHYSFS_close(mFile);
    }

    int eof() override
    {
        if(!mFile)
            return 1;
        return PHYSFS_eof(mFile) != 0;
    }

    unsigned int read(unsigned char* dst, unsigned int bytes) override
    {
        if(!mFile || !dst || bytes == 0)
            return 0;
        PHYSFS_sint64 r = PHYSFS_readBytes(mFile, dst, bytes);
        if(r < 0)
            return 0;
        return (unsigned int)r;
    }

    unsigned int length() override
    {
        if(mLength < 0)
            return 0;
        return (unsigned int)mLength;
    }

    void seek(int offset) override
    {
        if(!mFile)
            return;
        if(offset < 0)
            offset = 0;
        PHYSFS_seek(mFile, (PHYSFS_uint64)offset);
    }

    unsigned int pos() override
    {
        if(!mFile)
            return 0;
        PHYSFS_sint64 p = PHYSFS_tell(mFile);
        if(p < 0)
            return 0;
        return (unsigned int)p;
    }

private:
    PHYSFS_File*  mFile;
    PHYSFS_sint64 mLength;
};

static float clampf(float v, float lo, float hi)
{
    if(v < lo)
        return lo;
    if(v > hi)
        return hi;
    return v;
}

static AudioClip* audio_clip_get(AudioSystem* sys, AudioClipID id)
{
    if(!sys || id == 0)
        return NULL;
    uint32_t idx = id - 1;
    if(idx >= sys->clip_capacity)
        return NULL;
    AudioClip* clip = &sys->clips[idx];
    return clip->valid ? clip : NULL;
}

static AudioEmitter* audio_emitter_get(AudioSystem* sys, AudioEmitterID id)
{
    if(!sys || id == 0)
        return NULL;
    uint32_t idx = id - 1;
    if(idx >= sys->emitter_capacity)
        return NULL;
    AudioEmitter* emitter = &sys->emitters[idx];
    return emitter->valid ? emitter : NULL;
}

static AudioBus* audio_bus_get(AudioSystem* sys, AudioBusID id)
{
    if(!sys || id == 0)
        return NULL;
    uint32_t idx = id - 1;
    if(idx >= sys->bus_capacity)
        return NULL;
    AudioBus* bus = &sys->buses[idx];
    return bus->valid ? bus : NULL;
}

static AudioClipID audio_clip_alloc(AudioSystem* sys)
{
    for(uint32_t i = 0; i < sys->clip_capacity; ++i)
    {
        if(!sys->clips[i].valid)
        {
            sys->clips[i] = AudioClip{};
            sys->clips[i].valid = true;
            sys->clips[i].base_volume = 1.0f;
            return i + 1;
        }
    }
    return 0;
}

static AudioEmitterID audio_emitter_alloc(AudioSystem* sys)
{
    for(uint32_t i = 0; i < sys->emitter_capacity; ++i)
    {
        if(!sys->emitters[i].valid)
        {
            sys->emitters[i] = AudioEmitter{};
            sys->emitters[i].valid = true;
            sys->emitters[i].min_distance = 0.5f;
            sys->emitters[i].max_distance = 50.0f;
            sys->emitters[i].rolloff = 1.0f;
            sys->emitters[i].doppler = 1.0f;
            sys->emitters[i].volume = 1.0f;
            sys->emitters[i].occlusion = 1.0f;
            sys->emitters[i].attenuation = AUDIO_ATTENUATION_INVERSE;
            return i + 1;
        }
    }
    return 0;
}

static AudioBusID audio_bus_alloc(AudioSystem* sys)
{
    for(uint32_t i = 0; i < sys->bus_capacity; ++i)
    {
        if(!sys->buses[i].valid)
        {
            sys->buses[i].handle = 0;
            sys->buses[i].name[0] = '\0';
            sys->buses[i].reverb_enabled = false;
            sys->buses[i].valid = true;
            sys->buses[i].volume = 1.0f;
            return i + 1;
        }
    }
    return 0;
}

static SoLoud::File* audio_open_file(const char* path, bool* out_packed)
{
    if(out_packed)
        *out_packed = false;

    if(path && fs_exists(path))
    {
        PHYSFS_File* file = PHYSFS_openRead(path);
        if(file)
        {
            if(out_packed)
                *out_packed = true;
            return new PhysFSFile(file);
        }
    }

    SoLoud::DiskFile* disk = new SoLoud::DiskFile();
    if(disk->open(path) == SoLoud::SO_NO_ERROR)
        return disk;

    delete disk;
    return NULL;
}

static SoLoud::AudioSource* audio_clip_source(AudioClip* clip)
{
    if(!clip)
        return NULL;
    if(clip->streaming)
        return clip->stream;
    return clip->wav;
}

static void audio_apply_emitter_params(AudioSystem* sys, AudioEmitter* emitter, AudioClip* clip)
{
    if(!sys || !emitter || !clip || !sys->soloud.isValidVoiceHandle(emitter->handle))
        return;

    sys->soloud.set3dSourceParameters(emitter->handle, emitter->position[0], emitter->position[1], emitter->position[2],
                                      emitter->velocity[0], emitter->velocity[1], emitter->velocity[2]);

    sys->soloud.set3dSourceMinMaxDistance(emitter->handle, emitter->min_distance, emitter->max_distance);
    sys->soloud.set3dSourceAttenuation(emitter->handle, (unsigned int)emitter->attenuation, emitter->rolloff);
    sys->soloud.set3dSourceDopplerFactor(emitter->handle, emitter->doppler);

    float volume = clip->base_volume * emitter->volume * clampf(emitter->occlusion, 0.0f, 1.0f);
    sys->soloud.setVolume(emitter->handle, volume);

    sys->soloud.setPause(emitter->handle, emitter->paused);
}

static AudioBusID audio_bus_create_internal(AudioSystem* sys, const char* name, AudioBusID parent)
{
    if(!sys)
        return 0;

    AudioBusID id = audio_bus_alloc(sys);
    if(id == 0)
        return 0;

    AudioBus* bus = audio_bus_get(sys, id);
    if(!bus)
        return 0;

    if(name)
        strncpy(bus->name, name, sizeof(bus->name) - 1);

    bus->bus.setVisualizationEnable(sys->config.enable_visualization);

    if(parent != 0)
    {
        AudioBus* parent_bus = audio_bus_get(sys, parent);
        if(parent_bus)
            bus->handle = parent_bus->bus.play(bus->bus);
        else
            bus->handle = sys->soloud.play(bus->bus);
    }
    else
    {
        bus->handle = sys->soloud.play(bus->bus);
    }

    sys->soloud.setVolume(bus->handle, bus->volume);
    return id;
}

AudioConfig audio_default_config(void)
{
    AudioConfig cfg;
    cfg.max_clips = 256;
    cfg.max_emitters = 1024;
    cfg.max_buses = 16;
    cfg.max_voices = 255;
    cfg.master_volume = 1.0f;
    cfg.enable_visualization = true;
    return cfg;
}

AudioSystem* audio_create(const AudioConfig* config)
{
    AudioConfig cfg = config ? *config : audio_default_config();
    AudioSystem* sys = new(std::nothrow) AudioSystem{};
    if(!sys)
        return NULL;

    sys->config = cfg;
    sys->clip_capacity = cfg.max_clips;
    sys->emitter_capacity = cfg.max_emitters;
    sys->bus_capacity = cfg.max_buses;

    sys->clips = new(std::nothrow) AudioClip[sys->clip_capacity]{};
    sys->emitters = new(std::nothrow) AudioEmitter[sys->emitter_capacity]{};
    sys->buses = new(std::nothrow) AudioBus[sys->bus_capacity]{};

    if(!sys->clips || !sys->emitters || !sys->buses)
    {
        audio_destroy(sys);
        return NULL;
    }

    unsigned int flags = SoLoud::Soloud::CLIP_ROUNDOFF;
    if(cfg.enable_visualization)
        flags |= SoLoud::Soloud::ENABLE_VISUALIZATION;

    if(sys->soloud.init(flags) != SoLoud::SO_NO_ERROR)
    {
        audio_destroy(sys);
        return NULL;
    }

    sys->initialized = true;

    uint32_t max_voices = cfg.max_voices;
    if(max_voices == 0)
        max_voices = 1;
    if(max_voices > 255)
        max_voices = 255;
    sys->soloud.setMaxActiveVoiceCount(max_voices);
    sys->soloud.setGlobalVolume(cfg.master_volume);

    sys->master_bus = audio_bus_create_internal(sys, "master", 0);
    sys->sfx_bus = audio_bus_create_internal(sys, "sfx", sys->master_bus);
    sys->music_bus = audio_bus_create_internal(sys, "music", sys->master_bus);
    sys->ui_bus = audio_bus_create_internal(sys, "ui", sys->master_bus);

    return sys;
}

void audio_shutdown(AudioSystem* sys)
{
    if(!sys || !sys->initialized)
        return;

    sys->soloud.stopAll();

    for(uint32_t i = 0; i < sys->clip_capacity; ++i)
    {
        AudioClip* clip = &sys->clips[i];
        if(!clip->valid)
            continue;

        if(clip->stream)
        {
            delete clip->stream;
            clip->stream = NULL;
        }

        if(clip->wav)
        {
            delete clip->wav;
            clip->wav = NULL;
        }

        if(clip->stream_file)
        {
            delete clip->stream_file;
            clip->stream_file = NULL;
        }

        clip->valid = false;
    }

    sys->soloud.deinit();
    sys->initialized = false;
}

void audio_destroy(AudioSystem* sys)
{
    if(!sys)
        return;

    audio_shutdown(sys);

    delete[] sys->clips;
    delete[] sys->emitters;
    delete[] sys->buses;
    sys->clips = NULL;
    sys->emitters = NULL;
    sys->buses = NULL;

    delete sys;
}

AudioBusID audio_bus_master(AudioSystem* sys)
{
    return sys ? sys->master_bus : 0;
}

AudioBusID audio_bus_sfx(AudioSystem* sys)
{
    return sys ? sys->sfx_bus : 0;
}

AudioBusID audio_bus_music(AudioSystem* sys)
{
    return sys ? sys->music_bus : 0;
}

AudioBusID audio_bus_ui(AudioSystem* sys)
{
    return sys ? sys->ui_bus : 0;
}

AudioBusID audio_bus_create(AudioSystem* sys, const char* name, AudioBusID parent)
{
    return audio_bus_create_internal(sys, name, parent);
}

void audio_bus_set_volume(AudioSystem* sys, AudioBusID bus_id, float volume)
{
    AudioBus* bus = audio_bus_get(sys, bus_id);
    if(!bus)
        return;

    bus->volume = volume;
    sys->soloud.setVolume(bus->handle, volume);
}

void audio_bus_set_reverb(AudioSystem* sys, AudioBusID bus_id, float room_size, float damp, float width, float freeze, float wet)
{
    AudioBus* bus = audio_bus_get(sys, bus_id);
    if(!bus)
        return;

    if(wet <= 0.0f)
    {
        bus->bus.setFilter(0, NULL);
        bus->reverb_enabled = false;
        return;
    }

    bus->bus.setFilter(0, &bus->reverb);
    bus->reverb_enabled = true;

    sys->soloud.setFilterParameter(bus->handle, 0, SoLoud::FreeverbFilter::ROOMSIZE, clampf(room_size, 0.0f, 1.0f));
    sys->soloud.setFilterParameter(bus->handle, 0, SoLoud::FreeverbFilter::DAMP, clampf(damp, 0.0f, 1.0f));
    sys->soloud.setFilterParameter(bus->handle, 0, SoLoud::FreeverbFilter::WIDTH, clampf(width, 0.0f, 1.0f));
    sys->soloud.setFilterParameter(bus->handle, 0, SoLoud::FreeverbFilter::FREEZE, clampf(freeze, 0.0f, 1.0f));
    sys->soloud.setFilterParameter(bus->handle, 0, SoLoud::FreeverbFilter::WET, clampf(wet, 0.0f, 1.0f));
}

AudioClipID audio_clip_load(AudioSystem* sys, const char* path, uint32_t flags)
{
    if(!sys || !path || !path[0])
        return 0;

    bool packed = false;
    bool stream = (flags & AUDIO_CLIP_STREAM) != 0;
    bool stream_if_packed = (flags & AUDIO_CLIP_STREAM_IF_PACKED) != 0;
    bool loop = (flags & AUDIO_CLIP_LOOP) != 0;

    SoLoud::File* file = audio_open_file(path, &packed);
    if(!file)
        return 0;

    if(!stream && stream_if_packed && packed)
        stream = true;

    AudioClipID id = audio_clip_alloc(sys);
    if(id == 0)
    {
        delete file;
        return 0;
    }

    AudioClip* clip = audio_clip_get(sys, id);
    if(!clip)
    {
        delete file;
        return 0;
    }

    strncpy(clip->path, path, sizeof(clip->path) - 1);
    clip->loop = loop;

    if(stream)
    {
        clip->stream = new SoLoud::WavStream();
        if(clip->stream->loadFile(file) != SoLoud::SO_NO_ERROR)
        {
            delete clip->stream;
            clip->stream = NULL;
            delete file;
            clip->valid = false;
            return 0;
        }
        clip->stream->setLooping(loop);
        clip->streaming = true;
        clip->stream_file = file;
    }
    else
    {
        clip->wav = new SoLoud::Wav();
        if(clip->wav->loadFile(file) != SoLoud::SO_NO_ERROR)
        {
            delete clip->wav;
            clip->wav = NULL;
            delete file;
            clip->valid = false;
            return 0;
        }
        clip->wav->setLooping(loop);
        clip->streaming = false;
        delete file;
    }

    return id;
}

void audio_clip_release(AudioSystem* sys, AudioClipID clip_id)
{
    AudioClip* clip = audio_clip_get(sys, clip_id);
    if(!clip)
        return;

    if(clip->stream)
    {
        delete clip->stream;
        clip->stream = NULL;
    }

    if(clip->wav)
    {
        delete clip->wav;
        clip->wav = NULL;
    }

    if(clip->stream_file)
    {
        delete clip->stream_file;
        clip->stream_file = NULL;
    }

    clip->valid = false;
}

AudioEmitterID audio_emitter_create(AudioSystem* sys)
{
    if(!sys)
        return 0;
    return audio_emitter_alloc(sys);
}

void audio_emitter_release(AudioSystem* sys, AudioEmitterID emitter_id)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    if(sys->soloud.isValidVoiceHandle(emitter->handle))
        sys->soloud.stop(emitter->handle);

    emitter->valid = false;
}

void audio_emitter_set_clip(AudioSystem* sys, AudioEmitterID emitter_id, AudioClipID clip_id)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    AudioClip* clip = audio_clip_get(sys, clip_id);
    if(!emitter || !clip)
        return;

    emitter->clip = clip_id;
}

void audio_emitter_set_position(AudioSystem* sys, AudioEmitterID emitter_id, float x, float y, float z)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    emitter->position[0] = x;
    emitter->position[1] = y;
    emitter->position[2] = z;
}

void audio_emitter_set_velocity(AudioSystem* sys, AudioEmitterID emitter_id, float x, float y, float z)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    emitter->velocity[0] = x;
    emitter->velocity[1] = y;
    emitter->velocity[2] = z;
}

void audio_emitter_set_distances(AudioSystem* sys, AudioEmitterID emitter_id, float min_distance, float max_distance)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    emitter->min_distance = min_distance;
    emitter->max_distance = max_distance;
}

void audio_emitter_set_attenuation(AudioSystem* sys, AudioEmitterID emitter_id, AudioAttenuationModel model, float rolloff)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    emitter->attenuation = model;
    emitter->rolloff = rolloff;
}

void audio_emitter_set_doppler(AudioSystem* sys, AudioEmitterID emitter_id, float doppler)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    emitter->doppler = doppler;
}

void audio_emitter_set_volume(AudioSystem* sys, AudioEmitterID emitter_id, float volume)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    emitter->volume = volume;
}

void audio_emitter_set_occlusion(AudioSystem* sys, AudioEmitterID emitter_id, float occlusion)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    emitter->occlusion = occlusion;
}

void audio_emitter_set_paused(AudioSystem* sys, AudioEmitterID emitter_id, bool paused)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    emitter->paused = paused;
    if(sys->soloud.isValidVoiceHandle(emitter->handle))
        sys->soloud.setPause(emitter->handle, paused);
}

void audio_emitter_play(AudioSystem* sys, AudioEmitterID emitter_id, AudioBusID bus_id)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    AudioClip* clip = audio_clip_get(sys, emitter->clip);
    if(!clip)
        return;

    SoLoud::AudioSource* source = audio_clip_source(clip);
    if(!source)
        return;

    AudioBus* bus = audio_bus_get(sys, bus_id);
    unsigned int bus_handle = bus ? bus->handle : 0;

    float volume = clip->base_volume * emitter->volume * clampf(emitter->occlusion, 0.0f, 1.0f);
    emitter->handle = sys->soloud.play3d(*source,
                                         emitter->position[0], emitter->position[1], emitter->position[2],
                                         emitter->velocity[0], emitter->velocity[1], emitter->velocity[2],
                                         volume, emitter->paused, bus_handle);

    emitter->bus = bus_id;

    audio_apply_emitter_params(sys, emitter, clip);
}

void audio_emitter_stop(AudioSystem* sys, AudioEmitterID emitter_id)
{
    AudioEmitter* emitter = audio_emitter_get(sys, emitter_id);
    if(!emitter)
        return;

    if(sys->soloud.isValidVoiceHandle(emitter->handle))
        sys->soloud.stop(emitter->handle);

    emitter->handle = 0;
}

AudioEmitterID audio_play_3d(AudioSystem* sys, const AudioPlay3DDesc* desc)
{
    if(!sys || !desc)
        return 0;

    AudioEmitterID emitter_id = audio_emitter_create(sys);
    if(emitter_id == 0)
        return 0;

    audio_emitter_set_clip(sys, emitter_id, desc->clip);
    audio_emitter_set_position(sys, emitter_id, desc->position[0], desc->position[1], desc->position[2]);
    audio_emitter_set_velocity(sys, emitter_id, desc->velocity[0], desc->velocity[1], desc->velocity[2]);
    audio_emitter_set_distances(sys, emitter_id, desc->min_distance, desc->max_distance);
    audio_emitter_set_attenuation(sys, emitter_id, AUDIO_ATTENUATION_INVERSE, desc->rolloff);
    audio_emitter_set_doppler(sys, emitter_id, desc->doppler);
    audio_emitter_set_volume(sys, emitter_id, desc->volume);
    audio_emitter_set_occlusion(sys, emitter_id, desc->occlusion);
    audio_emitter_set_paused(sys, emitter_id, desc->paused);

    audio_emitter_play(sys, emitter_id, desc->bus);
    return emitter_id;
}

void audio_update(AudioSystem* sys, const AudioListener* listener)
{
    if(!sys || !sys->initialized)
        return;

    if(listener)
    {
        sys->listener = *listener;
        sys->soloud.set3dListenerParameters(listener->position[0], listener->position[1], listener->position[2],
                                             listener->at[0], listener->at[1], listener->at[2],
                                             listener->up[0], listener->up[1], listener->up[2],
                                             listener->velocity[0], listener->velocity[1], listener->velocity[2]);
    }

    for(uint32_t i = 0; i < sys->emitter_capacity; ++i)
    {
        AudioEmitter* emitter = &sys->emitters[i];
        if(!emitter->valid)
            continue;

        if(!sys->soloud.isValidVoiceHandle(emitter->handle))
        {
            emitter->handle = 0;
            continue;
        }

        AudioClip* clip = audio_clip_get(sys, emitter->clip);
        if(!clip)
            continue;

        audio_apply_emitter_params(sys, emitter, clip);
    }

    sys->soloud.update3dAudio();
}

const float* audio_get_fft(AudioSystem* sys)
{
    if(!sys)
        return NULL;
    return sys->soloud.calcFFT();
}

const float* audio_get_wave(AudioSystem* sys)
{
    if(!sys)
        return NULL;
    return sys->soloud.getWave();
}

float audio_get_output_volume(AudioSystem* sys, uint32_t channel)
{
    if(!sys)
        return 0.0f;
    return sys->soloud.getApproximateVolume(channel);
}

uint32_t audio_get_active_voice_count(AudioSystem* sys)
{
    if(!sys)
        return 0;
    return sys->soloud.getActiveVoiceCount();
}
