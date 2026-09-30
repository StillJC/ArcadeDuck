// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system10/namco_system10.h"

#include "core/arcade/systems/namco/system10/namco_system10_memn.h"

#include "common/error.h"
#include "common/log.h"

#include <optional>
#include <utility>

Log_SetChannel(NamcoSystem10);

namespace NamcoSystem10 {
namespace {

struct RuntimeState
{
  std::string set_name;
  MemNRawNAND nand0;
  MemNRawNAND nand1;
};

std::optional<RuntimeState> s_runtime;

} // namespace

bool InitializeMemN(MemNLoadedContent content, Error* error)
{
  if (s_runtime.has_value())
  {
    Error::SetStringView(error, "Namco System 10 runtime is already active.");
    return false;
  }

  if (content.set_name.empty())
  {
    Error::SetStringView(error, "Namco System 10 MEM(N) content has no set identity.");
    return false;
  }

  RuntimeState runtime;
  runtime.set_name = std::move(content.set_name);

  if (!runtime.nand0.Load(std::move(content.nand0), error) ||
      !runtime.nand1.Load(std::move(content.nand1), error))
  {
    return false;
  }

  VERBOSE_LOG("Namco System 10 MEM(N) runtime initialized set='{}' nand0={} nand1={}.", runtime.set_name,
              runtime.nand0.GetRawImage().size(), runtime.nand1.GetRawImage().size());

  s_runtime = std::move(runtime);
  return true;
}

void Reset()
{
  if (!s_runtime.has_value())
    return;

  // No MEM(N) command/state machine is installed yet. The raw NAND images are
  // immutable storage and intentionally survive a machine reset.
}

void Shutdown()
{
  s_runtime.reset();
}

bool IsActive()
{
  return s_runtime.has_value();
}

} // namespace NamcoSystem10