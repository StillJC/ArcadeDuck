// SPDX-License-Identifier: GPL-3.0-or-later
#include "konami_gv_cdrom.h"
#include "cdrom.h"
#include "konami.h"
#include "system.h"
#include "timing_event.h"
#include "util/cd_image.h"
#include "common/log.h"
#include <array>
#include <cstring>
#include <memory>

Log_SetChannel(KonamiGVCDROM);
namespace KonamiGVCDROM {
namespace {
std::unique_ptr<CDImage> s_media;
std::unique_ptr<TimingEvent> s_event;
bool s_playing, s_paused, s_completed, s_first_sector_logged;
u8 s_track, s_index, s_end_track, s_end_index;
CDImage::LBA s_lba, s_read_lba, s_end_lba;
std::array<u8, 4> s_channel = {{1, 2, 0, 0}};
std::array<u8, 4> s_volume = {{0xff, 0xff, 0, 0}};

bool Open()
{
  if (s_media) return true;
  const std::string_view path = Konami::GetGVCHDPath();
  if (path.empty()) return false;
  s_media = CDImage::Open(std::string(path).c_str(), false, nullptr);
  return static_cast<bool>(s_media);
}
bool Queue(CDImage::LBA lba)
{
  std::array<u8, CDImage::RAW_SECTOR_SIZE> raw = {};
  if (!s_media || !s_media->Seek(lba) || s_media->Read(CDImage::ReadMode::RawSector, 1, raw.data()) != 1) return false;
  std::array<u32, CDImage::RAW_SECTOR_SIZE / 4> frames = {};
  for (u32 i = 0; i < frames.size(); i++)
  {
    s16 left, right;
    std::memcpy(&left, raw.data() + i * 4, 2); std::memcpy(&right, raw.data() + i * 4 + 2, 2);
    const auto select = [&](u8 c) -> s32 { return c == 1 ? left : c == 2 ? right : c == 3 ? (static_cast<s32>(left) + right) / 2 : 0; };
    const s16 out_left = static_cast<s16>((select(s_channel[0]) * s_volume[0]) / (0xff * 12));
    const s16 out_right = static_cast<s16>((select(s_channel[1]) * s_volume[1]) / (0xff * 12));
    frames[i] = static_cast<u16>(out_left) | (static_cast<u32>(static_cast<u16>(out_right)) << 16);
  }
  CDROM::PushExternalCDAudioFrames(frames.data(), static_cast<u32>(frames.size()));
  if (!s_first_sector_logged) { INFO_LOG("KonamiGV.CDROM first_audio_sector lba={}", lba); s_first_sector_logged = true; }
  return true;
}
void Event(void*, TickCount, TickCount)
{
  if (!s_playing || s_paused) return;
  s_lba++;
  if (s_lba >= s_end_lba) { s_lba = s_end_lba - 1; s_playing = false; s_completed = true; s_event->Deactivate(); INFO_LOG("KonamiGV.CDROM playback_end lba={}", s_lba); return; }
  if (s_read_lba < s_end_lba && Queue(s_read_lba)) s_read_lba++;
}
bool Range(u8 track, u8 index, CDImage::LBA* start, CDImage::LBA* end)
{
  for (const auto& entry : s_media->GetIndices()) if (entry.track_number == track && entry.index_number == index) { *start = entry.start_lba_on_disc; *end = *start + entry.length; return true; }
  return false;
}
}
void Initialize()
{
  s_media.reset(); CDROM::ClearExternalCDAudioFrames();
  const TickCount ticks = System::GetTicksPerSecond() / CDImage::FRAMES_PER_SECOND;
  if (!s_event) s_event = std::make_unique<TimingEvent>("Konami GV CDDA", ticks, ticks, Event, nullptr); else { s_event->Deactivate(); s_event->SetPeriod(ticks); s_event->SetInterval(ticks); }
  s_playing = s_paused = s_completed = s_first_sector_logged = false; s_track = s_index = s_end_track = s_end_index = 1; s_lba = s_read_lba = s_end_lba = 0; s_channel = {{1,2,0,0}}; s_volume = {{0xff,0xff,0,0}};
}
void Reset() { if (s_event) s_event->Deactivate(); s_playing = s_paused = s_completed = false; CDROM::ClearExternalCDAudioFrames(); }
void Shutdown() { Reset(); s_event.reset(); s_media.reset(); }
bool ReadDataSector(u32 lba, u8* sector) { return Open() && s_media->Seek(1, lba) && s_media->Read(CDImage::ReadMode::DataOnly, 1, sector) == 1; }
u32 ReadTOC(const u8* cdb, u8* response, u32 response_size)
{
  if (!cdb || !response) return 0;
  return Konami::BuildGVTOC((cdb[1] & 2) != 0, cdb[6], response, response_size);
}
u32 ReadSubChannel(const u8* cdb, u8* r, u32 size)
{
  if (!Open() || !cdb || !r || size < 4) return 0; std::memset(r, 0, size);
  r[1] = s_playing ? (s_paused ? 0x12 : 0x11) : (s_completed ? 0x13 : 0x15);
  if ((cdb[2] & 0x40) == 0 || cdb[3] != 1 || size < 16) return 4;
  r[2] = 0; r[3] = 12; r[4] = 1; r[6] = s_track; r[7] = s_index;
  const bool msf = (cdb[1] & 2) != 0; const auto rel = s_lba;
  if (msf) { const auto a = CDImage::Position::FromLBA(s_lba); const auto b = CDImage::Position::FromLBA(rel); r[9]=a.minute; r[10]=a.second; r[11]=a.frame; r[13]=b.minute; r[14]=b.second; r[15]=b.frame; }
  else { for (u32 i=0;i<4;i++) { r[8+i]=static_cast<u8>(s_lba >> (24-i*8)); r[12+i]=static_cast<u8>(rel >> (24-i*8)); } }
  INFO_LOG("KonamiGV.CDROM read_subchannel status=0x{:02X} track={} index={} lba={}", r[1], s_track, s_index, s_lba); return 16;
}
bool PlayAudioTrackIndex(u8 start_track, u8 start_index, u8 end_track, u8 end_index)
{
  if (!Open() || start_index == 0 || end_index == 0 || start_track < s_media->GetFirstTrackNumber() || end_track < start_track) return false;
  CDImage::LBA start, ignored, end_start, end; if (!Range(start_track,start_index,&start,&ignored) || (!Range(end_track,end_index,&end_start,&end) && !Range(end_track,1,&end_start,&end))) return false;
  if (start >= end) return false; CDROM::ClearExternalCDAudioFrames(); s_track=start_track; s_index=start_index; s_end_track=end_track; s_end_index=end_index; s_lba=s_read_lba=start; s_end_lba=end; s_playing=true; s_paused=s_completed=false; s_first_sector_logged=false;
  for (u32 i=0;i<4 && s_read_lba<s_end_lba;i++) { if (!Queue(s_read_lba++)) return false; } s_event->Schedule(s_event->GetInterval()); INFO_LOG("KonamiGV.CDROM playback_start track={}/{} end={}/{} lba={} end_lba={}", start_track,start_index,end_track,end_index,start,end); return true;
}
void PauseAudio(bool resume)
{
  if (!s_playing) return; if (resume) { if (!s_paused) return; s_paused=false; s_event->Schedule(s_event->GetInterval()); INFO_LOG("KonamiGV.CDROM resume lba={}",s_lba); }
  else { if (s_paused) return; s_paused=true; s_event->Deactivate(); CDROM::ClearExternalCDAudioFrames(); s_read_lba=s_lba; INFO_LOG("KonamiGV.CDROM pause lba={}",s_lba); }
}
void SetAudioOutput(u8 o,u8 c,u8 v){ if(o<4){s_channel[o]=c&15;s_volume[o]=v;} }
bool GetAudioOutput(u8 o,u8* c,u8* v){if(o>=4||!c||!v)return false;*c=s_channel[o];*v=s_volume[o];return true;}
}
