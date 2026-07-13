#include "konami_gv_cdrom.h"
#include "cdrom.h"
#include "system.h"
#include "timing_event.h"
#include "common/cd_image.h"
#include <cstring>
#include <memory>

static std::unique_ptr<CDImage> s_konami_gv_cdrom_media;
static std::unique_ptr<TimingEvent> s_konami_gv_cdda_event;

static bool s_konami_gv_cdda_playing;
static bool s_konami_gv_cdda_paused;
static bool s_konami_gv_cdda_completed;

static u8 s_konami_gv_cdda_track;
static u8 s_konami_gv_cdda_index;

static u8 s_konami_gv_cdda_end_track;
static u8 s_konami_gv_cdda_end_index;

static CDImage::LBA s_konami_gv_cdda_lba;
static CDImage::LBA s_konami_gv_cdda_read_lba;
static CDImage::LBA s_konami_gv_cdda_end_lba;

static constexpr u32 KONAMI_GV_CDDA_READ_AHEAD_SECTORS = 4;
static u8 s_konami_gv_cdda_output_channel[4];
static u8 s_konami_gv_cdda_output_volume[4];

static bool KonamiGVCDROMQueueCDDASector(CDImage::LBA lba)
{
  u8 raw_sector[CDImage::RAW_SECTOR_SIZE];

  if (!s_konami_gv_cdrom_media || !s_konami_gv_cdrom_media->Seek(1, lba) ||
      s_konami_gv_cdrom_media->Read(CDImage::ReadMode::RawSector, 1, raw_sector) != 1)
  {
    return false;
  }

  constexpr u32 cdda_frames_per_sector = CDImage::RAW_SECTOR_SIZE / (sizeof(s16) * 2);

  u32 audio_frames[cdda_frames_per_sector];

  const u8* sector_ptr = raw_sector;

  for (u32 i = 0; i < cdda_frames_per_sector; i++)
  {
    s16 left;
    s16 right;

    std::memcpy(&left, sector_ptr, sizeof(left));
    std::memcpy(&right, sector_ptr + sizeof(left), sizeof(right));

    audio_frames[i] = static_cast<u32>(static_cast<u16>(left)) | (static_cast<u32>(static_cast<u16>(right)) << 16);

    sector_ptr += sizeof(s16) * 2;
  }

  g_cdrom.PushExternalCDAudioFrames(audio_frames, cdda_frames_per_sector);

  return true;
}

static void KonamiGVCDROMCDDASectorEvent(void*, TickCount, TickCount)
{
  if (!s_konami_gv_cdda_playing || s_konami_gv_cdda_paused)
    return;

  s_konami_gv_cdda_lba++;

  if (s_konami_gv_cdda_lba >= s_konami_gv_cdda_end_lba)
  {
    // The end LBA is exclusive. Keep subchannel position on the
    // final sector that was actually played.
    s_konami_gv_cdda_lba = s_konami_gv_cdda_end_lba - 1;

    s_konami_gv_cdda_playing = false;
    s_konami_gv_cdda_paused = false;
    s_konami_gv_cdda_completed = true;

    if (s_konami_gv_cdda_event)
      s_konami_gv_cdda_event->Deactivate();

    return;
  }

  if (s_konami_gv_cdda_read_lba >= s_konami_gv_cdda_end_lba)
    return;

  if (!KonamiGVCDROMQueueCDDASector(s_konami_gv_cdda_read_lba))
  {
    s_konami_gv_cdda_playing = false;
    s_konami_gv_cdda_paused = false;
    s_konami_gv_cdda_completed = false;

    if (s_konami_gv_cdda_event)
      s_konami_gv_cdda_event->Deactivate();

    return;
  }

  s_konami_gv_cdda_read_lba++;
}

static bool KonamiGVCDROMOpenMountedMedia()
{
  if (s_konami_gv_cdrom_media)
    return true;

  const std::string& filename = g_cdrom.GetMediaFileName();
  if (filename.empty())
    return false;

  s_konami_gv_cdrom_media = CDImage::Open(filename.c_str(), nullptr);
  return static_cast<bool>(s_konami_gv_cdrom_media);
}

void KonamiGVCDROMInitialize()
{
  s_konami_gv_cdrom_media.reset();

  const TickCount cdda_sector_ticks = System::GetTicksPerSecond() / CDImage::FRAMES_PER_SECOND;

  if (!s_konami_gv_cdda_event)
  {
    s_konami_gv_cdda_event = TimingEvents::CreateTimingEvent(
      "Konami GV CDDA Sector", cdda_sector_ticks, cdda_sector_ticks, KonamiGVCDROMCDDASectorEvent, nullptr, false);
  }
  else
  {
    s_konami_gv_cdda_event->Deactivate();
    s_konami_gv_cdda_event->SetPeriod(cdda_sector_ticks);
    s_konami_gv_cdda_event->SetInterval(cdda_sector_ticks);
  }

  s_konami_gv_cdda_playing = false;
  s_konami_gv_cdda_paused = false;
  s_konami_gv_cdda_completed = false;

  s_konami_gv_cdda_track = 1;
  s_konami_gv_cdda_index = 1;

  s_konami_gv_cdda_end_track = 1;
  s_konami_gv_cdda_end_index = 1;

  s_konami_gv_cdda_lba = 0;
  s_konami_gv_cdda_read_lba = 0;
  s_konami_gv_cdda_end_lba = 0;

  // Default GV stereo routing before the game supplies MODE SELECT
  // CD Audio Control page 0x0E values.
  s_konami_gv_cdda_output_channel[0] = 1;
  s_konami_gv_cdda_output_volume[0] = 0xFF;

  s_konami_gv_cdda_output_channel[1] = 2;
  s_konami_gv_cdda_output_volume[1] = 0xFF;

  s_konami_gv_cdda_output_channel[2] = 0;
  s_konami_gv_cdda_output_volume[2] = 0x00;

  s_konami_gv_cdda_output_channel[3] = 0;
  s_konami_gv_cdda_output_volume[3] = 0x00;
}

void KonamiGVCDROMShutdown()
{
  s_konami_gv_cdda_event.reset();
  s_konami_gv_cdrom_media.reset();

  s_konami_gv_cdda_playing = false;
  s_konami_gv_cdda_paused = false;
  s_konami_gv_cdda_completed = false;
}

bool KonamiGVCDROMReadDataSector(u32 lba, u8* sector)
{
  if (!KonamiGVCDROMOpenMountedMedia())
    return false;

  if (!s_konami_gv_cdrom_media->Seek(1, static_cast<CDImage::LBA>(lba)))
    return false;

  u8 raw_sector[CDImage::RAW_SECTOR_SIZE];

  if (s_konami_gv_cdrom_media->Read(CDImage::ReadMode::RawSector, 1, raw_sector) != 1)
    return false;

  static constexpr u8 mode1_sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

  const bool has_standard_sync = std::memcmp(raw_sector, mode1_sync, sizeof(mode1_sync)) == 0;

  // Standard Mode 1 sectors place the 2048-byte user-data area after
  // the 16-byte sync, address, and mode header.
  if (has_standard_sync && raw_sector[15] == 0x01)
  {
    std::memcpy(sector, raw_sector + 16, CDImage::DATA_SECTOR_SIZE);

    return true;
  }

  // Some GV CHDs expose an already-cooked data sector through RawSector.
  // With no standard sync/header present, the useful 2048-byte payload
  // begins at the start of the returned buffer.
  if (!has_standard_sync)
  {
    std::memcpy(sector, raw_sector, CDImage::DATA_SECTOR_SIZE);

    return true;
  }

  // Preserve DuckStation's cooked-sector path for other raw layouts,
  // such as data-sector formats which still contain a standard header.
  if (!s_konami_gv_cdrom_media->Seek(1, static_cast<CDImage::LBA>(lba)))
    return false;

  return s_konami_gv_cdrom_media->Read(CDImage::ReadMode::DataOnly, 1, sector) == 1;
}

u32 KonamiGVCDROMReadTOC(const u8* cdb, u8* response, u32 response_size)
{
  if (!KonamiGVCDROMOpenMountedMedia() || !cdb || !response || response_size < 4)
    return 0;

  const bool msf = (cdb[1] & 0x02) != 0;
  const u8 requested_track = cdb[6];
  const u8 first_track = static_cast<u8>(s_konami_gv_cdrom_media->GetFirstTrackNumber());
  const u8 last_track = static_cast<u8>(s_konami_gv_cdrom_media->GetLastTrackNumber());

  std::memset(response, 0, response_size);

  response[2] = first_track;
  response[3] = last_track;

  u32 write_offset = 4;

  const auto append_descriptor = [&](u8 track_number, u8 control_bits, CDImage::LBA lba) {
    if ((write_offset + 8) > response_size)
      return;

    response[write_offset + 0] = 0x00;

    // CDImage stores ADR in the low nibble and CONTROL in the high nibble.
    // SCSI TOC descriptors use ADR in the high nibble and CONTROL in the low nibble.
    response[write_offset + 1] = static_cast<u8>(((control_bits & 0x0F) << 4) | ((control_bits >> 4) & 0x0F));

    response[write_offset + 2] = track_number;
    response[write_offset + 3] = 0x00;

    if (msf)
    {
      const CDImage::Position position = CDImage::Position::FromLBA(lba);

      response[write_offset + 4] = 0x00;
      response[write_offset + 5] = position.minute;
      response[write_offset + 6] = position.second;
      response[write_offset + 7] = position.frame;
    }
    else
    {
      response[write_offset + 4] = static_cast<u8>(lba >> 24);
      response[write_offset + 5] = static_cast<u8>(lba >> 16);
      response[write_offset + 6] = static_cast<u8>(lba >> 8);
      response[write_offset + 7] = static_cast<u8>(lba);
    }

    write_offset += 8;
  };

  if (requested_track != CDImage::LEAD_OUT_TRACK_NUMBER)
  {
    const u8 start_track = (requested_track == 0 || requested_track < first_track) ? first_track : requested_track;

    for (u8 track = start_track; track <= last_track; track++)
    {
      const CDImage::Track& track_info = s_konami_gv_cdrom_media->GetTrack(track);

      append_descriptor(track, track_info.control.bits, track_info.start_lba);

      if ((write_offset + 8) > response_size)
        break;
    }
  }

  if ((write_offset + 8) <= response_size)
  {
    const CDImage::Track& last_track_info = s_konami_gv_cdrom_media->GetTrack(last_track);

    append_descriptor(CDImage::LEAD_OUT_TRACK_NUMBER, last_track_info.control.bits,
                      s_konami_gv_cdrom_media->GetLBACount());
  }

  const u16 toc_data_length = static_cast<u16>(write_offset - 2);

  response[0] = static_cast<u8>(toc_data_length >> 8);
  response[1] = static_cast<u8>(toc_data_length);

  return write_offset;
}

static bool KonamiGVCDROMGetTrackIndexRange(u8 track, u8 index, CDImage::LBA* start_lba, CDImage::LBA* end_lba)
{
  if (!s_konami_gv_cdrom_media || !start_lba || !end_lba)
    return false;

  for (const CDImage::Index& index_info : s_konami_gv_cdrom_media->GetIndices())
  {
    if (index_info.track_number != track || index_info.index_number != index)
      continue;

    *start_lba = index_info.start_lba_on_disc;
    *end_lba = index_info.start_lba_on_disc + index_info.length;
    return true;
  }

  return false;
}

static bool KonamiGVCDROMGetTrackEndLBA(u8 track, CDImage::LBA* end_lba)
{
  if (!s_konami_gv_cdrom_media || !end_lba)
    return false;

  bool found = false;
  CDImage::LBA track_end_lba = 0;

  for (const CDImage::Index& index_info : s_konami_gv_cdrom_media->GetIndices())
  {
    if (index_info.track_number != track)
      continue;

    const CDImage::LBA index_end_lba = index_info.start_lba_on_disc + index_info.length;

    if (!found || index_end_lba > track_end_lba)
      track_end_lba = index_end_lba;

    found = true;
  }

  if (!found)
    return false;

  *end_lba = track_end_lba;
  return true;
}

bool KonamiGVCDROMPlayAudioTrackIndex(u8 start_track, u8 start_index, u8 end_track, u8 end_index)
{
  if (!KonamiGVCDROMOpenMountedMedia())
    return false;

  const u8 first_track = static_cast<u8>(s_konami_gv_cdrom_media->GetFirstTrackNumber());
  const u8 last_track = static_cast<u8>(s_konami_gv_cdrom_media->GetLastTrackNumber());

  if (start_track < first_track || start_track > last_track || start_index == 0)
    return false;

  if (s_konami_gv_cdrom_media->GetTrackMode(start_track) != CDImage::TrackMode::Audio)
    return false;

  if (end_track < start_track || end_track == 0 || end_index == 0)
    return false;

  const u8 resolved_end_track = (end_track > last_track) ? last_track : end_track;

  if (s_konami_gv_cdrom_media->GetTrackMode(resolved_end_track) != CDImage::TrackMode::Audio)
    return false;

  CDImage::LBA start_lba;
  CDImage::LBA start_index_end_lba;

  if (!KonamiGVCDROMGetTrackIndexRange(start_track, start_index, &start_lba, &start_index_end_lba))
    return false;

  CDImage::LBA end_index_start_lba;
  CDImage::LBA end_lba;

  if (!KonamiGVCDROMGetTrackIndexRange(resolved_end_track, end_index, &end_index_start_lba, &end_lba) &&
      !KonamiGVCDROMGetTrackEndLBA(resolved_end_track, &end_lba))
  {
    return false;
  }

  if (start_lba >= end_lba)
    return false;

  // Some GV games repeatedly issue the same PLAY AUDIO command while the
  // requested track is already playing. Treat it as a status refresh rather
  // than restarting the track and clearing the buffered audio.
  if (s_konami_gv_cdda_playing && !s_konami_gv_cdda_paused && s_konami_gv_cdda_track == start_track &&
      s_konami_gv_cdda_index == start_index && s_konami_gv_cdda_end_track == resolved_end_track &&
      s_konami_gv_cdda_end_index == end_index)
  {
    return true;
  }

  g_cdrom.ClearExternalCDAudioFrames();

  s_konami_gv_cdda_playing = true;
  s_konami_gv_cdda_paused = false;

  s_konami_gv_cdda_track = start_track;
  s_konami_gv_cdda_index = start_index;

  s_konami_gv_cdda_end_track = resolved_end_track;
  s_konami_gv_cdda_end_index = end_index;

  s_konami_gv_cdda_lba = start_lba;
  s_konami_gv_cdda_read_lba = start_lba;
  s_konami_gv_cdda_end_lba = end_lba;

  for (u32 i = 0; i < KONAMI_GV_CDDA_READ_AHEAD_SECTORS && s_konami_gv_cdda_read_lba < s_konami_gv_cdda_end_lba; i++)
  {
    if (!KonamiGVCDROMQueueCDDASector(s_konami_gv_cdda_read_lba))
    {
      s_konami_gv_cdda_playing = false;
      s_konami_gv_cdda_paused = false;
      return false;
    }

    s_konami_gv_cdda_read_lba++;
  }

  s_konami_gv_cdda_completed = false;

  if (s_konami_gv_cdda_event)
    s_konami_gv_cdda_event->Reset();

  if (s_konami_gv_cdda_event)
    s_konami_gv_cdda_event->Activate();

  return true;
}

void KonamiGVCDROMPauseAudio(bool resume)
{
  if (!s_konami_gv_cdda_playing)
    return;

  if (!s_konami_gv_cdda_event)
  {
    s_konami_gv_cdda_paused = !resume;
    return;
  }

  if (resume)
  {
    if (!s_konami_gv_cdda_paused)
      return;

    s_konami_gv_cdda_paused = false;

    for (u32 i = 0; i < KONAMI_GV_CDDA_READ_AHEAD_SECTORS && s_konami_gv_cdda_read_lba < s_konami_gv_cdda_end_lba; i++)
    {
      if (!KonamiGVCDROMQueueCDDASector(s_konami_gv_cdda_read_lba))
      {
        s_konami_gv_cdda_playing = false;
        s_konami_gv_cdda_paused = false;
        s_konami_gv_cdda_completed = false;

        s_konami_gv_cdda_event->Deactivate();
        return;
      }

      s_konami_gv_cdda_read_lba++;
    }

    s_konami_gv_cdda_event->Reset();
    s_konami_gv_cdda_event->Activate();
  }
  else
  {
    if (s_konami_gv_cdda_paused)
      return;

    s_konami_gv_cdda_paused = true;

    s_konami_gv_cdda_event->Deactivate();

    g_cdrom.ClearExternalCDAudioFrames();

    // Refill from the current logical playback position when resumed
    // instead of skipping the sectors that were discarded above.
    s_konami_gv_cdda_read_lba = s_konami_gv_cdda_lba;
  }
}

void KonamiGVCDROMSetAudioOutput(u8 output, u8 channel, u8 volume)
{
  if (output >= 4)
    return;

  s_konami_gv_cdda_output_channel[output] = static_cast<u8>(channel & 0x0F);

  s_konami_gv_cdda_output_volume[output] = volume;
}

bool KonamiGVCDROMGetAudioOutput(u8 output, u8* channel, u8* volume)
{
  if (output >= 4 || !channel || !volume)
    return false;

  *channel = s_konami_gv_cdda_output_channel[output];
  *volume = s_konami_gv_cdda_output_volume[output];

  return true;
}

static s32 KonamiGVCDROMSelectAudioChannel(u8 channel, s16 input_left, s16 input_right)
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

void KonamiGVCDROMMixAudioFrame(s16 input_left, s16 input_right, s32* output_left, s32* output_right)
{
  if (!output_left || !output_right)
    return;

  const s32 routed_left = KonamiGVCDROMSelectAudioChannel(s_konami_gv_cdda_output_channel[0], input_left, input_right);

  const s32 routed_right = KonamiGVCDROMSelectAudioChannel(s_konami_gv_cdda_output_channel[1], input_left, input_right);

  // Keep the current temporary /12 maximum level while allowing
  // MODE SELECT volume values to scale each GV output independently.
  constexpr s32 maximum_output_scale = 0xFF * 12;

  *output_left = (routed_left * static_cast<s32>(s_konami_gv_cdda_output_volume[0])) / maximum_output_scale;

  *output_right = (routed_right * static_cast<s32>(s_konami_gv_cdda_output_volume[1])) / maximum_output_scale;
}

u32 KonamiGVCDROMReadSubChannel(const u8* cdb, u8* response, u32 response_size)
{
  if (!KonamiGVCDROMOpenMountedMedia() || !cdb || !response || response_size < 4)
    return 0;

  std::memset(response, 0, response_size);

  // Audio status:
  // 0x11 = playing
  // 0x12 = paused
  // 0x13 = playback completed successfully
  // 0x15 = no current audio status
  if (s_konami_gv_cdda_playing)
  {
    response[1] = s_konami_gv_cdda_paused ? 0x12 : 0x11;
  }
  else if (s_konami_gv_cdda_completed)
  {
    response[1] = 0x13;
  }
  else
  {
    response[1] = 0x15;
  }

  // Only current-position data format 0x01 is currently required.
  if ((cdb[2] & 0x40) == 0 || cdb[3] != 0x01 || response_size < 16)
  {
    response[2] = 0x00;
    response[3] = 0x00;
    return 4;
  }

  response[2] = 0x00;
  response[3] = 0x0C;
  response[4] = 0x01;

  const CDImage::Index* current_index = nullptr;

  for (const CDImage::Index& index_info : s_konami_gv_cdrom_media->GetIndices())
  {
    if (s_konami_gv_cdda_lba < index_info.start_lba_on_disc)
      continue;

    if ((s_konami_gv_cdda_lba - index_info.start_lba_on_disc) >= index_info.length)
      continue;

    current_index = &index_info;
    break;
  }

  if (current_index)
  {
    response[5] =
      static_cast<u8>(((current_index->control.bits & 0x0F) << 4) | ((current_index->control.bits >> 4) & 0x0F));

    response[6] = static_cast<u8>(current_index->track_number);
    response[7] = static_cast<u8>(current_index->index_number);
  }
  else
  {
    response[6] = s_konami_gv_cdda_track;
    response[7] = s_konami_gv_cdda_index;
  }

  const CDImage::LBA relative_lba = current_index ? (s_konami_gv_cdda_lba - current_index->start_lba_on_disc) : 0;

  const bool msf = (cdb[1] & 0x02) != 0;

  if (msf)
  {
    const CDImage::Position absolute_position = CDImage::Position::FromLBA(s_konami_gv_cdda_lba);

    const CDImage::Position relative_position = CDImage::Position::FromLBA(relative_lba);

    response[8] = 0x00;
    response[9] = absolute_position.minute;
    response[10] = absolute_position.second;
    response[11] = absolute_position.frame;

    response[12] = 0x00;
    response[13] = relative_position.minute;
    response[14] = relative_position.second;
    response[15] = relative_position.frame;
  }
  else
  {
    response[8] = static_cast<u8>(s_konami_gv_cdda_lba >> 24);
    response[9] = static_cast<u8>(s_konami_gv_cdda_lba >> 16);
    response[10] = static_cast<u8>(s_konami_gv_cdda_lba >> 8);
    response[11] = static_cast<u8>(s_konami_gv_cdda_lba);

    response[12] = static_cast<u8>(relative_lba >> 24);
    response[13] = static_cast<u8>(relative_lba >> 16);
    response[14] = static_cast<u8>(relative_lba >> 8);
    response[15] = static_cast<u8>(relative_lba);
  }

  return 16;
}