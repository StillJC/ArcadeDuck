// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Error;

namespace MAMECheats {

// MAME debugger expressions distinguish logical, physical, direct, region, and share accesses.
// Keeping those namespaces distinct prevents a logical maincpu.pb access from silently becoming physical.
enum class ExpressionMemorySpace : u8
{
  ProgramLogical,
  DataLogical,
  IOLogical,
  OpcodeLogical,
  ProgramPhysical,
  DataPhysical,
  IOPhysical,
  OpcodePhysical,
  ProgramDirect,
  OpcodeDirect,
  Region,
  Share,
};

class ExpressionMemoryAccess
{
public:
  virtual ~ExpressionMemoryAccess() = default;

  virtual bool IsValid(std::string_view name, ExpressionMemorySpace space) const = 0;
  virtual u64 Read(std::string_view name, ExpressionMemorySpace space, u32 address, u32 size,
                   bool disable_side_effects) const = 0;
  virtual void Write(std::string_view name, ExpressionMemorySpace space, u32 address, u32 size, u64 value,
                     bool disable_side_effects) const = 0;
};

class ExpressionSymbolTable
{
public:
  using Getter = std::function<u64()>;
  using Setter = std::function<void(u64)>;
  using Function = std::function<u64(u32, const u64*)>;

  ExpressionSymbolTable();
  ~ExpressionSymbolTable();

  ExpressionSymbolTable(const ExpressionSymbolTable&) = delete;
  ExpressionSymbolTable& operator=(const ExpressionSymbolTable&) = delete;

  void AddValue(std::string_view name, u64* value);
  void AddValue(std::string_view name, Getter getter, Setter setter = {});
  void AddConstant(std::string_view name, u64 value);
  void AddFunction(std::string_view name, u32 minimum_parameters, u32 maximum_parameters, Function function);
  void Clear();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  friend class Expression;
};

class Expression
{
public:
  Expression(ExpressionSymbolTable* symbols, const ExpressionMemoryAccess* memory);
  ~Expression();

  Expression(Expression&&) noexcept;
  Expression& operator=(Expression&&) noexcept;

  Expression(const Expression&) = delete;
  Expression& operator=(const Expression&) = delete;

  bool Parse(std::string_view expression, Error* error = nullptr);
  bool Execute(u64* result, Error* error = nullptr) const;
  bool IsEmpty() const;
  const std::string& GetOriginalString() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

u64 FromBCD(u64 value);
u64 ToBCD(u64 value);

} // namespace MAMECheats
