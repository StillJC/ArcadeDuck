// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "mame_cheat_expression.h"
#include "mame_cheats.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace MAMECheats {

// PS1-derived arcade cheats address the CPU through MAME's maincpu logical program space.
// Other MAME memory spaces remain unsupported until a system-specific bridge is implemented.
class MainCPUMemoryAccess final : public ExpressionMemoryAccess
{
public:
  bool IsValid(std::string_view name, ExpressionMemorySpace space) const override;
  u64 Read(std::string_view name, ExpressionMemorySpace space, u32 address, u32 size,
           bool disable_side_effects) const override;
  void Write(std::string_view name, ExpressionMemorySpace space, u32 address, u32 size, u64 value,
             bool disable_side_effects) const override;
};

struct RuntimeOutput
{
  std::string format;
  s32 line = 0;
  OutputAlignment alignment = OutputAlignment::Left;
  std::array<u64, MAX_OUTPUT_ARGUMENTS> arguments{};
  u32 argument_count = 0;
};

class RuntimeList;

class RuntimeEntry
{
public:
  ~RuntimeEntry();

  RuntimeEntry(const RuntimeEntry&) = delete;
  RuntimeEntry& operator=(const RuntimeEntry&) = delete;

  const Entry& GetSource() const;
  ScriptState GetState() const;
  bool IsValid() const;
  const std::string& GetError() const;

  bool HasParameter() const;
  u64 GetParameterValue() const;
  bool IsParameterMinimum() const;
  bool IsParameterMaximum() const;
  const char* GetParameterItemText() const;

  bool Activate();
  bool SelectDefaultState();
  bool SelectPreviousState();
  bool SelectNextState();

private:
  struct Impl;

  RuntimeEntry(RuntimeList* manager, const Entry& source);

  void FrameUpdate();
  void ExecuteOffScript();
  void ExecuteOnScript();

  std::unique_ptr<Impl> m_impl;

  friend class RuntimeList;
};

class RuntimeList
{
public:
  RuntimeList();
  ~RuntimeList();

  RuntimeList(const RuntimeList&) = delete;
  RuntimeList& operator=(const RuntimeList&) = delete;

  void Initialize(const List& list, const ExpressionMemoryAccess* memory);
  void Clear();

  u32 GetEntryCount() const { return static_cast<u32>(m_entries.size()); }
  RuntimeEntry& GetEntry(u32 index) { return *m_entries[index]; }
  const RuntimeEntry& GetEntry(u32 index) const { return *m_entries[index]; }

  bool IsEnabled() const { return m_enabled; }
  void SetEnabled(bool enabled);

  u64 GetFrame() const { return m_frame; }
  const std::vector<RuntimeOutput>& GetOutputs() const { return m_outputs; }
  void ClearOutputs() { m_outputs.clear(); }

  void FrameUpdate();

private:
  void AddOutput(RuntimeOutput output);
  const ExpressionMemoryAccess* GetMemory() const { return m_memory; }

  const ExpressionMemoryAccess* m_memory = nullptr;
  std::vector<std::unique_ptr<RuntimeEntry>> m_entries;
  std::vector<RuntimeOutput> m_outputs;
  u64 m_frame = 0;
  bool m_enabled = true;

  friend class RuntimeEntry;
};

} // namespace MAMECheats
