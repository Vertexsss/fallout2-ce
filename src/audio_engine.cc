#include "audio_engine.h"

#include <string.h>

#include <mutex>

#include <SDL.h>

namespace fallout {

#define AUDIO_ENGINE_SOUND_BUFFERS 8

static constexpr int kAudioEngineTargetSampleRate = 44100;

struct AudioEngineSoundBuffer {
    bool active;
    unsigned int size;
    int bitsPerSample;
    int channels;
    int rate;
    void* data;
    int volume;
    bool playing;
    bool looping;
    unsigned int pos;
    SDL_AudioStream* stream;
    std::recursive_mutex mutex;
};

extern bool gProgramIsActive;

static bool soundBufferIsValid(int soundBufferIndex);
static void audioEngineMixin(void* userData, Uint8* stream, int length);

static SDL_AudioSpec gAudioEngineSpec;
static SDL_AudioDeviceID gAudioEngineDeviceId = -1;

// Consecutive mixer callbacks that had nothing to mix, and whether the
// device is currently paused because of that.
static SDL_atomic_t gAudioSilentCallbacks;
static bool gAudioPausedBySilence = false;
static AudioEngineSoundBuffer gAudioEngineSoundBuffers[AUDIO_ENGINE_SOUND_BUFFERS];

static bool audioEngineIsInitialized()
{
    return gAudioEngineDeviceId != -1;
}

static bool soundBufferIsValid(int soundBufferIndex)
{
    return soundBufferIndex >= 0 && soundBufferIndex < AUDIO_ENGINE_SOUND_BUFFERS;
}

static void audioEngineMixin(void* userData, Uint8* stream, int length)
{
    memset(stream, gAudioEngineSpec.silence, length);

    if (!gProgramIsActive) {
        return;
    }

    bool mixedAny = false;

    for (int index = 0; index < AUDIO_ENGINE_SOUND_BUFFERS; index++) {
        AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[index]);
        std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

        if (soundBuffer->active && soundBuffer->playing) {
            mixedAny = true;
            int srcFrameSize = soundBuffer->bitsPerSample / 8 * soundBuffer->channels;

            unsigned char buffer[1024];
            int pos = 0;
            while (pos < length) {
                int remaining = length - pos;
                if (remaining > (int)sizeof(buffer)) {
                    remaining = (int)sizeof(buffer);
                }

                // Feed the stream in large chunks. Frame-by-frame conversion
                // costs tens of thousands of SDL_AudioStreamPut/Get calls per
                // second of audio - pure CPU (and battery) overhead for
                // byte-identical output.
                int fed = 0;
                if (SDL_AudioStreamAvailable(soundBuffer->stream) < remaining) {
                    int srcAvailable = soundBuffer->size - soundBuffer->pos;
                    int chunk = 8 * (int)sizeof(buffer);
                    if (chunk > srcAvailable) {
                        chunk = srcAvailable;
                    }
                    chunk -= chunk % srcFrameSize;

                    if (chunk > 0) {
                        SDL_AudioStreamPut(soundBuffer->stream, (unsigned char*)soundBuffer->data + soundBuffer->pos, chunk);
                        soundBuffer->pos += chunk;
                        fed = chunk;
                    }

                    if (soundBuffer->pos >= soundBuffer->size && soundBuffer->looping) {
                        soundBuffer->pos %= soundBuffer->size;
                    }
                }

                int bytesRead = SDL_AudioStreamGet(soundBuffer->stream, buffer, remaining);
                if (bytesRead == -1) {
                    break;
                }

                if (bytesRead == 0) {
                    if (fed == 0) {
                        // Source exhausted and the stream is drained.
                        if (!soundBuffer->looping) {
                            soundBuffer->playing = false;
                        }
                        break;
                    }
                    continue;
                }

                SDL_MixAudioFormat(stream + pos, buffer, gAudioEngineSpec.format, bytesRead, soundBuffer->volume);
                pos += bytesRead;

                if (soundBuffer->pos >= soundBuffer->size && !soundBuffer->looping
                    && SDL_AudioStreamAvailable(soundBuffer->stream) == 0) {
                    soundBuffer->playing = false;
                    break;
                }
            }
        }
    }

    if (mixedAny) {
        SDL_AtomicSet(&gAudioSilentCallbacks, 0);
    } else {
        SDL_AtomicAdd(&gAudioSilentCallbacks, 1);
    }

}

bool audioEngineInit()
{
    SDL_AudioSpec desiredSpec;
    // Request 44.1 kHz output so 44.1 kHz ACM music can play without being
    // downsampled by the mixer on the common path.
    desiredSpec.freq = kAudioEngineTargetSampleRate;
    desiredSpec.format = AUDIO_S16;
    desiredSpec.channels = 2;
    desiredSpec.samples = 1024;
    desiredSpec.callback = audioEngineMixin;

    int allowedChanges = SDL_AUDIO_ALLOW_FREQUENCY_CHANGE
        | SDL_AUDIO_ALLOW_FORMAT_CHANGE
        | SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
    gAudioEngineDeviceId = SDL_OpenAudioDevice(nullptr, 0, &desiredSpec, &gAudioEngineSpec, allowedChanges);
    if (gAudioEngineDeviceId == 0) {
        gAudioEngineDeviceId = -1;
        return false;
    }

    SDL_PauseAudioDevice(gAudioEngineDeviceId, 0);

    return true;
}

void audioEngineExit()
{
    if (audioEngineIsInitialized()) {
        SDL_CloseAudioDevice(gAudioEngineDeviceId);
        gAudioEngineDeviceId = -1;
    }
}

void audioEnginePause()
{
    if (audioEngineIsInitialized()) {
        SDL_PauseAudioDevice(gAudioEngineDeviceId, 1);
    }
}

void audioEngineMaintenance()
{
    if (!audioEngineIsInitialized()) {
        return;
    }

    // ~5 seconds of silent callbacks at 44100/1024 = ~43 callbacks/s.
    constexpr int kSilentCallbacksBeforePause = 215;

    if (!gAudioPausedBySilence
        && SDL_AtomicGet(&gAudioSilentCallbacks) >= kSilentCallbacksBeforePause) {
        gAudioPausedBySilence = true;
        SDL_PauseAudioDevice(gAudioEngineDeviceId, 1);
    }
}

void audioEngineResume()
{
    gAudioPausedBySilence = false;
    SDL_AtomicSet(&gAudioSilentCallbacks, 0);
    if (audioEngineIsInitialized()) {
        SDL_PauseAudioDevice(gAudioEngineDeviceId, 0);
    }
}

int audioEngineCreateSoundBuffer(unsigned int size, int bitsPerSample, int channels, int rate)
{
    if (!audioEngineIsInitialized()) {
        return -1;
    }

    for (int index = 0; index < AUDIO_ENGINE_SOUND_BUFFERS; index++) {
        AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[index]);
        std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

        if (!soundBuffer->active) {
            soundBuffer->active = true;
            soundBuffer->size = size;
            soundBuffer->bitsPerSample = bitsPerSample;
            soundBuffer->channels = channels;
            soundBuffer->rate = rate;
            soundBuffer->volume = SDL_MIX_MAXVOLUME;
            soundBuffer->playing = false;
            soundBuffer->looping = false;
            soundBuffer->pos = 0;
            soundBuffer->data = malloc(size);
            soundBuffer->stream = SDL_NewAudioStream(bitsPerSample == 16 ? AUDIO_S16 : AUDIO_S8, channels, rate, gAudioEngineSpec.format, gAudioEngineSpec.channels, gAudioEngineSpec.freq);
            return index;
        }
    }

    return -1;
}

bool audioEngineSoundBufferRelease(int soundBufferIndex)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->active = false;

    free(soundBuffer->data);
    soundBuffer->data = nullptr;

    SDL_FreeAudioStream(soundBuffer->stream);
    soundBuffer->stream = nullptr;

    return true;
}

bool audioEngineSoundBufferSetVolume(int soundBufferIndex, int volume)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->volume = volume;

    return true;
}

bool audioEngineSoundBufferGetVolume(int soundBufferIndex, int* volumePtr)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    *volumePtr = soundBuffer->volume;

    return true;
}

bool audioEngineSoundBufferSetPan(int soundBufferIndex, int pan)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    // NOTE: Audio engine does not support sound panning. I'm not sure it's
    // even needed. For now this value is silently ignored.

    return true;
}

bool audioEngineSoundBufferPlay(int soundBufferIndex, unsigned int flags)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->playing = true;

    // Wake the device if it was paused for silence.
    if (gAudioPausedBySilence) {
        gAudioPausedBySilence = false;
        SDL_AtomicSet(&gAudioSilentCallbacks, 0);
        SDL_PauseAudioDevice(gAudioEngineDeviceId, 0);
    }

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_PLAY_LOOPING) != 0) {
        soundBuffer->looping = true;
    }

    return true;
}

bool audioEngineSoundBufferStop(int soundBufferIndex)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->playing = false;

    return true;
}

bool audioEngineSoundBufferGetCurrentPosition(int soundBufferIndex, unsigned int* readPosPtr, unsigned int* writePosPtr)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    if (readPosPtr != nullptr) {
        // Raw fed position: the streaming refill bookkeeping
        // (_refreshSoundBuffers, movie ring) is built around it. Consumers
        // that must sync to the audible output use
        // audioEngineSoundBufferGetPlaybackPosition instead.
        *readPosPtr = soundBuffer->pos;
    }

    if (writePosPtr != nullptr) {
        *writePosPtr = soundBuffer->pos;

        if (soundBuffer->playing) {
            // 15 ms lead
            // See: https://docs.microsoft.com/en-us/previous-versions/windows/desktop/mt708925(v=vs.85)#remarks
            *writePosPtr += soundBuffer->rate / 150;
            *writePosPtr %= soundBuffer->size;
        }
    }

    return true;
}

// Playback-accurate read position: subtracts whatever still sits queued in
// the resampler stream from the batched feed position. For consumers that
// sync to the audible output (lip sync); the streaming refill bookkeeping
// must keep seeing the raw fed position.
bool audioEngineSoundBufferGetPlaybackPosition(int soundBufferIndex, unsigned int* posPtr)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    unsigned int pos = soundBuffer->pos;
    if (soundBuffer->stream != nullptr) {
        int queuedDst = SDL_AudioStreamAvailable(soundBuffer->stream);
        if (queuedDst > 0) {
            long long srcByteRate = (long long)soundBuffer->rate * soundBuffer->channels * (soundBuffer->bitsPerSample / 8);
            long long dstByteRate = (long long)gAudioEngineSpec.freq * gAudioEngineSpec.channels * (SDL_AUDIO_BITSIZE(gAudioEngineSpec.format) / 8);
            if (srcByteRate > 0 && dstByteRate > 0) {
                unsigned int queuedSrc = (unsigned int)(queuedDst * srcByteRate / dstByteRate);
                if (pos >= queuedSrc) {
                    pos -= queuedSrc;
                } else if (soundBuffer->looping && soundBuffer->size > 0) {
                    pos = (unsigned int)((pos + (unsigned long long)soundBuffer->size - queuedSrc % soundBuffer->size) % soundBuffer->size);
                } else {
                    pos = 0;
                }
            }
        }
    }

    if (posPtr != nullptr) {
        *posPtr = pos;
    }

    return true;
}

bool audioEngineSoundBufferSetCurrentPosition(int soundBufferIndex, unsigned int pos)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    soundBuffer->pos = pos % soundBuffer->size;

    return true;
}

bool audioEngineSoundBufferLock(int soundBufferIndex, unsigned int writePos, unsigned int writeBytes, void** audioPtr1, unsigned int* audioBytes1, void** audioPtr2, unsigned int* audioBytes2, unsigned int flags)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    if (audioBytes1 == nullptr) {
        return false;
    }

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_LOCK_FROM_WRITE_POS) != 0) {
        if (!audioEngineSoundBufferGetCurrentPosition(soundBufferIndex, nullptr, &writePos)) {
            return false;
        }
    }

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_LOCK_ENTIRE_BUFFER) != 0) {
        writeBytes = soundBuffer->size;
    }

    if (writePos + writeBytes <= soundBuffer->size) {
        *(unsigned char**)audioPtr1 = (unsigned char*)soundBuffer->data + writePos;
        *audioBytes1 = writeBytes;

        if (audioPtr2 != nullptr) {
            *audioPtr2 = nullptr;
        }

        if (audioBytes2 != nullptr) {
            *audioBytes2 = 0;
        }
    } else {
        unsigned int remainder = writePos + writeBytes - soundBuffer->size;
        *(unsigned char**)audioPtr1 = (unsigned char*)soundBuffer->data + writePos;
        *audioBytes1 = soundBuffer->size - writePos;

        if (audioPtr2 != nullptr) {
            *(unsigned char**)audioPtr2 = (unsigned char*)soundBuffer->data;
        }

        if (audioBytes2 != nullptr) {
            *audioBytes2 = writeBytes - (soundBuffer->size - writePos);
        }
    }

    // TODO: Mark range as locked.

    return true;
}

bool audioEngineSoundBufferUnlock(int soundBufferIndex, void* audioPtr1, unsigned int audioBytes1, void* audioPtr2, unsigned int audioBytes2)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    // TODO: Mark range as unlocked.

    return true;
}

bool audioEngineSoundBufferGetStatus(int soundBufferIndex, unsigned int* statusPtr)
{
    if (!audioEngineIsInitialized()) {
        return false;
    }

    if (!soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* soundBuffer = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(soundBuffer->mutex);

    if (!soundBuffer->active) {
        return false;
    }

    if (statusPtr == nullptr) {
        return false;
    }

    *statusPtr = 0;

    if (soundBuffer->playing) {
        *statusPtr |= AUDIO_ENGINE_SOUND_BUFFER_STATUS_PLAYING;

        if (soundBuffer->looping) {
            *statusPtr |= AUDIO_ENGINE_SOUND_BUFFER_STATUS_LOOPING;
        }
    }

    return true;
}

} // namespace fallout
