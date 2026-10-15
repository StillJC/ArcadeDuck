// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Error;

namespace MAMECheats {

// MAME exposes temp0-temp9 by default and allows a cheat to request additional persistent temporaries.
constexpr u32 DEFAULT_TEMP_VARIABLES = 10;
constexpr u32 MAX_OUTPUT_ARGUMENTS = 32;

enum class NumberFormat : u8
{
  Decimal,
  DecimalHash,
  HexDollar,
  HexC,
};

struct Number
{
  u64 value = 0;
  NumberFormat format = NumberFormat::Decimal;
};

enum class ScriptState : u8
{
  Off,
  On,
  Run,
  Change,
  Count,
};

enum class ScriptEntryType : u8
{
  Action,
  Output,
};

enum class OutputAlignment : u8
{
  Left,
  Center,
  Right,
};

struct ParameterItem
{
  std::string text;
  Number value;
};

struct Parameter
{
  Number minimum;
  Number maximum;
  Number step{1, NumberFormat::Decimal};
  std::vector<ParameterItem> items;

  bool HasItemList() const { return !items.empty(); }
};

struct OutputArgument
{
  std::string expression;
  u32 count = 1;
};

struct ScriptEntry
{
  ScriptEntryType type = ScriptEntryType::Action;
  std::string condition;
  std::string expression;
  std::string format;
  s32 line = 0;
  OutputAlignment alignment = OutputAlignment::Left;
  std::vector<OutputArgument> arguments;
};

struct Script
{
  ScriptState state = ScriptState::Run;
  std::vector<ScriptEntry> entries;
};

struct Entry
{
  std::string description;
  std::string comment;
  std::optional<Parameter> parameter;
  std::array<std::optional<Script>, static_cast<size_t>(ScriptState::Count)> scripts;
  u32 temp_variables = DEFAULT_TEMP_VARIABLES;

  const Script* GetScript(ScriptState state) const
  {
    const std::optional<Script>& script = scripts[static_cast<size_t>(state)];
    return script.has_value() ? &script.value() : nullptr;
  }
  bool HasScript(ScriptState state) const { return scripts[static_cast<size_t>(state)].has_value(); }
  bool IsTextOnly() const;
  bool IsOneShot() const;
  bool IsOnOff() const;
  bool IsValueParameter() const;
  bool IsItemListParameter() const;
  bool IsOneShotParameter() const;
};

class List
{
public:
  const Entry& GetEntry(u32 index) const { return m_entries[index]; }
  Entry& GetEntry(u32 index) { return m_entries[index]; }
  u32 GetEntryCount() const { return static_cast<u32>(m_entries.size()); }
  const std::vector<Entry>& GetEntries() const { return m_entries; }
  std::vector<Entry>& GetEntries() { return m_entries; }
  void Clear() { m_entries.clear(); }

private:
  std::vector<Entry> m_entries;
};

bool ParseXML(std::string_view filename, std::string_view xml, List* list, Error* error = nullptr);
bool LoadXMLFile(const char* filename, List* list, Error* error = nullptr);

} // namespace MAMECheats
