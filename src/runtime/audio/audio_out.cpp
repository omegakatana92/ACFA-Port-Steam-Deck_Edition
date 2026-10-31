#include "runtime/audio/audio_out.h"

#include <cstring>
#include <mutex>

#include "runtime/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#else
#include <SDL3/SDL.h>
#endif

namespace rt::audio {
namespace {

#ifdef _WIN32
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
const GUID kFloatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

IAudioClient* g_client = nullptr;
IAudioRenderClient* g_render = nullptr;
UINT32 g_buffer_frames = 0;
bool g_ok = false;
#else
// SDL3 pulls from an audio stream; the runtime pushes the same 48 kHz float stereo frames the
// Windows/WASAPI path does. The stream's queue replaces the WASAPI device buffer.
constexpr u32 kBufferFrames = 4800;  // 100 ms
SDL_AudioStream* g_stream = nullptr;
std::mutex g_mu;
bool g_ok = false;

void sdl_audio_callback(void*, SDL_AudioStream*, int, int) {}  // pull model: no callback work
#endif

}  // namespace

bool out_init() {
#ifdef _WIN32
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  (void)hr;
  IMMDeviceEnumerator* en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&en)))) {
    LOG_W("audio", "no MMDeviceEnumerator; audio output disabled");
    return false;
  }
  IMMDevice* dev = nullptr;
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
    LOG_W("audio", "no default audio endpoint; audio output disabled");
    en->Release();
    return false;
  }
  en->Release();
  if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&g_client)))) {
    dev->Release();
    return false;
  }
  dev->Release();
  WAVEFORMATEXTENSIBLE wf{};
  wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wf.Format.nChannels = 2;
  wf.Format.nSamplesPerSec = 48000;
  wf.Format.wBitsPerSample = 32;
  wf.Format.nBlockAlign = 8;
  wf.Format.nAvgBytesPerSec = 48000 * 8;
  wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  wf.Samples.wValidBitsPerSample = 32;
  wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  wf.SubFormat = kFloatSubtype;
  REFERENCE_TIME dur = 1000000;  // 100 ms buffer
  hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, dur, 0,
                            reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
  if (FAILED(hr)) {
    LOG_W("audio", "IAudioClient::Initialize failed 0x%08lx; audio output disabled", hr);
    return false;
  }
  g_client->GetBufferSize(&g_buffer_frames);
  if (FAILED(g_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&g_render)))) return false;
  g_client->Start();
  g_ok = true;
  LOG_I("audio", "WASAPI output: 48 kHz float stereo, device buffer %u frames", g_buffer_frames);
  return true;
#else
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    LOG_W("audio", "SDL audio init failed (%s); audio output disabled", SDL_GetError());
    return false;
  }
  SDL_AudioSpec spec{};
  spec.format = SDL_AUDIO_F32;
  spec.channels = 2;
  spec.freq = 48000;
  g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdl_audio_callback, nullptr);
  if (!g_stream) {
    LOG_W("audio", "no audio playback device (%s); audio output disabled", SDL_GetError());
    return false;
  }
  SDL_ResumeAudioStreamDevice(g_stream);
  g_ok = true;
  LOG_I("audio", "SDL3 output: 48 kHz float stereo, stream buffer %u frames", kBufferFrames);
  return true;
#endif
}

bool out_available() { return g_ok; }

u32 out_queued_frames() {
  if (!g_ok) return 0;
#ifdef _WIN32
  UINT32 pad = 0;
  g_client->GetCurrentPadding(&pad);
  return pad;
#else
  std::lock_guard lk(g_mu);
  int queued = SDL_GetAudioStreamQueued(g_stream);  // bytes
  return queued > 0 ? u32(queued / 8) : 0;
#endif
}

void out_write(const float* stereo, u32 frames) {
  if (!g_ok) return;
#ifdef _WIN32
  UINT32 pad = 0;
  g_client->GetCurrentPadding(&pad);
  if (pad + frames > g_buffer_frames) return;  // device full: drop (should not happen with pacing)
  BYTE* p = nullptr;
  if (FAILED(g_render->GetBuffer(frames, &p))) return;
  std::memcpy(p, stereo, size_t(frames) * 8);
  g_render->ReleaseBuffer(frames, 0);
#else
  std::lock_guard lk(g_mu);
  int queued = SDL_GetAudioStreamQueued(g_stream);
  if (queued > 0 && u32(queued / 8) + frames > kBufferFrames) return;  // stream full: drop
  SDL_PutAudioStreamData(g_stream, stereo, int(size_t(frames) * 8));
#endif
}

}  // namespace rt::audio

