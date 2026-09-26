// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "audio_stream.h"
#include "host.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/gsvector.h"
#include "common/log.h"
#include "common/settings_interface.h"
#include "common/timer.h"

#include "soundtouch/SoundTouch.h"
#include "soundtouch/SoundTouchDLL.h"

#ifndef __ANDROID__
#include "freesurround_decoder.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

Log_SetChannel(AudioStream);

static constexpr bool LOG_TIMESTRETCH_STATS = false;

static constexpr const std::array<std::pair<u8, u8>, static_cast<size_t>(AudioExpansionMode::Count)>
  s_expansion_channel_count = {{
    {u8(2), u8(2)}, // Disabled
    {u8(3), u8(3)}, // StereoLFE
    {u8(5), u8(4)}, // Quadraphonic
    {u8(5), u8(5)}, // QuadraphonicLFE
    {u8(6), u8(6)}, // Surround51
    {u8(8), u8(8)}, // Surround71
  }};

AudioStream::DeviceInfo::DeviceInfo(std::string name_, std::string display_name_, u32 minimum_latency_)
  : name(std::move(name_)), display_name(std::move(display_name_)), minimum_latency_frames(minimum_latency_)
{
}

AudioStream::DeviceInfo::~DeviceInfo() = default;

AudioStream::AudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
  : m_sample_rate(sample_rate), m_parameters(parameters),
    m_internal_channels(s_expansion_channel_count[static_cast<size_t>(parameters.expansion_mode)].first),
    m_output_channels(s_expansion_channel_count[static_cast<size_t>(parameters.expansion_mode)].second)
{
}

AudioStream::~AudioStream()
{
  StretchDestroy();
  DestroyBuffer();
}

std::unique_ptr<AudioStream> AudioStream::CreateNullStream(u32 sample_rate, u32 buffer_ms)
{
  // no point stretching with no output
  AudioStreamParameters params;
  params.expansion_mode = AudioExpansionMode::Disabled;
  params.stretch_mode = AudioStretchMode::Off;
  params.buffer_ms = static_cast<u16>(buffer_ms);

  std::unique_ptr<AudioStream> stream(new AudioStream(sample_rate, params));
  stream->BaseInitialize(&StereoSampleReaderImpl);
  return stream;
}

std::vector<std::pair<std::string, std::string>> AudioStream::GetDriverNames(AudioBackend backend)
{
  std::vector<std::pair<std::string, std::string>> ret;
  switch (backend)
  {
#ifndef __ANDROID__
    case AudioBackend::Cubeb:
      ret = GetCubebDriverNames();
      break;
#endif

    default:
      break;
  }

  return ret;
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetOutputDevices(AudioBackend backend, const char* driver,
                                                                   u32 sample_rate)
{
  std::vector<AudioStream::DeviceInfo> ret;
  switch (backend)
  {
#ifndef __ANDROID__
    case AudioBackend::Cubeb:
      ret = GetCubebOutputDevices(driver, sample_rate);
      break;
#endif

    default:
      break;
  }

  return ret;
}

std::unique_ptr<AudioStream> AudioStream::CreateStream(AudioBackend backend, u32 sample_rate,
                                                       const AudioStreamParameters& parameters, const char* driver_name,
                                                       const char* device_name, Error* error /* = nullptr */)
{
  switch (backend)
  {
#ifndef __ANDROID__
    case AudioBackend::Cubeb:
      return CreateCubebAudioStream(sample_rate, parameters, driver_name, device_name, error);

    case AudioBackend::SDL:
      return CreateSDLAudioStream(sample_rate, parameters, error);
#else
    case AudioBackend::AAudio:
      return CreateAAudioAudioStream(sample_rate, parameters, error);

    case AudioBackend::OpenSLES:
      return CreateOpenSLESAudioStream(sample_rate, parameters, error);
#endif

    case AudioBackend::Null:
      return CreateNullStream(sample_rate, parameters.buffer_ms);

    default:
      Error::SetStringView(error, "Unknown audio backend.");
      return nullptr;
  }
}

u32 AudioStream::GetAlignedBufferSize(u32 size)
{
  static_assert(Common::IsPow2(CHUNK_SIZE));
  return Common::AlignUpPow2(size, CHUNK_SIZE);
}

u32 AudioStream::GetBufferSizeForMS(u32 sample_rate, u32 ms)
{
  return GetAlignedBufferSize((ms * sample_rate) / 1000u);
}

u32 AudioStream::GetMSForBufferSize(u32 sample_rate, u32 buffer_size)
{
  buffer_size = GetAlignedBufferSize(buffer_size);
  return (buffer_size * 1000u) / sample_rate;
}

static constexpr const std::array s_backend_names = {
  "Null",
#ifndef __ANDROID__
  "Cubeb",
  "SDL",
#else
  "AAudio",
  "OpenSLES",
#endif
};
static constexpr const std::array s_backend_display_names = {
  TRANSLATE_NOOP("AudioStream", "Null (No Output)"),
#ifndef __ANDROID__
  TRANSLATE_NOOP("AudioStream", "Cubeb"),
  TRANSLATE_NOOP("AudioStream", "SDL"),
#else
  "AAudio",
  "OpenSL ES",
#endif
};

std::optional<AudioBackend> AudioStream::ParseBackendName(const char* str)
{
  int index = 0;
  for (const char* name : s_backend_names)
  {
    if (std::strcmp(name, str) == 0)
      return static_cast<AudioBackend>(index);

    index++;
  }

  return std::nullopt;
}

const char* AudioStream::GetBackendName(AudioBackend backend)
{
  return s_backend_names[static_cast<int>(backend)];
}

const char* AudioStream::GetBackendDisplayName(AudioBackend backend)
{
  return Host::TranslateToCString("AudioStream", s_backend_display_names[static_cast<int>(backend)]);
}

static constexpr const std::array s_expansion_mode_names = {
  "Disabled", "StereoLFE", "Quadraphonic", "QuadraphonicLFE", "Surround51", "Surround71",
};
static constexpr const std::array s_expansion_mode_display_names = {
  TRANSLATE_NOOP("AudioStream", "Disabled (Stereo)"), TRANSLATE_NOOP("AudioStream", "Stereo with LFE"),
  TRANSLATE_NOOP("AudioStream", "Quadraphonic"),      TRANSLATE_NOOP("AudioStream", "Quadraphonic with LFE"),
  TRANSLATE_NOOP("AudioStream", "5.1 Surround"),      TRANSLATE_NOOP("AudioStream", "7.1 Surround"),
};

const char* AudioStream::GetExpansionModeName(AudioExpansionMode mode)
{
  return (static_cast<u32>(mode) < s_expansion_mode_names.size()) ? s_expansion_mode_names[static_cast<u32>(mode)] : "";
}

const char* AudioStream::GetExpansionModeDisplayName(AudioExpansionMode mode)
{
  return (static_cast<u32>(mode) < s_expansion_mode_display_names.size()) ?
           Host::TranslateToCString("AudioStream", s_expansion_mode_display_names[static_cast<u32>(mode)]) :
           "";
}

std::optional<AudioExpansionMode> AudioStream::ParseExpansionMode(const char* name)
{
  for (u8 i = 0; i < static_cast<u8>(AudioExpansionMode::Count); i++)
  {
    if (std::strcmp(name, s_expansion_mode_names[i]) == 0)
      return static_cast<AudioExpansionMode>(i);
  }

  return std::nullopt;
}

static constexpr const std::array s_stretch_mode_names = {
  "None",
  "Resample",
  "TimeStretch",
  "LowLatency",
};
static constexpr const std::array s_stretch_mode_display_names = {
  TRANSLATE_NOOP("AudioStream", "Off (Noisy)"),
  TRANSLATE_NOOP("AudioStream", "Resampling (Pitch Shift)"),
  TRANSLATE_NOOP("AudioStream", "Time Stretch (Tempo Change, Best Sound)"),
  TRANSLATE_NOOP("AudioStream", "Low Latency (Tight Sync)"),
};

const char* AudioStream::GetStretchModeName(AudioStretchMode mode)
{
  return (static_cast<u32>(mode) < s_stretch_mode_names.size()) ? s_stretch_mode_names[static_cast<u32>(mode)] : "";
}

const char* AudioStream::GetStretchModeDisplayName(AudioStretchMode mode)
{
  return (static_cast<u32>(mode) < s_stretch_mode_display_names.size()) ?
           Host::TranslateToCString("AudioStream", s_stretch_mode_display_names[static_cast<u32>(mode)]) :
           "";
}

std::optional<AudioStretchMode> AudioStream::ParseStretchMode(const char* name)
{
  for (u8 i = 0; i < static_cast<u8>(AudioStretchMode::Count); i++)
  {
    if (std::strcmp(name, s_stretch_mode_names[i]) == 0)
      return static_cast<AudioStretchMode>(i);
  }

  return std::nullopt;
}

u32 AudioStream::GetBufferedFramesRelaxed() const
{
  const u32 rpos = m_rpos.load(std::memory_order_relaxed);
  const u32 wpos = m_wpos.load(std::memory_order_relaxed);
  return (wpos + m_buffer_size - rpos) % m_buffer_size;
}

void AudioStream::SetBackendPeriodFrames(u32 frames)
{
  m_backend_period_frames.store(frames, std::memory_order_relaxed);
}

void AudioStream::ReadFrames(SampleType* samples, u32 num_frames)
{
  m_backend_callback_frames.store(num_frames, std::memory_order_relaxed);
  if (m_parameters.stretch_mode == AudioStretchMode::LowLatency)
  {
    LowLatencyObserveCallback(num_frames);

    // The callback owns m_rpos. Producers only request that stale buffered audio be discarded.
    const u32 requested_discard = m_low_latency_discard_frames.exchange(0, std::memory_order_acq_rel);
    if (requested_discard > 0)
    {
      const u32 buffered = GetBufferedFramesRelaxed();
      const u32 discard = std::min(requested_discard, buffered);
      if (discard > 0)
      {
        const u32 rpos = m_rpos.load(std::memory_order_relaxed);
        m_rpos.store((rpos + discard) % m_buffer_size, std::memory_order_release);
      }
    }
  }

  const u32 available_frames = GetBufferedFramesRelaxed();
  u32 frames_to_read = num_frames;
  u32 silence_frames = 0;

  if (m_filling.load(std::memory_order_relaxed))
  {
    u32 toFill = (m_parameters.stretch_mode == AudioStretchMode::LowLatency) ?
                   LowLatencyGetEffectiveTargetFrames() :
                   m_buffer_size / ((m_parameters.stretch_mode != AudioStretchMode::TimeStretch) ? 32 : 400);
    toFill = GetAlignedBufferSize(toFill);

    if (available_frames < toFill)
    {
      silence_frames = num_frames;
      frames_to_read = 0;
    }
    else
    {
      m_filling.store(false, std::memory_order_relaxed);
      VERBOSE_LOG("Underrun compensation done ({} frames buffered)", toFill);
    }
  }

  if (available_frames < frames_to_read)
  {
    silence_frames = frames_to_read - available_frames;
    frames_to_read = available_frames;

    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
      StretchUnderrun();
    else if (m_parameters.stretch_mode == AudioStretchMode::LowLatency)
    {
      LowLatencyHandleUnderrun(available_frames, num_frames);
      m_low_latency_reset_requested.store(true, std::memory_order_release);
    }

    m_filling.store(true, std::memory_order_relaxed);
  }

  if (frames_to_read > 0)
  {
    u32 rpos = m_rpos.load(std::memory_order_acquire);

    u32 end = m_buffer_size - rpos;
    if (end > frames_to_read)
      end = frames_to_read;

    // towards the end of the buffer
    if (end > 0)
    {
      m_sample_reader(samples, &m_buffer[rpos * m_internal_channels], end);
      rpos += end;
      rpos = (rpos == m_buffer_size) ? 0 : rpos;
    }

    // after wrapping around
    const u32 start = frames_to_read - end;
    if (start > 0)
    {
      m_sample_reader(&samples[end * m_output_channels], &m_buffer[0], start);
      rpos = start;
    }

    m_rpos.store(rpos, std::memory_order_release);
  }

  if (silence_frames > 0)
  {
    if (frames_to_read > 0)
    {
      // Super-basic resampler: spread the available input over the requested output.
      const u32 increment =
        static_cast<u32>(65536.0f * (static_cast<float>(frames_to_read) / static_cast<float>(num_frames)));

      SampleType* resample_ptr =
        static_cast<SampleType*>(alloca(frames_to_read * m_output_channels * sizeof(SampleType)));
      std::memcpy(resample_ptr, samples, frames_to_read * m_output_channels * sizeof(SampleType));

      SampleType* out_ptr = samples;
      const u32 copy_stride = sizeof(SampleType) * m_output_channels;
      u32 resample_subpos = 0;
      for (u32 i = 0; i < num_frames; i++)
      {
        std::memcpy(out_ptr, resample_ptr, copy_stride);
        out_ptr += m_output_channels;

        resample_subpos += increment;
        resample_ptr += (resample_subpos >> 16) * m_output_channels;
        resample_subpos %= 65536u;
      }

      VERBOSE_LOG("Audio buffer underflow, resampled {} frames to {}", frames_to_read, num_frames);
    }
    else
    {
      std::memset(samples + (frames_to_read * m_output_channels), 0,
                  silence_frames * m_output_channels * sizeof(s16));
    }
  }

  if (m_output_gain_changed.exchange(false, std::memory_order_acq_rel))
    m_limiter_gain = 1.0f;

  const float output_gain = m_output_gain.load(std::memory_order_acquire);
  if (output_gain != 1.0f || m_limiter_gain != 1.0f)
  {
    static constexpr float LIMITER_CEILING = 0.9885531f; // -0.1 dBFS
    static constexpr float LIMITER_RELEASE_SECONDS = 0.050f;

    const float release_step =
      1.0f - std::exp(-1.0f / (static_cast<float>(m_sample_rate) * LIMITER_RELEASE_SECONDS));
    SampleType* frame = samples;

    for (u32 i = 0; i < num_frames; i++, frame += m_output_channels)
    {
      float required_limiter_gain = 1.0f;
      float peak = 0.0f;
      if (output_gain > 1.0f)
      {
        for (u32 ch = 0; ch < m_output_channels; ch++)
          peak = std::max(peak, std::abs(static_cast<float>(frame[ch])) / 32768.0f);

        const float gained_peak = peak * output_gain;
        if (gained_peak > LIMITER_CEILING)
          required_limiter_gain = LIMITER_CEILING / gained_peak;
      }

      if (required_limiter_gain < m_limiter_gain)
        m_limiter_gain = required_limiter_gain;
      else
        m_limiter_gain += (1.0f - m_limiter_gain) * release_step;

      const float frame_gain = output_gain * m_limiter_gain;
      for (u32 ch = 0; ch < m_output_channels; ch++)
      {
        frame[ch] =
          static_cast<s16>(std::clamp(static_cast<float>(frame[ch]) * frame_gain, -32768.0f, 32767.0f));
      }
    }
  }
}


void AudioStream::StereoSampleReaderImpl(SampleType* dest, const SampleType* src, u32 num_frames)
{
  std::memcpy(dest, src, num_frames * 2 * sizeof(SampleType));
}

void AudioStream::InternalWriteFrames(s16* data, u32 num_frames)
{
  const u32 free = m_buffer_size - GetBufferedFramesRelaxed();
  if (free <= num_frames)
  {
    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
    {
      StretchOverrun();
    }
    else if (m_parameters.stretch_mode == AudioStretchMode::LowLatency)
    {
      const u32 buffered = GetBufferedFramesRelaxed();
      const u32 required_discard = (num_frames - free) + 1;
      const u32 discard = std::min(buffered, std::max(required_discard, CHUNK_SIZE));

      // Do not advance m_rpos here: the audio callback may be reading it concurrently.
      m_low_latency_discard_frames.fetch_add(discard, std::memory_order_release);
      LowLatencyReset();
      DEBUG_LOG("Low latency buffer overrun, requested discard of {} frames", discard);
      return;
    }
    else
    {
      DEBUG_LOG("Buffer overrun, chunk dropped");
      return;
    }
  }

  u32 wpos = m_wpos.load(std::memory_order_acquire);

  // wrapping around the end of the buffer?
  if ((m_buffer_size - wpos) <= num_frames)
  {
    const u32 end = m_buffer_size - wpos;
    const u32 start = num_frames - end;

    std::memcpy(&m_buffer[wpos * m_internal_channels], data, end * m_internal_channels * sizeof(SampleType));
    if (start > 0)
      std::memcpy(&m_buffer[0], data + end * m_internal_channels, start * m_internal_channels * sizeof(SampleType));

    wpos = start;
  }
  else
  {
    std::memcpy(&m_buffer[wpos * m_internal_channels], data, num_frames * m_internal_channels * sizeof(SampleType));
    wpos += num_frames;
  }

  m_wpos.store(wpos, std::memory_order_release);
}


void AudioStream::BaseInitialize(SampleReader sample_reader)
{
  m_sample_reader = sample_reader;

  AllocateBuffer();
  ExpandAllocate();
  StretchAllocate();
}

void AudioStream::AllocateBuffer()
{
  if (m_parameters.stretch_mode == AudioStretchMode::LowLatency)
  {
    m_low_latency_callback_candidate_frames = 0;
    m_low_latency_callback_candidate_count = 0;

    const u32 requested_target = std::max<u32>(
      GetAlignedBufferSize((m_sample_rate * m_parameters.low_latency_buffer_ms) / 1000u), CHUNK_SIZE);
    const u32 backend_period = m_backend_period_frames.load(std::memory_order_relaxed);
    const u32 backend_safe_target =
      (backend_period > 0) ? GetAlignedBufferSize(backend_period * 2u) : requested_target;

    m_target_buffer_size = std::max(requested_target, backend_safe_target);
    m_low_latency_effective_target_frames.store(m_target_buffer_size, std::memory_order_relaxed);

    const u32 low_latency_capacity = std::max<u32>(m_target_buffer_size * 4u, CHUNK_SIZE * 4u);
    const u32 fallback_capacity =
      GetAlignedBufferSize(((m_parameters.buffer_ms * 2u) * m_sample_rate) / 1000u);
    const u32 adaptive_capacity = LowLatencyGetMaximumTargetFrames() * 2u;
    m_buffer_size = std::max({low_latency_capacity, fallback_capacity, adaptive_capacity});
    m_filling.store(true, std::memory_order_relaxed);

    DEV_LOG("Low Latency target: requested={} ms ({} frames), backend period={} frames, initial target={} frames.",
            m_parameters.low_latency_buffer_ms, requested_target, backend_period, m_target_buffer_size);
  }
  else
  {
    // Use a larger buffer when time stretching, since it needs more working room.
    const u32 multiplier = (m_parameters.stretch_mode == AudioStretchMode::TimeStretch) ?
                             16 :
                             ((m_parameters.stretch_mode == AudioStretchMode::Off) ? 1 : 2);
    m_buffer_size = GetAlignedBufferSize(((m_parameters.buffer_ms * multiplier) * m_sample_rate) / 1000);
    m_target_buffer_size = GetAlignedBufferSize((m_sample_rate * m_parameters.buffer_ms) / 1000u);
    m_filling.store(false, std::memory_order_relaxed);
  }

  m_buffer = std::make_unique<s16[]>(m_buffer_size * m_internal_channels);
  m_staging_buffer = std::make_unique<s16[]>((CHUNK_SIZE * 2u) * m_internal_channels);
  m_float_buffer = std::make_unique<float[]>((CHUNK_SIZE * 2u) * m_internal_channels);

  if (IsExpansionEnabled())
    m_expand_buffer = std::make_unique<float[]>(m_parameters.expand_block_size * NUM_INPUT_CHANNELS);

  DEV_LOG(
    "Allocated buffer of {} frames for buffer of {} ms [expansion {} (block size {}), stretch {}, target size {}].",
    m_buffer_size,
    (m_parameters.stretch_mode == AudioStretchMode::LowLatency) ? m_parameters.low_latency_buffer_ms :
                                                                  m_parameters.buffer_ms,
    GetExpansionModeName(m_parameters.expansion_mode), m_parameters.expand_block_size,
    GetStretchModeName(m_parameters.stretch_mode),
    (m_parameters.stretch_mode == AudioStretchMode::LowLatency) ? LowLatencyGetEffectiveTargetFrames() :
                                                                  m_target_buffer_size);
}


void AudioStream::DestroyBuffer()
{
  m_expand_buffer.reset();
  m_staging_buffer.reset();
  m_float_buffer.reset();
  m_buffer.reset();
  m_buffer_size = 0;
  m_wpos.store(0, std::memory_order_release);
  m_rpos.store(0, std::memory_order_release);
  m_low_latency_discard_frames.store(0, std::memory_order_relaxed);
  m_low_latency_reset_requested.store(false, std::memory_order_relaxed);
}


void AudioStream::EmptyBuffer()
{
#ifndef __ANDROID__
  if (IsExpansionEnabled())
  {
    m_expander->Flush();
    m_expand_output_buffer = nullptr;
    m_expand_buffer_pos = 0;
  }
#endif

  if (IsStretchEnabled())
  {
    if (m_soundtouch)
      soundtouch_clear(m_soundtouch);

    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch ||
        (m_parameters.stretch_mode == AudioStretchMode::LowLatency && m_low_latency_time_stretch))
    {
      soundtouch_setTempo(m_soundtouch, m_nominal_rate);
    }

    if (m_parameters.stretch_mode == AudioStretchMode::LowLatency)
      LowLatencyReset();
  }

  m_wpos.store(m_rpos.load(std::memory_order_acquire), std::memory_order_release);
}


void AudioStream::SetNominalRate(float tempo)
{
  m_nominal_rate = tempo;
  if (m_parameters.stretch_mode == AudioStretchMode::Resample)
  {
    soundtouch_setRate(m_soundtouch, tempo);
  }
  else if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch && m_stretch_inactive)
  {
    soundtouch_setTempo(m_soundtouch, tempo);
  }
  else if (m_parameters.stretch_mode == AudioStretchMode::LowLatency)
  {
    const bool use_time_stretch = m_low_latency_time_stretch ?
                                    (tempo < 0.97f || tempo > 1.03f) :
                                    (tempo < 0.95f || tempo > 1.05f);

    if (use_time_stretch != m_low_latency_time_stretch)
    {
      m_low_latency_time_stretch = use_time_stretch;
      LowLatencyReset();
      if (m_soundtouch)
        soundtouch_clear(m_soundtouch);
    }

    if (m_low_latency_time_stretch && m_soundtouch)
      soundtouch_setTempo(m_soundtouch, tempo);
  }
}


void AudioStream::SetStretchMode(AudioStretchMode mode)
{
  if (m_parameters.stretch_mode == mode)
    return;

  // can't resize the buffers while paused
  bool paused = m_paused;
  if (!paused)
    SetPaused(true);

  DestroyBuffer();
  StretchDestroy();
  m_parameters.stretch_mode = mode;

  AllocateBuffer();
  if (m_parameters.stretch_mode != AudioStretchMode::Off)
    StretchAllocate();

  if (!paused)
    SetPaused(false);
}

void AudioStream::SetPaused(bool paused)
{
  m_paused = paused;
}

void AudioStream::SetOutputVolume(u32 volume, s32 gain_db)
{
  m_volume = volume;
  m_output_gain_db = gain_db;
  const float output_gain =
    (static_cast<float>(volume) / 100.0f) * std::pow(10.0f, static_cast<float>(gain_db) / 20.0f);
  m_output_gain.store(output_gain, std::memory_order_release);
  m_output_gain_changed.store(true, std::memory_order_release);
}


void AudioStream::BeginWrite(SampleType** buffer_ptr, u32* num_frames)
{
  // TODO: Write directly to buffer when not using stretching.
  *buffer_ptr = &m_staging_buffer[m_staging_buffer_pos];
  *num_frames = CHUNK_SIZE - (m_staging_buffer_pos / NUM_INPUT_CHANNELS);
}

static void S16ChunkToFloat(const s16* src, float* dst, u32 num_samples)
{
  constexpr GSVector4 S16_TO_FLOAT_V = GSVector4::cxpr(1.0f / 32767.0f);

  const u32 iterations = (num_samples + 7) / 8;
  for (u32 i = 0; i < iterations; i++)
  {
    const GSVector4i sv = GSVector4i::load<false>(src);
    src += 8;

    GSVector4i iv1 = sv.upl16(sv);  // [0, 0, 1, 1, 2, 2, 3, 3]
    GSVector4i iv2 = sv.uph16(sv);  // [4, 4, 5, 5, 6, 6, 7, 7]
    iv1 = iv1.sra32<16>();          // [0, 1, 2, 3]
    iv2 = iv2.sra32<16>();          // [4, 5, 6, 7]
    GSVector4 fv1 = GSVector4(iv1); // [f0, f1, f2, f3]
    GSVector4 fv2 = GSVector4(iv2); // [f4, f5, f6, f7]
    fv1 = fv1 * S16_TO_FLOAT_V;
    fv2 = fv2 * S16_TO_FLOAT_V;

    GSVector4::store<false>(dst + 0, fv1);
    GSVector4::store<false>(dst + 4, fv2);
    dst += 8;
  }
}

static void FloatChunkToS16(s16* dst, const float* src, u32 num_samples)
{
  const GSVector4 FLOAT_TO_S16_V = GSVector4::cxpr(32767.0f);

  const u32 iterations = (num_samples + 7) / 8;
  for (u32 i = 0; i < iterations; i++)
  {
    GSVector4 fv1 = GSVector4::load<false>(src + 0);
    GSVector4 fv2 = GSVector4::load<false>(src + 4);
    src += 8;

    fv1 = fv1 * FLOAT_TO_S16_V;
    fv2 = fv2 * FLOAT_TO_S16_V;
    GSVector4i iv1 = GSVector4i(fv1);
    GSVector4i iv2 = GSVector4i(fv2);

    const GSVector4i iv = iv1.ps32(iv2);
    GSVector4i::store<false>(dst, iv);
    dst += 8;
  }
}

void AudioStream::ExpandAllocate()
{
  DebugAssert(!m_expander);
  if (m_parameters.expansion_mode == AudioExpansionMode::Disabled)
    return;

#ifndef __ANDROID__
  static constexpr std::array<std::pair<FreeSurroundDecoder::ChannelSetup, bool>,
                              static_cast<size_t>(AudioExpansionMode::Count)>
    channel_setup_mapping = {{
      {FreeSurroundDecoder::ChannelSetup::Stereo, false},     // Disabled
      {FreeSurroundDecoder::ChannelSetup::Stereo, true},      // StereoLFE
      {FreeSurroundDecoder::ChannelSetup::Surround41, false}, // Quadraphonic
      {FreeSurroundDecoder::ChannelSetup::Surround41, true},  // QuadraphonicLFE
      {FreeSurroundDecoder::ChannelSetup::Surround51, true},  // Surround51
      {FreeSurroundDecoder::ChannelSetup::Surround71, true},  // Surround71
    }};

  const auto [fs_setup, fs_lfe] = channel_setup_mapping[static_cast<size_t>(m_parameters.expansion_mode)];

  m_expander = std::make_unique<FreeSurroundDecoder>(fs_setup, m_parameters.expand_block_size);
  m_expander->SetBassRedirection(fs_lfe);
  m_expander->SetCircularWrap(m_parameters.expand_circular_wrap);
  m_expander->SetShift(m_parameters.expand_shift);
  m_expander->SetDepth(m_parameters.expand_depth);
  m_expander->SetFocus(m_parameters.expand_focus);
  m_expander->SetCenterImage(m_parameters.expand_center_image);
  m_expander->SetFrontSeparation(m_parameters.expand_front_separation);
  m_expander->SetRearSeparation(m_parameters.expand_rear_separation);
  m_expander->SetLowCutoff(static_cast<float>(m_parameters.expand_low_cutoff) / m_sample_rate * 2);
  m_expander->SetHighCutoff(static_cast<float>(m_parameters.expand_high_cutoff) / m_sample_rate * 2);
#else
  Panic("Attempting to use expansion on Android.");
#endif
}

void AudioStream::EndWrite(u32 num_frames)
{
  // don't bother committing anything when muted
  if (m_volume == 0)
    return;

  m_staging_buffer_pos += num_frames * NUM_INPUT_CHANNELS;
  DebugAssert(m_staging_buffer_pos <= (CHUNK_SIZE * NUM_INPUT_CHANNELS));
  if ((m_staging_buffer_pos / NUM_INPUT_CHANNELS) < CHUNK_SIZE)
    return;

  m_staging_buffer_pos = 0;

  if (!IsExpansionEnabled() && !IsStretchEnabled())
  {
    InternalWriteFrames(m_staging_buffer.get(), CHUNK_SIZE);
    return;
  }

#ifndef __ANDROID__
  if (IsExpansionEnabled())
  {
    // StretchWriteBlock() overwrites the staging buffer on output, so we need to copy into the expand buffer first.
    S16ChunkToFloat(m_staging_buffer.get(), m_expand_buffer.get() + m_expand_buffer_pos * NUM_INPUT_CHANNELS,
                    CHUNK_SIZE * NUM_INPUT_CHANNELS);

    // Output the corresponding block.
    if (m_expand_output_buffer)
      StretchWriteBlock(m_expand_output_buffer + m_expand_buffer_pos * m_internal_channels);

    // Decode the next block if we buffered enough.
    m_expand_buffer_pos += CHUNK_SIZE;
    if (m_expand_buffer_pos == m_parameters.expand_block_size)
    {
      m_expand_buffer_pos = 0;
      m_expand_output_buffer = m_expander->Decode(m_expand_buffer.get());
    }
  }
  else
#endif
  {
    S16ChunkToFloat(m_staging_buffer.get(), m_float_buffer.get(), CHUNK_SIZE * NUM_INPUT_CHANNELS);
    StretchWriteBlock(m_float_buffer.get());
  }
}

// Time stretching algorithm based on PCSX2 implementation.

template<class T>
ALWAYS_INLINE static bool IsInRange(const T& val, const T& min, const T& max)
{
  return (min <= val && val <= max);
}

void AudioStream::StretchAllocate()
{
  if (m_parameters.stretch_mode == AudioStretchMode::Off)
    return;

  m_soundtouch = soundtouch_createInstance();
  soundtouch_setSampleRate(m_soundtouch, m_sample_rate);
  soundtouch_setChannels(m_soundtouch, m_internal_channels);

  soundtouch_setSetting(m_soundtouch, SETTING_USE_QUICKSEEK, m_parameters.stretch_use_quickseek);
  soundtouch_setSetting(m_soundtouch, SETTING_USE_AA_FILTER, m_parameters.stretch_use_aa_filter);

  soundtouch_setSetting(m_soundtouch, SETTING_SEQUENCE_MS, m_parameters.stretch_sequence_length_ms);
  soundtouch_setSetting(m_soundtouch, SETTING_SEEKWINDOW_MS, m_parameters.stretch_seekwindow_ms);
  soundtouch_setSetting(m_soundtouch, SETTING_OVERLAP_MS, m_parameters.stretch_overlap_ms);

  if (m_parameters.stretch_mode == AudioStretchMode::Resample)
    soundtouch_setRate(m_soundtouch, m_nominal_rate);
  else
    soundtouch_setTempo(m_soundtouch, m_nominal_rate);

  if (m_parameters.stretch_mode == AudioStretchMode::LowLatency)
  {
    m_low_latency_time_stretch = (m_nominal_rate < 0.95f || m_nominal_rate > 1.05f);
    LowLatencyReset();
  }

  m_stretch_reset = STRETCH_RESET_THRESHOLD;
  m_stretch_inactive = false;
  m_stretch_ok_count = 0;
  m_dynamic_target_usage = 0.0f;
  m_average_position = 0;
  m_average_available = 0;

  m_staging_buffer_pos = 0;
}


void AudioStream::StretchDestroy()
{
  if (m_soundtouch)
  {
    soundtouch_destroyInstance(m_soundtouch);
    m_soundtouch = nullptr;
  }
}

void AudioStream::StretchWriteBlock(const float* block)
{
  if (m_parameters.stretch_mode == AudioStretchMode::LowLatency && !m_low_latency_time_stretch)
  {
    LowLatencyWriteBlock(block);
    return;
  }

  if (IsStretchEnabled())
  {
    soundtouch_putSamples(m_soundtouch, block, CHUNK_SIZE);

    u32 tempProgress;
    while (tempProgress = soundtouch_receiveSamples(m_soundtouch, m_float_buffer.get(), CHUNK_SIZE), tempProgress != 0)
    {
      FloatChunkToS16(m_staging_buffer.get(), m_float_buffer.get(), tempProgress * m_internal_channels);
      InternalWriteFrames(m_staging_buffer.get(), tempProgress);
    }

    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
      UpdateStretchTempo();
  }
  else
  {
    FloatChunkToS16(m_staging_buffer.get(), block, CHUNK_SIZE * m_internal_channels);
    InternalWriteFrames(m_staging_buffer.get(), CHUNK_SIZE);
  }
}


u32 AudioStream::LowLatencyGetEffectiveTargetFrames() const
{
  const u32 target = m_low_latency_effective_target_frames.load(std::memory_order_relaxed);
  return (target > 0) ? target : m_target_buffer_size;
}

u32 AudioStream::LowLatencyGetBackendQuantumFrames() const
{
  const u32 backend_period = m_backend_period_frames.load(std::memory_order_relaxed);
  return (backend_period > 0) ? backend_period : m_backend_callback_frames.load(std::memory_order_relaxed);
}

u32 AudioStream::LowLatencyGetMaximumTargetFrames() const
{
  static constexpr u32 MAX_ADAPTIVE_TARGET_MS = 100;
  return std::max<u32>(GetAlignedBufferSize((m_sample_rate * MAX_ADAPTIVE_TARGET_MS) / 1000u), CHUNK_SIZE);
}

void AudioStream::LowLatencyObserveCallback(u32 num_frames)
{
  static constexpr u32 CALLBACK_CONFIRMATION_COUNT = 4;

  if (num_frames == 0)
    return;

  if (m_low_latency_callback_candidate_frames != num_frames)
  {
    m_low_latency_callback_candidate_frames = num_frames;
    m_low_latency_callback_candidate_count = 1;
    return;
  }

  if (m_low_latency_callback_candidate_count < CALLBACK_CONFIRMATION_COUNT)
    m_low_latency_callback_candidate_count++;
  if (m_low_latency_callback_candidate_count != CALLBACK_CONFIRMATION_COUNT)
    return;

  const u32 previous_period = m_backend_period_frames.load(std::memory_order_relaxed);
  if (previous_period == num_frames)
    return;

  // Only replace the backend period after several matching callbacks. This rejects one-time startup/full-buffer
  // callbacks while still allowing a confirmed steady quantum to replace a stale device/backend hint.
  m_backend_period_frames.store(num_frames, std::memory_order_relaxed);
  DEV_LOG("Low Latency confirmed backend callback period: {} frames (previous hint={} frames).", num_frames,
          previous_period);

  const u32 safe_target = std::min(GetAlignedBufferSize(num_frames * 2u), LowLatencyGetMaximumTargetFrames());
  const u32 current_target = LowLatencyGetEffectiveTargetFrames();
  if (safe_target <= current_target)
    return;

  m_low_latency_effective_target_frames.store(safe_target, std::memory_order_relaxed);
  m_filling.store(true, std::memory_order_relaxed);
  m_low_latency_reset_requested.store(true, std::memory_order_release);
  DEV_LOG("Low Latency backend floor increased target from {} to {} frames (confirmed callback={} frames).",
          current_target, safe_target, num_frames);
}

void AudioStream::LowLatencyHandleUnderrun(u32 available_frames, u32 requested_frames)
{
  static constexpr float UNDERRUN_WINDOW_SECONDS = 8.0f;
  static constexpr u32 UNDERRUNS_TO_PROMOTE = 3;

  // Deep underruns normally indicate a long host-side stall rather than a steady-state queue that is slightly
  // too small. Only shallow underruns contribute to adaptive promotion.
  if (requested_frames == 0 || available_frames < (requested_frames / 2u))
  {
    m_low_latency_underrun_window_start_time = 0;
    m_low_latency_underruns_in_window = 0;
    DEV_LOG("Low Latency transient stall ignored for promotion (available={} requested={} target={}).",
            available_frames, requested_frames, LowLatencyGetEffectiveTargetFrames());
    return;
  }

  const u64 now = Common::Timer::GetCurrentValue();
  if (m_low_latency_underrun_window_start_time == 0 ||
      Common::Timer::ConvertValueToSeconds(now - m_low_latency_underrun_window_start_time) > UNDERRUN_WINDOW_SECONDS)
  {
    m_low_latency_underrun_window_start_time = now;
    m_low_latency_underruns_in_window = 1;
    return;
  }

  m_low_latency_underruns_in_window++;
  if (m_low_latency_underruns_in_window < UNDERRUNS_TO_PROMOTE)
    return;

  const u32 quantum = LowLatencyGetBackendQuantumFrames();
  const u32 current_target = LowLatencyGetEffectiveTargetFrames();
  const u32 maximum_target = LowLatencyGetMaximumTargetFrames();
  const u32 new_target = (quantum > 0) ?
                           std::min(GetAlignedBufferSize(current_target + quantum), maximum_target) :
                           current_target;

  if (new_target > current_target)
  {
    m_low_latency_effective_target_frames.store(new_target, std::memory_order_relaxed);
    m_low_latency_adaptive_promotions++;
    DEV_LOG("Low Latency adaptive promotion #{}: target {} -> {} frames.",
            m_low_latency_adaptive_promotions, current_target, new_target);
  }

  m_low_latency_underrun_window_start_time = now;
  m_low_latency_underruns_in_window = 0;
}

void AudioStream::LowLatencyReset()
{
  m_low_latency_source_pos = 0.0;
  m_low_latency_correction = 1.0f;
  m_low_latency_integral = 0.0f;
  m_low_latency_filtered_error = 0.0f;
}

void AudioStream::LowLatencyWriteBlock(const float* block)
{
  if (m_low_latency_reset_requested.exchange(false, std::memory_order_acq_rel))
    LowLatencyReset();

  static constexpr u32 MAX_OUTPUT_FRAMES = CHUNK_SIZE * 2;
  const double rate =
    static_cast<double>(std::clamp(m_nominal_rate * m_low_latency_correction, 0.05f, 50.0f));

  double source_pos = m_low_latency_source_pos;
  u32 output_frames = 0;

  while (source_pos < static_cast<double>(CHUNK_SIZE - 1) && output_frames < MAX_OUTPUT_FRAMES)
  {
    const s32 source_index = static_cast<s32>(std::floor(source_pos));
    const float fraction = static_cast<float>(source_pos - static_cast<double>(source_index));

    for (u32 ch = 0; ch < m_internal_channels; ch++)
    {
      const float sample0 = (source_index < 0) ?
                              m_low_latency_last_frame[ch] :
                              block[(static_cast<u32>(source_index) * m_internal_channels) + ch];
      const float sample1 = (source_index < 0) ?
                              block[ch] :
                              block[((static_cast<u32>(source_index) + 1) * m_internal_channels) + ch];
      const float sample = sample0 + ((sample1 - sample0) * fraction);
      m_staging_buffer[(output_frames * m_internal_channels) + ch] =
        static_cast<s16>(std::clamp(sample * 32767.0f, -32768.0f, 32767.0f));
    }

    output_frames++;
    source_pos += rate;
  }

  source_pos -= static_cast<double>(CHUNK_SIZE);
  m_low_latency_source_pos = source_pos;

  for (u32 ch = 0; ch < m_internal_channels; ch++)
    m_low_latency_last_frame[ch] = block[((CHUNK_SIZE - 1) * m_internal_channels) + ch];

  if (output_frames > 0)
    InternalWriteFrames(m_staging_buffer.get(), output_frames);

  LowLatencyUpdateRate();
}

void AudioStream::LowLatencyUpdateRate()
{
  static constexpr float FILTER_ALPHA = 0.05f;
  static constexpr float PROPORTIONAL_GAIN = 0.020f;
  static constexpr float INTEGRAL_GAIN = 0.050f;
  static constexpr float MAX_INTEGRAL = 0.005f;
  static constexpr float MAX_CORRECTION = 0.030f;
  static constexpr float ERROR_DEADBAND = 0.010f;
  static constexpr float MAX_SLEW_PER_BLOCK = 0.00005f;

  const u32 target_frames = LowLatencyGetEffectiveTargetFrames();
  if (target_frames == 0)
    return;

  if (m_filling.load(std::memory_order_relaxed))
  {
    m_low_latency_correction = 1.0f;
    m_low_latency_integral = 0.0f;
    m_low_latency_filtered_error = 0.0f;
    return;
  }

  const float target = static_cast<float>(target_frames);
  const float buffered = static_cast<float>(GetBufferedFramesRelaxed());
  const float error = (buffered - target) / target;

  m_low_latency_filtered_error += (error - m_low_latency_filtered_error) * FILTER_ALPHA;
  float filtered_error = m_low_latency_filtered_error;
  if (std::abs(filtered_error) < ERROR_DEADBAND)
    filtered_error = 0.0f;

  const float block_seconds = static_cast<float>(CHUNK_SIZE) / static_cast<float>(m_sample_rate);
  const float proposed_integral =
    std::clamp(m_low_latency_integral + (filtered_error * INTEGRAL_GAIN * block_seconds),
               -MAX_INTEGRAL, MAX_INTEGRAL);
  const float proposed_correction =
    1.0f + (filtered_error * PROPORTIONAL_GAIN) + proposed_integral;

  const bool saturated_high = (proposed_correction > (1.0f + MAX_CORRECTION) && filtered_error > 0.0f);
  const bool saturated_low = (proposed_correction < (1.0f - MAX_CORRECTION) && filtered_error < 0.0f);
  if (!saturated_high && !saturated_low)
    m_low_latency_integral = proposed_integral;

  const float target_correction =
    std::clamp(1.0f + (filtered_error * PROPORTIONAL_GAIN) + m_low_latency_integral,
               1.0f - MAX_CORRECTION, 1.0f + MAX_CORRECTION);
  const float delta =
    std::clamp(target_correction - m_low_latency_correction, -MAX_SLEW_PER_BLOCK, MAX_SLEW_PER_BLOCK);
  m_low_latency_correction += delta;
}

float AudioStream::AddAndGetAverageTempo(float val)
{
  if (m_stretch_reset >= STRETCH_RESET_THRESHOLD)
    m_average_available = 0;
  if (m_average_available < AVERAGING_BUFFER_SIZE)
    m_average_available++;

  m_average_fullness[m_average_position] = val;
  m_average_position = (m_average_position + 1U) % AVERAGING_BUFFER_SIZE;

  const u32 actual_window = std::min<u32>(m_average_available, AVERAGING_WINDOW);
  const u32 first_index = (m_average_position - actual_window + AVERAGING_BUFFER_SIZE) % AVERAGING_BUFFER_SIZE;

  float sum = 0;
  for (u32 i = first_index; i < first_index + actual_window; i++)
    sum += m_average_fullness[i % AVERAGING_BUFFER_SIZE];
  sum = sum / actual_window;

  return (sum != 0.0f) ? sum : 1.0f;
}

void AudioStream::UpdateStretchTempo()
{
  static constexpr float MIN_TEMPO = 0.05f;
  static constexpr float MAX_TEMPO = 50.0f;

  // Which range we will run in 1:1 mode for.
  static constexpr float INACTIVE_GOOD_FACTOR = 1.04f;
  static constexpr float INACTIVE_BAD_FACTOR = 1.2f;
  static constexpr u32 INACTIVE_MIN_OK_COUNT = 50;
  static constexpr u32 COMPENSATION_DIVIDER = 100;

  float base_target_usage = static_cast<float>(m_target_buffer_size) * m_nominal_rate;

  // state vars
  if (m_stretch_reset >= STRETCH_RESET_THRESHOLD)
  {
    VERBOSE_LOG("___ Stretcher is being reset.");
    m_stretch_inactive = false;
    m_stretch_ok_count = 0;
    m_dynamic_target_usage = base_target_usage;
  }

  const u32 ibuffer_usage = GetBufferedFramesRelaxed();
  float buffer_usage = static_cast<float>(ibuffer_usage);
  float tempo = buffer_usage / m_dynamic_target_usage;
  tempo = AddAndGetAverageTempo(tempo);

  // Dampening when we get close to target.
  if (tempo < 2.0f)
    tempo = std::sqrt(tempo);

  tempo = std::clamp(tempo, MIN_TEMPO, MAX_TEMPO);

  if (tempo < 1.0f)
    base_target_usage /= std::sqrt(tempo);

  m_dynamic_target_usage +=
    static_cast<float>(base_target_usage / tempo - m_dynamic_target_usage) / static_cast<float>(COMPENSATION_DIVIDER);
  if (IsInRange(tempo, 0.9f, 1.1f) &&
      IsInRange(m_dynamic_target_usage, base_target_usage * 0.9f, base_target_usage * 1.1f))
  {
    m_dynamic_target_usage = base_target_usage;
  }

  if (!m_stretch_inactive)
  {
    if (IsInRange(tempo, 1.0f / INACTIVE_GOOD_FACTOR, INACTIVE_GOOD_FACTOR))
      m_stretch_ok_count++;
    else
      m_stretch_ok_count = 0;

    if (m_stretch_ok_count >= INACTIVE_MIN_OK_COUNT)
    {
      VERBOSE_LOG("=== Stretcher is now inactive.");
      m_stretch_inactive = true;
    }
  }
  else if (!IsInRange(tempo, 1.0f / INACTIVE_BAD_FACTOR, INACTIVE_BAD_FACTOR))
  {
    VERBOSE_LOG("~~~ Stretcher is now active @ tempo {}.", tempo);
    m_stretch_inactive = false;
    m_stretch_ok_count = 0;
  }

  if (m_stretch_inactive)
    tempo = m_nominal_rate;

  if constexpr (LOG_TIMESTRETCH_STATS)
  {
    static int iterations = 0;
    static u64 last_log_time = 0;

    const u64 now = Common::Timer::GetCurrentValue();

    if (Common::Timer::ConvertValueToSeconds(now - last_log_time) > 1.0f)
    {
      VERBOSE_LOG("buffers: {:4d} ms ({:3.0f}%), tempo: {}, comp: {:2.3f}, iters: {}, reset:{}",
                  (ibuffer_usage * 1000u) / m_sample_rate, 100.0f * buffer_usage / base_target_usage, tempo,
                  m_dynamic_target_usage / base_target_usage, iterations, m_stretch_reset);

      last_log_time = now;
      iterations = 0;
    }

    iterations++;
  }

  soundtouch_setTempo(m_soundtouch, tempo);

  if (m_stretch_reset >= STRETCH_RESET_THRESHOLD)
    m_stretch_reset = 0;
}

void AudioStream::StretchUnderrun()
{
  // Didn't produce enough frames in time.
  m_stretch_reset++;
}

void AudioStream::StretchOverrun()
{
  // Produced more frames than can fit in the buffer.
  m_stretch_reset++;

  // Drop two packets to give the time stretcher a bit more time to slow things down.
  const u32 discard = CHUNK_SIZE * 2;
  m_rpos.store((m_rpos.load(std::memory_order_acquire) + discard) % m_buffer_size, std::memory_order_release);
}

void AudioStreamParameters::Load(SettingsInterface& si, const char* section)
{
  stretch_mode =
    AudioStream::ParseStretchMode(
      si.GetStringValue(section, "StretchMode", AudioStream::GetStretchModeName(DEFAULT_STRETCH_MODE)).c_str())
      .value_or(DEFAULT_STRETCH_MODE);
#ifndef __ANDROID__
  expansion_mode =
    AudioStream::ParseExpansionMode(
      si.GetStringValue(section, "ExpansionMode", AudioStream::GetExpansionModeName(DEFAULT_EXPANSION_MODE)).c_str())
      .value_or(DEFAULT_EXPANSION_MODE);
#else
  expansion_mode = AudioExpansionMode::Disabled;
#endif
  output_latency_ms = static_cast<u16>(std::min<u32>(
    si.GetUIntValue(section, "OutputLatencyMS", DEFAULT_OUTPUT_LATENCY_MS), std::numeric_limits<u16>::max()));
  output_latency_minimal = si.GetBoolValue(section, "OutputLatencyMinimal", DEFAULT_OUTPUT_LATENCY_MINIMAL);
  wasapi_raw_output = si.GetBoolValue(section, "WasapiRawOutput", DEFAULT_WASAPI_RAW_OUTPUT);
  buffer_ms = static_cast<u16>(
    std::min<u32>(si.GetUIntValue(section, "BufferMS", DEFAULT_BUFFER_MS), std::numeric_limits<u16>::max()));
  low_latency_buffer_ms = static_cast<u16>(std::min<u32>(
    si.GetUIntValue(section, "LowLatencyBufferMS", DEFAULT_LOW_LATENCY_BUFFER_MS), std::numeric_limits<u16>::max()));

  stretch_sequence_length_ms =
    static_cast<u16>(std::min<u32>(si.GetUIntValue(section, "StretchSequenceLengthMS", DEFAULT_STRETCH_SEQUENCE_LENGTH),
                                   std::numeric_limits<u16>::max()));
  stretch_seekwindow_ms = static_cast<u16>(std::min<u32>(
    si.GetUIntValue(section, "StretchSeekWindowMS", DEFAULT_STRETCH_SEEKWINDOW), std::numeric_limits<u16>::max()));
  stretch_overlap_ms = static_cast<u16>(std::min<u32>(
    si.GetUIntValue(section, "StretchOverlapMS", DEFAULT_STRETCH_OVERLAP), std::numeric_limits<u16>::max()));
  stretch_use_quickseek = si.GetBoolValue(section, "StretchUseQuickSeek", DEFAULT_STRETCH_USE_QUICKSEEK);
  stretch_use_aa_filter = si.GetBoolValue(section, "StretchUseAAFilter", DEFAULT_STRETCH_USE_AA_FILTER);

  expand_block_size = static_cast<u16>(std::min<u32>(
    si.GetUIntValue(section, "ExpandBlockSize", DEFAULT_EXPAND_BLOCK_SIZE), std::numeric_limits<u16>::max()));
  expand_block_size = std::clamp<u16>(
    Common::IsPow2(expand_block_size) ? expand_block_size : Common::NextPow2(expand_block_size), 128, 8192);
  expand_circular_wrap =
    std::clamp(si.GetFloatValue(section, "ExpandCircularWrap", DEFAULT_EXPAND_CIRCULAR_WRAP), 0.0f, 360.0f);
  expand_shift = std::clamp(si.GetFloatValue(section, "ExpandShift", DEFAULT_EXPAND_SHIFT), -1.0f, 1.0f);
  expand_depth = std::clamp(si.GetFloatValue(section, "ExpandDepth", DEFAULT_EXPAND_DEPTH), 0.0f, 5.0f);
  expand_focus = std::clamp(si.GetFloatValue(section, "ExpandFocus", DEFAULT_EXPAND_FOCUS), -1.0f, 1.0f);
  expand_center_image =
    std::clamp(si.GetFloatValue(section, "ExpandCenterImage", DEFAULT_EXPAND_CENTER_IMAGE), 0.0f, 1.0f);
  expand_front_separation =
    std::clamp(si.GetFloatValue(section, "ExpandFrontSeparation", DEFAULT_EXPAND_FRONT_SEPARATION), 0.0f, 10.0f);
  expand_rear_separation =
    std::clamp(si.GetFloatValue(section, "ExpandRearSeparation", DEFAULT_EXPAND_REAR_SEPARATION), 0.0f, 10.0f);
  expand_low_cutoff =
    static_cast<u8>(std::min<u32>(si.GetUIntValue(section, "ExpandLowCutoff", DEFAULT_EXPAND_LOW_CUTOFF), 100));
  expand_high_cutoff =
    static_cast<u8>(std::min<u32>(si.GetUIntValue(section, "ExpandHighCutoff", DEFAULT_EXPAND_HIGH_CUTOFF), 100));
}

void AudioStreamParameters::Save(SettingsInterface& si, const char* section) const
{
  si.SetStringValue(section, "StretchMode", AudioStream::GetStretchModeName(stretch_mode));
  si.SetStringValue(section, "ExpansionMode", AudioStream::GetExpansionModeName(expansion_mode));
  si.SetUIntValue(section, "BufferMS", buffer_ms);
  si.SetUIntValue(section, "LowLatencyBufferMS", low_latency_buffer_ms);
  si.SetUIntValue(section, "OutputLatencyMS", output_latency_ms);
  si.SetBoolValue(section, "OutputLatencyMinimal", output_latency_minimal);
  si.SetBoolValue(section, "WasapiRawOutput", wasapi_raw_output);

  si.SetUIntValue(section, "StretchSequenceLengthMS", stretch_sequence_length_ms);
  si.SetUIntValue(section, "StretchSeekWindowMS", stretch_seekwindow_ms);
  si.SetUIntValue(section, "StretchOverlapMS", stretch_overlap_ms);
  si.SetBoolValue(section, "StretchUseQuickSeek", stretch_use_quickseek);
  si.SetBoolValue(section, "StretchUseAAFilter", stretch_use_aa_filter);

  si.SetUIntValue(section, "ExpandBlockSize", expand_block_size);
  si.SetFloatValue(section, "ExpandCircularWrap", expand_circular_wrap);
  si.SetFloatValue(section, "ExpandShift", expand_shift);
  si.SetFloatValue(section, "ExpandDepth", expand_depth);
  si.SetFloatValue(section, "ExpandFocus", expand_focus);
  si.SetFloatValue(section, "ExpandCenterImage", expand_center_image);
  si.SetFloatValue(section, "ExpandFrontSeparation", expand_front_separation);
  si.SetFloatValue(section, "ExpandRearSeparation", expand_rear_separation);
  si.SetUIntValue(section, "ExpandLowCutoff", expand_low_cutoff);
  si.SetUIntValue(section, "ExpandHighCutoff", expand_high_cutoff);
}

void AudioStreamParameters::Clear(SettingsInterface& si, const char* section)
{
  si.DeleteValue(section, "StretchMode");
  si.DeleteValue(section, "ExpansionMode");
  si.DeleteValue(section, "BufferMS");
  si.DeleteValue(section, "LowLatencyBufferMS");
  si.DeleteValue(section, "OutputLatencyMS");
  si.DeleteValue(section, "OutputLatencyMinimal");
  si.DeleteValue(section, "WasapiRawOutput");

  si.DeleteValue(section, "StretchSequenceLengthMS");
  si.DeleteValue(section, "StretchSeekWindowMS");
  si.DeleteValue(section, "StretchOverlapMS");
  si.DeleteValue(section, "StretchUseQuickSeek");
  si.DeleteValue(section, "StretchUseAAFilter");

  si.DeleteValue(section, "ExpandBlockSize");
  si.DeleteValue(section, "ExpandCircularWrap");
  si.DeleteValue(section, "ExpandShift");
  si.DeleteValue(section, "ExpandDepth");
  si.DeleteValue(section, "ExpandFocus");
  si.DeleteValue(section, "ExpandCenterImage");
  si.DeleteValue(section, "ExpandFrontSeparation");
  si.DeleteValue(section, "ExpandRearSeparation");
  si.DeleteValue(section, "ExpandLowCutoff");
  si.DeleteValue(section, "ExpandHighCutoff");
}

bool AudioStreamParameters::operator!=(const AudioStreamParameters& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) != 0);
}

bool AudioStreamParameters::operator==(const AudioStreamParameters& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) == 0);
}
