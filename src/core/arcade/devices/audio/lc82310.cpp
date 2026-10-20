// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-FileCopyrightText: windyfairy
// SPDX-License-Identifier: GPL-3.0-only
//
// Control-port protocol, buffer behaviour and volume table follow MAME's
// BSD-3-Clause lc82310.cpp. See THIRD-PARTY-LICENSES.txt.

#include "core/arcade/devices/audio/lc82310.h"

// The decoder holds a 4 KiB stream buffer, too small for minimp3's default of
// ten consecutive frames when synchronising (as configured by MAME).
#define MAX_FRAME_SYNC_MATCHES 2
#define MINIMP3_IMPLEMENTATION
#include "core/arcade/third_party/minimp3/minimp3.h"

#include <algorithm>
#include <cstring>

namespace Arcade {
namespace {
constexpr u8 COMMAND_VOLUME_LEFT = 0x13;
constexpr u8 COMMAND_VOLUME_RIGHT = 0x15;

// Attenuation steps 0-76 as measured against the game's volume settings.
constexpr std::array<float, 77> GAIN_TABLE = {
  1.0f,          30.0f / 31.0f, 29.0f / 31.0f, 28.0f / 31.0f, 27.0f / 31.0f, 26.0f / 31.0f, 25.0f / 31.0f,
  24.0f / 31.0f, 23.0f / 31.0f, 22.0f / 31.0f, 21.0f / 31.0f, 20.0f / 31.0f, 19.0f / 31.0f, 18.0f / 31.0f,
  17.0f / 31.0f, 16.0f / 31.0f, 15.0f / 31.0f, 14.0f / 31.0f, 13.0f / 31.0f, 12.0f / 31.0f, 11.0f / 31.0f,
  10.0f / 31.0f, 9.0f / 31.0f,  8.5f / 31.0f,  8.0f / 31.0f,  7.0f / 31.0f,  6.0f / 31.0f,  5.5f / 31.0f,
  5.0f / 31.0f,  4.5f / 31.0f,  4.0f / 31.0f,  3.5f / 31.0f,  3.0f / 31.0f,  2.75f / 31.0f, 2.5f / 31.0f,
  2.25f / 31.0f, 2.0f / 31.0f,
  (1.0f + 5.0f / 6.0f) / 31.0f, (1.0f + 4.0f / 6.0f) / 31.0f, (1.0f + 3.0f / 6.0f) / 31.0f,
  (1.0f + 2.0f / 6.0f) / 31.0f, (1.0f + 1.0f / 6.0f) / 31.0f, 1.0f / 31.0f,
  33.0f / 34.0f / 31.0f, 32.0f / 34.0f / 31.0f, 31.0f / 34.0f / 31.0f, 30.0f / 34.0f / 31.0f,
  29.0f / 34.0f / 31.0f, 28.0f / 34.0f / 31.0f, 27.0f / 34.0f / 31.0f, 26.0f / 34.0f / 31.0f,
  25.0f / 34.0f / 31.0f, 24.0f / 34.0f / 31.0f, 23.0f / 34.0f / 31.0f, 22.0f / 34.0f / 31.0f,
  21.0f / 34.0f / 31.0f, 20.0f / 34.0f / 31.0f, 19.0f / 34.0f / 31.0f, 18.0f / 34.0f / 31.0f,
  17.0f / 34.0f / 31.0f, 16.0f / 34.0f / 31.0f, 15.0f / 34.0f / 31.0f, 14.0f / 34.0f / 31.0f,
  13.0f / 34.0f / 31.0f, 12.0f / 34.0f / 31.0f, 11.0f / 34.0f / 31.0f, 10.0f / 34.0f / 31.0f,
  9.0f / 34.0f / 31.0f,  8.0f / 34.0f / 31.0f,  7.0f / 34.0f / 31.0f,  6.0f / 34.0f / 31.0f,
  5.0f / 34.0f / 31.0f,  4.0f / 34.0f / 31.0f,  3.0f / 34.0f / 31.0f,  2.0f / 34.0f / 31.0f,
  1.0f / 34.0f / 31.0f,  0.0f,
};
} // namespace

struct LC82310::Decoder
{
  mp3dec_t state;
};

LC82310::LC82310() : m_decoder(std::make_unique<Decoder>())
{
  Reset();
}

LC82310::~LC82310() = default;

void LC82310::Reset()
{
  m_gain.fill(0.0f);
  m_cs = true;
  m_clock = false;
  m_di = false;
  m_do = false;
  m_accepting_parameter = false;
  m_command = 0;
  m_shift = 0;
  m_bits = 0;
  m_out_shift = 0;
  ResetPlayback();
}

void LC82310::ResetPlayback()
{
  mp3dec_init(&m_decoder->state);
  m_buffer.fill(0);
  m_buffer_count = 0;
  m_stream_found = false;
  m_frame_release = false;
  m_samples.fill(0);
  m_sample_count = 0;
  m_sample_index = 0;
  m_rate_phase = 0;
}

void LC82310::SetClock(bool state)
{
  // Data is sampled on the rising clock edge while chip select is low.
  if (!m_cs && !m_clock && state)
  {
    m_shift |= static_cast<u8>((m_di ? 1 : 0) << m_bits);
    if (++m_bits == 8)
    {
      if (!m_accepting_parameter)
      {
        // Read-back commands return their value while the parameter byte is
        // clocked in; no read-back register is modelled.
        m_out_shift = 0;
        m_command = m_shift;
        m_accepting_parameter = true;
      }
      else
      {
        HandleCommand(m_command, m_shift);
        m_accepting_parameter = false;
      }
      m_shift = 0;
      m_bits = 0;
    }

    m_do = (m_out_shift & 1) != 0;
    m_out_shift >>= 1;
  }
  m_clock = state;
}

void LC82310::HandleCommand(u8 command, u8 parameter)
{
  if (command == COMMAND_VOLUME_LEFT || command == COMMAND_VOLUME_RIGHT)
    m_gain[(command == COMMAND_VOLUME_RIGHT) ? 1 : 0] = GAIN_TABLE[std::min<size_t>(parameter, GAIN_TABLE.size() - 1)];
}

bool LC82310::GetDemand() const
{
  return m_buffer_count < BUFFER_SIZE;
}

bool LC82310::ReadDemand()
{
  // Hosts stream while DEMAND is high and service the decoder when it drops,
  // so it must also drop while the buffer has room or a host that has sent
  // all the data it holds would never get round to supplying more.
  if (m_frame_release)
  {
    m_frame_release = false;
    return false;
  }
  return GetDemand();
}

void LC82310::WriteData(u8 value)
{
  if (m_buffer_count == BUFFER_SIZE)
  {
    // A full buffer drops its oldest byte.
    std::memmove(m_buffer.data(), m_buffer.data() + 1, BUFFER_SIZE - 1);
    m_buffer_count--;
  }
  m_buffer[m_buffer_count++] = value;
}

// Decoding starts only once the buffer holds consecutive frames with matching
// headers, so stray bytes that look like a frame header are skipped.
bool LC82310::FindStream()
{
  constexpr int REQUIRED_MATCHES = MAX_FRAME_SYNC_MATCHES;
  const int limit = static_cast<int>(m_buffer_count);
  int free_format_bytes = 0;
  int frame_bytes = 0;
  const int offset = mp3d_find_frame(m_buffer.data(), limit, &free_format_bytes, &frame_bytes);
  if (frame_bytes == 0 || offset + frame_bytes > limit)
    return false;

  int position = offset;
  int matches = 0;
  for (; matches < REQUIRED_MATCHES; matches++)
  {
    position += hdr_frame_bytes(m_buffer.data() + position, frame_bytes) + hdr_padding(m_buffer.data() + position);
    if (position + HDR_SIZE > limit || !hdr_compare(m_buffer.data() + offset, m_buffer.data() + position))
      break;
  }
  return matches >= REQUIRED_MATCHES;
}

bool LC82310::DecodeFrame()
{
  if (m_buffer_count == 0)
    return false;

  if (!m_stream_found && !(m_stream_found = FindStream()))
  {
    // A full buffer without a stream is skipped one byte at a time.
    if (m_buffer_count == BUFFER_SIZE)
    {
      std::memmove(m_buffer.data(), m_buffer.data() + 1, BUFFER_SIZE - 1);
      m_buffer_count--;
    }
    return false;
  }

  mp3dec_frame_info_t info = {};
  const int samples = mp3dec_decode_frame(&m_decoder->state, m_buffer.data(), static_cast<int>(m_buffer_count),
                                          m_samples.data(), &info);
  if (info.frame_bytes == 0)
  {
    // Not enough data for a frame. A full buffer without a frame is skipped
    // one byte at a time.
    if (m_buffer_count == BUFFER_SIZE)
    {
      std::memmove(m_buffer.data(), m_buffer.data() + 1, BUFFER_SIZE - 1);
      m_buffer_count--;
    }
    return false;
  }

  const u32 consumed = std::min<u32>(static_cast<u32>(info.frame_bytes), m_buffer_count);
  std::memmove(m_buffer.data(), m_buffer.data() + consumed, m_buffer_count - consumed);
  m_buffer_count -= consumed;
  if (samples <= 0)
    return false;

  if (info.channels == 1)
  {
    for (int i = samples - 1; i >= 0; i--)
      m_samples[i * 2] = m_samples[i * 2 + 1] = m_samples[i];
  }
  m_frame_release = true;
  m_sample_count = static_cast<u32>(samples);
  m_sample_index = 0;
  m_frame_rate = (info.hz > 0) ? static_cast<u32>(info.hz) : OUTPUT_RATE;
  return true;
}

void LC82310::GenerateFrame(s32* left, s32* right)
{
  if (m_sample_index >= m_sample_count && !DecodeFrame())
  {
    *left = 0;
    *right = 0;
    return;
  }

  *left = static_cast<s32>(m_samples[m_sample_index * 2] * m_gain[0]);
  *right = static_cast<s32>(m_samples[m_sample_index * 2 + 1] * m_gain[1]);

  // Step through the frame at its own sample rate.
  m_rate_phase += m_frame_rate;
  while (m_rate_phase >= OUTPUT_RATE)
  {
    m_rate_phase -= OUTPUT_RATE;
    m_sample_index++;
  }
}

} // namespace Arcade
