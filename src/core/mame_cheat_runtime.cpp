// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "mame_cheat_runtime.h"

#include "cpu_core.h"

#include "common/error.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace MAMECheats {

namespace {

u64 ReadFailureValue(u32 size)
{
  if (size >= sizeof(u64))
    return std::numeric_limits<u64>::max();
  return (u64{1} << (size * 8)) - 1;
}

} // namespace

bool MainCPUMemoryAccess::IsValid(std::string_view name, ExpressionMemorySpace space) const
{
  // pb/pw/pd/pq are logical program-space accesses. pp* physical accesses are a distinct MAME namespace.
  return name == "maincpu" && space == ExpressionMemorySpace::ProgramLogical;
}

u64 MainCPUMemoryAccess::Read(std::string_view name, ExpressionMemorySpace space, u32 address, u32 size,
                              bool /*disable_side_effects*/) const
{
  if (!IsValid(name, space))
    return ReadFailureValue(size);

  switch (size)
  {
    case 1:
    {
      u8 value;
      return CPU::SafeReadMemoryByte(address, &value) ? value : ReadFailureValue(size);
    }
    case 2:
    {
      u16 value;
      return CPU::SafeReadMemoryHalfWord(address, &value) ? value : ReadFailureValue(size);
    }
    case 4:
    {
      u32 value;
      return CPU::SafeReadMemoryWord(address, &value) ? value : ReadFailureValue(size);
    }
    case 8:
    {
      // The PS1 bus is 32-bit; qword accesses are two adjacent little-endian word operations.
      u32 low;
      u32 high;
      if (!CPU::SafeReadMemoryWord(address, &low) || !CPU::SafeReadMemoryWord(address + 4, &high))
        return ReadFailureValue(size);
      return static_cast<u64>(low) | (static_cast<u64>(high) << 32);
    }
    default:
      return ReadFailureValue(size);
  }
}

void MainCPUMemoryAccess::Write(std::string_view name, ExpressionMemorySpace space, u32 address, u32 size, u64 value,
                                bool /*disable_side_effects*/) const
{
  if (!IsValid(name, space))
    return;

  switch (size)
  {
    case 1:
      CPU::SafeWriteMemoryByte(address, static_cast<u8>(value));
      break;
    case 2:
      CPU::SafeWriteMemoryHalfWord(address, static_cast<u16>(value));
      break;
    case 4:
      CPU::SafeWriteMemoryWord(address, static_cast<u32>(value));
      break;
    case 8:
      CPU::SafeWriteMemoryWord(address, static_cast<u32>(value));
      CPU::SafeWriteMemoryWord(address + 4, static_cast<u32>(value >> 32));
      break;
    default:
      break;
  }
}

namespace {

struct RuntimeOutputArgument
{
  RuntimeOutputArgument(ExpressionSymbolTable* symbols, const ExpressionMemoryAccess* memory, u32 count)
    : expression(symbols, memory), count(count)
  {
  }

  Expression expression;
  u32 count = 1;
};

struct RuntimeScriptEntry
{
  RuntimeScriptEntry(ExpressionSymbolTable* symbols, const ExpressionMemoryAccess* memory)
    : condition(symbols, memory), expression(symbols, memory)
  {
  }

  ScriptEntryType type = ScriptEntryType::Action;
  Expression condition;
  Expression expression;
  std::string format;
  s32 line = 0;
  OutputAlignment alignment = OutputAlignment::Left;
  std::vector<RuntimeOutputArgument> arguments;
};

struct RuntimeScript
{
  ScriptState state = ScriptState::Run;
  std::vector<RuntimeScriptEntry> entries;
};

void SetFirstError(std::string* destination, const Error& error)
{
  if (destination->empty() && error.IsValid())
    *destination = error.GetDescription();
}

} // namespace

struct RuntimeEntry::Impl
{
  Impl(RuntimeList* manager_, const Entry& source_)
    : manager(manager_), source(source_), parameter_value(0), argindex(0)
  {
    Compile();
  }

  void Compile()
  {
    if (!manager || !manager->GetMemory())
    {
      compile_error = "MAME cheat runtime has no memory provider";
      return;
    }

    // Temporary variables persist for the lifetime of one cheat entry, matching MAME script context.
    temp_values.resize(source.temp_variables);

    symbols.AddValue("frame", [this]() { return manager->GetFrame(); });
    symbols.AddFunction("frombcd", 1, 1, [](u32, const u64* parameters) { return FromBCD(parameters[0]); });
    symbols.AddFunction("tobcd", 1, 1, [](u32, const u64* parameters) { return ToBCD(parameters[0]); });
    symbols.AddValue("argindex", [this]() { return argindex; });

    for (u32 i = 0; i < static_cast<u32>(temp_values.size()); i++)
      symbols.AddValue("temp" + std::to_string(i), &temp_values[i]);

    if (source.parameter.has_value())
      symbols.AddValue("param", [this]() { return parameter_value; });

    for (u32 state_index = 0; state_index < static_cast<u32>(ScriptState::Count); state_index++)
    {
      const ScriptState state = static_cast<ScriptState>(state_index);
      const Script* source_script = source.GetScript(state);
      if (!source_script)
        continue;

      RuntimeScript runtime_script;
      runtime_script.state = state;
      runtime_script.entries.reserve(source_script->entries.size());
      for (const ScriptEntry& source_entry : source_script->entries)
      {
        RuntimeScriptEntry runtime_entry(&symbols, manager->GetMemory());
        runtime_entry.type = source_entry.type;
        runtime_entry.format = source_entry.format;
        runtime_entry.line = source_entry.line;
        runtime_entry.alignment = source_entry.alignment;

        Error error;
        if (!source_entry.condition.empty() && !runtime_entry.condition.Parse(source_entry.condition, &error))
        {
          compile_error = error.GetDescription();
          return;
        }

        if (source_entry.type == ScriptEntryType::Action)
        {
          if (!runtime_entry.expression.Parse(source_entry.expression, &error))
          {
            compile_error = error.GetDescription();
            return;
          }
        }
        else
        {
          runtime_entry.arguments.reserve(source_entry.arguments.size());
          for (const OutputArgument& source_argument : source_entry.arguments)
          {
            RuntimeOutputArgument runtime_argument(&symbols, manager->GetMemory(), source_argument.count);
            if (!runtime_argument.expression.Parse(source_argument.expression, &error))
            {
              compile_error = error.GetDescription();
              return;
            }
            runtime_entry.arguments.push_back(std::move(runtime_argument));
          }
        }

        runtime_script.entries.push_back(std::move(runtime_entry));
      }

      scripts[state_index] = std::move(runtime_script);
    }

    valid = true;
  }

  RuntimeScript* GetScript(ScriptState state)
  {
    std::optional<RuntimeScript>& script = scripts[static_cast<size_t>(state)];
    return script.has_value() ? &script.value() : nullptr;
  }

  bool ExecuteScript(ScriptState state)
  {
    if (!valid || !manager->IsEnabled())
      return false;

    RuntimeScript* script = GetScript(state);
    if (!script)
      return false;

    bool executed = false;
    for (RuntimeScriptEntry& entry : script->entries)
    {
      if (!entry.condition.IsEmpty())
      {
        Error error;
        u64 result = 0;
        if (!entry.condition.Execute(&result, &error))
        {
          SetFirstError(&runtime_error, error);
          continue;
        }
        if (!result)
          continue;
      }

      if (entry.type == ScriptEntryType::Action)
      {
        Error error;
        u64 result = 0;
        if (!entry.expression.Execute(&result, &error))
        {
          SetFirstError(&runtime_error, error);
          continue;
        }
        executed = true;
        continue;
      }

      RuntimeOutput output;
      output.format = entry.format;
      output.line = entry.line;
      output.alignment = entry.alignment;

      for (RuntimeOutputArgument& argument : entry.arguments)
      {
        // count= repeats one expression while exposing the current iteration as argindex.
        for (argindex = 0; argindex < argument.count && output.argument_count < MAX_OUTPUT_ARGUMENTS; argindex++)
        {
          Error error;
          u64 value = 0;
          if (!argument.expression.Execute(&value, &error))
            SetFirstError(&runtime_error, error);
          output.arguments[output.argument_count++] = value;
        }
      }

      manager->AddOutput(std::move(output));
      executed = true;
    }

    return executed;
  }

  bool IsMinimum() const
  {
    if (!source.parameter.has_value())
      return false;
    const Parameter& parameter = source.parameter.value();
    return parameter_value == (parameter.HasItemList() ? parameter.items.front().value.value : parameter.minimum.value);
  }

  bool IsMaximum() const
  {
    if (!source.parameter.has_value())
      return false;
    const Parameter& parameter = source.parameter.value();
    return parameter_value == (parameter.HasItemList() ? parameter.items.back().value.value : parameter.maximum.value);
  }

  bool SetMinimumParameter()
  {
    if (!source.parameter.has_value())
      return false;
    const u64 previous = parameter_value;
    const Parameter& parameter = source.parameter.value();
    parameter_value = parameter.HasItemList() ? parameter.items.front().value.value : parameter.minimum.value;
    return parameter_value != previous;
  }

  bool SetPreviousParameter()
  {
    if (!source.parameter.has_value())
      return false;

    const u64 previous = parameter_value;
    const Parameter& parameter = source.parameter.value();
    if (!parameter.HasItemList())
    {
      if (parameter_value < (parameter.minimum.value + parameter.step.value))
        parameter_value = parameter.minimum.value;
      else
        parameter_value -= parameter.step.value;
    }
    else
    {
      auto it = std::find_if(parameter.items.begin(), parameter.items.end(),
                             [this](const ParameterItem& item) { return item.value.value == parameter_value; });
      if (it != parameter.items.begin())
        parameter_value = std::prev(it)->value.value;
    }
    return parameter_value != previous;
  }

  bool SetNextParameter()
  {
    if (!source.parameter.has_value())
      return false;

    const u64 previous = parameter_value;
    const Parameter& parameter = source.parameter.value();
    if (!parameter.HasItemList())
    {
      if (parameter.step.value == 0 || parameter_value >= parameter.maximum.value ||
          parameter.step.value > (parameter.maximum.value - parameter_value))
      {
        parameter_value = parameter.maximum.value;
      }
      else
      {
        parameter_value += parameter.step.value;
      }
    }
    else
    {
      auto it = std::find_if(parameter.items.begin(), parameter.items.end(),
                             [this](const ParameterItem& item) { return item.value.value == parameter_value; });
      if (it != parameter.items.end() && ++it != parameter.items.end())
        parameter_value = it->value.value;
    }
    return parameter_value != previous;
  }

  bool SetState(ScriptState new_state)
  {
    // Entering/leaving a live state executes the corresponding MAME on/off transition script.
    if (state == new_state)
      return false;

    state = new_state;
    activation_result = ActivationResult::None;
    if (new_state == ScriptState::Off)
      ExecuteScript(ScriptState::Off);
    else if (new_state == ScriptState::On || new_state == ScriptState::Run)
      ExecuteScript(ScriptState::On);
    return true;
  }

  RuntimeList* manager;
  Entry source;
  ExpressionSymbolTable symbols;
  std::array<std::optional<RuntimeScript>, static_cast<size_t>(ScriptState::Count)> scripts;
  std::vector<u64> temp_values;
  u64 parameter_value;
  u64 argindex;
  ScriptState state = ScriptState::Off;
  ActivationResult activation_result = ActivationResult::None;
  bool valid = false;
  std::string compile_error;
  std::string runtime_error;
};

RuntimeEntry::RuntimeEntry(RuntimeList* manager, const Entry& source) : m_impl(std::make_unique<Impl>(manager, source))
{
}

RuntimeEntry::~RuntimeEntry() = default;

const Entry& RuntimeEntry::GetSource() const
{
  return m_impl->source;
}

ScriptState RuntimeEntry::GetState() const
{
  return m_impl->state;
}

ActivationResult RuntimeEntry::GetActivationResult() const
{
  return m_impl->activation_result;
}

bool RuntimeEntry::IsValid() const
{
  return m_impl->valid;
}

const std::string& RuntimeEntry::GetError() const
{
  return m_impl->compile_error.empty() ? m_impl->runtime_error : m_impl->compile_error;
}

bool RuntimeEntry::HasParameter() const
{
  return m_impl->source.parameter.has_value();
}

u64 RuntimeEntry::GetParameterValue() const
{
  return m_impl->parameter_value;
}

bool RuntimeEntry::IsParameterMinimum() const
{
  return m_impl->IsMinimum();
}

bool RuntimeEntry::IsParameterMaximum() const
{
  return m_impl->IsMaximum();
}

const char* RuntimeEntry::GetParameterItemText() const
{
  if (!m_impl->source.parameter.has_value())
    return nullptr;

  const Parameter& parameter = m_impl->source.parameter.value();
  const auto it = std::find_if(parameter.items.begin(), parameter.items.end(), [this](const ParameterItem& item) {
    return item.value.value == m_impl->parameter_value;
  });
  return (it != parameter.items.end()) ? it->text.c_str() : nullptr;
}

bool RuntimeEntry::Activate()
{
  if (!m_impl->valid || !m_impl->manager->IsEnabled())
    return false;

  if (m_impl->source.IsOneShot())
  {
    const bool executed = m_impl->ExecuteScript(ScriptState::On);
    m_impl->activation_result = executed ? ActivationResult::Executed : ActivationResult::NotApplied;
    return executed;
  }

  if (m_impl->source.IsOneShotParameter())
  {
    if (m_impl->state == ScriptState::Off)
      SelectNextState();
    const bool executed = m_impl->ExecuteScript(ScriptState::Change);
    m_impl->activation_result = executed ? ActivationResult::Executed : ActivationResult::NotApplied;
    return executed;
  }

  return false;
}

bool RuntimeEntry::SelectDefaultState()
{
  if (!m_impl->valid || m_impl->source.IsOneShot())
    return false;
  return m_impl->SetState(ScriptState::Off);
}

bool RuntimeEntry::SelectPreviousState()
{
  if (!m_impl->valid || m_impl->source.IsOneShot())
    return false;

  if (m_impl->source.IsOnOff())
    return m_impl->SetState(ScriptState::Off);

  if (!m_impl->source.parameter.has_value())
    return false;

  if (m_impl->IsMinimum())
    return m_impl->SetState(ScriptState::Off);

  const bool changed = m_impl->SetPreviousParameter();
  if (changed)
  {
    m_impl->SetState(ScriptState::Run);
    m_impl->activation_result = ActivationResult::None;
    if (!m_impl->source.IsOneShotParameter())
      m_impl->ExecuteScript(ScriptState::Change);
  }
  return changed;
}

bool RuntimeEntry::SelectNextState()
{
  if (!m_impl->valid || m_impl->source.IsOneShot())
    return false;

  if (m_impl->source.IsOnOff())
    return m_impl->SetState(ScriptState::Run);

  if (!m_impl->source.parameter.has_value())
    return false;

  bool changed = false;
  if (m_impl->state == ScriptState::Off)
  {
    changed = m_impl->SetState(ScriptState::Run);
    m_impl->SetMinimumParameter();
  }
  else
  {
    changed = m_impl->SetNextParameter();
  }

  if (changed)
  {
    m_impl->activation_result = ActivationResult::None;
    if (!m_impl->source.IsOneShotParameter())
      m_impl->ExecuteScript(ScriptState::Change);
  }
  return changed;
}

void RuntimeEntry::FrameUpdate()
{
  if (m_impl->valid && m_impl->state == ScriptState::Run)
    m_impl->ExecuteScript(ScriptState::Run);
}

void RuntimeEntry::ExecuteOffScript()
{
  m_impl->ExecuteScript(ScriptState::Off);
}

void RuntimeEntry::ExecuteOnScript()
{
  m_impl->ExecuteScript(ScriptState::On);
}

RuntimeList::RuntimeList() = default;
RuntimeList::~RuntimeList() = default;

void RuntimeList::Initialize(const List& list, const ExpressionMemoryAccess* memory)
{
  Clear();
  m_memory = memory;
  m_entries.reserve(list.GetEntryCount());
  for (const Entry& entry : list.GetEntries())
    m_entries.push_back(std::unique_ptr<RuntimeEntry>(new RuntimeEntry(this, entry)));
}

void RuntimeList::Clear()
{
  m_entries.clear();
  m_outputs.clear();
  m_memory = nullptr;
  m_frame = 0;
  m_enabled = true;
}

void RuntimeList::SetEnabled(bool enabled)
{
  // Global disable runs off scripts for active cheats; re-enable restores their on scripts.
  if (m_enabled == enabled)
    return;

  if (!enabled)
  {
    for (const std::unique_ptr<RuntimeEntry>& entry : m_entries)
    {
      if (entry->GetState() == ScriptState::Run)
        entry->ExecuteOffScript();
    }
    m_enabled = false;
  }
  else
  {
    m_enabled = true;
    for (const std::unique_ptr<RuntimeEntry>& entry : m_entries)
    {
      if (entry->GetState() == ScriptState::Run)
        entry->ExecuteOnScript();
    }
  }
}

void RuntimeList::FrameUpdate()
{
  // MAME output strings are regenerated by run scripts every frame rather than retained as OSD state.
  m_outputs.clear();
  for (const std::unique_ptr<RuntimeEntry>& entry : m_entries)
    entry->FrameUpdate();
  m_frame++;
}

void RuntimeList::AddOutput(RuntimeOutput output)
{
  m_outputs.push_back(std::move(output));
}

} // namespace MAMECheats
