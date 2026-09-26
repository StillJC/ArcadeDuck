// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/konami/gq/konami_gq_eeprom.h"

#include "core/settings.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include <array>
#include <cstring>
#include <optional>
#include <string>

Log_SetChannel(KonamiGQEEPROM);

namespace KonamiGQEEPROM {

namespace {

static constexpr u32 WORD_COUNT = 64;
static constexpr u32 DSW_BASE_OFFSET = 0x238000;
static constexpr u32 DSW_DEFAULT_VALUE = 0x000000fd;
static constexpr u32 COMMAND_BITS = 8;
static constexpr u32 DATA_BITS = 16;

static constexpr std::array<u8, EEPROM_SIZE> s_cryptklr_default = {{
  0x2b, 0x29, 0x56, 0x52, 0x94, 0x20, 0x55, 0x41, 0x41, 0x00, 0x14, 0x14, 0x03, 0x00, 0x01, 0x01,
  0x03, 0x01, 0x00, 0x00, 0x07, 0x07, 0x01, 0x00, 0x00, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
  0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
  0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
  0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
  0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
  0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
  0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
}};

enum class ProtocolState : u8
{
  WaitForStartBit,
  WaitForCommand,
  ReadingData,
  WaitingForWriteData,
};

struct State
{
  std::string set_name;
  std::string persistence_directory;
  std::string path;
  std::array<u8, EEPROM_SIZE> data = {};

  bool active = false;
  bool dirty = false;
  bool access_logged = false;
  bool write_enabled = false;
  bool di = false;
  bool data_out = true;
  bool cs = false;
  bool clk = false;
  bool sound_cpu_released = false;

  ProtocolState protocol_state = ProtocolState::WaitForStartBit;
  u32 command_shift = 0;
  u32 command_bits = 0;
  u16 data_shift = 0;
  u32 data_bits = 0;
  u8 address = 0;
  bool write_all = false;

  u32 transaction_index = 0;
  bool dsw_value_logged = false;
};

static std::optional<State> s_state;

static void ResetTransaction(State& state)
{
  state.data_out = true;
  state.protocol_state = ProtocolState::WaitForStartBit;
  state.command_shift = 0;
  state.command_bits = 0;
  state.data_shift = 0;
  state.data_bits = 0;
  state.address = 0;
  state.write_all = false;
}

static void LogFirstAccess(State& state)
{
  if (state.access_logged)
    return;

  state.access_logged = true;
  DEV_LOG("KonamiGQ.EEPROM first_access canonical_set='{}'", state.set_name);
}

static u16 GetWord(const State& state, u8 address)
{
  const size_t offset = static_cast<size_t>(address & 0x3f) * 2;
  return static_cast<u16>(state.data[offset] | (static_cast<u16>(state.data[offset + 1]) << 8));
}

static void MarkDirty(State& state)
{
  if (!state.dirty)
    DEV_LOG("KonamiGQ.EEPROM dirty canonical_set='{}'", state.set_name);
  state.dirty = true;
}

static void SetWord(State& state, u8 address, u16 value)
{
  const size_t offset = static_cast<size_t>(address & 0x3f) * 2;
  if (GetWord(state, address) == value)
    return;

  state.data[offset] = static_cast<u8>(value);
  state.data[offset + 1] = static_cast<u8>(value >> 8);
  MarkDirty(state);
}

static bool EnsurePersistenceDirectory(const State& state)
{
  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  return FileSystem::CreateDirectory(nvram_root.c_str(), false) &&
         FileSystem::CreateDirectory(state.persistence_directory.c_str(), false);
}

static bool Save(State& state)
{
  if (!state.dirty)
    return true;

  if (!EnsurePersistenceDirectory(state) ||
      !FileSystem::WriteBinaryFile(state.path.c_str(), state.data.data(), state.data.size()))
  {
    ERROR_LOG("KonamiGQ.EEPROM save_failed canonical_set='{}' path='{}'", state.set_name, state.path);
    return false;
  }

  state.dirty = false;
  VERBOSE_LOG("KonamiGQ.EEPROM saved canonical_set='{}' path='{}'", state.set_name, state.path);
  return true;
}

static void CompleteWrite(State& state)
{
  if (!state.write_enabled)
  {
    DEV_LOG("KonamiGQ.EEPROM write_ignored_locked transaction={} address=0x{:02X} value=0x{:04X}",
                state.transaction_index, state.address, state.data_shift);
  }
  else if (state.write_all)
  {
    for (u8 address = 0; address < WORD_COUNT; address++)
      SetWord(state, address, state.data_shift);

    Save(state);
    DEV_LOG("KonamiGQ.EEPROM write_all transaction={} value=0x{:04X}", state.transaction_index,
             state.data_shift);
  }
  else
  {
    SetWord(state, state.address, state.data_shift);
    Save(state);
    DEV_LOG("KonamiGQ.EEPROM write_word transaction={} address=0x{:02X} value=0x{:04X}",
             state.transaction_index, state.address, state.data_shift);
  }

  state.data_out = true;
  state.protocol_state = ProtocolState::WaitForStartBit;
  state.command_shift = 0;
  state.command_bits = 0;
  state.data_shift = 0;
  state.data_bits = 0;
  state.address = 0;
  state.write_all = false;
}

static void DecodeCommand(State& state)
{
  const u8 opcode = static_cast<u8>((state.command_shift >> 6) & 0x03);
  const u8 address = static_cast<u8>(state.command_shift & 0x3f);
  state.address = address;
  state.command_shift = 0;
  state.command_bits = 0;

  switch (opcode)
  {
    case 0x02: // READ
    {
      const u16 value = GetWord(state, address);
      state.data_shift = value;
      state.data_bits = 0;
      state.data_out = false; // Required dummy zero before the first data clock.
      state.protocol_state = ProtocolState::ReadingData;
      break;
    }

    case 0x01: // WRITE
      state.data_shift = 0;
      state.data_bits = 0;
      state.data_out = false;
      state.write_all = false;
      state.protocol_state = ProtocolState::WaitingForWriteData;
      DEV_LOG("KonamiGQ.EEPROM write_begin transaction={} address=0x{:02X} enabled={}",
               state.transaction_index, address, state.write_enabled);
      break;

    case 0x03: // ERASE
      if (state.write_enabled)
      {
        SetWord(state, address, 0xffff);
        Save(state);
        DEV_LOG("KonamiGQ.EEPROM erase_word transaction={} address=0x{:02X}", state.transaction_index,
                 address);
      }
      else
      {
        DEV_LOG("KonamiGQ.EEPROM erase_ignored_locked transaction={} address=0x{:02X}",
                    state.transaction_index, address);
      }
      ResetTransaction(state);
      break;

    case 0x00:
    default:
    {
      switch ((address >> 4) & 0x03)
      {
        case 0x00: // EWDS
          state.write_enabled = false;
          DEV_LOG("KonamiGQ.EEPROM write_disable transaction={}", state.transaction_index);
          ResetTransaction(state);
          break;

        case 0x01: // WRAL
          state.data_shift = 0;
          state.data_bits = 0;
          state.data_out = false;
          state.write_all = true;
          state.address = 0;
          state.protocol_state = ProtocolState::WaitingForWriteData;
          DEV_LOG("KonamiGQ.EEPROM write_all_begin transaction={} enabled={}", state.transaction_index,
                   state.write_enabled);
          break;

        case 0x02: // ERAL
          if (state.write_enabled)
          {
            for (u8 erase_address = 0; erase_address < WORD_COUNT; erase_address++)
              SetWord(state, erase_address, 0xffff);
            Save(state);
            DEV_LOG("KonamiGQ.EEPROM erase_all transaction={}", state.transaction_index);
          }
          else
          {
            DEV_LOG("KonamiGQ.EEPROM erase_all_ignored_locked transaction={}", state.transaction_index);
          }
          ResetTransaction(state);
          break;

        case 0x03: // EWEN
          state.write_enabled = true;
          DEV_LOG("KonamiGQ.EEPROM write_enable transaction={}", state.transaction_index);
          ResetTransaction(state);
          break;
      }
      break;
    }
  }
}

static void HandleClockRisingEdge(State& state)
{
  switch (state.protocol_state)
  {
    case ProtocolState::WaitForStartBit:
      if (state.di)
      {
        state.protocol_state = ProtocolState::WaitForCommand;
        state.command_shift = 0;
        state.command_bits = 0;
      }
      break;

    case ProtocolState::WaitForCommand:
      state.command_shift = (state.command_shift << 1) | (state.di ? 1U : 0U);
      state.command_bits++;
      if (state.command_bits == COMMAND_BITS)
        DecodeCommand(state);
      break;

    case ProtocolState::ReadingData:
      if (state.data_bits < DATA_BITS)
      {
        state.data_out = ((state.data_shift >> (15 - state.data_bits)) & 1) != 0;
        state.data_bits++;
      }
      else
      {
        // Non-streaming 93C46 reads shift high after the 16 data bits until CS falls.
        state.data_out = true;
      }
      break;

    case ProtocolState::WaitingForWriteData:
      state.data_shift = static_cast<u16>((state.data_shift << 1) | (state.di ? 1 : 0));
      state.data_bits++;
      if (state.data_bits == DATA_BITS)
        CompleteWrite(state);
      break;
  }
}

} // namespace

bool Initialize(std::string_view set_name, std::string_view persistence_directory, Error* error)
{
  Shutdown();

  if (set_name != "cryptklr")
  {
    Error::SetStringFmt(error, "No Konami GQ EEPROM default is defined for '{}'.", set_name);
    return false;
  }

  State state;
  state.set_name.assign(set_name);
  state.persistence_directory.assign(persistence_directory);
  state.path = Path::Combine(state.persistence_directory, "eeprom");

  const bool persistence_exists = FileSystem::FileExists(state.path.c_str());
  if (persistence_exists)
  {
    const auto persisted = FileSystem::ReadBinaryFile(state.path.c_str(), error);
    if (!persisted || persisted->size() != state.data.size())
    {
      Error::SetStringFmt(error, "Konami GQ EEPROM '{}' has invalid size; expected {} bytes.", state.path,
                          static_cast<u32>(state.data.size()));
      ERROR_LOG("KonamiGQ.EEPROM malformed_persistence canonical_set='{}' path='{}'", state.set_name, state.path);
      return false;
    }

    std::memcpy(state.data.data(), persisted->data(), state.data.size());
  }
  else
  {
    state.data = s_cryptklr_default;
    if (!EnsurePersistenceDirectory(state) ||
        !FileSystem::WriteBinaryFile(state.path.c_str(), state.data.data(), state.data.size()))
    {
      Error::SetStringFmt(error, "Failed to create Konami GQ EEPROM persistence '{}'.", state.path);
      ERROR_LOG("KonamiGQ.EEPROM persistence_write_failed canonical_set='{}' path='{}'", state.set_name,
                state.path);
      return false;
    }
  }

  ResetTransaction(state);
  state.active = true;
  state.write_enabled = false;
  s_state.emplace(std::move(state));

  VERBOSE_LOG("KonamiGQ.EEPROM {} canonical_set='{}' path='{}' size={}",
           persistence_exists ? "loaded_persistence" : "initialized_default_persistence", s_state->set_name,
           s_state->path, static_cast<u32>(s_state->data.size()));
  return true;
}

void Reset()
{
  if (!s_state || !s_state->active)
    return;

  State& state = *s_state;
  ResetTransaction(state);
  state.access_logged = false;
  state.write_enabled = false;
  state.di = false;
  state.cs = false;
  state.clk = false;
  state.sound_cpu_released = false;
  state.transaction_index = 0;
  state.dsw_value_logged = false;
  VERBOSE_LOG("KonamiGQ.EEPROM reset canonical_set='{}'", state.set_name);
}

void Shutdown()
{
  if (!s_state)
    return;

  Save(*s_state);
  VERBOSE_LOG("KonamiGQ.EEPROM shutdown canonical_set='{}'", s_state->set_name);
  s_state.reset();
}

bool IsActive()
{
  return s_state.has_value() && s_state->active;
}

bool IsSoundCPUReleased()
{
  return IsActive() && s_state->sound_cpu_released;
}

void WriteControl(u32 value)
{
  if (!IsActive())
    return;

  State& state = *s_state;
  LogFirstAccess(state);

  const bool di = (value & 0x01) != 0;
  const bool cs = (value & 0x02) != 0;
  const bool clk = (value & 0x04) != 0;
  const bool sound_cpu_released = (value & 0x40) != 0;
  if (sound_cpu_released != state.sound_cpu_released)
  {
    state.sound_cpu_released = sound_cpu_released;
    DEV_LOG("KonamiGQ.EEPROM sound_cpu_reset canonical_set='{}' state='{}'", state.set_name,
             sound_cpu_released ? "released" : "asserted");
  }

  state.di = di;

  if (cs != state.cs)
  {
    state.cs = cs;
    state.clk = clk;

    if (cs)
    {
      ResetTransaction(state);
      state.transaction_index++;
    }
    else
    {
      ResetTransaction(state);
    }

    // MAME's 93C46 state machine ignores a CLK edge that arrives in the same register write as CS rising.
    return;
  }

  if (!state.cs)
  {
    state.clk = clk;
    return;
  }

  if (!state.clk && clk)
    HandleClockRisingEdge(state);

  state.clk = clk;
}

static u8 GetDSWValue(const State& state)
{
  if (state.set_name != "cryptklr")
    return static_cast<u8>(DSW_DEFAULT_VALUE);

  // GQ420 DIP bank. Bits 6-7 are physically present but still unidentified, so preserve their inactive defaults.
  u8 value = 0xc0;

  if (g_settings.arcade_crypt_killer_stereo)
    value |= 0x01;
  if (g_settings.arcade_crypt_killer_endless_stages)
    value |= 0x02;
  if (!g_settings.arcade_crypt_killer_mirror)
    value |= 0x04;
  if (!g_settings.arcade_crypt_killer_woofer)
    value |= 0x08;
  if (g_settings.arcade_crypt_killer_three_players)
    value |= 0x10;
  if (g_settings.arcade_crypt_killer_common_coin_mechanism)
    value |= 0x20;

  return value;
}

u32 ReadDSW(u32 width, u32 offset)
{
  if (!IsActive())
    return UINT32_C(0xffffffff);

  State& state = *s_state;
  LogFirstAccess(state);

  const u8 dsw_value = GetDSWValue(state);
  const u32 value = static_cast<u32>(dsw_value) | (state.data_out ? UINT32_C(0x00010000) : 0);
  if (!state.dsw_value_logged)
  {
    state.dsw_value_logged = true;
    VERBOSE_LOG(
      "KonamiGQ.EEPROM dsw_snapshot canonical_set='{}' value=0x{:02X} stereo={} endless={} mirror={} woofer={} "
      "three_players={} common_coin={}",
      state.set_name, dsw_value, static_cast<bool>(g_settings.arcade_crypt_killer_stereo),
      static_cast<bool>(g_settings.arcade_crypt_killer_endless_stages),
      static_cast<bool>(g_settings.arcade_crypt_killer_mirror),
      static_cast<bool>(g_settings.arcade_crypt_killer_woofer),
      static_cast<bool>(g_settings.arcade_crypt_killer_three_players),
      static_cast<bool>(g_settings.arcade_crypt_killer_common_coin_mechanism));
  }
  const u32 byte_offset = offset - DSW_BASE_OFFSET;
  if (byte_offset >= sizeof(value))
    return UINT32_C(0xffffffff);
  const u32 shifted = value >> (byte_offset * 8);
  if (width == 1)
    return shifted & 0xff;
  if (width == 2)
    return shifted & 0xffff;
  return shifted;
}

} // namespace KonamiGQEEPROM
