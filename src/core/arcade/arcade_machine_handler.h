// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/arcade_database.h"
#include "core/types.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

class Error;

namespace BIOS {
struct Image;
}

namespace Arcade {

enum class VideoTimingStandard : u8
{
  Invalid,
  NTSCDerived,
  PALDerived,
};

/// Immutable database identity plus launch-specific paths for one arcade boot.
/// Database definitions are referenced directly and remain owned by Arcade::Database.
struct BootContext
{
  std::string_view canonical_game_id;
  std::string archive_path;
  const Database::GameDefinition* game_definition = nullptr;
  const Database::SystemDefinition* system_definition = nullptr;
  std::string_view system_id;
  std::string_view machine_handler_id;
  std::string_view bios_profile_id;
  const Database::FirmwareDefinition* firmware_definition = nullptr;
  std::string_view region;
  const Database::DisplayDefinition* display = nullptr;
  std::string data_root;
};

BootContext BuildBootContext(std::string_view archive_path, const Database::GameDefinition& game,
                             const Database::SystemDefinition& system, std::string_view data_root);

std::string GetPersistentStorageDirectory(std::string_view data_root, std::string_view canonical_game_id);

class MachineHandler
{
public:
  virtual ~MachineHandler() = default;

  /// Returns true only after this handler's complete-system save states have been certified.
  virtual bool SupportsSaveStates() const { return false; }

  /// Selects the base hardware timing before shared GPU construction.
  virtual VideoTimingStandard GetVideoTiming() const = 0;

  /// Validates and retains the board firmware and game content required after shared initialization.
  virtual bool Preflight(const BootContext& context, Error* error) = 0;

  /// Returns a board RAM override, or no value to retain the generic configured RAM size.
  virtual std::optional<u32> GetRAMSizeOverride() const { return std::nullopt; }

  /// Applies board-specific configuration which must precede shared GPU/system construction.
  virtual void PrepareSharedHardware() const = 0;

  /// Installs validated firmware/content and initializes board devices after shared initialization.
  virtual bool Initialize(const BootContext& context, Error* error) = 0;

  /// Returns an installed BIOS whose identity should be published by System, when applicable.
  virtual const BIOS::Image* GetBIOSImageForSystemIdentity() const { return nullptr; }
};

/// Creates a registered machine handler. Returns null for unknown or unsupported handler IDs.
std::unique_ptr<MachineHandler> CreateMachineHandler(std::string_view id);

/// Queries a registered handler's save-state capability without starting the machine.
/// Unknown handlers are unsupported.
bool MachineHandlerSupportsSaveStates(std::string_view id);

} // namespace Arcade
