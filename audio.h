#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioSystem AudioSystem;

typedef uint32_t AudioClipID;
typedef uint32_t AudioEmitterID;
typedef uint32_t AudioBusID;

typedef enum AudioClipFlags
{
    AUDIO_CLIP_STREAM = 1u << 0,
    AUDIO_CLIP_STREAM_IF_PACKED = 1u << 1,
    AUDIO_CLIP_LOOP = 1u << 2,
} AudioClipFlags;

typedef enum AudioAttenuationModel
{
    AUDIO_ATTENUATION_NONE = 0,
    AUDIO_ATTENUATION_INVERSE = 1,
    AUDIO_ATTENUATION_LINEAR = 2,
    AUDIO_ATTENUATION_EXPONENTIAL = 3,
} AudioAttenuationModel;

typedef struct AudioConfig
{
    uint32_t max_clips;
    uint32_t max_emitters;
    uint32_t max_buses;
    uint32_t max_voices;
    float    master_volume;
    bool     enable_visualization;
} AudioConfig;

typedef struct AudioListener
{
    float position[3];
    float velocity[3];
    float at[3];
    float up[3];
} AudioListener;

typedef struct AudioPlay3DDesc
{
    AudioClipID clip;
    AudioBusID  bus;
    float       position[3];
    float       velocity[3];
    float       volume;
    float       min_distance;
    float       max_distance;
    float       rolloff;
    float       doppler;
    float       occlusion;
    bool        paused;
} AudioPlay3DDesc;

AudioConfig audio_default_config(void);
AudioSystem* audio_create(const AudioConfig* config);
void audio_shutdown(AudioSystem* sys);
void audio_destroy(AudioSystem* sys);

AudioBusID audio_bus_master(AudioSystem* sys);
AudioBusID audio_bus_sfx(AudioSystem* sys);
AudioBusID audio_bus_music(AudioSystem* sys);
AudioBusID audio_bus_ui(AudioSystem* sys);

AudioBusID audio_bus_create(AudioSystem* sys, const char* name, AudioBusID parent);
void audio_bus_set_volume(AudioSystem* sys, AudioBusID bus, float volume);
void audio_bus_set_reverb(AudioSystem* sys, AudioBusID bus, float room_size, float damp, float width, float freeze, float wet);

AudioClipID audio_clip_load(AudioSystem* sys, const char* path, uint32_t flags);
void audio_clip_release(AudioSystem* sys, AudioClipID clip);

AudioEmitterID audio_emitter_create(AudioSystem* sys);
void audio_emitter_release(AudioSystem* sys, AudioEmitterID emitter);
void audio_emitter_set_clip(AudioSystem* sys, AudioEmitterID emitter, AudioClipID clip);
void audio_emitter_set_position(AudioSystem* sys, AudioEmitterID emitter, float x, float y, float z);
void audio_emitter_set_velocity(AudioSystem* sys, AudioEmitterID emitter, float x, float y, float z);
void audio_emitter_set_distances(AudioSystem* sys, AudioEmitterID emitter, float min_distance, float max_distance);
void audio_emitter_set_attenuation(AudioSystem* sys, AudioEmitterID emitter, AudioAttenuationModel model, float rolloff);
void audio_emitter_set_doppler(AudioSystem* sys, AudioEmitterID emitter, float doppler);
void audio_emitter_set_volume(AudioSystem* sys, AudioEmitterID emitter, float volume);
void audio_emitter_set_occlusion(AudioSystem* sys, AudioEmitterID emitter, float occlusion);
void audio_emitter_set_paused(AudioSystem* sys, AudioEmitterID emitter, bool paused);
void audio_emitter_play(AudioSystem* sys, AudioEmitterID emitter, AudioBusID bus);
void audio_emitter_stop(AudioSystem* sys, AudioEmitterID emitter);

AudioEmitterID audio_play_3d(AudioSystem* sys, const AudioPlay3DDesc* desc);

void audio_update(AudioSystem* sys, const AudioListener* listener);

const float* audio_get_fft(AudioSystem* sys);
const float* audio_get_wave(AudioSystem* sys);
float audio_get_output_volume(AudioSystem* sys, uint32_t channel);
uint32_t audio_get_active_voice_count(AudioSystem* sys);

#ifdef __cplusplus
}
#endif
