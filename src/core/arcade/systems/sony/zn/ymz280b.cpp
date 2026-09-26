// SPDX-FileCopyrightText: 2026 StillJC

// SPDX-License-Identifier: GPL-3.0-only



#include "core/arcade/systems/sony/zn/ymz280b.h"



#include "common/log.h"



Log_SetChannel(SonyZNYMZ280B);



namespace SonyZN {



static constexpr std::array<u16, 8> ADPCM_INDEX_SCALE = {

  UINT16_C(0x0e6), UINT16_C(0x0e6), UINT16_C(0x0e6), UINT16_C(0x0e6),

  UINT16_C(0x133), UINT16_C(0x199), UINT16_C(0x200), UINT16_C(0x266),

};



void YMZ280B::Initialize(const std::vector<u8>& rom, u32 clock_hz, IRQCallback irq_callback, void* irq_userdata)

{

  m_rom = rom;

  m_clock_hz = clock_hz;

  m_native_sample_rate = clock_hz / 192;

  m_irq_callback = irq_callback;

  m_irq_userdata = irq_userdata;

  Reset();

}



void YMZ280B::Reset()

{

  if (m_irq_state && m_irq_callback)

    m_irq_callback(m_irq_userdata, false);



  m_voice = {};

  for (Voice& voice : m_voice)

  {

    voice.step = 0x7f;

    voice.loop_step = 0x7f;

    voice.output_step = 1;

    voice.output_pos = FRAC_ONE;

  }



  m_register = 0;

  m_status = 0;

  m_irq_mask = 0;

  m_irq_enable = false;

  m_irq_state = false;

  m_keyon_enable = false;

  m_ext_mem_enable = false;

  m_ext_mem_address = 0;

  m_ext_mem_address_hi = 0;

  m_ext_mem_address_mid = 0;

  m_ext_read_latch = 0;

  m_native_phase = 0;

  m_last_left = 0;

  m_last_right = 0;

  m_unknown_register_logged = false;

}



void YMZ280B::Shutdown()

{

  if (m_irq_state && m_irq_callback)

    m_irq_callback(m_irq_userdata, false);



  m_rom.clear();

  m_clock_hz = 0;

  m_native_sample_rate = 0;

  m_irq_callback = nullptr;

  m_irq_userdata = nullptr;

  Reset();

}



void YMZ280B::SetIRQCallback(IRQCallback callback, void* userdata)

{

  if (m_irq_callback && m_irq_state)

    m_irq_callback(m_irq_userdata, false);



  m_irq_callback = callback;

  m_irq_userdata = userdata;

  if (m_irq_callback)

    m_irq_callback(m_irq_userdata, m_irq_state);

}



u8 YMZ280B::ReadROMByte(u32 address) const

{

  address &= UINT32_C(0x00ffffff);

  return (address < m_rom.size()) ? m_rom[address] : UINT8_C(0xff);

}



void YMZ280B::UpdateIRQState()

{

  const bool new_state = m_irq_enable && ((m_status & m_irq_mask) != 0);

  if (new_state == m_irq_state)

    return;



  m_irq_state = new_state;

  if (m_irq_callback)

    m_irq_callback(m_irq_userdata, m_irq_state);

}



void YMZ280B::UpdateVoiceStep(Voice& voice)

{

  const u16 mask = (voice.mode == 1) ? UINT16_C(0x00ff) : UINT16_C(0x01ff);

  voice.output_step = static_cast<u16>((voice.fnum & mask) + 1);

}



void YMZ280B::UpdateVoiceVolumes(Voice& voice)

{

  if (voice.pan == 8)

  {

    voice.output_left = voice.level;

    voice.output_right = voice.level;

  }

  else if (voice.pan < 8)

  {

    voice.output_left = voice.level;

    voice.output_right = (voice.pan == 0) ? 0 : static_cast<u16>((voice.level * (voice.pan - 1)) / 7);

  }

  else

  {

    voice.output_left = static_cast<u16>((voice.level * (15 - voice.pan)) / 7);

    voice.output_right = voice.level;

  }

}



void YMZ280B::StartVoice(Voice& voice)

{

  voice.playing = true;

  voice.position = voice.start;

  voice.signal = 0;

  voice.step = 0x7f;

  voice.loop_signal = 0;

  voice.loop_step = 0x7f;

  voice.loop_state_captured = false;

  voice.output_pos = FRAC_ONE;

  voice.last_sample = 0;

  voice.curr_sample = 0;

}



void YMZ280B::EndVoice(u32 index)

{

  Voice& voice = m_voice[index];

  if (!voice.playing)

    return;



  voice.playing = false;

  m_status |= static_cast<u8>(1u << index);

  UpdateIRQState();

}



s16 YMZ280B::DecaySample(s16 sample) const

{

  s32 value = sample;

  if (value < 0)

    value = -((-value * 15) >> 4);

  else if (value > 0)

    value = (value * 15) >> 4;



  if (value > -2 && value < 2)

    value = 0;

  return static_cast<s16>(value);

}



s16 YMZ280B::DecodeADPCM(u32 index, Voice& voice)

{

  const u8 packed = ReadROMByte(voice.position >> 1);

  const u8 nibble = static_cast<u8>((voice.position & 1) ? (packed & 0x0f) : (packed >> 4));

  const s32 magnitude = static_cast<s32>((nibble & 0x07) * 2 + 1);

  const s32 difference = (nibble & 0x08) ? -magnitude : magnitude;



  voice.signal += (voice.step * difference) / 8;

  if (voice.signal > 32767)

    voice.signal = 32767;

  else if (voice.signal < -32768)

    voice.signal = -32768;



  voice.step = (voice.step * ADPCM_INDEX_SCALE[nibble & 0x07]) >> 8;

  if (voice.step > 0x6000)

    voice.step = 0x6000;

  else if (voice.step < 0x7f)

    voice.step = 0x7f;



  const s16 result = static_cast<s16>(voice.signal);

  voice.position++;



  if (voice.looping)

  {

    if (voice.position == voice.loop_start && !voice.loop_state_captured)

    {

      voice.loop_signal = voice.signal;

      voice.loop_step = voice.step;

      voice.loop_state_captured = true;

    }



    if (voice.position >= voice.loop_end && voice.keyon && voice.loop_end > voice.loop_start)

    {

      voice.position = voice.loop_start;

      voice.signal = voice.loop_signal;

      voice.step = voice.loop_step;

    }

  }



  if (voice.position >= voice.stop)

    EndVoice(index);



  return result;

}



s16 YMZ280B::DecodePCM8(u32 index, Voice& voice)

{

  const u8 value = ReadROMByte(voice.position >> 1);

  const s16 result = static_cast<s16>(static_cast<s8>(value)) * 256;

  voice.position += 2;



  if (voice.looping && voice.position >= voice.loop_end && voice.keyon && voice.loop_end > voice.loop_start)

    voice.position = voice.loop_start;



  if (voice.position >= voice.stop)

    EndVoice(index);



  return result;

}



s16 YMZ280B::DecodePCM16(u32 index, Voice& voice)

{

  const u32 address = voice.position >> 1;

  const u16 value = static_cast<u16>(ReadROMByte(address)) |

                    static_cast<u16>(static_cast<u16>(ReadROMByte(address + 1)) << 8);

  const s16 result = static_cast<s16>(value);

  voice.position += 4;



  if (voice.looping && voice.position >= voice.loop_end && voice.keyon && voice.loop_end > voice.loop_start)

    voice.position = voice.loop_start;



  if (voice.position >= voice.stop)

    EndVoice(index);



  return result;

}



s16 YMZ280B::NextSourceSample(u32 index, Voice& voice)

{

  if (!voice.playing)

    return DecaySample(voice.curr_sample);



  if (voice.position >= voice.stop)

  {

    EndVoice(index);

    return DecaySample(voice.curr_sample);

  }



  switch (voice.mode)

  {

    case 1:

      return DecodeADPCM(index, voice);



    case 2:

      return DecodePCM8(index, voice);



    case 3:

      return DecodePCM16(index, voice);



    default:

      EndVoice(index);

      return 0;

  }

}



s32 YMZ280B::GenerateVoiceSample(u32 index, Voice& voice)

{

  if (!voice.playing && voice.curr_sample == 0 && voice.last_sample == 0)

  {

    voice.output_pos = FRAC_ONE;

    return 0;

  }



  if (voice.output_pos >= FRAC_ONE)

  {

    voice.output_pos -= FRAC_ONE;

    voice.last_sample = voice.curr_sample;

    voice.curr_sample = NextSourceSample(index, voice);

  }



  const s32 sample =

    ((static_cast<s32>(voice.last_sample) * static_cast<s32>(FRAC_ONE - voice.output_pos)) +

     (static_cast<s32>(voice.curr_sample) * static_cast<s32>(voice.output_pos))) >>

    FRAC_BITS;

  voice.output_pos += voice.output_step;

  return sample;

}



void YMZ280B::GenerateNativeSample()

{

  s32 left = 0;

  s32 right = 0;



  for (u32 index = 0; index < m_voice.size(); index++)

  {

    Voice& voice = m_voice[index];

    const s32 sample = GenerateVoiceSample(index, voice);

    left += (sample * voice.output_left) / 512;

    right += (sample * voice.output_right) / 512;

  }



  m_last_left = left;

  m_last_right = right;

}



void YMZ280B::AdvanceCycles(u32 source_cycles, u32 source_clock_hz)

{

  if (source_cycles == 0 || source_clock_hz == 0 || m_native_sample_rate == 0)

    return;



  m_native_phase += static_cast<u64>(source_cycles) * m_native_sample_rate;

  while (m_native_phase >= source_clock_hz)

  {

    m_native_phase -= source_clock_hz;

    GenerateNativeSample();

  }

}



void YMZ280B::WriteAddress(u8 value)

{

  m_register = value;

}



void YMZ280B::WriteData(u8 value)

{

  WriteRegister(m_register, value);

}



u8 YMZ280B::ReadData()

{

  if (!m_ext_mem_enable)

    return UINT8_C(0xff);



  const u8 result = m_ext_read_latch;

  m_ext_read_latch = ReadROMByte(m_ext_mem_address);

  m_ext_mem_address = (m_ext_mem_address + 1) & UINT32_C(0x00ffffff);

  return result;

}



u8 YMZ280B::ReadStatus()

{

  const u8 result = m_status;

  m_status = 0;

  UpdateIRQState();

  return result;

}



void YMZ280B::GetOutput(s32* left, s32* right) const

{

  if (left)

    *left = m_last_left;

  if (right)

    *right = m_last_right;

}



void YMZ280B::WriteRegister(u8 reg, u8 value)

{

  if (reg < 0x80)

  {

    Voice& voice = m_voice[(reg >> 2) & 7];



    switch (reg & 0xe3)

    {

      case 0x00:

        voice.fnum = static_cast<u16>((voice.fnum & 0x100) | value);

        UpdateVoiceStep(voice);

        return;



      case 0x01:

      {

        voice.fnum = static_cast<u16>((voice.fnum & 0x00ff) | ((value & 0x01) << 8));

        voice.looping = (value & 0x10) != 0;



        u8 control = value;

        if ((control & 0x60) == 0)

          control &= 0x7f;

        else

          voice.mode = static_cast<u8>((control >> 5) & 0x03);



        const bool new_keyon = (control & 0x80) != 0;

        if (!voice.keyon && new_keyon && m_keyon_enable)

          StartVoice(voice);

        else if (voice.keyon && !new_keyon)

          voice.playing = false;



        voice.keyon = new_keyon;

        UpdateVoiceStep(voice);

        return;

      }



      case 0x02:

        voice.level = value;

        UpdateVoiceVolumes(voice);

        return;



      case 0x03:

        voice.pan = value & 0x0f;

        UpdateVoiceVolumes(voice);

        return;



      case 0x20:

        voice.start = (voice.start & (UINT32_C(0x00ffff) << 1)) | (static_cast<u32>(value) << 17);

        return;



      case 0x21:

        voice.loop_start = (voice.loop_start & (UINT32_C(0x00ffff) << 1)) | (static_cast<u32>(value) << 17);

        return;



      case 0x22:

        voice.loop_end = (voice.loop_end & (UINT32_C(0x00ffff) << 1)) | (static_cast<u32>(value) << 17);

        return;



      case 0x23:

        voice.stop = (voice.stop & (UINT32_C(0x00ffff) << 1)) | (static_cast<u32>(value) << 17);

        return;



      case 0x40:

        voice.start = (voice.start & (UINT32_C(0xff00ff) << 1)) | (static_cast<u32>(value) << 9);

        return;



      case 0x41:

        voice.loop_start = (voice.loop_start & (UINT32_C(0xff00ff) << 1)) | (static_cast<u32>(value) << 9);

        return;



      case 0x42:

        voice.loop_end = (voice.loop_end & (UINT32_C(0xff00ff) << 1)) | (static_cast<u32>(value) << 9);

        return;



      case 0x43:

        voice.stop = (voice.stop & (UINT32_C(0xff00ff) << 1)) | (static_cast<u32>(value) << 9);

        return;



      case 0x60:

        voice.start = (voice.start & (UINT32_C(0xffff00) << 1)) | (static_cast<u32>(value) << 1);

        return;



      case 0x61:

        voice.loop_start = (voice.loop_start & (UINT32_C(0xffff00) << 1)) | (static_cast<u32>(value) << 1);

        return;



      case 0x62:

        voice.loop_end = (voice.loop_end & (UINT32_C(0xffff00) << 1)) | (static_cast<u32>(value) << 1);

        return;



      case 0x63:

        voice.stop = (voice.stop & (UINT32_C(0xffff00) << 1)) | (static_cast<u32>(value) << 1);

        return;



      default:

        break;

    }

  }

  else

  {

    switch (reg)

    {

      case 0x80:

      case 0x81:

      case 0x82:

        return;



      case 0x84:

        m_ext_mem_address_hi = static_cast<u32>(value) << 16;

        return;



      case 0x85:

        m_ext_mem_address_mid = static_cast<u32>(value) << 8;

        return;



      case 0x86:

        m_ext_mem_address =

          (m_ext_mem_address_hi | m_ext_mem_address_mid | value) & UINT32_C(0x00ffffff);

        if (m_ext_mem_enable)

          m_ext_read_latch = ReadROMByte(m_ext_mem_address);

        return;



      case 0x87:

        if (m_ext_mem_enable)

          m_ext_mem_address = (m_ext_mem_address + 1) & UINT32_C(0x00ffffff);

        return;



      case 0xfe:

        m_irq_mask = value;

        UpdateIRQState();

        return;



      case 0xff:

      {

        const bool new_keyon_enable = (value & 0x80) != 0;

        m_ext_mem_enable = (value & 0x40) != 0;

        m_irq_enable = (value & 0x10) != 0;

        UpdateIRQState();



        if (m_keyon_enable && !new_keyon_enable)

        {

          for (Voice& voice : m_voice)

            voice.playing = false;

        }

        else if (!m_keyon_enable && new_keyon_enable)

        {

          for (Voice& voice : m_voice)

          {

            if (voice.keyon && voice.looping)

              StartVoice(voice);

          }

        }



        m_keyon_enable = new_keyon_enable;

        return;

      }



      default:

        break;

    }

  }



  if (value != 0 && !m_unknown_register_logged)

  {

    m_unknown_register_logged = true;

    DEV_LOG("YMZ280B first unsupported register write reg=0x{:02X} value=0x{:02X}", reg, value);

  }

}



} // namespace SonyZN