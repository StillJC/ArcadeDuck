// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_output.h"
#include "core/arcade/arcade_output_network.h"
#include "core/arcade/arcade_output_win32.h"

#include "core/settings.h"
#include "core/system.h"

#include "common/log.h"
#include "common/timer.h"

#include "util/imgui_manager.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

Log_SetChannel(ArcadeOutput);

namespace Arcade::Output {
namespace {

struct OutputState
{
  std::string name;
  std::string source;
  s32 value = 0;
  s32 previous_value = 0;
  u64 change_count = 0;
  u64 activation_count = 0;
  u32 last_change_frame = 0;
  u32 last_activation_frame = 0;
  Common::Timer::Value last_change_time = 0;
  Common::Timer::Value last_activation_time = 0;
};

std::mutex s_mutex;
std::vector<OutputState> s_outputs;
std::string s_game_id;
std::string s_system_id;
bool s_win32_transport_enabled = false;
bool s_network_transport_enabled = false;

void MarkChanged(OutputState& state, s32 previous_value, s32 new_value)
{
  const Common::Timer::Value now = Common::Timer::GetCurrentValue();
  const u32 frame = System::IsValid() ? System::GetFrameNumber() : 0;

  state.previous_value = previous_value;
  state.value = new_value;
  state.change_count++;
  state.last_change_frame = frame;
  state.last_change_time = now;

  if (previous_value == 0 && new_value != 0)
  {
    state.activation_count++;
    state.last_activation_frame = frame;
    state.last_activation_time = now;
  }
}

std::string FormatElapsed(Common::Timer::Value now, Common::Timer::Value then)
{
  if (then == 0 || now < then)
    return "never";

  const double seconds = Common::Timer::ConvertValueToSeconds(now - then);
  char buffer[64];
  if (seconds < 1.0)
    std::snprintf(buffer, sizeof(buffer), "%.0f ms ago", seconds * 1000.0);
  else
    std::snprintf(buffer, sizeof(buffer), "%.2f s ago", seconds);
  return buffer;
}

} // namespace

void Reset()
{
  NetworkTransport::Stop();
  Win32Transport::Stop();

  std::lock_guard<std::mutex> lock(s_mutex);
  s_outputs.clear();
  s_game_id.clear();
  s_system_id.clear();
  s_win32_transport_enabled = false;
  s_network_transport_enabled = false;
}

void SetContext(std::string_view game_id, std::string_view system_id)
{
  bool start_win32 = false;
  bool start_network = false;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_game_id.assign(game_id.data(), game_id.size());
    s_system_id.assign(system_id.data(), system_id.size());

    if (g_settings.arcade_external_outputs_enabled)
    {
      s_win32_transport_enabled =
        (g_settings.arcade_external_outputs_protocol == 0 || g_settings.arcade_external_outputs_protocol == 2);
      s_network_transport_enabled =
        (g_settings.arcade_external_outputs_protocol == 1 || g_settings.arcade_external_outputs_protocol == 2);
    }
    else
    {
      s_win32_transport_enabled = false;
      s_network_transport_enabled = false;
    }

    start_win32 = s_win32_transport_enabled;
    start_network = s_network_transport_enabled;
  }

  if (start_win32)
    Win32Transport::Start(game_id);
  if (start_network)
    NetworkTransport::Start(game_id);
}

void SetValue(std::string_view name, s32 value, std::string_view source)
{
  if (name.empty())
    return;

  std::lock_guard<std::mutex> lock(s_mutex);

  auto it = std::find_if(s_outputs.begin(), s_outputs.end(),
                         [name](const OutputState& state) { return std::string_view(state.name) == name; });
  if (it == s_outputs.end())
  {
    OutputState state;
    state.name.assign(name.data(), name.size());
    state.source.assign(source.data(), source.size());

    if (value != 0)
      MarkChanged(state, 0, value);
    else
      state.value = value;

    s_outputs.push_back(std::move(state));
    if (s_win32_transport_enabled)
      Win32Transport::Notify(s_outputs.back().name, s_outputs.back().value);
    if (s_network_transport_enabled)
      NetworkTransport::Notify(s_outputs.back().name, s_outputs.back().value);

    if (g_settings.enable_debug_logging)
    {
      DEV_LOG("Registered arcade output '{}' value={} source='{}' game='{}' system='{}'.", name, value, source,
              s_game_id, s_system_id);
    }
    return;
  }

  if (!source.empty())
    it->source.assign(source.data(), source.size());

  if (it->value == value)
    return;

  const s32 previous_value = it->value;
  MarkChanged(*it, previous_value, value);
  if (s_win32_transport_enabled)
    Win32Transport::Notify(it->name, it->value);
  if (s_network_transport_enabled)
    NetworkTransport::Notify(it->name, it->value);

  if (g_settings.enable_debug_logging)
  {
    DEV_LOG("Arcade output '{}' {} -> {} source='{}' game='{}' system='{}' frame={}.", it->name, previous_value,
            value, it->source, s_game_id, s_system_id, it->last_change_frame);
  }
}

void ClearValues()
{
  std::lock_guard<std::mutex> lock(s_mutex);
  for (OutputState& state : s_outputs)
  {
    if (state.value == 0)
      continue;

    const s32 previous_value = state.value;
    MarkChanged(state, previous_value, 0);
    if (s_win32_transport_enabled)
      Win32Transport::Notify(state.name, state.value);
    if (s_network_transport_enabled)
      NetworkTransport::Notify(state.name, state.value);

    if (g_settings.enable_debug_logging)
    {
      DEV_LOG("Arcade output '{}' {} -> 0 (reset/shutdown) source='{}' game='{}' system='{}'.", state.name,
              previous_value, state.source, s_game_id, s_system_id);
    }
  }
}

void Shutdown()
{
  NetworkTransport::Stop();
  Win32Transport::Stop();

  std::lock_guard<std::mutex> lock(s_mutex);
  s_win32_transport_enabled = false;
  s_network_transport_enabled = false;
}

void ResetStatistics()
{
  std::lock_guard<std::mutex> lock(s_mutex);
  for (OutputState& state : s_outputs)
  {
    state.previous_value = state.value;
    state.change_count = 0;
    state.activation_count = 0;
    state.last_change_frame = 0;
    state.last_activation_frame = 0;
    state.last_change_time = 0;
    state.last_activation_time = 0;
  }
}

void DrawDebugWindow()
{
  std::vector<OutputState> outputs;
  std::string game_id;
  std::string system_id;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    outputs = s_outputs;
    game_id = s_game_id;
    system_id = s_system_id;
  }

  // This is intentionally a small, non-interactive overlay rather than a
  // normal ImGui tool window. Light-gun/mouse capture can prevent dragging
  // debug windows while a game is running, and the monitor must not obscure
  // the middle of a service/output test.
  const float scale = ImGuiManager::GetGlobalScale();
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const float margin = 10.0f * scale;
  const float width = 470.0f * scale;
  const float row_height = ImGui::GetTextLineHeightWithSpacing();
  const float max_height = std::max(160.0f * scale, display_size.y - (margin * 2.0f));
  const float desired_height =
    std::min(max_height, (104.0f * scale) + (row_height * static_cast<float>(std::max<size_t>(outputs.size(), 1))));

  ImGui::SetNextWindowPos(ImVec2(display_size.x - margin, margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(width, desired_height), ImGuiCond_Appearing);
  ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * scale, 140.0f * scale),
                                      ImVec2(std::max(360.0f * scale, display_size.x - (margin * 2.0f)), max_height));
  ImGui::SetNextWindowBgAlpha(0.82f);

  constexpr ImGuiWindowFlags window_flags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

  if (!ImGui::Begin("Arcade Outputs##ArcadeOutputOverlay", nullptr, window_flags))
  {
    ImGui::End();
    return;
  }

  // Grow vertically as new outputs are discovered, but do not continually
  // override a size the user chose manually. If the list exceeds the display,
  // normal ImGui scrolling remains available.
  static size_t last_output_count = 0;
  if (outputs.size() > last_output_count)
  {
    const ImVec2 current_size = ImGui::GetWindowSize();
    if (current_size.y < desired_height)
      ImGui::SetWindowSize(ImVec2(current_size.x, desired_height), ImGuiCond_Always);
  }
  last_output_count = outputs.size();

  if (game_id.empty())
    ImGui::TextUnformatted("Arcade Outputs");
  else if (system_id.empty())
    ImGui::Text("Arcade Outputs - %s", game_id.c_str());
  else
    ImGui::Text("Arcade Outputs - %s [%s]", game_id.c_str(), system_id.c_str());

  if (outputs.empty())
  {
    ImGui::TextDisabled("No cabinet outputs observed yet.");
    ImGui::End();
    return;
  }

  const Common::Timer::Value now = Common::Timer::GetCurrentValue();
  constexpr ImGuiTableFlags table_flags =
    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

  if (ImGui::BeginTable("arcade_outputs_overlay", 5, table_flags))
  {
    ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 48.0f * scale);
    ImGui::TableSetupColumn("Changes", ImGuiTableColumnFlags_WidthFixed, 62.0f * scale);
    ImGui::TableSetupColumn("Activations", ImGuiTableColumnFlags_WidthFixed, 76.0f * scale);
    ImGui::TableSetupColumn("Last Active", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();

    for (const OutputState& state : outputs)
    {
      const std::string last_active = FormatElapsed(now, state.last_activation_time);

      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(state.name.c_str());

      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%d", state.value);

      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%llu", static_cast<unsigned long long>(state.change_count));

      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%llu", static_cast<unsigned long long>(state.activation_count));

      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(last_active.c_str());
    }

    ImGui::EndTable();
  }

  ImGui::End();
}
} // namespace Arcade::Output