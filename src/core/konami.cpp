// SPDX-FileCopyrightText: 2026 ArcadeDuck Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "konami.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"
#include "util/cd_image.h"

#include <array>
#include <algorithm>

Log_SetChannel(Konami);

namespace Konami {

namespace {

// Source authority: simpsons-bowling-duckstation dc6720ae7 and MAME's konamigv.cpp.
constexpr std::array<GVGameDefinition, 14> s_gv_game_definitions = {{
  {"powyak96", "Jikkyou Powerful Pro Yakyuu '96", GVBIOSProfile::KonamiGV, "konamigv_standard", "powyak96.25c", 0x405a7fc9, "e2d978f49748ba3c4a425188abcd3d272ec23907", "powyak96", "ebd0ea18ff9ce300ea1e30d66a739a96acfb0621", true},
  {"hyperath", "Hyper Athlete", GVBIOSProfile::KonamiGV, "konamigv_standard", "hyperath.25c", 0x20a8c435, "a0f203a999757fba68b391c525ac4b9684a57ba9", "gv021-j1", "579442444025b18da658cd6455c51459fbc3de0e", false},
  {"lacrazyc", "Let's Attack Crazy Cross", GVBIOSProfile::KonamiGV, "konamigv_standard", "lacrazyc.25c", 0xe20e5730, "066b49236c658a4ef2930f7bacc4b2354dd7f240", "gv027-a1", "840d0d4876cf1b814c9d8db975aa6c92e1fe4039", true},
  {"susume", "Susume! Taisen Puzzle-Dama", GVBIOSProfile::KonamiGV, "konamigv_standard", "susume.25c", 0x52f17df7, "b8ad7787b0692713439d7d9bebfa0c801c806006", "gv027j1", "e7e6749ac65de7771eb8fed7d5eefaec3f902255", true},
  {"btchamp", "Beat the Champ", GVBIOSProfile::KonamiGV, "konamigv_btchamp", "btchmp.25c", 0x6d02ea54, "d3babf481fd89db3aec17f589d0d3d999a2aa6e1", "btchamp", "c9c858e9034826e1a12c3c003dd068a49a3577e1", true},
  {"kdeadeye", "Dead Eye", GVBIOSProfile::KonamiGV, "konamigv_kdeadeye", "kdeadeye.25c", 0x3935d2df, "cbb855c475269077803c380dbc3621e522efe51e", "054uaa01", "a05079e4e5024ca66b7f6b81de74695d86c62dd8", false},
  {"weddingr", "Wedding Rhapsody", GVBIOSProfile::KonamiGV, "konamigv_standard", "weddingr.25c", 0xb90509a0, "41510a0ceded81dcb26a70eba97636d38d3742c3", "weddingr", "4e7122b191747ab7220fe4ce1b4483d62ab579af", true},
  {"tmosh", "Tokimeki Memorial Oshiete Your Heart", GVBIOSProfile::KonamiGV, "konamigv_tokimeki", "tmosh.25c", 0x2f6a27fc, "4ead9313f07e9bf7aa0272dba59db6b21510e00b", "673jaa01", "eaa76073749f9db48c1bee3dff9bea955683c8a8", true},
  {"tmoshs", "Tokimeki Memorial Oshiete Your Heart Seal Version", GVBIOSProfile::KonamiGV, "konamigv_tokimeki", "tmoshs.25c", 0xe57b833f, "f18a0974a6be69dc179706643aab837ff61c2738", "755jaa01", "fc742a0b763ba38350ba7eb5d775948632aafd9d", false},
  {"tmoshsp", "Tokimeki Memorial Oshiete Your Heart Seal Version Plus", GVBIOSProfile::KonamiGV, "konamigv_tokimeki", "tmoshsp.25c", 0xaf4cdd87, "97041e287e4c80066043967450779b81b62b2b8e", "756jab01", "b2c59b9801debccbbd986728152f314535c67e53", false},
  {"tmoshspa", "Tokimeki Memorial Oshiete Your Heart Seal Version Plus", GVBIOSProfile::KonamiGV, "konamigv_tokimeki", "tmoshsp.25c", 0xaf4cdd87, "97041e287e4c80066043967450779b81b62b2b8e", "756jaa01", "5e6d349ad1a22c0dbb1ec26aa05febc830254339", true},
  {"nagano98", "Nagano Winter Olympics '98", GVBIOSProfile::KonamiGV, "konamigv_standard", "nagano98.25c", 0xb64b7451, "a77a37e0cc580934d1e7e05d523bae0acd2c1480", "nagano98", "1be7bd4531f249ff2233dd40a206c8d60054a8c6", true},
  {"naganoj", "Hyper Olympics in Nagano", GVBIOSProfile::KonamiGV, "konamigv_standard", "720ja.25c", 0x34c473ba, "768225b04a293bdbc114a092d14dee28d52044e9", "720jaa01", "437160996551ef4dfca43899d1d14beca62eb4c9", false},
  {"simpbowl", "The Simpsons Bowling", GVBIOSProfile::KonamiGV, "konamigv_simpbowl", "simpbowl.25c", 0x2c61050c, "16ae7f81cbe841c429c5c7326cf83e87db1782bf", "829uaa02", "2ec4cc608d5582e478ee047b60ccee67b52f060c", false},
}};

} // namespace

const GVGameDefinition* GetGVGameDefinition(std::string_view set_name)
{
  for (const GVGameDefinition& definition : s_gv_game_definitions)
  {
    if (StringUtil::EqualNoCase(definition.set_name, set_name))
      return &definition;
  }

  return nullptr;
}

bool IsGVArchivePath(std::string_view path)
{
  return StringUtil::EqualNoCase(Path::GetExtension(path), "zip");
}

std::string_view GetGVSetNameFromArchivePath(std::string_view path)
{
  return IsGVArchivePath(path) ? Path::GetFileTitle(path) : std::string_view{};
}

const GVGameDefinition* IdentifyGVArchive(std::string_view path)
{
  return GetGVGameDefinition(GetGVSetNameFromArchivePath(path));
}

std::string GetGVCHDFilename(const GVGameDefinition& definition)
{
  return Path::GetExtension(definition.chd_disk_name).empty() ? std::string(definition.chd_disk_name) + ".chd" :
                                                               std::string(definition.chd_disk_name);
}

std::string GetGVCompanionCHDPath(std::string_view zip_path, const GVGameDefinition& definition)
{
  const std::string zip_directory(Path::GetDirectory(zip_path));
  return Path::Combine(Path::Combine(zip_directory, definition.set_name), GetGVCHDFilename(definition));
}

std::optional<GVLoadedContent> LoadGVContent(const char* archive_path, Error* error)
{
  const GVGameDefinition* const definition = IdentifyGVArchive(archive_path);
  if (!definition)
  {
    Error::SetStringFmt(error, "Konami GV content archive '{}' is not a recognized MAME set.", archive_path);
    return std::nullopt;
  }

  INFO_LOG("KonamiGV.Content opening_zip canonical_set='{}' eeprom_member='{}'", definition->set_name,
           definition->eeprom_member_name);
  unzFile zf = MinizipHelpers::OpenUnzFile(archive_path);
  if (!zf)
  {
    Error::SetStringFmt(error, "Failed to open Konami GV set ZIP '{}'.", archive_path);
    return std::nullopt;
  }

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    Error::SetStringFmt(error, "Konami GV set ZIP '{}' is empty or unreadable.", archive_path);
    return std::nullopt;
  }

  GVLoadedContent content = {definition, definition->set_name, {}, {}, definition->chd_sha1};
  bool found_eeprom = false;
  for (;;)
  {
    unz_file_info64 file_info = {};
    char member_name[512] = {};
    if (unzGetCurrentFileInfo64(zf, &file_info, member_name, sizeof(member_name), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed to read Konami GV ZIP member information from '{}'.", archive_path);
      return std::nullopt;
    }

    if (StringUtil::EqualNoCase(member_name, definition->eeprom_member_name))
    {
      found_eeprom = true;
      if (file_info.uncompressed_size != definition->eeprom_size ||
          content.default_eeprom.size() != definition->eeprom_size)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Konami GV EEPROM '{}' has size {}, expected {} bytes.", definition->eeprom_member_name,
                            file_info.uncompressed_size, definition->eeprom_size);
        return std::nullopt;
      }
      if (static_cast<u32>(file_info.crc) != definition->eeprom_crc32)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Konami GV EEPROM '{}' CRC32 mismatch.", definition->eeprom_member_name);
        return std::nullopt;
      }
      if (unzOpenCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed to decompress Konami GV EEPROM '{}'.", definition->eeprom_member_name);
        return std::nullopt;
      }
      const int bytes_read = unzReadCurrentFile(zf, content.default_eeprom.data(),
                                                 static_cast<unsigned>(content.default_eeprom.size()));
      if (bytes_read != static_cast<int>(content.default_eeprom.size()) || unzCloseCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed reading or validating Konami GV EEPROM '{}'.", definition->eeprom_member_name);
        return std::nullopt;
      }
      auto digest = SHA1Digest::GetDigest(std::span<const u8>(content.default_eeprom));
      const std::string digest_string = SHA1Digest::DigestToString(digest);
      if (!StringUtil::EqualNoCase(digest_string, definition->eeprom_sha1))
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Konami GV EEPROM '{}' SHA-1 mismatch.", definition->eeprom_member_name);
        return std::nullopt;
      }
      INFO_LOG("KonamiGV.Content eeprom_validated canonical_set='{}' size={} crc32={:08x} sha1='{}' bad_dump={}",
               definition->set_name, definition->eeprom_size, definition->eeprom_crc32,
               definition->eeprom_sha1, definition->bad_dump);
      break;
    }

    const int next_result = unzGoToNextFile(zf);
    if (next_result == UNZ_END_OF_LIST_OF_FILE)
      break;
    if (next_result != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed while scanning Konami GV set ZIP '{}'.", archive_path);
      return std::nullopt;
    }
  }
  unzClose(zf);
  if (!found_eeprom)
  {
    Error::SetStringFmt(error, "Konami GV set ZIP '{}' is missing EEPROM '{}'.", archive_path, definition->eeprom_member_name);
    return std::nullopt;
  }

  content.chd_path = GetGVCompanionCHDPath(archive_path, *definition);
  INFO_LOG("KonamiGV.Content expected_chd canonical_set='{}' disk_basename='{}' candidate_path='{}'", definition->set_name,
           definition->chd_disk_name, content.chd_path);
  if (!FileSystem::FileExists(content.chd_path.c_str()))
  {
    Error::SetStringFmt(error, "Konami GV CHD for '{}' was not found:\n{}", definition->set_name, content.chd_path);
    return std::nullopt;
  }
  INFO_LOG("KonamiGV.Content validating_chd canonical_set='{}' path='{}'", definition->set_name, content.chd_path);
  if (!CDImage::Open(content.chd_path.c_str(), false, error))
    return std::nullopt;
  std::array<u8, SHA1Digest::DIGEST_SIZE> chd_sha1;
  if (!CDImage::GetCHDImageSHA1(content.chd_path.c_str(), &chd_sha1, error))
    return std::nullopt;
  const std::string chd_sha1_string = SHA1Digest::DigestToString(chd_sha1);
  if (!StringUtil::EqualNoCase(chd_sha1_string, definition->chd_sha1))
  {
    Error::SetStringFmt(error, "Konami GV CHD '{}' SHA-1 mismatch.", content.chd_path);
    return std::nullopt;
  }
  INFO_LOG("KonamiGV.Content preflight_complete canonical_set='{}' chd_sha1='{}'", definition->set_name,
           definition->chd_sha1);
  return content;
}

bool IsGVSet(std::string_view set_name)
{
  return (GetGVGameDefinition(set_name) != nullptr);
}

const char* GetGVBIOSProfileName(GVBIOSProfile profile)
{
  switch (profile)
  {
    case GVBIOSProfile::KonamiGV:
      return "konamigv";

    default:
      return "unknown";
  }
}

} // namespace Konami
