#pragma once
#include "core/types.h"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class CDImage;

struct GameDatabaseEntry
{
  std::string serial;
  std::string title;
  std::string genre;
  std::string developer;
  std::string publisher;

  // ArcadeDuck metadata. For arcade sets, "serial" is the MAME set shortname.
  std::string arcade_system;
  std::string arcade_chd_folder;
  std::string arcade_chd_name;
  std::string arcade_eeprom;
  std::string arcade_hardware_profile;
  std::string arcade_input_profile;

  u64 release_date;
  u32 min_players;
  u32 max_players;
  u32 min_blocks;
  u32 max_blocks;
  u32 supported_controllers_mask;
};

class GameDatabase
{
public:
  GameDatabase();
  ~GameDatabase();

  bool Load();
  void Unload();

  bool GetEntryForDisc(CDImage* image, GameDatabaseEntry* entry);

  bool GetEntryForCode(const std::string_view& code, GameDatabaseEntry* entry);

  bool GetTitleAndSerialForDisc(CDImage* image, GameDatabaseEntry* entry);
  //bool Get

private:
  void* m_json = nullptr;
};
