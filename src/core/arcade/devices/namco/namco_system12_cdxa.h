// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/devices/cpu/sh7014.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

class CDImage;
class Error;
class StateWrapper;

namespace Arcade::Namco
{
class System12CDXA
{
public:
  bool DoState(StateWrapper& sw);

  static constexpr uint32_t PSX_SHARED_BASE = 0x007C0000u;
  static constexpr uint32_t PSX_SHARED_END = 0x007CFFFFu;
  static constexpr uint32_t PSX_CONTROL_BASE = 0x007D6000u;
  static constexpr uint32_t PSX_CONTROL_END = 0x007D601Fu;

  System12CDXA();
  ~System12CDXA();

  bool SetMedia(const char* path, Error* error);
  void Reset();
  uint64_t Run(uint64_t cycles);

  bool ReadPSX(uint32_t width, uint32_t exp1_offset, uint32_t& value) const;
  bool WritePSX(uint32_t width, uint32_t exp1_offset, uint32_t value);
  void PopAudioFrame(int32_t* left, int32_t* right);

  bool IsSH2Running() const { return m_sh2_running; }
  bool IsSRAMSelected() const { return m_sram_selected; }

  SH7014& GetSH7014() { return m_sh2; }
  const SH7014& GetSH7014() const { return m_sh2; }

private:
  static bool ReadShared(const uint8_t* data, size_t size, uint32_t width, uint32_t offset, uint32_t& value);
  static bool WriteShared(uint8_t* data, size_t size, uint32_t width, uint32_t offset, uint32_t value);

  ArcadeDuck::SH2::CPU::Bus MakeSH2ExternalBus();
  uint8_t SH2ExternalRead8(uint32_t address) const;
  uint16_t SH2ExternalRead16(uint32_t address) const;
  uint32_t SH2ExternalRead32(uint32_t address) const;
  void SH2ExternalWrite8(uint32_t address, uint8_t value);
  void SH2ExternalWrite16(uint32_t address, uint16_t value);
  void SH2ExternalWrite32(uint32_t address, uint32_t value);

  void ResetATAPI();
  uint16_t ReadATACS0(uint32_t address) const;
  void WriteATACS0(uint32_t address, uint16_t value);
  uint16_t ReadATACS1(uint32_t address) const;
  void WriteATACS1(uint32_t address, uint16_t value);
  uint16_t ReadATAData() const;
  void WriteATAData(uint16_t value);
  void ExecuteATACommand(uint8_t command);
  void ExecutePacketCommand();
  void BeginATAPIDataIn(const uint8_t* data, size_t size);
  void CompleteATAPICommand();

  void HandleSCI0Transmit(uint8_t value);
  void HandlePortBWrite(uint16_t value);
  void WriteVolumeControl(uint8_t encoded_pins);
  void RecalculateVolumeGain(uint32_t channel);
  void PushAudioFrame(int16_t left, int16_t right);

  struct StereoFrame
  {
    int16_t left = 0;
    int16_t right = 0;
  };

  static constexpr uint32_t HOST_AUDIO_RATE_HZ = 44'100u;
  static constexpr uint32_t CDXA_SCI_BYTES_PER_SECOND = 151'200u;
  static constexpr size_t AUDIO_QUEUE_CAPACITY = 4096;
  // Keep enough native 37.8 kHz CDXA PCM queued to bridge the host mixer's
  // coarse output batches. A shallow prime repeatedly drains between SH7014
  // producer bursts even when the long-term SCI rate is exactly 151.2 kB/s.
  static constexpr size_t AUDIO_PRIME_FRAMES = 2048;

  enum class ATAPIPhase : uint8_t
  {
    Idle,
    IdentifyData,
    PacketCommand,
    PacketDataIn,
  };

  SH7014 m_sh2;
  std::unique_ptr<CDImage> m_media;
  bool m_sh2_running = false;
  bool m_sram_selected = false;
  bool m_ide_sh2_enabled = false;
  bool m_ide_ps1_enabled = false;
  bool m_psx_int10_busy = false;

  uint16_t m_boot_latch = 0;
  uint16_t m_unknown_6018 = 0;
  uint16_t m_sh2_417000_latch = 0;

  std::array<uint16_t, 4> m_volume_latch{};
  std::array<uint32_t, 4> m_volume_gain_q16{};
  uint8_t m_volume_control = 0;
  uint8_t m_volume_data = 0;
  bool m_volume_write_phase = false;

  uint16_t m_audio_word = 0;
  uint8_t m_audio_word_byte = 0;
  bool m_audio_left_channel = true;
  int16_t m_audio_pending_left = 0;
  bool m_audio_muted = false;

  std::array<StereoFrame, AUDIO_QUEUE_CAPACITY> m_audio_queue{};
  size_t m_audio_queue_read = 0;
  size_t m_audio_queue_write = 0;
  size_t m_audio_queue_count = 0;
  bool m_audio_host_primed = false;
  StereoFrame m_audio_current{};
  StereoFrame m_audio_next{};
  uint64_t m_audio_resample_phase = 0;
  uint64_t m_audio_sci_bytes = 0;
  uint64_t m_audio_native_frames = 0;
  uint64_t m_audio_nonzero_frames = 0;
  uint64_t m_audio_host_pops = 0;
  uint32_t m_audio_peak_abs = 0;
  uint64_t m_audio_underflows = 0;
  uint64_t m_audio_reprime_count = 0;

  ATAPIPhase m_atapi_phase = ATAPIPhase::Idle;
  uint8_t m_ata_error = 0;
  uint8_t m_ata_features = 0;
  uint8_t m_ata_sector_count = 1;
  uint8_t m_ata_lba_mid = 0x14;
  uint8_t m_ata_lba_high = 0xEB;
  uint8_t m_ata_device = 0;
  uint8_t m_ata_status = 0x50;
  uint8_t m_ata_device_control = 0;
  bool m_atapi_media_change = true;
  uint8_t m_atapi_sense_key = 0;
  uint8_t m_atapi_sense_asc = 0;
  uint8_t m_atapi_sense_ascq = 0;
  std::array<uint16_t, 256> m_identify_words{};
  size_t m_identify_word = 0;
  std::array<uint8_t, 12> m_packet{};
  size_t m_packet_size = 0;
  std::array<uint8_t, 64 * 1024> m_atapi_data{};
  size_t m_atapi_data_size = 0;
  size_t m_atapi_data_offset = 0;
};
} // namespace Arcade::Namco
