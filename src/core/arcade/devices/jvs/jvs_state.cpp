// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/jvs/jvs_bus.h"
#include "core/arcade/devices/jvs/jvs_chain.h"
#include "core/arcade/devices/jvs/namco_cyberlead.h"
#include "core/arcade/devices/jvs/namco_emio102.h"
#include "core/arcade/devices/jvs/namco_empri101.h"
#include "core/arcade/devices/jvs/namco_fca10.h"

#include "util/state_wrapper.h"

namespace Arcade::JVS {

bool Bus::DoState(StateWrapper& sw)
{
  sw.Do(&m_in_frame);
  sw.Do(&m_escape);
  sw.DoArray(m_packet.data(), m_packet.size());
  sw.Do(&m_packet_length);
  sw.DoArray(m_response.data(), m_response.size());
  sw.Do(&m_response_read);
  sw.Do(&m_response_length);
  return !sw.HasError() && m_packet_length <= m_packet.size() && m_response_length <= m_response.size() &&
         m_response_read <= m_response_length;
}

bool Node::DoNodeState(StateWrapper& sw)
{
  sw.Do(&m_address);
  return !sw.HasError();
}

// The test-switch mode is host configuration. Diagnostic counters are excluded.
bool NamcoFCA10::DoState(StateWrapper& sw)
{
  sw.Do(&m_test_latched);
  sw.Do(&m_test_previous);
  sw.Do(&m_address);
  sw.DoArray(m_coin_counter.data(), m_coin_counter.size());
  for (bool& previous : m_coin_previous)
    sw.Do(&previous);
  return !sw.HasError();
}

bool NamcoEMIO102::DoState(StateWrapper& sw)
{
  if (!DoNodeState(sw))
    return false;
  sw.DoArray(m_coin_counter.data(), m_coin_counter.size());
  for (bool& previous : m_coin_previous)
    sw.Do(&previous);
  sw.DoArray(m_outputs.data(), m_outputs.size());
  sw.Do(&m_test_latched);
  sw.Do(&m_test_previous);
  return !sw.HasError();
}

// A completed print job is delivered to the host callback; only the job being received is machine state.
bool NamcoEMPri101::DoState(StateWrapper& sw)
{
  if (!DoNodeState(sw))
    return false;
  u32 size = static_cast<u32>(m_job_data.size());
  sw.Do(&size);
  if (sw.IsReading())
  {
    if (sw.HasError() || size > 0x400000)
      return false;
    m_job_data.resize(size);
  }
  sw.DoBytes(m_job_data.data(), m_job_data.size());
  return !sw.HasError();
}

bool NamcoCyberLead::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("CyberLead"))
    return false;

  sw.Do(&m_address);
  sw.DoArray(m_coin_counter.data(), m_coin_counter.size());
  sw.DoArray(m_coin_used.data(), m_coin_used.size());
  for (bool& previous : m_coin_previous)
    sw.Do(&previous);

  Link& l = m_link;
  sw.Do(&l.state);
  sw.Do(&l.step);
  sw.Do(&l.clock);
  sw.Do(&l.wake_clock);
  sw.Do(&l.clock_fraction);
  sw.Do(&l.generation);
  sw.Do(&l.started_generation);
  sw.Do(&l.loop_counter);
  sw.Do(&l.attempts);
  sw.Do(&l.idle_countdown);
  sw.Do(&l.profile_result);
  sw.Do(&l.transaction_id);
  sw.Do(&l.transfer_result);
  sw.Do(&l.poll_count);
  sw.DoArray(l.control_data.data(), l.control_data.size());
  sw.DoArray(l.profile_id.data(), l.profile_id.size());
  sw.DoArray(l.block.data(), l.block.size());
  sw.DoArray(l.reply.data(), l.reply.size());
  sw.Do(&l.reply_size);
  sw.Do(&l.reply_state);
  sw.Do(&l.reply_remaining);
  sw.Do(&l.reply_previous);
  sw.Do(&l.reply_received);
  sw.Do(&l.handshake_complete);
  sw.Do(&l.initial_upload_complete);
  sw.Do(&l.upload_change_baseline);
  sw.Do(&l.flash_changed_since_handshake);
  sw.Do(&l.mode_since_last_change);
  sw.Do(&l.commit_count);
  sw.Do(&l.mode_count);
  sw.Do(&l.setup_closed_since_query);
  if (sw.IsReading() &&
      (sw.HasError() || l.state > LinkState::Mode || l.step > LinkStep::Evaluate || l.reply_size > l.reply.size()))
  {
    return false;
  }

  return m_led_link.DoState(sw) && m_led_device.DoState(sw);
}

} // namespace Arcade::JVS
