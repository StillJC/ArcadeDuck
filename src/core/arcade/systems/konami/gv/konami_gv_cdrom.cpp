// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/konami/gv/konami_gv_cdrom.h"

#include "core/cdrom.h"
#include "core/arcade/systems/konami/konami.h"
#include "core/system.h"
#include "core/timing_event.h"
#include "util/cd_image.h"

#include "common/log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

Log_SetChannel(KonamiGVCDROM);

namespace KonamiGVCDROM {
namespace {

constexpr u32 CDDA_READ_AHEAD_SECTORS = 4;
constexpr u32 CDDA_FRAMES_PER_SECTOR = CDImage::RAW_SECTOR_SIZE / (sizeof(s16) * 2);

std::unique_ptr<CDImage> s_media;
std::unique_ptr<TimingEvent> s_event;
bool s_playing = false;
bool s_paused = false;
bool s_completed = false;
u8 s_track = 1;
u8 s_index = 1;
u8 s_end_track = 1;
u8 s_end_index = 1;
CDImage::LBA s_lba = 0;
CDImage::LBA s_read_lba = 0;
CDImage::LBA s_end_lba = 0;
std::array<u8, 4> s_channel = {{1, 2, 0, 0}};
std::array<u8, 4> s_volume = {{0xff, 0xff, 0, 0}};

bool Open()
{
  if (s_media)
    return true;

  const std::string_view path = Konami::GetGVCHDPath();
  if (path.empty())
    return false;

  const std::string filename(path);
  s_media = CDImage::Open(filename.c_str(), false, nullptr);
  return static_cast<bool>(s_media);
}

s32 SelectAudioChannel(u8 channel, s16 input_left, s16 input_right)
{
  switch (channel)
  {
    case 1:
      return static_cast<s32>(input_left);
    case 2:
      return static_cast<s32>(input_right);
    case 3:
      return (static_cast<s32>(input_left) + static_cast<s32>(input_right)) / 2;
    default:
      return 0;
  }
}

bool Queue(CDImage::LBA lba)
{
  std::array<u8, CDImage::RAW_SECTOR_SIZE> raw = {};
  if (!s_media || !s_media->Seek(lba) || s_media->Read(CDImage::ReadMode::RawSector, 1, raw.data()) != 1)
    return false;

  std::array<u32, CDDA_FRAMES_PER_SECTOR> frames = {};
  for (u32 i = 0; i < frames.size(); i++)
  {
    s16 left;
    s16 right;
    std::memcpy(&left, raw.data() + (i * 4), sizeof(left));
    std::memcpy(&right, raw.data() + (i * 4) + sizeof(left), sizeof(right));

    frames[i] = static_cast<u32>(static_cast<u16>(left)) |
                (static_cast<u32>(static_cast<u16>(right)) << 16);
  }

  CDROM::PushExternalCDAudioFrames(frames.data(), static_cast<u32>(frames.size()));
  return true;
}
void Event(void*, TickCount, TickCount)
{
  if (!s_playing || s_paused)
    return;

  s_lba++;
  if (s_lba >= s_end_lba)
  {
    s_lba = s_end_lba - 1;
    s_playing = false;
    s_paused = false;
    s_completed = true;
    if (s_event)
      s_event->Deactivate();
    return;
  }

  if (s_read_lba >= s_end_lba)
    return;

  if (!Queue(s_read_lba))
  {
    s_playing = false;
    s_paused = false;
    s_completed = false;
    if (s_event)
      s_event->Deactivate();
    ERROR_LOG("KonamiGV.CDROM audio_sector_read_failed lba={}", s_read_lba);
    return;
  }
  s_read_lba++;
}
bool GetTrackIndexRange(u8 track, u8 index, CDImage::LBA* start, CDImage::LBA* end)
{
  if (!s_media || !start || !end)
    return false;

  for (const CDImage::Index& entry : s_media->GetIndices())
  {
    if (entry.track_number == track && entry.index_number == index)
    {
      *start = entry.start_lba_on_disc;
      *end = entry.start_lba_on_disc + entry.length;
      return true;
    }
  }
  return false;
}

bool GetTrackEndLBA(u8 track, CDImage::LBA* end)
{
  if (!s_media || !end)
    return false;

  bool found = false;
  CDImage::LBA track_end = 0;
  for (const CDImage::Index& entry : s_media->GetIndices())
  {
    if (entry.track_number != track)
      continue;
    track_end = std::max(track_end, entry.start_lba_on_disc + entry.length);
    found = true;
  }
  if (found)
    *end = track_end;
  return found;
}

const CDImage::Index* FindCurrentIndex()
{
  if (!s_media)
    return nullptr;

  for (const CDImage::Index& entry : s_media->GetIndices())
  {
    if (s_lba >= entry.start_lba_on_disc && (s_lba - entry.start_lba_on_disc) < entry.length)
      return &entry;
  }
  return nullptr;
}

} // namespace

void Initialize()
{
  s_media.reset();
  CDROM::ClearExternalCDAudioFrames();

  const TickCount ticks = System::GetTicksPerSecond() / CDImage::FRAMES_PER_SECOND;
  if (!s_event)
    s_event = std::make_unique<TimingEvent>("Konami GV CDDA", ticks, ticks, Event, nullptr);
  else
  {
    s_event->Deactivate();
    s_event->SetPeriod(ticks);
    s_event->SetInterval(ticks);
  }

  s_playing = false;
  s_paused = false;
  s_completed = false;
  s_track = 1;
  s_index = 1;
  s_end_track = 1;
  s_end_index = 1;
  s_lba = 0;
  s_read_lba = 0;
  s_end_lba = 0;
  s_channel = {{1, 2, 0, 0}};
  s_volume = {{0xff, 0xff, 0, 0}};
}
void Reset()
{
  if (s_event)
    s_event->Deactivate();
  CDROM::ClearExternalCDAudioFrames();
  s_playing = false;
  s_paused = false;
  s_completed = false;
  s_track = 1;
  s_index = 1;
  s_end_track = 1;
  s_end_index = 1;
  s_lba = 0;
  s_read_lba = 0;
  s_end_lba = 0;
  s_channel = {{1, 2, 0, 0}};
  s_volume = {{0xff, 0xff, 0, 0}};
}
void Shutdown()
{
  Reset();
  s_event.reset();
  s_media.reset();
}

u32 GetLBACount()
{
  return Open() ? static_cast<u32>(s_media->GetLBACount()) : 0;
}
bool ReadDataSector(u32 lba, u8* sector)
{
  if (!Open() || !sector || !s_media->Seek(1, static_cast<CDImage::LBA>(lba)))
    return false;

  std::array<u8, CDImage::RAW_SECTOR_SIZE> raw = {};
  if (s_media->Read(CDImage::ReadMode::RawSector, 1, raw.data()) != 1)
    return false;

  static constexpr std::array<u8, 12> sync = {{0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                0xff, 0xff, 0xff, 0xff, 0xff, 0x00}};
  const bool has_sync = std::memcmp(raw.data(), sync.data(), sync.size()) == 0;
  if (!has_sync)
  {
    std::memcpy(sector, raw.data(), CDImage::DATA_SECTOR_SIZE);
    return true;
  }
  if (raw[15] == 0x01)
  {
    std::memcpy(sector, raw.data() + 16, CDImage::DATA_SECTOR_SIZE);
    return true;
  }
  if (raw[15] == 0x02)
  {
    std::memcpy(sector, raw.data() + 24, CDImage::DATA_SECTOR_SIZE);
    return true;
  }
  return false;
}

u32 ReadTOC(const u8* cdb, u8* response, u32 response_size)
{
  if (!cdb || !response)
    return 0;
  return Konami::BuildGVTOC((cdb[1] & 0x02) != 0, cdb[6], response, response_size);
}

u32 ReadSubChannel(const u8* cdb, u8* response, u32 response_size)
{
  if (!Open() || !cdb || !response || response_size < 4)
    return 0;

  std::memset(response, 0, response_size);
  response[1] = s_playing ? (s_paused ? 0x12 : 0x11) : (s_completed ? 0x13 : 0x15);
  if ((cdb[2] & 0x40) == 0 || cdb[3] != 0x01 || response_size < 16)
    return 4;

  response[3] = 0x0c;
  response[4] = 0x01;

  // Match the Toshiba XM-5401/MAME current-position response. The CDImage CHD
  // index control field does not carry the MMC ADR=1 bit, so deriving this byte
  // from current_index->control.bits reports 0x00 for audio tracks. The drive
  // reports ADR=1, CONTROL=0 while audio is active/paused, and CONTROL=4 when
  // there is no active audio operation.
  response[5] = s_playing ? 0x10 : 0x14;

  const CDImage::Index* current_index = FindCurrentIndex();
  if (current_index)
    response[6] = static_cast<u8>(current_index->track_number);
  else
    response[6] = s_track;

  // Toshiba/MAME reports index 0 for READ SUB-CHANNEL current-position data.
  response[7] = 0x00;

  const CDImage::LBA relative_lba = current_index ? (s_lba - current_index->start_lba_on_disc) : 0;
  if ((cdb[1] & 0x02) != 0)
  {
    const CDImage::Position absolute_position = CDImage::Position::FromLBA(s_lba);
    const CDImage::Position relative_position = CDImage::Position::FromLBA(relative_lba);
    response[9] = absolute_position.minute;
    response[10] = absolute_position.second;
    response[11] = absolute_position.frame;
    response[13] = relative_position.minute;
    response[14] = relative_position.second;
    response[15] = relative_position.frame;
  }
  else
  {
    response[8] = static_cast<u8>(s_lba >> 24);
    response[9] = static_cast<u8>(s_lba >> 16);
    response[10] = static_cast<u8>(s_lba >> 8);
    response[11] = static_cast<u8>(s_lba);
    response[12] = static_cast<u8>(relative_lba >> 24);
    response[13] = static_cast<u8>(relative_lba >> 16);
    response[14] = static_cast<u8>(relative_lba >> 8);
    response[15] = static_cast<u8>(relative_lba);
  }

  return 16;
}
bool PlayAudioTrackIndex(u8 start_track, u8 start_index, u8 end_track, u8 end_index)
{
  if (!Open() || start_index == 0 || end_index == 0)
    return false;

  const u8 first_track = static_cast<u8>(s_media->GetFirstTrackNumber());
  const u8 last_track = static_cast<u8>(s_media->GetLastTrackNumber());
  if (start_track < first_track || start_track > last_track || end_track < start_track || end_track == 0)
    return false;
  if (s_media->GetTrack(start_track).mode != CDImage::TrackMode::Audio)
    return false;

  const u8 resolved_end_track = std::min(end_track, last_track);
  if (s_media->GetTrack(resolved_end_track).mode != CDImage::TrackMode::Audio)
    return false;

  CDImage::LBA start_lba;
  CDImage::LBA ignored_start_end;
  if (!GetTrackIndexRange(start_track, start_index, &start_lba, &ignored_start_end))
    return false;

  CDImage::LBA ignored_end_start;
  CDImage::LBA end_lba;
  if (!GetTrackIndexRange(resolved_end_track, end_index, &ignored_end_start, &end_lba) &&
      !GetTrackEndLBA(resolved_end_track, &end_lba))
  {
    return false;
  }
  if (start_lba >= end_lba)
    return false;

  if (s_playing && !s_paused && s_track == start_track && s_index == start_index &&
      s_end_track == resolved_end_track && s_end_index == end_index)
  {
    return true;
  }

  CDROM::ClearExternalCDAudioFrames();
  s_playing = true;
  s_paused = false;
  s_completed = false;
  s_track = start_track;
  s_index = start_index;
  s_end_track = resolved_end_track;
  s_end_index = end_index;
  s_lba = start_lba;
  s_read_lba = start_lba;
  s_end_lba = end_lba;

  for (u32 i = 0; i < CDDA_READ_AHEAD_SECTORS && s_read_lba < s_end_lba; i++)
  {
    if (!Queue(s_read_lba))
    {
      s_playing = false;
      s_paused = false;
      return false;
    }
    s_read_lba++;
  }

  if (s_event)
    s_event->Schedule(s_event->GetInterval());
  return true;
}
void PauseAudio(bool resume)
{
  if (!s_playing)
    return;

  if (resume)
  {
    if (!s_paused)
      return;
    s_paused = false;
    for (u32 i = 0; i < CDDA_READ_AHEAD_SECTORS && s_read_lba < s_end_lba; i++)
    {
      if (!Queue(s_read_lba))
      {
        s_playing = false;
        s_paused = false;
        s_completed = false;
        if (s_event)
          s_event->Deactivate();
        return;
      }
      s_read_lba++;
    }
    if (s_event)
      s_event->Schedule(s_event->GetInterval());
  }
  else
  {
    if (s_paused)
      return;
    s_paused = true;
    if (s_event)
      s_event->Deactivate();
    CDROM::ClearExternalCDAudioFrames();
    s_read_lba = s_lba;
  }
}
void SetAudioOutput(u8 output, u8 channel, u8 volume)
{
  if (output >= s_channel.size())
    return;
  s_channel[output] = channel & 0x0f;
  s_volume[output] = volume;
}

bool GetAudioOutput(u8 output, u8* channel, u8* volume)
{
  if (output >= s_channel.size() || !channel || !volume)
    return false;
  *channel = s_channel[output];
  *volume = s_volume[output];
  return true;
}

void MixAudioFrame(s16 input_left, s16 input_right, s32* output_left, s32* output_right)
{
  if (!output_left || !output_right)
    return;

  const s32 routed_left = SelectAudioChannel(s_channel[0], input_left, input_right);
  const s32 routed_right = SelectAudioChannel(s_channel[1], input_left, input_right);

  // The Toshiba XM-5401 CD Audio Control page programs each output as an
  // 8-bit linear amplitude. Preserve the software-programmed value here;
  // board-level SPU/CDDA balance is handled by the GV analog-output model.
  constexpr s32 maximum_output_scale = 0xff;

  *output_left = (routed_left * static_cast<s32>(s_volume[0])) / maximum_output_scale;
  *output_right = (routed_right * static_cast<s32>(s_volume[1])) / maximum_output_scale;
}
} // namespace KonamiGVCDROM
