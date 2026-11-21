// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "types.h"

#include <string_view>

class StateWrapper;

// Event callback type. Second parameter is the number of cycles the event was executed "late".
using TimingEventCallback = void (*)(void* param, TickCount ticks, TickCount ticks_late);

class TimingEvent
{
public:
  TimingEvent(const std::string_view name, TickCount period, TickCount interval, TimingEventCallback callback,
              void* callback_param);
  ~TimingEvent();

  ALWAYS_INLINE const std::string_view GetName() const { return m_name; }
  ALWAYS_INLINE bool IsActive() const { return m_active; }

  // Returns the number of ticks between each event.
  ALWAYS_INLINE TickCount GetPeriod() const { return m_period; }
  ALWAYS_INLINE TickCount GetInterval() const { return m_interval; }

  // Includes pending time.
  TickCount GetTicksSinceLastExecution() const;
  TickCount GetTicksUntilNextExecution() const;

  // Adds ticks to current execution.
  void Delay(TickCount ticks);

  void Schedule(TickCount ticks);
  void SetIntervalAndSchedule(TickCount ticks);
  void SetPeriodAndSchedule(TickCount ticks);

  // Services the event with the current accmulated time. If force is set, when not enough time is pending to
  // simulate a single cycle, the callback will still be invoked, otherwise it won't be.
  void InvokeEarly(bool force = false);

  // Deactivates the event, preventing it from firing again.
  // Do not call within a callback, return Deactivate instead.
  void Activate();
  void Deactivate();

  ALWAYS_INLINE void SetState(bool active)
  {
    if (active)
      Activate();
    else
      Deactivate();
  }

  // Directly alters the interval of the event.
  void SetInterval(TickCount interval) { m_interval = interval; }
  void SetPeriod(TickCount period) { m_period = period; }

  TimingEvent* prev = nullptr;
  TimingEvent* next = nullptr;

  TimingEventCallback m_callback;
  void* m_callback_param;

  GlobalTicks m_next_run_time = 0;
  GlobalTicks m_last_run_time = 0;

  TickCount m_period;
  TickCount m_interval;
  bool m_active = false;

  std::string_view m_name;
};

namespace TimingEvents {

GlobalTicks GetGlobalTickCounter();
GlobalTicks GetEventRunTickCounter();

void Initialize();
void Reset();
void Shutdown();

/// In strict mode, the set of events active now must equal the saved set exactly; any difference fails the load.
bool DoState(StateWrapper& sw, bool strict = false);

bool IsRunningEvents();
void CancelRunningEvent();
void RunEvents();
void CommitLeftoverTicks();

/// Runs the events which are due from inside a branch/delay-slot pair. The CPU state is not resumable at this point,
/// so deferred frame host work waits for the next RunEvents() at an instruction boundary.
void RunEventsInsideInstructionPair();

/// Defers the end-of-frame host work (message pump, input poll, execution exit) to the next RunEvents() at a
/// resumable CPU boundary. The frame can end inside an instruction (GPU register reads synchronize the CRTC) or
/// between a branch and its delay slot, where state saves and execution exits would capture partial CPU state.
void DeferFrameHostWork();

void UpdateCPUDowncount();

TimingEvent** GetHeadEventPtr();

} // namespace TimingEvents