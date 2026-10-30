// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/audio/namco_c352.h"

#include "util/state_wrapper.h"

namespace Arcade::Audio {

// The mu-law table and sample ROM view are host configuration.
bool NamcoC352::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("C352"))
    return false;

  for (Voice& v : m_voices)
  {
    sw.Do(&v.pos);
    sw.Do(&v.counter);
    sw.Do(&v.sample);
    sw.Do(&v.last_sample);
    sw.Do(&v.vol_f);
    sw.Do(&v.vol_r);
    sw.DoArray(v.curr_vol.data(), v.curr_vol.size());
    sw.Do(&v.freq);
    sw.Do(&v.flags);
    sw.Do(&v.wave_bank);
    sw.Do(&v.wave_start);
    sw.Do(&v.wave_end);
    sw.Do(&v.wave_loop);
  }
  sw.Do(&m_random);
  sw.Do(&m_control);
  sw.Do(&m_active_mask);
  sw.Do(&m_pending_h8_clocks);
  for (StereoFrame& frame : m_audio_queue)
  {
    sw.Do(&frame.left);
    sw.Do(&frame.right);
  }
  u32 read = static_cast<u32>(m_queue_read), write = static_cast<u32>(m_queue_write);
  u32 count = static_cast<u32>(m_queue_count);
  sw.Do(&read);
  sw.Do(&write);
  sw.Do(&count);
  sw.Do(&m_host_primed);
  if (sw.IsReading())
  {
    if (sw.HasError() || read >= AUDIO_QUEUE_CAPACITY || write >= AUDIO_QUEUE_CAPACITY || count > AUDIO_QUEUE_CAPACITY)
    {
      return false;
    }
    m_queue_read = read;
    m_queue_write = write;
    m_queue_count = count;
  }

  return !sw.HasError();
}

} // namespace Arcade::Audio
