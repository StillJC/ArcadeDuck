#pragma once

#include "common/types.h"

#include <array>
#include <span>

namespace Arcade::Audio {

// Namco C352 - System 12 sound device.
//
// Hardware behavior is kept aligned with MAME's C352 core and ArcadeDuck's
// already-validated System 11 implementation. The System 12 H8 adapter is
// big-endian; the C352 core itself remains logical 16-bit.
//
// Timing integration:
//   H8/3002: 16,934,400 Hz
//   C352:    25,401,600 / 288 = 88,200 Hz
//   ratio:   exactly 192 H8 clocks per native C352 output frame
//
// AdvanceH8Clocks() is intentionally cheap: it only accumulates emulated H8
// time. Native C352 frames are generated in batches before C352 register
// accesses and before the host consumes audio. This preserves register-write
// ordering without executing the 32-voice mixer from inside every H8 step.
class NamcoC352
{
public:
  static constexpr u32 VOICE_COUNT = 32;
  static constexpr u64 H8_CLOCKS_PER_NATIVE_FRAME = UINT64_C(192);

  NamcoC352()
  {
    InitializeMuLaw();
    Reset();
  }

  void Reset()
  {
    m_voices = {};
    m_random = UINT16_C(0x1234);
    m_control = 0;
    m_active_mask = 0;

    m_pending_h8_clocks = 0;
    m_audio_queue = {};
    m_queue_read = 0;
    m_queue_write = 0;
    m_queue_count = 0;
    m_host_primed = false;
    m_sample_rom = {};
  }

  void AdvanceH8Clocks(u64 clocks, std::span<const u8> sample_rom)
  {
    // Keep this path tiny: it is called once per executed H8 instruction.
    m_sample_rom = sample_rom;
    m_pending_h8_clocks += clocks;
  }

  u8 ReadH8Byte(u32 byte_offset)
  {
    FlushPending();

    const u16 value = ReadRegister(byte_offset >> 1);
    // H8/3002 is big-endian: high byte at even address.
    return (byte_offset & 1) ? static_cast<u8>(value) : static_cast<u8>(value >> 8);
  }

  void WriteH8Byte(u32 byte_offset, u8 value)
  {
    // MAME updates the sound stream before every C352 register write. Flushing
    // accumulated H8 time here gives us the same ordering without per-step
    // mixer work.
    FlushPending();

    // Logical C352 offset 0x202 is the 16-bit key-on/key-off commit command.
    // H8 writes high byte at byte offset 0x404 and low byte at 0x405.
    if (byte_offset == UINT32_C(0x405))
    {
      ExecuteKeyOns();
      return;
    }
    if (byte_offset == UINT32_C(0x404))
      return;

    const u32 offset = byte_offset >> 1;
    u16 reg = ReadRegister(offset);

    if (byte_offset & 1)
      reg = static_cast<u16>((reg & UINT16_C(0xFF00)) | value);
    else
      reg = static_cast<u16>((reg & UINT16_C(0x00FF)) | (static_cast<u16>(value) << 8));

    WriteRegister(offset, reg);
  }

  void PopHostFrame(s32* left, s32* right)
  {
    if (!left || !right)
      return;

    *left = 0;
    *right = 0;

    FlushPending();

    // The H8 scheduler runs in ~1 ms slices while the SPU consumes at 44.1 kHz.
    // Keep a small emulated-time cushion so producer/consumer event ordering
    // cannot turn each scheduler boundary into a short burst of silence.
    if (!m_host_primed)
    {
      if (m_queue_count < HOST_PRIME_NATIVE_FRAMES)
        return;
      m_host_primed = true;
    }

    if (m_queue_count < 2)
    {
      // Lose priming only on a real internal starvation. We do not synthesize
      // samples; silence until the emulated stream has rebuilt its cushion.
      m_host_primed = false;
      return;
    }

    const StereoFrame a = PopNativeFrame();
    const StereoFrame b = PopNativeFrame();

    // Established ArcadeDuck System 11 host integration: 88.2 -> 44.1 kHz.
    *left = (static_cast<s32>(a.left) + static_cast<s32>(b.left)) / 2;
    *right = (static_cast<s32>(a.right) + static_cast<s32>(b.right)) / 2;
  }

private:
  static constexpr u16 FLAG_BUSY     = UINT16_C(0x8000);
  static constexpr u16 FLAG_KEYON    = UINT16_C(0x4000);
  static constexpr u16 FLAG_KEYOFF   = UINT16_C(0x2000);
  static constexpr u16 FLAG_LOOPTRG  = UINT16_C(0x1000);
  static constexpr u16 FLAG_LOOPHIST = UINT16_C(0x0800);
  static constexpr u16 FLAG_FM       = UINT16_C(0x0400);
  static constexpr u16 FLAG_PHASERL  = UINT16_C(0x0200);
  static constexpr u16 FLAG_PHASEFL  = UINT16_C(0x0100);
  static constexpr u16 FLAG_PHASEFR  = UINT16_C(0x0080);
  static constexpr u16 FLAG_LDIR     = UINT16_C(0x0040);
  static constexpr u16 FLAG_LINK     = UINT16_C(0x0020);
  static constexpr u16 FLAG_NOISE    = UINT16_C(0x0010);
  static constexpr u16 FLAG_MULAW    = UINT16_C(0x0008);
  static constexpr u16 FLAG_FILTER   = UINT16_C(0x0004);
  static constexpr u16 FLAG_LOOP     = UINT16_C(0x0002);
  static constexpr u16 FLAG_REVERSE  = UINT16_C(0x0001);

  struct Voice
  {
    u32 pos = 0;
    u32 counter = 0;
    s16 sample = 0;
    s16 last_sample = 0;

    u16 vol_f = 0;
    u16 vol_r = 0;
    std::array<u8, 4> curr_vol{};

    u16 freq = 0;
    u16 flags = 0;

    u16 wave_bank = 0;
    u16 wave_start = 0;
    u16 wave_end = 0;
    u16 wave_loop = 0;
  };

  struct StereoFrame
  {
    s16 left = 0;
    s16 right = 0;
  };

  // 16K native frames ~= 186 ms at 88.2 kHz. This is capacity, not normal
  // latency. Normal host priming is only 2 ms.
  static constexpr size_t AUDIO_QUEUE_CAPACITY = 16384;
  static constexpr size_t HOST_PRIME_NATIVE_FRAMES = 176;

  void InitializeMuLaw()
  {
    s32 value = 0;

    for (u32 i = 0; i < 128; i++)
    {
      m_mulaw[i] = static_cast<s16>(value << 5);

      if (i < 16)
        value += 1;
      else if (i < 24)
        value += 2;
      else if (i < 48)
        value += 4;
      else if (i < 100)
        value += 8;
      else
        value += 16;
    }

    for (u32 i = 0; i < 128; i++)
      m_mulaw[i + 128] =
        static_cast<s16>((~static_cast<u16>(m_mulaw[i])) & UINT16_C(0xFFE0));
  }

  u16 ReadRegister(u32 offset) const
  {
    if (offset < UINT32_C(0x100))
    {
      const Voice& voice = m_voices[offset / 8];

      switch (offset & 7)
      {
        case 0: return voice.vol_f;
        case 1: return voice.vol_r;
        case 2: return voice.freq;
        case 3: return voice.flags;
        case 4: return voice.wave_bank;
        case 5: return voice.wave_start;
        case 6: return voice.wave_end;
        case 7: return voice.wave_loop;
        default: return 0;
      }
    }

    if (offset == UINT32_C(0x200))
      return m_control;

    return 0;
  }

  void WriteRegister(u32 offset, u16 value)
  {
    if (offset < UINT32_C(0x100))
    {
      Voice& voice = m_voices[offset / 8];

      switch (offset & 7)
      {
        case 0: voice.vol_f = value; break;
        case 1: voice.vol_r = value; break;
        case 2: voice.freq = value; break;
        case 3: voice.flags = value; break;
        case 4: voice.wave_bank = value; break;
        case 5: voice.wave_start = value; break;
        case 6: voice.wave_end = value; break;
        case 7: voice.wave_loop = value; break;
        default: break;
      }
      return;
    }

    if (offset == UINT32_C(0x200))
      m_control = value;
  }

  static void RampVolume(u8& current, u8 target)
  {
    if (current < target)
      current++;
    else if (current > target)
      current--;
  }

  void FetchSample(Voice& voice)
  {
    voice.last_sample = voice.sample;

    if ((voice.flags & FLAG_NOISE) != 0)
    {
      const u16 feedback = (m_random & 1) ? UINT16_C(0xFFF6) : 0;
      m_random = static_cast<u16>((m_random >> 1) ^ feedback);
      voice.sample = static_cast<s16>(m_random);
      return;
    }

    const u32 address = voice.pos & UINT32_C(0x00FFFFFF);
    const u8 raw_sample =
      (address < m_sample_rom.size()) ? m_sample_rom[address] : UINT8_C(0x00);

    if ((voice.flags & FLAG_MULAW) != 0)
    {
      voice.sample = m_mulaw[raw_sample];
    }
    else
    {
      const s32 signed_sample =
        (raw_sample & UINT8_C(0x80)) ?
          (static_cast<s32>(raw_sample) - 0x100) :
          static_cast<s32>(raw_sample);
      voice.sample = static_cast<s16>(signed_sample * 0x100);
    }

    const u16 pos = static_cast<u16>(voice.pos);

    if ((voice.flags & FLAG_LOOP) != 0 && (voice.flags & FLAG_REVERSE) != 0)
    {
      if ((voice.flags & FLAG_LDIR) != 0 && pos == voice.wave_loop)
        voice.flags = static_cast<u16>(voice.flags & static_cast<u16>(~FLAG_LDIR));
      else if ((voice.flags & FLAG_LDIR) == 0 && pos == voice.wave_end)
        voice.flags = static_cast<u16>(voice.flags | FLAG_LDIR);

      voice.pos =
        (voice.pos + (((voice.flags & FLAG_LDIR) != 0) ?
                       UINT32_C(0x00FFFFFF) : UINT32_C(1))) &
        UINT32_C(0x00FFFFFF);
    }
    else if (pos == voice.wave_end)
    {
      if ((voice.flags & FLAG_LINK) != 0 && (voice.flags & FLAG_LOOP) != 0)
      {
        // C352 link format takes the new bank from wave_start's low byte.
        voice.pos =
          ((static_cast<u32>(voice.wave_start) & UINT32_C(0xFF)) << 16) |
          voice.wave_loop;
        voice.flags = static_cast<u16>(voice.flags | FLAG_LOOPHIST);
      }
      else if ((voice.flags & FLAG_LOOP) != 0)
      {
        voice.pos =
          (voice.pos & UINT32_C(0x00FF0000)) |
          static_cast<u32>(voice.wave_loop);
        voice.flags = static_cast<u16>(voice.flags | FLAG_LOOPHIST);
      }
      else
      {
        voice.flags =
          static_cast<u16>((voice.flags | FLAG_KEYOFF) &
                           static_cast<u16>(~FLAG_BUSY));
        voice.sample = 0;
      }
    }
    else
    {
      voice.pos =
        (voice.pos + (((voice.flags & FLAG_REVERSE) != 0) ?
                       UINT32_C(0x00FFFFFF) : UINT32_C(1))) &
        UINT32_C(0x00FFFFFF);
    }
  }

  void ExecuteKeyOns()
  {
    for (u32 index = 0; index < VOICE_COUNT; index++)
    {
      Voice& voice = m_voices[index];
      const u32 bit = UINT32_C(1) << index;

      if ((voice.flags & FLAG_KEYON) != 0)
      {
        voice.pos =
          ((static_cast<u32>(voice.wave_bank) << 16) |
           static_cast<u32>(voice.wave_start)) &
          UINT32_C(0x00FFFFFF);

        voice.sample = 0;
        voice.last_sample = 0;
        voice.counter = UINT32_C(0xFFFF);

        voice.flags =
          static_cast<u16>((voice.flags | FLAG_BUSY) &
                           static_cast<u16>(~(FLAG_KEYON | FLAG_LOOPHIST)));

        voice.curr_vol.fill(0);
        m_active_mask |= bit;
      }

      if ((voice.flags & FLAG_KEYOFF) != 0)
      {
        voice.flags =
          static_cast<u16>(voice.flags &
                           static_cast<u16>(~(FLAG_BUSY | FLAG_KEYOFF)));
        voice.counter = UINT32_C(0xFFFF);
        m_active_mask &= ~bit;
      }
    }
  }

  void GenerateNativeFrame(s16* left, s16* right)
  {
    s32 output_left = 0;
    s32 output_right = 0;

    u32 mask = m_active_mask;
    u32 index = 0;

    // Inactive C352 voices contribute exactly zero. Tracking the busy mask
    // avoids running the full PCM/interpolation path for all 32 channels on
    // every 88.2 kHz native frame.
    while (mask != 0 && index < VOICE_COUNT)
    {
      if ((mask & 1) != 0)
      {
        Voice& voice = m_voices[index];
        s32 sample = 0;

        if ((voice.flags & FLAG_BUSY) != 0)
        {
          const u32 next_counter = voice.counter + voice.freq;

          if ((next_counter & UINT32_C(0x10000)) != 0)
            FetchSample(voice);

          if (((next_counter ^ voice.counter) & UINT32_C(0x18000)) != 0)
          {
            RampVolume(voice.curr_vol[0], static_cast<u8>(voice.vol_f >> 8));
            RampVolume(voice.curr_vol[1], static_cast<u8>(voice.vol_f));
            RampVolume(voice.curr_vol[2], static_cast<u8>(voice.vol_r >> 8));
            RampVolume(voice.curr_vol[3], static_cast<u8>(voice.vol_r));
          }

          voice.counter = next_counter & UINT32_C(0xFFFF);

          // Match ArcadeDuck's validated System 11 path: if fetching the final
          // sample made the voice idle, do not feed a stale value to the DAC.
          if ((voice.flags & FLAG_BUSY) != 0)
          {
            sample = voice.sample;

            if ((voice.flags & FLAG_FILTER) == 0)
            {
              // IMPORTANT: the product can exceed signed 32-bit range.
              // Use signed 64-bit math exactly as the validated System 11
              // implementation does. The previous System 12 core used s32
              // multiplication here, which can overflow on large transients.
              const s32 difference =
                static_cast<s32>(voice.sample) -
                static_cast<s32>(voice.last_sample);

              sample =
                static_cast<s32>(voice.last_sample) +
                static_cast<s32>(
                  (static_cast<s64>(voice.counter) *
                   static_cast<s64>(difference)) >> 16);
            }
          }
        }

        if ((voice.flags & FLAG_BUSY) == 0)
          m_active_mask &= ~(UINT32_C(1) << index);

        const s32 left_sample =
          (voice.flags & FLAG_PHASEFL) ? -sample : sample;
        const s32 right_sample =
          (voice.flags & FLAG_PHASEFR) ? -sample : sample;

        output_left +=
          (left_sample * static_cast<s32>(voice.curr_vol[0])) >> 8;
        output_right +=
          (right_sample * static_cast<s32>(voice.curr_vol[1])) >> 8;
      }

      mask >>= 1;
      index++;
    }

    // MAME/System11 C352 front DAC scaling.
    *left = static_cast<s16>(output_left >> 3);
    *right = static_cast<s16>(output_right >> 3);
  }

  void FlushPending()
  {
    if (m_sample_rom.empty())
      return;

    while (m_pending_h8_clocks >= H8_CLOCKS_PER_NATIVE_FRAME)
    {
      m_pending_h8_clocks -= H8_CLOCKS_PER_NATIVE_FRAME;

      s16 left = 0;
      s16 right = 0;
      GenerateNativeFrame(&left, &right);
      PushNativeFrame(left, right);
    }
  }

  void PushNativeFrame(s16 left, s16 right)
  {
    if (m_queue_count == AUDIO_QUEUE_CAPACITY)
    {
      // Keep audio latency bounded if the host temporarily stops consuming.
      m_queue_read = (m_queue_read + 1) % AUDIO_QUEUE_CAPACITY;
      m_queue_count--;
    }

    m_audio_queue[m_queue_write] = {left, right};
    m_queue_write = (m_queue_write + 1) % AUDIO_QUEUE_CAPACITY;
    m_queue_count++;
  }

  StereoFrame PopNativeFrame()
  {
    if (m_queue_count == 0)
      return {};

    const StereoFrame result = m_audio_queue[m_queue_read];
    m_queue_read = (m_queue_read + 1) % AUDIO_QUEUE_CAPACITY;
    m_queue_count--;
    return result;
  }

  std::array<Voice, VOICE_COUNT> m_voices{};
  std::array<s16, 256> m_mulaw{};

  u16 m_random = UINT16_C(0x1234);
  u16 m_control = 0;
  u32 m_active_mask = 0;

  std::span<const u8> m_sample_rom{};
  u64 m_pending_h8_clocks = 0;

  std::array<StereoFrame, AUDIO_QUEUE_CAPACITY> m_audio_queue{};
  size_t m_queue_read = 0;
  size_t m_queue_write = 0;
  size_t m_queue_count = 0;
  bool m_host_primed = false;
};

} // namespace Arcade::Audio