// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/arcade_save_state.h"
#include "core/arcade/devices/jvs/namco_cyberlead_led.h"
#include "common/types.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class StateWrapper;

namespace Arcade::JVS {

class NamcoCyberLead
{
public:
  bool DoState(StateWrapper& sw);

  struct PacketResult
  {
    std::array<u8, 256> response{};
    u32 response_length = 0;
    bool reset_requested = false;
    bool initialize_sense_after_response = false;
  };

  void Reset();
  PacketResult ProcessPacket(std::span<const u8> packet);

  // Quiz panels wire their four answer buttons to the player's UP/DOWN/LEFT/RIGHT switch inputs.
  void SetButtonsOnDirectionLines(bool enabled) { m_buttons_on_direction_lines = enabled; }

  bool SetLEDFirmware(std::span<const u8> firmware);
  void ResetLED();
  void AdvanceLED(u32 source_clocks, u32 source_clock_hz);
  bool CopyLEDFrame(std::span<u8> pixels, std::array<u32, 4>* intensity_counts = nullptr,
                    u16* start_address = nullptr, u32* scroll_x = nullptr,
                    u32* scroll_y = nullptr, u64* generation = nullptr) const;

  const NamcoCyberLeadLEDDevice& GetLEDDevice() const { return m_led_device; }
  NamcoCyberLeadLEDDevice& GetLEDDevice() { return m_led_device; }

  // Factory image of the LED PCB AT29C020 (cl1-leda.ic5) from the BIOS folder, either loose or
  // in namco_cyberlead_led.zip. Empty when it is missing or does not match the known dump.
  static std::vector<u8> LoadFactoryLEDFirmware();

  // Starts the LED PCB from the image a game previously programmed (saved at flash_path),
  // falling back to the factory image. Returns false when the LED cannot run.
  bool InitializeLEDFlash(std::span<const u8> factory_firmware, std::string flash_path);

  // Saves the LED flash to the path given to InitializeLEDFlash when it was reprogrammed.
  void SaveLEDFlash();

  // Whether a save state can be taken now. With the LED PCB running, saving waits until:
  //   - the LED C77 has answered the profile handshake,
  //   - the LED data is in place: the game has switched the LED to display mode (71/08) since the last upload
  //     that changed the flash (first-boot installation ends this way; an installed board goes straight to
  //     display mode), or every committed upload since the handshake rewrote identical data,
  //   - the LED link is idle (no profile, status, block, commit or display-mode transaction in progress), and
  //   - the LED flash has no unfinished command sequence or page load.
  // All LED flash programming happens inside those transactions; the periodic 71/01 control frames never
  // program it. Without the LED PCB nothing blocks saving.
  bool IsLEDSaveReady() const;
  bool IsLEDPresent() const { return m_led_device.HasFirmware(); }
  u8 GetLinkStateCode() const { return static_cast<u8>(m_link.state); }
  std::string DescribeLinkDiagnostics() const;

  // The LED flash as an NVRAM component while the LED PCB is running.
  void GetNVRAMComponents(std::vector<SaveState::NVRAMComponent>* components) const;

  // The Cyber Lead board attached to the running machine, used for LED presentation.
  static NamcoCyberLead* GetActive();
  static void SetActive(NamcoCyberLead* device);
  static bool IsLEDHardwareActive();

  // True for database games whose cabinet uses the Cyber Lead I/O board.
  static bool IsCyberLeadGame(std::string_view serial);

private:
  // CL1-I/OB Ver1.03 LED-board link. The I/O C77 main loop calls the state
  // function at C38C once per pass; each state sends one frame to the LED C77.
  enum class LinkState : u8
  {
    Idle,         // 1494: 71/01 control and input data
    Profile,      // 11EC: 71/02 profile query
    Status,       // 1278: 71/04
    Block,        // 12DE: 71/05 data block
    BlockDone,    // 135E: sends nothing until the game continues
    Commit,       // 1360: 71/06
    CommitFinish, // 13CC: 71/07
    Mode,         // 142E: 71/08
  };

  enum class LinkStep : u8
  {
    Run,
    SecondWait,
    Evaluate,
  };

  struct Link
  {
    LinkState state = LinkState::Idle;
    LinkStep step = LinkStep::Run;
    u64 clock = 0;
    u64 wake_clock = 0;
    u64 clock_fraction = 0;
    u32 generation = 0;
    u32 started_generation = 0;

    u8 loop_counter = 0;    // C0B3
    u8 attempts = 0;        // C389
    u16 idle_countdown = 0; // C39C
    u8 profile_result = 0;  // C398
    u8 transaction_id = 0;  // C399
    u8 transfer_result = 0; // C39A
    u8 poll_count = 0;      // C39B
    std::array<u8, 8> control_data{}; // C3A0
    std::array<u8, 8> profile_id{};   // C390
    std::array<u8, 70> block{};       // C500

    // LED reply receiver (C380-C388), frame stored as at C400.
    std::array<u8, 32> reply{};
    u8 reply_size = 0;
    u8 reply_state = 0;
    u8 reply_remaining = 0;
    u8 reply_previous = 0;
    bool reply_received = false;

    // Save readiness, latched from the protocol since the LED link was last reset: the LED C77 has answered a
    // profile query, and a block upload has been committed through 71/07.
    bool handshake_complete = false;
    bool initial_upload_complete = false;

    // LED flash content changes counted when the last upload transaction completed, and whether that
    // transaction changed the flash (installation) rather than rewriting identical data (refresh).
    u64 upload_change_baseline = 0;
    bool flash_changed_since_handshake = false;
    bool mode_since_last_change = false;
    u32 commit_count = 0;
    u32 mode_count = 0;

    // 71/04 accepted since the last profile query: the game ended LED setup without an upload (install mode 0, or
    // the operator declined the transfer). Installing games never send it before or during an upload.
    bool setup_closed_since_query = false;
  };

  u32 ReadPlayerInputs(u32 player) const;
  void UpdateCoinCounters();
  u8 ReadBoardMemory(u16 address) const;

  u64 SendLEDCommand(std::span<const u8> command);
  void SetLinkState(LinkState state);
  void RunLinkStep();
  void StartLinkState();
  void EvaluateLinkState();
  void ReceiveLEDByte(u8 value);
  bool LEDReplyAccepted() const;

  u8 m_address = UINT8_C(0xFF);
  bool m_buttons_on_direction_lines = false;
  std::array<u16, 2> m_coin_counter{};
  std::array<u16, 2> m_coin_used{};
  std::array<bool, 2> m_coin_previous{};
  Link m_link{};
  NamcoCyberLeadLEDLink m_led_link{};
  NamcoCyberLeadLEDDevice m_led_device{};
  std::string m_led_flash_path;
  SaveState::PersistentFile m_led_flash_file;
};

} // namespace Arcade::JVS
