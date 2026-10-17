#pragma once

#include "common/types.h"

#include <array>
#include <optional>

namespace Arcade::H8 {

// H8/3002 DMA controller, channels 0 and 1 (registers FF20-FF3F).
//
// Full address mode with auto-request activation is executed. The transfer is
// performed when DTE and DTME are both set and completes before the CPU resumes,
// as in burst mode. Short address mode, block transfer mode and DREQ/timer
// activated transfers are not modelled; they leave DTE set and are flagged
// through UnsupportedModeSeen().
class H83002DMAC
{
public:
  static constexpr u16 REGISTER_BASE = UINT16_C(0xFF20);
  static constexpr u16 REGISTER_END = UINT16_C(0xFF3F);
  static constexpr u32 DEND_VECTOR_BASE = 44;

  using ReadByte = u8 (*)(void* opaque, u32 address);
  using WriteByte = void (*)(void* opaque, u32 address, u8 value);

  H83002DMAC() { Reset(); }

  void SetBus(void* opaque, ReadByte read, WriteByte write)
  {
    m_bus_opaque = opaque;
    m_bus_read = read;
    m_bus_write = write;
  }

  void Reset()
  {
    m_regs.fill(0);
    m_pending_vectors = 0;
    m_unsupported_seen = false;
  }

  bool Handles(u16 reg) const { return reg >= REGISTER_BASE && reg <= REGISTER_END; }

  u8 Read(u16 reg) const { return m_regs[reg - REGISTER_BASE]; }

  // Returns the bus states taken by a transfer this write started.
  u32 Write(u16 reg, u8 data)
  {
    m_regs[reg - REGISTER_BASE] = data;

    const u32 offset = static_cast<u32>(reg - REGISTER_BASE);
    if ((offset & 0x7) != 0x7)
      return 0;

    return TryStartFullAddress(offset >> 4);
  }

  bool UnsupportedModeSeen() const { return m_unsupported_seen; }

  std::optional<u32> TakePendingInterrupt()
  {
    for (u32 i = 0; i < 4; i++)
    {
      if ((m_pending_vectors & (1u << i)) != 0)
      {
        m_pending_vectors &= ~(1u << i);
        return DEND_VECTOR_BASE + i;
      }
    }
    return std::nullopt;
  }

private:
  static constexpr u8 DTCRA_DTE = 0x80;
  static constexpr u8 DTCRB_DTME = 0x80;
  static constexpr u8 DTCRA_DTSZ = 0x40;
  static constexpr u8 DTCR_ID = 0x20;
  static constexpr u8 DTCR_IDE = 0x10;
  static constexpr u8 DTCRA_DTIE = 0x08;
  static constexpr u8 DTCRA_FULL_ADDRESS = 0x06;
  static constexpr u8 DTCRA_BLOCK = 0x01;
  static constexpr u8 DTCRB_DTS = 0x07;

  // Per channel: MARA 0-3, ETCRA 4-5, IOARA 6, DTCRA 7, MARB 8-B, ETCRB C-D, IOARB E, DTCRB F.
  u32 ReadMAR(u32 base) const
  {
    return (static_cast<u32>(m_regs[base + 1]) << 16) | (static_cast<u32>(m_regs[base + 2]) << 8) |
           static_cast<u32>(m_regs[base + 3]);
  }

  void WriteMAR(u32 base, u32 address)
  {
    m_regs[base + 1] = static_cast<u8>(address >> 16);
    m_regs[base + 2] = static_cast<u8>(address >> 8);
    m_regs[base + 3] = static_cast<u8>(address);
  }

  u32 TryStartFullAddress(u32 channel)
  {
    const u32 a = channel * 0x10;
    const u32 b = a + 0x8;
    const u8 dtcra = m_regs[a + 7];
    const u8 dtcrb = m_regs[b + 7];
    if ((dtcra & DTCRA_DTE) == 0 || (dtcrb & DTCRB_DTME) == 0)
      return 0;

    if ((dtcra & DTCRA_FULL_ADDRESS) != DTCRA_FULL_ADDRESS || (dtcra & DTCRA_BLOCK) != 0 ||
        (dtcrb & DTCRB_DTS) != 0 || !m_bus_read || !m_bus_write)
    {
      m_unsupported_seen = true;
      return 0;
    }

    const u32 unit = (dtcra & DTCRA_DTSZ) ? 2 : 1;
    const s32 src_step = (dtcra & DTCR_IDE) ? ((dtcra & DTCR_ID) ? -static_cast<s32>(unit) : static_cast<s32>(unit)) : 0;
    const s32 dst_step = (dtcrb & DTCR_IDE) ? ((dtcrb & DTCR_ID) ? -static_cast<s32>(unit) : static_cast<s32>(unit)) : 0;

    u32 count = (static_cast<u32>(m_regs[a + 4]) << 8) | m_regs[a + 5];
    if (count == 0)
      count = 0x10000;

    u32 src = ReadMAR(a);
    u32 dst = ReadMAR(b);
    for (u32 i = 0; i < count; i++)
    {
      for (u32 j = 0; j < unit; j++)
        m_bus_write(m_bus_opaque, (dst + j) & 0xFFFFFF, m_bus_read(m_bus_opaque, (src + j) & 0xFFFFFF));
      src = static_cast<u32>(static_cast<s32>(src) + src_step) & 0xFFFFFF;
      dst = static_cast<u32>(static_cast<s32>(dst) + dst_step) & 0xFFFFFF;
    }

    WriteMAR(a, src);
    WriteMAR(b, dst);
    m_regs[a + 4] = 0;
    m_regs[a + 5] = 0;
    m_regs[a + 7] = static_cast<u8>(dtcra & ~DTCRA_DTE);

    if (dtcra & DTCRA_DTIE)
      m_pending_vectors |= 1u << (channel * 2);

    // One read and one write bus cycle of two states per unit.
    return count * 4;
  }

  std::array<u8, REGISTER_END - REGISTER_BASE + 1> m_regs{};
  void* m_bus_opaque = nullptr;
  ReadByte m_bus_read = nullptr;
  WriteByte m_bus_write = nullptr;
  u32 m_pending_vectors = 0;
  bool m_unsupported_seen = false;
};

} // namespace Arcade::H8
