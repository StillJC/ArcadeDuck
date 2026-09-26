// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_machine_handler.h"

#include "core/arcade/systems/konami/konami.h"
#include "core/arcade/systems/namco/system11/namco_system11.h"
#include "core/arcade/systems/sony/zn/sony_zn.h"
#include "core/bios.h"
#include "core/bus.h"
#include "core/gpu.h"
#include "core/settings.h"

#include "common/error.h"
#include "common/log.h"
#include "common/path.h"

#include <utility>

Log_SetChannel(ArcadeMachineHandler);

namespace Arcade {
namespace {

std::string GetContextPersistenceDirectory(const BootContext& context)
{
  return GetPersistentStorageDirectory(context.data_root, context.canonical_game_id);
}

class KonamiGVHandler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    m_bios = BIOS::LoadKonamiGVImageFromDirectory(EmuFolders::Bios.c_str(), error);
    if (!m_bios.has_value())
      return false;

    m_content = Konami::LoadGVContent(context.archive_path.c_str(), error);
    return m_content.has_value();
  }

  void PrepareSharedHardware() const override { GPU::SetGQMode(false); }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_bios.has_value() || !m_content.has_value())
    {
      Error::SetStringView(error, "Invalid Konami GV boot content.");
      return false;
    }

    const std::string persistence_directory = GetContextPersistenceDirectory(context);
    if (!Konami::InitializeGV(*m_bios, *m_content, persistence_directory, error))
      return false;

    VERBOSE_LOG("KonamiGV.Loader dispatch_ready canonical_set='{}' chd_path='{}'", context.canonical_game_id,
             m_content->chd_path);
    return true;
  }

private:
  std::optional<BIOS::Image> m_bios;
  std::optional<Konami::GVLoadedContent> m_content;
};

class KonamiGQHandler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    m_bios = BIOS::LoadKonamiGQImageFromArchive(context.archive_path.c_str(), error);
    if (!m_bios.has_value())
      return false;

    m_content = Konami::LoadGQContent(context.archive_path.c_str(), error);
    return m_content.has_value();
  }

  std::optional<u32> GetRAMSizeOverride() const override { return Bus::RAM_4MB_SIZE; }

  void PrepareSharedHardware() const override { GPU::SetGQMode(true); }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_bios.has_value() || !m_content.has_value())
    {
      Error::SetStringView(error, "Invalid Konami GQ boot content.");
      return false;
    }

    const size_t sound_program_size = m_content->sound_program.size();
    const size_t pcm_size = m_content->pcm_samples.size();
    const std::string persistence_directory = GetContextPersistenceDirectory(context);
    if (!Konami::InitializeGQ(*m_bios, std::move(*m_content), persistence_directory, error))
      return false;

    VERBOSE_LOG("KonamiGQ.Loader dispatch_ready canonical_set='{}' sound_program_size={} pcm_size={} chd_path='{}'",
             context.canonical_game_id, sound_program_size, pcm_size, Konami::GetGQCHDPath());
    return true;
  }

  const BIOS::Image* GetBIOSImageForSystemIdentity() const override
  {
    return m_bios.has_value() ? &m_bios.value() : nullptr;
  }

private:
  std::optional<BIOS::Image> m_bios;
  std::optional<Konami::GQLoadedContent> m_content;
};

class NamcoSystem11Handler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    if (!context.game_definition)
    {
      Error::SetStringView(error, "Missing Namco System 11 game definition.");
      return false;
    }

    // MAME maps Tekken and the older Tekken 2 revisions to COH-100/CXD8538Q.
    // Later System 11 profiles use COH-110/CXD8561Q; both boards have 2 MiB VRAM.
    m_coh100 = (context.game_definition->hardware_profile == "tekken" ||
                context.game_definition->hardware_profile == "tekken2o");
    if (!context.firmware_definition)
    {
      Error::SetStringView(error, "Missing Namco System 11 C76 firmware definition.");
      return false;
    }

    const std::string firmware_archive_path =
      Path::Combine(EmuFolders::Bios, context.firmware_definition->archive_name);

    m_content = NamcoSystem11::LoadSystem11Content(context.archive_path.c_str(), *context.game_definition,
                                                firmware_archive_path.c_str(), *context.firmware_definition, error);
    return m_content.has_value();
  }

  std::optional<u32> GetRAMSizeOverride() const override { return Bus::RAM_4MB_SIZE; }

  void PrepareSharedHardware() const override
  {
    if (m_coh100)
    {
      // COH-100 uses the CXD8538Q/type-1 register layout with 2 MiB VRAM.
      GPU::SetGQMode(true);
    }
    else
    {
      // COH-110 uses the CXD8561Q/type-2 register layout with 2 MiB VRAM.
      GPU::SetCXD8561QMode(true);
    }
  }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_content.has_value())
    {
      Error::SetStringView(error, "Invalid Namco System 11 boot content.");
      return false;
    }

    if (!NamcoSystem11::Initialize(std::move(*m_content), error))
      return false;

    VERBOSE_LOG("NamcoSystem11.Loader dispatch_ready canonical_set='{}' profile='{}'", context.canonical_game_id,
             context.game_definition->hardware_profile);
    return true;
  }

private:
  bool m_coh100 = false;
  std::optional<NamcoSystem11::LoadedContent> m_content;
};

class CapcomZN1Handler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    if (!context.game_definition || !context.firmware_definition)
    {
      Error::SetStringView(error, "Missing Capcom ZN-1 game or firmware definition.");
      return false;
    }

    // MAME uses the same Capcom ZN-1 machine around both board populations:
    // COH-1000C has 1 MiB VRAM, while COH-1002C has the second 1 MiB VRAM populated.
    const bool supported_coh1000c =
      context.canonical_game_id == "ts2" || context.canonical_game_id == "ts2u" ||
      context.canonical_game_id == "ts2ua" || context.canonical_game_id == "ts2j" ||
      context.canonical_game_id == "ts2ja" || context.canonical_game_id == "starglad" ||
      context.canonical_game_id == "stargladj" || context.canonical_game_id == "glpracr" ||
      context.canonical_game_id == "glpracrj";
    const bool supported_coh1002c =
      context.canonical_game_id == "sfex" || context.canonical_game_id == "sfexu" ||
      context.canonical_game_id == "sfexa" || context.canonical_game_id == "sfexj" ||
      context.canonical_game_id == "sfexp" || context.canonical_game_id == "sfexpu1" ||
      context.canonical_game_id == "sfexpj" || context.canonical_game_id == "sfexpj1";

    const bool profile_matches =
      (supported_coh1000c && context.game_definition->hardware_profile == "coh1000c") ||
      (supported_coh1002c && context.game_definition->hardware_profile == "coh1002c");
    if (!profile_matches)
    {
      Error::SetStringFmt(
        error,
        "Capcom ZN-1 bring-up supports TS2/Star Gladiator/Gallop Racer on COH-1000C and "
        "Street Fighter EX/EX Plus on COH-1002C; requested '{}' with profile '{}'.",
        context.canonical_game_id, context.game_definition->hardware_profile);
      return false;
    }

    m_use_2mb_vram = supported_coh1002c;

    const std::string firmware_archive_path =
      Path::Combine(EmuFolders::Bios, context.firmware_definition->archive_name);

    // MAME's COH-1000C BIOS definition selects the Japanese mask ROM as the default BIOS.
    // Keep that explicit during bring-up rather than silently inferring a BIOS from game region.
    m_bios = SonyZN::LoadFirmwareBIOS(firmware_archive_path.c_str(), *context.firmware_definition, "j", error);
    if (!m_bios.has_value())
      return false;

    m_content = SonyZN::LoadCapcomZN1Content(context.archive_path.c_str(), *context.game_definition,
                                            firmware_archive_path.c_str(), *context.firmware_definition, error);
    return m_content.has_value();
  }

  std::optional<u32> GetRAMSizeOverride() const override { return Bus::RAM_4MB_SIZE; }

  void PrepareSharedHardware() const override
  {
    // Both boards use CXD8561Q/type-2 GPU behavior. COH-1002C populates the
    // second 1 MiB VRAM device; COH-1000C leaves it unpopulated.
    GPU::SetCXD8561QMode(m_use_2mb_vram);
  }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_bios.has_value() || !m_content.has_value())
    {
      Error::SetStringView(error, "Invalid Capcom ZN-1 boot content.");
      return false;
    }

    const std::string persistence_directory = GetContextPersistenceDirectory(context);
    if (!SonyZN::InitializeCapcomZN1(*m_bios, std::move(*m_content), persistence_directory, error))
      return false;

    VERBOSE_LOG("SonyZN.Loader dispatch_ready canonical_set='{}' profile='{}'", context.canonical_game_id,
             context.game_definition->hardware_profile);
    return true;
  }

  const BIOS::Image* GetBIOSImageForSystemIdentity() const override
  {
    return m_bios.has_value() ? &m_bios.value() : nullptr;
  }

private:
  bool m_use_2mb_vram = false;
  std::optional<BIOS::Image> m_bios;
  std::optional<SonyZN::CapcomZN1Content> m_content;
};

class VideoSystemZN1Handler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    if (!context.game_definition || !context.firmware_definition)
    {
      Error::SetStringView(error, "Missing Video System ZN-1 game or firmware definition.");
      return false;
    }

    const bool supported = context.canonical_game_id == "aerofgts" || context.canonical_game_id == "aerofgtst" ||
                           context.canonical_game_id == "sncwgltd";
    if (!supported || context.game_definition->hardware_profile != "coh1002v")
    {
      Error::SetStringFmt(error, "Unsupported Video System ZN-1 set '{}' with profile '{}'.",
                          context.canonical_game_id, context.game_definition->hardware_profile);
      return false;
    }

    const std::string firmware_archive_path =
      Path::Combine(EmuFolders::Bios, context.firmware_definition->archive_name);

    m_bios = SonyZN::LoadFirmwareBIOS(firmware_archive_path.c_str(), *context.firmware_definition, "", error);
    if (!m_bios.has_value())
      return false;

    m_content = SonyZN::LoadVideoSystemZN1Content(context.archive_path.c_str(), *context.game_definition,
                                                  firmware_archive_path.c_str(), *context.firmware_definition, error);
    return m_content.has_value();
  }

  std::optional<u32> GetRAMSizeOverride() const override { return Bus::RAM_4MB_SIZE; }

  void PrepareSharedHardware() const override
  {
    // COH-1002V uses the CXD8561Q/type-2 GPU with both 1 MiB VRAM devices populated.
    GPU::SetCXD8561QMode(true);
  }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_bios.has_value() || !m_content.has_value())
    {
      Error::SetStringView(error, "Invalid Video System ZN-1 boot content.");
      return false;
    }

    const std::string persistence_directory = GetContextPersistenceDirectory(context);
    if (!SonyZN::InitializeVideoSystemZN1(*m_bios, std::move(*m_content), persistence_directory, error))
      return false;

    VERBOSE_LOG("SonyZN.Loader dispatch_ready canonical_set='{}' profile='{}' family='video_system_zn1'",
             context.canonical_game_id, context.game_definition->hardware_profile);
    return true;
  }

  const BIOS::Image* GetBIOSImageForSystemIdentity() const override
  {
    return m_bios.has_value() ? &m_bios.value() : nullptr;
  }

private:
  std::optional<BIOS::Image> m_bios;
  std::optional<SonyZN::VideoSystemZN1Content> m_content;
};

class AtlusZN1Handler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    if (!context.game_definition || !context.firmware_definition)
    {
      Error::SetStringView(error, "Missing Atlus ZN-1 game or firmware definition.");
      return false;
    }

    if (context.canonical_game_id != "hvnsgate" || context.game_definition->hardware_profile != "coh1001l")
    {
      Error::SetStringFmt(error, "Unsupported Atlus ZN-1 set '{}' with profile '{}'.",
                          context.canonical_game_id, context.game_definition->hardware_profile);
      return false;
    }

    const std::string firmware_archive_path =
      Path::Combine(EmuFolders::Bios, context.firmware_definition->archive_name);

    m_bios = SonyZN::LoadFirmwareBIOS(firmware_archive_path.c_str(), *context.firmware_definition, "", error);
    if (!m_bios.has_value())
      return false;

    m_content = SonyZN::LoadAtlusZN1Content(context.archive_path.c_str(), *context.game_definition,
                                           firmware_archive_path.c_str(), *context.firmware_definition, error);
    return m_content.has_value();
  }

  std::optional<u32> GetRAMSizeOverride() const override { return Bus::RAM_4MB_SIZE; }

  void PrepareSharedHardware() const override
  {
    // COH-1001L uses the ZN-1 type-2 GPU path with both 1 MiB VRAM devices populated.
    GPU::SetCXD8561QMode(true);
  }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_bios.has_value() || !m_content.has_value())
    {
      Error::SetStringView(error, "Invalid Atlus ZN-1 boot content.");
      return false;
    }

    const std::string persistence_directory = GetContextPersistenceDirectory(context);
    if (!SonyZN::InitializeAtlusZN1(*m_bios, std::move(*m_content), persistence_directory, error))
      return false;

    VERBOSE_LOG("SonyZN.Loader dispatch_ready canonical_set='{}' profile='{}' family='atlus_zn1'",
             context.canonical_game_id, context.game_definition->hardware_profile);
    return true;
  }

  const BIOS::Image* GetBIOSImageForSystemIdentity() const override
  {
    return m_bios.has_value() ? &m_bios.value() : nullptr;
  }

private:
  std::optional<BIOS::Image> m_bios;
  std::optional<SonyZN::AtlusZN1Content> m_content;
};

class EightingRaizingZN1Handler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    if (!context.game_definition || !context.firmware_definition)
    {
      Error::SetStringView(error, "Missing Eighting/Raizing ZN-1 game or firmware definition.");
      return false;
    }

    m_use_bam2 = false;
    m_content.reset();
    m_bam2_content.reset();

    const bool bam2_hdd =
      context.canonical_game_id == "bam2" && context.game_definition->hardware_profile == "bam2hdd";
    const bool bam2_cdrom =
      context.canonical_game_id == "bam2a" && context.game_definition->hardware_profile == "bam2cdrom";
    m_use_bam2 = bam2_hdd || bam2_cdrom;

    const bool supported_eighting = context.canonical_game_id == "beastrzr" ||
                                    context.canonical_game_id == "beastrzra" ||
                                    context.canonical_game_id == "bldyroar" ||
                                    context.canonical_game_id == "bldyror2" ||
                                    context.canonical_game_id == "bldyror2u" ||
                                    context.canonical_game_id == "bldyror2a" ||
                                    context.canonical_game_id == "bldyror2j";
    if (!m_use_bam2 && (!supported_eighting || context.game_definition->hardware_profile != "coh1002e"))
    {
      Error::SetStringFmt(error, "Eighting/Raizing ZN-1 does not support '{}' with profile '{}'.",
                          context.canonical_game_id, context.game_definition->hardware_profile);
      return false;
    }

    const std::string firmware_archive_path =
      Path::Combine(EmuFolders::Bios, context.firmware_definition->archive_name);

    m_bios = SonyZN::LoadFirmwareBIOS(firmware_archive_path.c_str(), *context.firmware_definition, "", error);
    if (!m_bios.has_value())
      return false;

    if (m_use_bam2)
    {
      m_bam2_content = SonyZN::LoadBustAMove2ZN1Content(
        context.archive_path.c_str(), *context.game_definition, firmware_archive_path.c_str(),
        *context.firmware_definition, error);
      return m_bam2_content.has_value();
    }

    m_content = SonyZN::LoadEightingRaizingZN1Content(context.archive_path.c_str(), *context.game_definition,
                                                      firmware_archive_path.c_str(), *context.firmware_definition,
                                                      error);
    return m_content.has_value();
  }

  std::optional<u32> GetRAMSizeOverride() const override { return Bus::RAM_4MB_SIZE; }

  void PrepareSharedHardware() const override
  {
    // COH-1002E uses the CXD8561Q/type-2 GPU with both 1 MiB VRAM devices populated.
    GPU::SetCXD8561QMode(true);
  }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_bios.has_value() || (m_use_bam2 ? !m_bam2_content.has_value() : !m_content.has_value()))
    {
      Error::SetStringView(error, "Invalid Eighting/Raizing ZN-1 boot content.");
      return false;
    }

    const std::string persistence_directory = GetContextPersistenceDirectory(context);
    if (m_use_bam2)
    {
      if (!SonyZN::InitializeBustAMove2ZN1(*m_bios, std::move(*m_bam2_content), persistence_directory, error))
        return false;

      VERBOSE_LOG("SonyZN.Loader dispatch_ready canonical_set='{}' profile='{}' family='eighting_raizing_zn1/bam2'",
               context.canonical_game_id, context.game_definition->hardware_profile);
      return true;
    }

    if (!SonyZN::InitializeEightingRaizingZN1(*m_bios, std::move(*m_content), persistence_directory, error))
      return false;

    VERBOSE_LOG("SonyZN.Loader dispatch_ready canonical_set='{}' profile='{}' family='eighting_raizing_zn1'",
             context.canonical_game_id, context.game_definition->hardware_profile);
    return true;
  }

  const BIOS::Image* GetBIOSImageForSystemIdentity() const override
  {
    return m_bios.has_value() ? &m_bios.value() : nullptr;
  }

private:
  std::optional<BIOS::Image> m_bios;
  std::optional<SonyZN::EightingRaizingZN1Content> m_content;
  std::optional<SonyZN::BustAMove2ZN1Content> m_bam2_content;
  bool m_use_bam2 = false;
};

class TimeWarnerZN1Handler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    if (!context.game_definition || !context.firmware_definition)
    {
      Error::SetStringView(error, "Missing Time Warner ZN-1 game or firmware definition.");
      return false;
    }

    if (context.canonical_game_id != "primrag2" || context.game_definition->hardware_profile != "coh1000w")
    {
      Error::SetStringFmt(error, "Unsupported Time Warner ZN-1 set '{}' with profile '{}'.",
                          context.canonical_game_id, context.game_definition->hardware_profile);
      return false;
    }

    const std::string firmware_archive_path =
      Path::Combine(EmuFolders::Bios, context.firmware_definition->archive_name);

    m_bios = SonyZN::LoadFirmwareBIOS(firmware_archive_path.c_str(), *context.firmware_definition, "", error);
    if (!m_bios.has_value())
      return false;

    m_content = SonyZN::LoadTimeWarnerZN1Content(context.archive_path.c_str(), *context.game_definition,
                                                firmware_archive_path.c_str(), *context.firmware_definition, error);
    return m_content.has_value();
  }

  std::optional<u32> GetRAMSizeOverride() const override { return Bus::RAM_8MB_SIZE; }

  void PrepareSharedHardware() const override
  {
    // COH-1000W uses the CXD8561Q/type-2 GPU with 2 MiB VRAM populated.
    GPU::SetCXD8561QMode(true);
  }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_bios.has_value() || !m_content.has_value())
    {
      Error::SetStringView(error, "Invalid Time Warner ZN-1 boot content.");
      return false;
    }

    const std::string persistence_directory = GetContextPersistenceDirectory(context);
    if (!SonyZN::InitializeTimeWarnerZN1(*m_bios, std::move(*m_content), persistence_directory, error))
      return false;

    VERBOSE_LOG("SonyZN.Loader dispatch_ready canonical_set='{}' profile='{}' family='time_warner_zn1'",
             context.canonical_game_id, context.game_definition->hardware_profile);
    return true;
  }

  const BIOS::Image* GetBIOSImageForSystemIdentity() const override
  {
    return m_bios.has_value() ? &m_bios.value() : nullptr;
  }

private:
  std::optional<BIOS::Image> m_bios;
  std::optional<SonyZN::TimeWarnerZN1Content> m_content;
};

class AcclaimZN1Handler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    if (!context.game_definition || !context.firmware_definition)
    {
      Error::SetStringView(error, "Missing Acclaim ZN-1 game or firmware definition.");
      return false;
    }

    const bool supported =
      context.canonical_game_id == "nbajamex" || context.canonical_game_id == "nbajamexa" ||
      context.canonical_game_id == "jdredd" || context.canonical_game_id == "jdreddb";
    const bool profile_matches =
      (context.game_definition->hardware_profile == "nbajamex" &&
       (context.canonical_game_id == "nbajamex" || context.canonical_game_id == "nbajamexa")) ||
      (context.game_definition->hardware_profile == "jdredd" &&
       (context.canonical_game_id == "jdredd" || context.canonical_game_id == "jdreddb"));
    if (!supported || !profile_matches)
    {
      Error::SetStringFmt(error, "Unsupported Acclaim ZN-1 set '{}' with profile '{}'.",
                          context.canonical_game_id, context.game_definition->hardware_profile);
      return false;
    }

    const std::string firmware_archive_path =
      Path::Combine(EmuFolders::Bios, context.firmware_definition->archive_name);

    m_bios = SonyZN::LoadFirmwareBIOS(firmware_archive_path.c_str(), *context.firmware_definition, "", error);
    if (!m_bios.has_value())
      return false;

    m_content = SonyZN::LoadAcclaimZN1Content(context.archive_path.c_str(), *context.game_definition,
                                             firmware_archive_path.c_str(), *context.firmware_definition, error);
    return m_content.has_value();
  }

  std::optional<u32> GetRAMSizeOverride() const override { return Bus::RAM_4MB_SIZE; }

  void PrepareSharedHardware() const override
  {
    // COH-1000A uses the CXD8561Q/type-2 GPU with both 1 MiB VRAM devices populated.
    GPU::SetCXD8561QMode(true);
  }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_bios.has_value() || !m_content.has_value())
    {
      Error::SetStringView(error, "Invalid Acclaim ZN-1 boot content.");
      return false;
    }

    const std::string persistence_directory = GetContextPersistenceDirectory(context);
    if (!SonyZN::InitializeAcclaimZN1(*m_bios, std::move(*m_content), persistence_directory, error))
      return false;

    VERBOSE_LOG("SonyZN.Loader dispatch_ready canonical_set='{}' profile='{}' family='acclaim_zn1'",
             context.canonical_game_id, context.game_definition->hardware_profile);
    return true;
  }

  const BIOS::Image* GetBIOSImageForSystemIdentity() const override
  {
    return m_bios.has_value() ? &m_bios.value() : nullptr;
  }

private:
  std::optional<BIOS::Image> m_bios;
  std::optional<SonyZN::AcclaimZN1Content> m_content;
};

class TecmoTPSHandler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    if (!context.game_definition || !context.firmware_definition)
    {
      Error::SetStringView(error, "Missing Tecmo TPS game or firmware definition.");
      return false;
    }

    // Ordinary COH-1002M is validated. CBAJ uses the same base board plus its separate Z80/YMZ/FIFO sound board.
    // Brave Blade is the documented hybrid: COH-1002M/MG01 motherboard BIOS and security with the
    // Eighting/Raizing PS9805 game/sound board and Raizing bank select.
    m_use_eighting_hybrid = false;
    m_content.reset();
    m_eighting_content.reset();

    const bool ordinary_supported =
      context.canonical_game_id == "glpracr2" || context.canonical_game_id == "glpracr2j" ||
      context.canonical_game_id == "doapp" || context.canonical_game_id == "doappk" ||
      context.canonical_game_id == "shngmtkb" || context.canonical_game_id == "tondemo" ||
      context.canonical_game_id == "glpracr3" || context.canonical_game_id == "glpracr3j" ||
      context.canonical_game_id == "flamegun" || context.canonical_game_id == "flamegunj" ||
      context.canonical_game_id == "lpadv" || context.canonical_game_id == "tblkkuzu" ||
      context.canonical_game_id == "1on1gov" || context.canonical_game_id == "twcupmil" ||
      context.canonical_game_id == "mfjump";
    const bool cbaj_supported =
      context.canonical_game_id == "cbaj" || context.canonical_game_id == "cbajbl";
    const bool brave_blade_supported =
      context.canonical_game_id == "brvblade" || context.canonical_game_id == "brvbladeu" ||
      context.canonical_game_id == "brvbladea" || context.canonical_game_id == "brvbladej";
    const bool link_supported = context.canonical_game_id == "glpracr2l";
    const bool profile_matches =
      (ordinary_supported && context.game_definition->hardware_profile == "coh1002m") ||
      (cbaj_supported && context.game_definition->hardware_profile == "cbaj") ||
      (brave_blade_supported && context.game_definition->hardware_profile == "coh1002e") ||
      (link_supported && context.game_definition->hardware_profile == "coh1002ml");
    if (!profile_matches)
    {
      Error::SetStringFmt(error,
                          "Tecmo TPS handler does not yet support '{}' with profile '{}'.",
                          context.canonical_game_id, context.game_definition->hardware_profile);
      return false;
    }

    const std::string firmware_archive_path =
      Path::Combine(EmuFolders::Bios, context.firmware_definition->archive_name);

    m_bios = SonyZN::LoadFirmwareBIOS(firmware_archive_path.c_str(), *context.firmware_definition, "", error);
    if (!m_bios.has_value())
      return false;

    if (brave_blade_supported)
    {
      m_use_eighting_hybrid = true;
      m_eighting_content = SonyZN::LoadEightingRaizingZN1Content(
        context.archive_path.c_str(), *context.game_definition, firmware_archive_path.c_str(),
        *context.firmware_definition, error);
      return m_eighting_content.has_value();
    }

    m_content = SonyZN::LoadTecmoTPSContent(context.archive_path.c_str(), *context.game_definition,
                                            firmware_archive_path.c_str(), *context.firmware_definition, error);
    return m_content.has_value();
  }

  std::optional<u32> GetRAMSizeOverride() const override { return Bus::RAM_4MB_SIZE; }

  void PrepareSharedHardware() const override
  {
    // COH-1002M uses the CXD8561Q/type-2 GPU with both 1 MiB VRAM devices populated.
    GPU::SetCXD8561QMode(true);
  }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_bios.has_value() ||
        (m_use_eighting_hybrid ? !m_eighting_content.has_value() : !m_content.has_value()))
    {
      Error::SetStringView(error, "Invalid Tecmo TPS boot content.");
      return false;
    }

    const std::string persistence_directory = GetContextPersistenceDirectory(context);
    if (m_use_eighting_hybrid)
    {
      if (!SonyZN::InitializeEightingRaizingZN1(*m_bios, std::move(*m_eighting_content), persistence_directory, error))
        return false;

      VERBOSE_LOG("SonyZN.Loader dispatch_ready canonical_set='{}' profile='{}' family='tecmo_tps/brave_blade_hybrid'",
               context.canonical_game_id, context.game_definition->hardware_profile);
      return true;
    }

    if (!SonyZN::InitializeTecmoTPS(*m_bios, std::move(*m_content), persistence_directory, error))
      return false;

    VERBOSE_LOG("SonyZN.Loader dispatch_ready canonical_set='{}' profile='{}' family='tecmo_tps'",
             context.canonical_game_id, context.game_definition->hardware_profile);
    return true;
  }

  const BIOS::Image* GetBIOSImageForSystemIdentity() const override
  {
    return m_bios.has_value() ? &m_bios.value() : nullptr;
  }

private:
  std::optional<BIOS::Image> m_bios;
  std::optional<SonyZN::TecmoTPSContent> m_content;
  std::optional<SonyZN::EightingRaizingZN1Content> m_eighting_content;
  bool m_use_eighting_hybrid = false;
};

class TaitoFX1Handler final : public MachineHandler
{
public:
  VideoTimingStandard GetVideoTiming() const override { return VideoTimingStandard::NTSCDerived; }

  bool Preflight(const BootContext& context, Error* error) override
  {
    if (!context.game_definition || !context.firmware_definition)
    {
      Error::SetStringView(error, "Missing Taito FX-1 game or firmware definition.");
      return false;
    }

    const bool supported_taito_fx1a_family =
      context.canonical_game_id == "psyforce" || context.canonical_game_id == "psyforcej" ||
      context.canonical_game_id == "psyforcex" || context.canonical_game_id == "sfchamp" ||
      context.canonical_game_id == "sfchampo" || context.canonical_game_id == "sfchampu" ||
      context.canonical_game_id == "sfchampj" || context.canonical_game_id == "mgcldate" ||
      context.canonical_game_id == "mgcldtex";

    const bool supported_raystorm_family =
      context.canonical_game_id == "raystorm" || context.canonical_game_id == "raystormo" ||
      context.canonical_game_id == "raystormu" || context.canonical_game_id == "raystormj";

    const bool supported_ftimpact_family =
      context.canonical_game_id == "ftimpact" || context.canonical_game_id == "ftimpactu" ||
      context.canonical_game_id == "ftimpactj" || context.canonical_game_id == "ftimpcta";

    const bool supported_gdarius_family =
      context.canonical_game_id == "gdarius" || context.canonical_game_id == "gdariusu" ||
      context.canonical_game_id == "gdariusj" || context.canonical_game_id == "gdarius2";

    if (supported_taito_fx1a_family && context.game_definition->hardware_profile == "coh1000ta")
    {
      m_family = Family::FX1A;
      m_use_2mb_vram = false;
    }
    else if ((supported_raystorm_family || supported_ftimpact_family) &&
             context.game_definition->hardware_profile == "coh1000tb")
    {
      m_family = Family::FX1B;
      m_use_2mb_vram = false;
    }
    else if (supported_gdarius_family && context.game_definition->hardware_profile == "coh1002tb")
    {
      m_family = Family::FX1B;
      m_use_2mb_vram = true;
    }
    else
    {
      Error::SetStringFmt(
        error,
        "Taito FX-1 support covers the validated FX-1A families on COH-1000TA; RayStorm and Fighters' Impact "
        "families on COH-1000TB; and G-Darius on COH-1002TB; requested '{}' with profile '{}'.",
        context.canonical_game_id, context.game_definition->hardware_profile);
      return false;
    }

    const std::string firmware_archive_path =
      Path::Combine(EmuFolders::Bios, context.firmware_definition->archive_name);

    m_bios = SonyZN::LoadFirmwareBIOS(firmware_archive_path.c_str(), *context.firmware_definition, "", error);
    if (!m_bios.has_value())
      return false;

    if (m_family == Family::FX1A)
    {
      m_fx1a_content = SonyZN::LoadTaitoFX1AContent(context.archive_path.c_str(), *context.game_definition,
                                                   firmware_archive_path.c_str(), *context.firmware_definition, error);
      return m_fx1a_content.has_value();
    }

    m_fx1b_content = SonyZN::LoadTaitoFX1BContent(context.archive_path.c_str(), *context.game_definition,
                                                 firmware_archive_path.c_str(), *context.firmware_definition, error);
    return m_fx1b_content.has_value();
  }

  std::optional<u32> GetRAMSizeOverride() const override { return Bus::RAM_4MB_SIZE; }

  void PrepareSharedHardware() const override
  {
    // COH-1000TA/COH-1000TB use 1 MiB VRAM. G-Darius uses COH-1002TB
    // with the second 1 MiB VRAM device populated.
    GPU::SetCXD8561QMode(m_use_2mb_vram);
  }

  bool Initialize(const BootContext& context, Error* error) override
  {
    if (!m_bios.has_value())
    {
      Error::SetStringView(error, "Invalid Taito FX-1 boot content.");
      return false;
    }

    const std::string persistence_directory = GetContextPersistenceDirectory(context);
    if (m_family == Family::FX1A)
    {
      if (!m_fx1a_content.has_value())
      {
        Error::SetStringView(error, "Invalid Taito FX-1A boot content.");
        return false;
      }

      if (!SonyZN::InitializeTaitoFX1A(*m_bios, std::move(*m_fx1a_content), persistence_directory, error))
        return false;
    }
    else if (m_family == Family::FX1B)
    {
      if (!m_fx1b_content.has_value())
      {
        Error::SetStringView(error, "Invalid Taito FX-1B boot content.");
        return false;
      }

      if (!SonyZN::InitializeTaitoFX1B(*m_bios, std::move(*m_fx1b_content), persistence_directory, error))
        return false;
    }
    else
    {
      Error::SetStringView(error, "Taito FX-1 handler has no selected hardware family.");
      return false;
    }

    VERBOSE_LOG("SonyZN.Loader dispatch_ready canonical_set='{}' profile='{}' family='{}'",
             context.canonical_game_id, context.game_definition->hardware_profile,
             m_family == Family::FX1A ? "taito_fx1a" : "taito_fx1b");
    return true;
  }

  const BIOS::Image* GetBIOSImageForSystemIdentity() const override
  {
    return m_bios.has_value() ? &m_bios.value() : nullptr;
  }

private:
  enum class Family : u8
  {
    None,
    FX1A,
    FX1B,
  };

  Family m_family = Family::None;
  bool m_use_2mb_vram = false;
  std::optional<BIOS::Image> m_bios;
  std::optional<SonyZN::TaitoFX1AContent> m_fx1a_content;
  std::optional<SonyZN::TaitoFX1BContent> m_fx1b_content;
};
} // namespace

BootContext BuildBootContext(std::string_view archive_path, const Database::GameDefinition& game,
                             const Database::SystemDefinition& system, std::string_view data_root)
{
  BootContext context;
  context.canonical_game_id = game.id;
  context.archive_path.assign(archive_path);
  context.game_definition = &game;
  context.system_definition = &system;
  context.system_id = game.system_id;
  context.machine_handler_id = system.machine_handler;
  context.bios_profile_id = system.bios_profile;
  context.firmware_definition = Database::ResolveFirmwareProfile(system);
  context.region = game.region;
  context.display = &game.display;
  context.data_root.assign(data_root);

  return context;
}

std::string GetPersistentStorageDirectory(std::string_view data_root, std::string_view canonical_game_id)
{
  return Path::Combine(Path::Combine(data_root, "nvram"), canonical_game_id);
}

std::unique_ptr<MachineHandler> CreateMachineHandler(std::string_view id)
{
  if (id == "konami_gv")
    return std::make_unique<KonamiGVHandler>();
  if (id == "konami_gq")
    return std::make_unique<KonamiGQHandler>();
  if (id == "namco_system11")
    return std::make_unique<NamcoSystem11Handler>();
  if (id == "capcom_zn1")
    return std::make_unique<CapcomZN1Handler>();
  if (id == "video_system_zn1")
    return std::make_unique<VideoSystemZN1Handler>();
  if (id == "atlus_zn1")
    return std::make_unique<AtlusZN1Handler>();
  if (id == "eighting_raizing_zn1")
    return std::make_unique<EightingRaizingZN1Handler>();
  if (id == "acclaim_zn1")
    return std::make_unique<AcclaimZN1Handler>();
  if (id == "time_warner_zn1")
    return std::make_unique<TimeWarnerZN1Handler>();
  if (id == "tecmo_tps")
    return std::make_unique<TecmoTPSHandler>();
  if (id == "taito_fx1")
    return std::make_unique<TaitoFX1Handler>();

  return {};
}

bool MachineHandlerSupportsSaveStates(std::string_view id)
{
  const std::unique_ptr<MachineHandler> handler = CreateMachineHandler(id);
  return handler && handler->SupportsSaveStates();
}

} // namespace Arcade
