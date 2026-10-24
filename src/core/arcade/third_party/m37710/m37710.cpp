// license:BSD-3-Clause
// copyright-holders:R. Belmont, Karl Stenerud
/*
    Mitsubishi M37702/37710/37720 CPU Emulator

    The 7700 series is based on the WDC 65C816 core, with the following
    notable changes:

    - Second 16-bit accumulator called "B" (on the 65816, "A" and "B" were the
      two 8-bit halves of the 16-bit "C" accumulator).
    - 6502 emulation mode and XCE instruction are not present.
    - No NMI line.  BRK and the watchdog interrupt are non-maskable, but there
      is no provision for the traditional 6502/65816 NMI line.
    - 3-bit interrupt priority levels like the 68000.  Interrupts in general
      are very different from the 65816.
    - New single-instruction immediate-to-memory move instructions (LDM)
      replaces STZ.
    - CLM and SEM (clear and set "M" status bit) replace CLD/SED.  Decimal
      mode is still available via REP/SEP instructions.
    - INC and DEC (0x1A and 0x3A) switch places for no particular reason.
    - The microcode bug that caused MVN/NVP to take 2 extra cycles per byte
      on the 65816 seems to have been fixed.
    - The WDM (0x42) and BIT immediate (0x89) instructions are now prefixes.
      0x42 when used before an instruction involving the A accumulator makes
      it use the B accumulator instead.  0x89 adds multiply and divide
      opcodes, which the real 65816 doesn't have.
    - The 65C816 preserves the upper 8 bits of A when in 8-bit M mode, but
      not the upper 8 bits of X or Y when in 8-bit X.  The 7700 preserves
      the top bits of all registers in all modes (code in the C74 BIOS
      starting at d881 requires this!).
    - Unlike the 65C816, the program bank register (known here as PG) is
      incremented when PC overflows from 0xFFFF, and may be incremented or
      decremented when the address for a relative branch is calculated.
    - The external bus, if used, allows for 16-bit transfers, and can be
      dynamically reduced to 8 bits by asserting the BYTE input. (The
      65C816 has an 8-bit data bus.) Internal memory is also 16 bits wide,
      but parallel port registers must be accessed as individual bytes.

    The various 7700 series models differ primarily by their on board
    peripherals.  The 7750 and later models do include some additional
    instructions, vs. the 770x/1x/2x, notably signed multiply/divide and
    sign extension opcodes.

    Peripherals common across the 7700 series include: programmable timers,
    digital I/O ports, and analog to digital converters.

    Reference: 7700 Family Software User's Manual (instruction set)
               7702/7703 Family User's Manual (on-board peripherals)
           7720 Family User's Manual

    Emulator by R. Belmont.
    Based on G65816 Emulator by Karl Stenrud.

    History:
    - v1.0  RB  First version, basic operation OK, timers not complete
    - v1.1  RB  Data bus is 16-bit, dozens of bugfixes to IRQs, opcodes,
                    and opcode mapping.  New opcodes added, internal timers added.
    - v1.2  RB  Fixed execution outside of bank 0, fixed LDM outside of bank 0,
                fixed so top 8 bits of X & Y are preserved while in 8-bit mode,
        added save state support.
*/

#include "m37710.h"

#include "m37710cm.h"
#include "m37710il.h"

// verbose logging for peripherals, etc.
#define LOG_PORTS (1U << 1)
#define LOG_AD    (1U << 2)
#define LOG_UART  (1U << 3)
#define LOG_TIMER (1U << 4)
#define LOG_INT   (1U << 5)
//#define VERBOSE (LOG_GENERAL | LOG_PORTS | LOG_AD | LOG_UART | LOG_TIMER | LOG_INT)
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

#define BIT(V, B) (((V) >> (B)) & 1U)
#define LOGMASKED(...) do { } while (0)
#define LOG(...) do { } while (0)
#if 0
DEFINE_DEVICE_TYPE(M37702M2, m37702m2_device, "m37702m2", "Mitsubishi M37702M2")
DEFINE_DEVICE_TYPE(M37702S1, m37702s1_device, "m37702s1", "Mitsubishi M37702S1")
DEFINE_DEVICE_TYPE(M37710S4, m37710s4_device, "m37710s4", "Mitsubishi M37710S4")
DEFINE_DEVICE_TYPE(M37720S1, m37720s1_device, "m37720s1", "Mitsubishi M37720S1")
DEFINE_DEVICE_TYPE(M37730S2, m37730s2_device, "m37730s2", "Mitsubishi M37730S2")
DEFINE_DEVICE_TYPE(M37732S4, m37732s4_device, "m37732s4", "Mitsubishi M37732S4")
#endif

m37710_cpu_device::m37710_cpu_device() {
	m_a = m_b = m_ba = m_bb = m_x = m_y = m_xh = m_yh = 0;
	m_s = m_pc = m_ppc = m_pg = m_dt = m_dpr = 0;
	m_flag_e = m_flag_m = m_flag_x = m_flag_n = m_flag_v = 0;
	m_flag_d = m_flag_i = m_flag_z = m_flag_c = 0;
	m_line_irq = m_ipl = m_ir = m_im = m_im2 = m_im3 = m_im4 = 0;
	m_irq_delay = m_source = m_destination = m_stopped = 0;
	m_ICount = 0;
	std::fill(std::begin(m_port_regs), std::end(m_port_regs), 0);
	std::fill(std::begin(m_port_dir), std::end(m_port_dir), 0);
	m_ad_control = m_ad_sweep = 0;
	std::fill(std::begin(m_ad_result), std::end(m_ad_result), 0);
	std::fill(std::begin(m_uart_mode), std::end(m_uart_mode), 0);
	std::fill(std::begin(m_uart_baud), std::end(m_uart_baud), 0);
	std::fill(std::begin(m_uart_ctrl_reg0), std::end(m_uart_ctrl_reg0), 0);
	std::fill(std::begin(m_uart_ctrl_reg1), std::end(m_uart_ctrl_reg1), 0);
	std::fill(std::begin(m_uart_recv_buf), std::end(m_uart_recv_buf), 0);
	m_count_start = m_one_shot_start = m_up_down_reg = 0;
	std::fill(std::begin(m_timer_reg), std::end(m_timer_reg), 0);
	std::fill(std::begin(m_timer_mode), std::end(m_timer_mode), 0);
	std::fill(std::begin(m_timer_cycles), std::end(m_timer_cycles), 0);
	std::fill(std::begin(m_reload), std::end(m_reload), 0);
	std::fill(std::begin(m_timer_out), std::end(m_timer_out), 0);
	m_ad_cycles = 0;
	m_proc_mode = m_watchdog_freq = m_rto_control = m_dram_control = 0;
	m_dmac_control = 0;
	std::fill(std::begin(m_dma_src), std::end(m_dma_src), 0);
	std::fill(std::begin(m_dma_dst), std::end(m_dma_dst), 0);
	std::fill(std::begin(m_dma_cnt), std::end(m_dma_cnt), 0);
	std::fill(std::begin(m_dma_mode), std::end(m_dma_mode), 0);
	std::fill(std::begin(m_int_control), std::end(m_int_control), 0);
	m_debugger_pc = m_debugger_pg = m_debugger_dt = m_debugger_ps = 0;
	m_debugger_a = m_debugger_b = 0;
	m37710i_set_execution_mode(EXECUTION_MODE_M0X0);
}

bool m37710_cpu_device::load_internal_rom(const uint8_t* data, std::size_t size) {
	if (!data || size != m_internal_rom.size()) return false;
	std::copy(data, data + size, m_internal_rom.begin());
	m_rom_loaded = true;
	return true;
}

void m37710_cpu_device::set_memory_callbacks(read_callback read, write_callback write,
		read_word_callback read_word, write_word_callback write_word) {
	m_external_read = std::move(read);
	m_external_write = std::move(write);
	m_external_read_word = std::move(read_word);
	m_external_write_word = std::move(write_word);
}

void m37710_cpu_device::set_port_callbacks(unsigned port, port_read_callback read,
		port_write_callback write) {
	if (port >= m_port_in_cb.size()) return;
	m_port_in_cb[port] = std::move(read);
	m_port_out_cb[port] = std::move(write);
}

void m37710_cpu_device::set_analog_callback(unsigned channel, analog_callback read) {
	if (channel < m_analog_cb.size()) m_analog_cb[channel] = std::move(read);
}

void m37710_cpu_device::set_uart_trace_callback(uart_trace_callback callback) {
	m_uart_trace = std::move(callback);
}

bool m37710_cpu_device::uart_receive_byte(unsigned uart, uint8_t data) {
	if (uart >= 2 || !(m_uart_ctrl_reg1[uart] & 0x04))
		return false;

	// Do not overwrite an unread byte. A complete UART model can add overrun
	// handling later; for now preserve the first pending byte deterministically.
	if (m_uart_ctrl_reg1[uart] & 0x08)
		return false;

	m_uart_recv_buf[uart] = data;
	m_uart_ctrl_reg1[uart] |= 0x08;

	const int line = (uart == 0) ? M37710_LINE_UART0RECV : M37710_LINE_UART1RECV;
	m37710_set_irq_line(line, ASSERT_LINE);
	return true;
}

uint8_t m37710_cpu_device::read_byte(uint32_t address) {
	address &= 0x00ffffff;
	if (address >= 0x02 && address <= 0x15) {
		const unsigned pair = (address - 2) / 4;
		const unsigned port = pair * 2 + (address & 1);
		const bool direction = ((address - 2) & 2) != 0;
		return direction ? get_port_dir(port) : get_port_reg(port);
	}
	if (address == 0x1e) return ad_control_r();
	if (address == 0x1f) return ad_sweep_r();
	if (address >= 0x20 && address <= 0x2f) {
		const uint16_t value = ad_result_r((address - 0x20) / 2);
		return static_cast<uint8_t>(value >> ((address & 1) * 8));
	}
	switch (address) {
		case 0x30: return uart0_mode_r();
		case 0x34: return uart0_ctrl_reg0_r();
		case 0x35: return uart0_ctrl_reg1_r();
		case 0x36: return static_cast<uint8_t>(uart0_rbuf_r());
		case 0x37: return static_cast<uint8_t>(m_uart_recv_buf[0] >> 8);
		case 0x38: return uart1_mode_r();
		case 0x3c: return uart1_ctrl_reg0_r();
		case 0x3d: return uart1_ctrl_reg1_r();
		case 0x3e: return static_cast<uint8_t>(uart1_rbuf_r());
		case 0x3f: return static_cast<uint8_t>(m_uart_recv_buf[1] >> 8);
		case 0x40: return count_start_r();
		case 0x44: return up_down_r();
		case 0x5e: return proc_mode_r(0);
		case 0x61: return watchdog_freq_r();
		default: break;
	}
	if (address >= 0x46 && address <= 0x55) {
		const uint16_t value = timer_reg_r((address - 0x46) / 2, 0xffff);
		return static_cast<uint8_t>(value >> ((address & 1) * 8));
	}
	if (address >= 0x56 && address <= 0x5d)
		return timer_mode_r(address - 0x56);
	if (address >= 0x70 && address <= 0x7f) {
		static constexpr int lines[16] = {
			M37710_LINE_ADC, M37710_LINE_UART0XMIT, M37710_LINE_UART0RECV,
			M37710_LINE_UART1XMIT, M37710_LINE_UART1RECV,
			M37710_LINE_TIMERA0, M37710_LINE_TIMERA1, M37710_LINE_TIMERA2,
			M37710_LINE_TIMERA3, M37710_LINE_TIMERA4, M37710_LINE_TIMERB0,
			M37710_LINE_TIMERB1, M37710_LINE_TIMERB2,
			M37710_LINE_IRQ0, M37710_LINE_IRQ1, M37710_LINE_IRQ2
		};
		return get_int_control(lines[address - 0x70]);
	}
	if (address >= 0x80 && address <= 0x27f)
		return m_internal_ram[address - 0x80];
	if (address >= 0xc000 && address <= 0xffff)
		return m_internal_rom[address - 0xc000];
	return m_external_read ? m_external_read(address) : 0xff;
}

void m37710_cpu_device::write_byte(uint32_t address, uint8_t data) {
	address &= 0x00ffffff;

	if (address >= 0x02 && address <= 0x15) {
		const unsigned pair = (address - 2) / 4;
		const unsigned port = pair * 2 + (address & 1);
		const bool direction = ((address - 2) & 2) != 0;
		if (direction) set_port_dir(port, data); else set_port_reg(port, data);
		return;
	}
	if (address == 0x1e) { ad_control_w(data); return; }
	if (address == 0x1f) { ad_sweep_w(data); return; }
	switch (address) {
		case 0x30: uart0_mode_w(data); return;
		case 0x31: uart0_baud_w(data); return;
		case 0x34: uart0_ctrl_reg0_w(data); return;
		case 0x35: uart0_ctrl_reg1_w(data); return;
		case 0x38: uart1_mode_w(data); return;
		case 0x39: uart1_baud_w(data); return;
		case 0x3c: uart1_ctrl_reg0_w(data); return;
		case 0x3d: uart1_ctrl_reg1_w(data); return;
		case 0x40: count_start_w(data); return;
		case 0x42: one_shot_start_w(data); return;
		case 0x44: up_down_w(data); return;
		case 0x5e: proc_mode_w(data); return;
		case 0x60: watchdog_timer_w(data); return;
		case 0x61: watchdog_freq_w(data); return;
		default: break;
	}
	if (address == 0x32 || address == 0x33) {
		uint16_t value = data;
		if (address & 1) value <<= 8;
		uart0_tbuf_w(value);
		return;
	}
	if (address == 0x3a || address == 0x3b) {
		uint16_t value = data;
		if (address & 1) value <<= 8;
		uart1_tbuf_w(value);
		return;
	}
	if (address >= 0x46 && address <= 0x55) {
		const uint16_t mask = (address & 1) ? 0xff00 : 0x00ff;
		timer_reg_w((address - 0x46) / 2,
		            static_cast<uint16_t>(data << ((address & 1) * 8)), mask);
		return;
	}
	if (address >= 0x56 && address <= 0x5d) {
		timer_mode_w(address - 0x56, data);
		return;
	}
	if (address >= 0x70 && address <= 0x7f) {
		static constexpr int lines[16] = {
			M37710_LINE_ADC, M37710_LINE_UART0XMIT, M37710_LINE_UART0RECV,
			M37710_LINE_UART1XMIT, M37710_LINE_UART1RECV,
			M37710_LINE_TIMERA0, M37710_LINE_TIMERA1, M37710_LINE_TIMERA2,
			M37710_LINE_TIMERA3, M37710_LINE_TIMERA4, M37710_LINE_TIMERB0,
			M37710_LINE_TIMERB1, M37710_LINE_TIMERB2,
			M37710_LINE_IRQ0, M37710_LINE_IRQ1, M37710_LINE_IRQ2
		};
		set_int_control(lines[address - 0x70], data);
		return;
	}
	if (address >= 0x80 && address <= 0x27f) {
		m_internal_ram[address - 0x80] = data;
		return;
	}
	if (m_external_write) m_external_write(address, data);
}

uint16_t m37710_cpu_device::read_word(uint32_t address) {
	address &= 0x00ffffff;

	if (address == 0x36)
		return uart0_rbuf_r();
	if (address == 0x3e)
		return uart1_rbuf_r();

	if (address >= 0x280 && !(address >= 0xc000 && address <= 0xffff) &&
		m_external_read_word)
		return m_external_read_word(address);
	return static_cast<uint16_t>(read_byte(address) | (read_byte(address + 1) << 8));
}

void m37710_cpu_device::write_word(uint32_t address, uint16_t data) {
	address &= 0x00ffffff;

	if (address == 0x32) {
		uart0_tbuf_w(data);
		return;
	}
	if (address == 0x3a) {
		uart1_tbuf_w(data);
		return;
	}

	if (address >= 0x280 && !(address >= 0xc000 && address <= 0xffff) &&
		m_external_write_word) {
		m_external_write_word(address, data);
		return;
	}
	write_byte(address, static_cast<uint8_t>(data));
	write_byte(address + 1, static_cast<uint8_t>(data >> 8));
}


// On-board RAM, ROM, and peripherals

template <int Base>
uint8_t m37710_cpu_device::port_r(uint32_t offset)
{
	int p = (offset & ~1) + Base;

	uint8_t result = 0;
	if (BIT(offset, 0))
		result = get_port_dir(p);
	else
		result = get_port_reg(p);

	LOGMASKED(LOG_PORTS, "port_r from %02x: Port P%d %s = %x\n",
		0x02 + (Base + offset) * 2 - (Base & 1),
		p,
		BIT(offset, 0) ? "dir reg" : "reg", result);

	return result;
}

template <int Base>
void m37710_cpu_device::port_w(uint32_t offset, uint8_t data)
{
	int p = (offset & ~1) + Base;

	LOGMASKED(LOG_PORTS, "port_w %x to %02x: Port P%d %s = %x\n",
		data,
		0x02 + (Base + offset) * 2 - (Base & 1),
		p,
		BIT(offset, 0) ? "dir reg" : "reg",
		BIT(offset, 0) ? m_port_dir[p] : m_port_regs[p]);

	if (BIT(offset, 0))
		set_port_dir(p, data);
	else
		set_port_reg(p, data);
}

template <int Level>
uint8_t m37710_cpu_device::int_control_r()
{
	return get_int_control(Level);
}

template <int Level>
void m37710_cpu_device::int_control_w(uint8_t data)
{
	set_int_control(Level, data);
}

#if 0

void m37710_cpu_device::ad_register_map(address_map &map)
{
	map(0x00001e, 0x00001e).rw(FUNC(m37710_cpu_device::ad_control_r), FUNC(m37710_cpu_device::ad_control_w));
	map(0x00001f, 0x00001f).rw(FUNC(m37710_cpu_device::ad_sweep_r), FUNC(m37710_cpu_device::ad_sweep_w));
	map(0x000020, 0x00002f).r(FUNC(m37710_cpu_device::ad_result_r));
	map(0x000070, 0x000070).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_ADC>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_ADC>));
}

void m37710_cpu_device::uart0_register_map(address_map &map)
{
	map(0x000030, 0x000030).rw(FUNC(m37710_cpu_device::uart0_mode_r), FUNC(m37710_cpu_device::uart0_mode_w));
	map(0x000031, 0x000031).w(FUNC(m37710_cpu_device::uart0_baud_w));
	map(0x000032, 0x000033).w(FUNC(m37710_cpu_device::uart0_tbuf_w));
	map(0x000034, 0x000034).rw(FUNC(m37710_cpu_device::uart0_ctrl_reg0_r), FUNC(m37710_cpu_device::uart0_ctrl_reg0_w));
	map(0x000035, 0x000035).rw(FUNC(m37710_cpu_device::uart0_ctrl_reg1_r), FUNC(m37710_cpu_device::uart0_ctrl_reg1_w));
	map(0x000036, 0x000037).r(FUNC(m37710_cpu_device::uart0_rbuf_r));
	map(0x000071, 0x000071).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_UART0XMIT>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_UART0XMIT>));
	map(0x000072, 0x000072).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_UART0RECV>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_UART0RECV>));
}

void m37710_cpu_device::uart1_register_map(address_map &map)
{
	map(0x000038, 0x000038).rw(FUNC(m37710_cpu_device::uart1_mode_r), FUNC(m37710_cpu_device::uart1_mode_w));
	map(0x000039, 0x000039).w(FUNC(m37710_cpu_device::uart1_baud_w));
	map(0x00003a, 0x00003b).w(FUNC(m37710_cpu_device::uart1_tbuf_w));
	map(0x00003c, 0x00003c).rw(FUNC(m37710_cpu_device::uart1_ctrl_reg0_r), FUNC(m37710_cpu_device::uart1_ctrl_reg0_w));
	map(0x00003d, 0x00003d).rw(FUNC(m37710_cpu_device::uart1_ctrl_reg1_r), FUNC(m37710_cpu_device::uart1_ctrl_reg1_w));
	map(0x00003e, 0x00003f).r(FUNC(m37710_cpu_device::uart1_rbuf_r));
	map(0x000073, 0x000073).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_UART1XMIT>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_UART1XMIT>));
	map(0x000074, 0x000074).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_UART1RECV>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_UART1RECV>));
}

void m37710_cpu_device::timer_register_map(address_map &map)
{
	map(0x000040, 0x000040).rw(FUNC(m37710_cpu_device::count_start_r), FUNC(m37710_cpu_device::count_start_w));
	map(0x000042, 0x000042).w(FUNC(m37710_cpu_device::one_shot_start_w));
	map(0x000044, 0x000044).rw(FUNC(m37710_cpu_device::up_down_r), FUNC(m37710_cpu_device::up_down_w));
	map(0x000046, 0x000055).rw(FUNC(m37710_cpu_device::timer_reg_r), FUNC(m37710_cpu_device::timer_reg_w));
	map(0x000056, 0x00005d).rw(FUNC(m37710_cpu_device::timer_mode_r), FUNC(m37710_cpu_device::timer_mode_w));
	map(0x000075, 0x000075).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERA0>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERA0>));
	map(0x000076, 0x000076).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERA1>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERA1>));
	map(0x000077, 0x000077).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERA2>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERA2>));
	map(0x000078, 0x000078).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERA3>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERA3>));
	map(0x000079, 0x000079).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERA4>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERA4>));
	map(0x00007a, 0x00007a).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERB0>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERB0>));
	map(0x00007b, 0x00007b).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERB1>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERB1>));
	map(0x00007c, 0x00007c).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERB2>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERB2>));
}

void m37710_cpu_device::timer_6channel_register_map(address_map &map)
{
	map(0x000040, 0x000040).rw(FUNC(m37710_cpu_device::count_start_r), FUNC(m37710_cpu_device::count_start_w));
	map(0x000042, 0x000042).w(FUNC(m37710_cpu_device::one_shot_start_w));
	map(0x000044, 0x000044).rw(FUNC(m37710_cpu_device::up_down_r), FUNC(m37710_cpu_device::up_down_w));
	map(0x000046, 0x000051).rw(FUNC(m37710_cpu_device::timer_reg_r), FUNC(m37710_cpu_device::timer_reg_w));
	map(0x000056, 0x00005b).rw(FUNC(m37710_cpu_device::timer_mode_r), FUNC(m37710_cpu_device::timer_mode_w));
	map(0x000075, 0x000075).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERA0>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERA0>));
	map(0x000076, 0x000076).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERA1>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERA1>));
	map(0x000077, 0x000077).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERA2>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERA2>));
	map(0x000078, 0x000078).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERA3>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERA3>));
	map(0x000079, 0x000079).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERA4>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERA4>));
	map(0x00007a, 0x00007a).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_TIMERB0>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_TIMERB0>));
}

void m37710_cpu_device::irq_register_map(address_map &map)
{
	map(0x00007d, 0x00007d).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_IRQ0>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_IRQ0>));
	map(0x00007e, 0x00007e).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_IRQ1>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_IRQ1>));
	map(0x00007f, 0x00007f).rw(FUNC(m37710_cpu_device::int_control_r<M37710_LINE_IRQ2>), FUNC(m37710_cpu_device::int_control_w<M37710_LINE_IRQ2>));
}

// M37702M2: 512 bytes internal RAM, 16K internal mask ROM
// (M37702E2: same with EPROM instead of mask ROM)
void m37702m2_device::map(address_map &map)
{
	map(0x000000, 0x00007f).noprw();
	map(0x000002, 0x000015).rw(FUNC(m37702m2_device::port_r<0>), FUNC(m37702m2_device::port_w<0>)).umask16(0x00ff);
	map(0x000002, 0x000011).rw(FUNC(m37702m2_device::port_r<1>), FUNC(m37702m2_device::port_w<1>)).umask16(0xff00);
	map(0x00005e, 0x00005e).rw(FUNC(m37702m2_device::proc_mode_r), FUNC(m37702m2_device::proc_mode_w));
	map(0x000060, 0x000060).w(FUNC(m37702m2_device::watchdog_timer_w));
	map(0x000061, 0x000061).rw(FUNC(m37702m2_device::watchdog_freq_r), FUNC(m37702m2_device::watchdog_freq_w));
	ad_register_map(map);
	uart0_register_map(map);
	uart1_register_map(map);
	timer_register_map(map);
	irq_register_map(map);
	map(0x000080, 0x00027f).ram();
	map(0x00c000, 0x00ffff).rom().region(M37710_INTERNAL_ROM_REGION, 0);
}


// M37702S1: 512 bytes internal RAM, no internal ROM
void m37702s1_device::map(address_map &map)
{
	map(0x000000, 0x00007f).noprw();
	map(0x000002, 0x000015).rw(FUNC(m37702s1_device::port_r<0>), FUNC(m37702s1_device::port_w<0>)).umask16(0x00ff);
	map(0x000002, 0x000011).rw(FUNC(m37702s1_device::port_r<1>), FUNC(m37702s1_device::port_w<1>)).umask16(0xff00);
	map(0x00005e, 0x00005e).rw(FUNC(m37702s1_device::proc_mode_r), FUNC(m37702s1_device::proc_mode_w));
	map(0x000060, 0x000060).w(FUNC(m37702s1_device::watchdog_timer_w));
	map(0x000061, 0x000061).rw(FUNC(m37702s1_device::watchdog_freq_r), FUNC(m37702s1_device::watchdog_freq_w));
	ad_register_map(map);
	uart0_register_map(map);
	uart1_register_map(map);
	timer_register_map(map);
	irq_register_map(map);
	map(0x000080, 0x00027f).ram();
}


// M37710S4: 2048 bytes internal RAM, no internal ROM
void m37710s4_device::map(address_map &map)
{
	map(0x000000, 0x000001).noprw();
	map(0x00000a, 0x00007f).noprw();
	map(0x00000a, 0x000015).rw(FUNC(m37710s4_device::port_r<4>), FUNC(m37710s4_device::port_w<4>)).umask16(0x00ff);
	map(0x00000a, 0x000011).rw(FUNC(m37710s4_device::port_r<5>), FUNC(m37710s4_device::port_w<5>)).umask16(0xff00);
	map(0x00001a, 0x00001d).w(FUNC(m37710s4_device::da_reg_w)).umask16(0x00ff);
	map(0x00005e, 0x00005e).rw(FUNC(m37710s4_device::proc_mode_r), FUNC(m37710s4_device::proc_mode_w));
	map(0x000060, 0x000060).w(FUNC(m37710s4_device::watchdog_timer_w));
	map(0x000061, 0x000061).rw(FUNC(m37710s4_device::watchdog_freq_r), FUNC(m37710s4_device::watchdog_freq_w));
	map(0x000062, 0x000062).rw(FUNC(m37710s4_device::waveform_mode_r), FUNC(m37710s4_device::waveform_mode_w));
	map(0x000064, 0x000065).w(FUNC(m37710s4_device::pulse_output_w));
	ad_register_map(map);
	uart0_register_map(map);
	uart1_register_map(map);
	timer_register_map(map);
	irq_register_map(map);
	map(0x000080, 0x00087f).ram();
}

// M37720S1: 512 bytes internal RAM, no internal ROM, built-in DMA
void m37720s1_device::map(address_map &map)
{
	map(0x000000, 0x000001).noprw();
	map(0x00000a, 0x00007f).noprw();
	map(0x00000a, 0x000019).rw(FUNC(m37720s1_device::port_r<4>), FUNC(m37720s1_device::port_w<4>)).umask16(0x00ff);
	map(0x00000a, 0x000015).rw(FUNC(m37720s1_device::port_r<5>), FUNC(m37720s1_device::port_w<5>)).umask16(0xff00);
	map(0x00001a, 0x00001d).w(FUNC(m37720s1_device::pulse_output_w)).umask16(0x00ff);
	map(0x00005e, 0x00005e).rw(FUNC(m37720s1_device::proc_mode_r), FUNC(m37720s1_device::proc_mode_w));
	map(0x000060, 0x000060).w(FUNC(m37720s1_device::watchdog_timer_w));
	map(0x000061, 0x000061).rw(FUNC(m37720s1_device::watchdog_freq_r), FUNC(m37720s1_device::watchdog_freq_w));
	map(0x000062, 0x000062).rw(FUNC(m37720s1_device::rto_control_r), FUNC(m37720s1_device::rto_control_w));
	map(0x000064, 0x000064).rw(FUNC(m37720s1_device::dram_control_r), FUNC(m37720s1_device::dram_control_w));
	map(0x000066, 0x000066).w(FUNC(m37720s1_device::refresh_timer_w));
	map(0x000068, 0x000069).rw(FUNC(m37720s1_device::dmac_control_r), FUNC(m37720s1_device::dmac_control_w));
	map(0x00006c, 0x00006c).rw(FUNC(m37720s1_device::int_control_r<M37710_LINE_DMA0>), FUNC(m37720s1_device::int_control_w<M37710_LINE_DMA0>));
	map(0x00006d, 0x00006d).rw(FUNC(m37720s1_device::int_control_r<M37710_LINE_DMA1>), FUNC(m37720s1_device::int_control_w<M37710_LINE_DMA1>));
	map(0x00006e, 0x00006e).rw(FUNC(m37720s1_device::int_control_r<M37710_LINE_DMA2>), FUNC(m37720s1_device::int_control_w<M37710_LINE_DMA2>));
	map(0x00006f, 0x00006f).rw(FUNC(m37720s1_device::int_control_r<M37710_LINE_DMA3>), FUNC(m37720s1_device::int_control_w<M37710_LINE_DMA3>));
	ad_register_map(map);
	uart0_register_map(map);
	uart1_register_map(map);
	timer_register_map(map);
	irq_register_map(map);
	map(0x000080, 0x00027f).ram();
}

// M37730S2: 1024 bytes internal RAM, no internal ROM
void m37730s2_device::map(address_map &map)
{
	map(0x000000, 0x000001).noprw();
	map(0x00000a, 0x00007f).noprw();
	map(0x00000a, 0x000015).rw(FUNC(m37730s2_device::port_r<4>), FUNC(m37730s2_device::port_w<4>)).umask16(0x00ff);
	map(0x00000a, 0x00000d).rw(FUNC(m37730s2_device::port_r<5>), FUNC(m37730s2_device::port_w<5>)).umask16(0xff00);
	map(0x00005e, 0x00005e).rw(FUNC(m37730s2_device::proc_mode_r), FUNC(m37730s2_device::proc_mode_w));
	map(0x000060, 0x000060).w(FUNC(m37730s2_device::watchdog_timer_w));
	map(0x000061, 0x000061).rw(FUNC(m37730s2_device::watchdog_freq_r), FUNC(m37730s2_device::watchdog_freq_w));
	map(0x000062, 0x000062).rw(FUNC(m37730s2_device::waveform_mode_r), FUNC(m37730s2_device::waveform_mode_w));
	map(0x000064, 0x000065).w(FUNC(m37730s2_device::pulse_output_w));
	uart0_register_map(map);
	timer_6channel_register_map(map);
	irq_register_map(map);
	map(0x000080, 0x00047f).ram();
}

// M37732S4: 2048 bytes internal RAM, no internal ROM
void m37732s4_device::map(address_map &map)
{
	map(0x000000, 0x000001).noprw();
	map(0x00000a, 0x00007f).noprw();
	map(0x00000a, 0x000015).rw(FUNC(m37732s4_device::port_r<4>), FUNC(m37732s4_device::port_w<4>)).umask16(0x00ff);
	map(0x00000a, 0x00000d).rw(FUNC(m37732s4_device::port_r<5>), FUNC(m37732s4_device::port_w<5>)).umask16(0xff00);
	map(0x00005e, 0x00005e).rw(FUNC(m37732s4_device::proc_mode_r), FUNC(m37732s4_device::proc_mode_w));
	map(0x000060, 0x000060).w(FUNC(m37732s4_device::watchdog_timer_w));
	map(0x000061, 0x000061).rw(FUNC(m37732s4_device::watchdog_freq_r), FUNC(m37732s4_device::watchdog_freq_w));
	map(0x000062, 0x000062).rw(FUNC(m37732s4_device::waveform_mode_r), FUNC(m37732s4_device::waveform_mode_w));
	map(0x000064, 0x000065).w(FUNC(m37732s4_device::pulse_output_w));
	ad_register_map(map);
	uart0_register_map(map);
	uart1_register_map(map);
	timer_register_map(map);
	irq_register_map(map);
	map(0x000080, 0x00087f).ram();
}

// many other combinations of RAM and ROM size exist


m37710_cpu_device::m37710_cpu_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock, address_map_constructor map_delegate)
	: cpu_device(mconfig, type, tag, owner, clock)
	, m_program_config("program", ENDIANNESS_LITTLE, 16, 24, 0, map_delegate)
	, m_port_in_cb(*this, 0xff)
	, m_port_out_cb(*this)
	, m_analog_cb(*this, 0)
{
}


m37702m2_device::m37702m2_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: m37702m2_device(mconfig, M37702M2, tag, owner, clock)
{
}


m37702m2_device::m37702m2_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock)
	: m37710_cpu_device(mconfig, type, tag, owner, clock, address_map_constructor(FUNC(m37702m2_device::map), this))
{
}


m37702s1_device::m37702s1_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: m37710_cpu_device(mconfig, M37702S1, tag, owner, clock, address_map_constructor(FUNC(m37702s1_device::map), this))
{
}


m37710s4_device::m37710s4_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: m37710_cpu_device(mconfig, M37710S4, tag, owner, clock, address_map_constructor(FUNC(m37710s4_device::map), this))
{
}

m37720s1_device::m37720s1_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: m37710_cpu_device(mconfig, M37720S1, tag, owner, clock, address_map_constructor(FUNC(m37720s1_device::map), this))
{
}

m37730s2_device::m37730s2_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: m37710_cpu_device(mconfig, M37730S2, tag, owner, clock, address_map_constructor(FUNC(m37730s2_device::map), this))
{
}

m37732s4_device::m37732s4_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: m37710_cpu_device(mconfig, M37732S4, tag, owner, clock, address_map_constructor(FUNC(m37732s4_device::map), this))
{
}

std::vector<std::pair<int, const address_space_config *>> m37710_cpu_device::memory_space_config() const
{
	return std::vector<std::pair<int, const address_space_config *>> {
		std::make_pair(AS_PROGRAM, &m_program_config)
	};
}
#endif

/* interrupt control mapping */

const int m37710_cpu_device::m37710_irq_vectors[M37710_INTERRUPT_MAX] =
{
	// maskable
	0xffce, // DMA3
	0xffd0, // DMA2
	0xffd2, // DMA1
	0xffd4, // DMA0
	0xffd6, // A-D converter
	0xffd8, // UART1 transmit
	0xffda, // UART1 receive
	0xffdc, // UART0 transmit
	0xffde, // UART0 receive
	0xffe0, // Timer B2
	0xffe2, // Timer B1
	0xffe4, // Timer B0
	0xffe6, // Timer A4
	0xffe8, // Timer A3
	0xffea, // Timer A2
	0xffec, // Timer A1
	0xffee, // Timer A0
	0xfff0, // external INT2 pin
	0xfff2, // external INT1 pin
	0xfff4, // external INT0 pin

	// non-maskable
	0xfff6, // watchdog timer
	0xfff8, // debugger control (not used in shipping ICs?)
	0xfffa, // BRK
	0xfffc, // divide by zero
	0xfffe, // RESET
};

// M37710 internal peripherals

const char *const m37710_cpu_device::m37710_tnames[8] =
{
	"A0", "A1", "A2", "A3", "A4", "B0", "B1", "B2"
};

const char *const m37710_cpu_device::m37710_intnames[M37710_INTERRUPT_MAX] =
{
	"DMA3", "DMA2", "DMA1", "DMA0",
	"A/D",
	"UART1 xmit", "UART1 recv",
	"UART0 xmit", "UART0 recv",
	"Timer B2", "Timer B1", "Timer B0",
	"Timer A4", "Timer A3", "Timer A2", "Timer A1", "Timer A0",
	"INT2", "INT1", "INT0",
	"Watchdog timer", "DBC", "BRK", "Zero division", "RESET" // nonmaskable
};

void m37710_cpu_device::m37710_timer_cb(int which)
{
	int curirq = M37710_LINE_TIMERA0 - which;
	m37710_set_irq_line(curirq, ASSERT_LINE);
}

void m37710_cpu_device::m37710_external_tick(int timer, int state)
{
	// we only care if the state is "on"
	if (!state)
	{
		return;
	}

	// check if enabled and in event counter mode
	if (BIT(m_count_start, timer))
	{
		if ((m_timer_mode[timer] & 0x3) == 1)
		{
			int upcount = 0;

			// timer b always counts down
			if (timer <= 4)
			{
				if (BIT(m_timer_mode[timer], 4))
				{
					// up/down determined by timer out pin
					upcount = m_timer_out[timer];
				}
				else
					upcount = m_up_down_reg >> timer & 1;
			}

			if (upcount)
				m_timer_reg[timer]++;
			else
				m_timer_reg[timer]--;
		}
		else
		{
			std::fprintf(stderr, "M37710: external tick for timer %d outside event mode\n", timer);
		}
	}
}

void m37710_cpu_device::m37710_recalc_timer(int timer)
{
	int tval;
	static const int tscales[4] = { 2, 16, 64, 512 };

	// check if enabled
	if (BIT(m_count_start, timer))
	{
		LOGMASKED(LOG_TIMER, "Timer %d (%s) is enabled\n", timer, m37710_tnames[timer]);

		// set the timer's value
		tval = m_timer_reg[timer];

		// HACK: ignore if timer is 8MHz (MAME slows down to a crawl)
		if (tval == 0 && (m_timer_mode[timer]&0xc0) == 0) return;

		if ((m_timer_mode[timer] & 0x3) == 0) {
			m_reload[timer] = std::max(1, tscales[m_timer_mode[timer] >> 6] *
			                             (tval + 1));
			m_timer_cycles[timer] = m_reload[timer];
		}
	}
}

uint8_t m37710_cpu_device::get_port_reg(int p)
{
	assert(p >= 0 && p < 11);

	uint8_t d = m_port_dir[p];
	if (d != 0xff)
		return ((m_port_in_cb[p] ? m_port_in_cb[p]() : 0xff) & ~d) |
		       (m_port_regs[p] & d);
	else
		return m_port_regs[p];
}

uint8_t m37710_cpu_device::get_port_dir(int p)
{
	assert(p >= 0 && p < 11);

	return m_port_dir[p];
}

void m37710_cpu_device::set_port_reg(int p, uint8_t data)
{
	assert(p >= 0 && p < 11);

	uint8_t d = m_port_dir[p];
	if (d != 0 && m_port_out_cb[p])
		m_port_out_cb[p](data & d);
	m_port_regs[p] = data;
}

void m37710_cpu_device::set_port_dir(int p, uint8_t data)
{
	assert(p >= 0 && p < 11);

	m_port_dir[p] = data;
}

void m37710_cpu_device::da_reg_w([[maybe_unused]] uint32_t offset, [[maybe_unused]] uint8_t data)
{
	LOG("da_reg_w %x to %02X: D/A %d\n", data, (int)(offset * 2) + 0x1a, offset);
}

void m37710_cpu_device::pulse_output_w([[maybe_unused]] uint32_t offset, [[maybe_unused]] uint8_t data)
{
	LOG("pulse_output_w %x: Pulse output data register %d\n", data, offset);
}

uint8_t m37710_cpu_device::ad_control_r()
{
	return m_ad_control;
}

void m37710_cpu_device::ad_control_w(uint8_t data)
{
	LOGMASKED(LOG_AD, "ad_control_w %x: A/D control reg = %x\n", data, m_ad_control);

	if (BIT(data, 6) && !BIT(m_ad_control, 6))
	{
		// A-D conversion clock may be selected as f2/4 or f2/2
		m_ad_cycles = 57 * 2 * (BIT(data, 7) ? 2 : 4);
		if (BIT(data, 4))
			data &= 0xf8;
	}
	else if (!BIT(data, 6))
		m_ad_cycles = 0;

	m_ad_control = data;
}

void m37710_cpu_device::ad_timer_cb()
{
	int line = m_ad_control & 0x07;

	m_ad_result[line] = m_analog_cb[line] ? m_analog_cb[line]() : 0;

	if (BIT(m_ad_control, 4))
		m_ad_control = (m_ad_control & 0xf8) | ((line + 1) & 0x07);

	// repeat or sweep conversion
	if (BIT(m_ad_control, 3) || (BIT(m_ad_control, 4) && line != (m_ad_sweep & 0x03) * 2 + 1))
	{
		LOGMASKED(LOG_AD, "AN%d input converted = %x (repeat/sweep)\n", line, m_ad_result[line]);
		m_ad_cycles = 57 * 2 * (BIT(m_ad_control, 7) ? 2 : 4);
	}
	else
	{
		// interrupt occurs only when conversion stops
		LOGMASKED(LOG_AD, "AN%d input converted = %x (finished)\n", line, m_ad_result[line]);
		m37710_set_irq_line(M37710_LINE_ADC, ASSERT_LINE);
		m_ad_control &= 0xbf;
		m_ad_cycles = 0;
	}
}

uint8_t m37710_cpu_device::ad_sweep_r()
{
	return m_ad_sweep;
}

void m37710_cpu_device::ad_sweep_w(uint8_t data)
{
	LOGMASKED(LOG_AD, "ad_sweep_w %x: A/D sweep pin select = %x\n", data, m_ad_sweep);

	m_ad_sweep = data;
}

uint16_t m37710_cpu_device::ad_result_r(uint32_t offset)
{
	uint16_t result = m_ad_result[offset];

	LOGMASKED(LOG_AD, "ad_result_r from %02x: A/D %d = %x (PC=%x)\n", (int)(offset * 2) + 0x20, offset, result, REG_PG | REG_PC);

	return result;
}

uint8_t m37710_cpu_device::uart0_mode_r()
{
	LOGMASKED(LOG_UART, "uart0_mode_r: UART0 transmit/recv mode = %x (PC=%x)\n", m_uart_mode[0], REG_PG | REG_PC);
	if (m_uart_trace)
		m_uart_trace(0, false, 0x30, m_uart_mode[0]);

	return m_uart_mode[0];
}

void m37710_cpu_device::uart0_mode_w(uint8_t data)
{
	LOGMASKED(LOG_UART, "uart0_mode_w %x: UART0 transmit/recv mode = %x\n", data, m_uart_mode[0]);
	if (m_uart_trace)
		m_uart_trace(0, true, 0x30, data);

	m_uart_mode[0] = data;
}

uint8_t m37710_cpu_device::uart1_mode_r()
{
	LOGMASKED(LOG_UART, "uart1_mode_r: UART1 transmit/recv mode = %x (PC=%x)\n", m_uart_mode[1], REG_PG | REG_PC);
	if (m_uart_trace)
		m_uart_trace(1, false, 0x38, m_uart_mode[1]);

	return m_uart_mode[1];
}

void m37710_cpu_device::uart1_mode_w(uint8_t data)
{
	LOGMASKED(LOG_UART, "uart1_mode_w %x: UART1 transmit/recv mode = %x\n", data, m_uart_mode[1]);
	if (m_uart_trace)
		m_uart_trace(1, true, 0x38, data);

	m_uart_mode[1] = data;
}

void m37710_cpu_device::uart0_baud_w(uint8_t data)
{
	LOGMASKED(LOG_UART, "uart0_baud_w %x: UART0 baud rate = %x\n", data, m_uart_baud[0]);
	if (m_uart_trace)
		m_uart_trace(0, true, 0x31, data);

	m_uart_baud[0] = data;
}

void m37710_cpu_device::uart1_baud_w(uint8_t data)
{
	LOGMASKED(LOG_UART, "uart1_baud_w %x: UART1 baud rate = %x\n", data, m_uart_baud[1]);
	if (m_uart_trace)
		m_uart_trace(1, true, 0x39, data);

	m_uart_baud[1] = data;
}

void m37710_cpu_device::uart0_tbuf_w(uint16_t data)
{
	LOGMASKED(LOG_UART, "uart0_tbuf_w %x: UART0 transmit buf\n", data);
	if (m_uart_trace)
		m_uart_trace(0, true, 0x32, data);
}

void m37710_cpu_device::uart1_tbuf_w(uint16_t data)
{
	LOGMASKED(LOG_UART, "uart1_tbuf_w %x: UART1 transmit buf\n", data);
	if (m_uart_trace)
		m_uart_trace(1, true, 0x3a, data);
}

uint8_t m37710_cpu_device::uart0_ctrl_reg0_r()
{
	LOGMASKED(LOG_UART, "uart0_ctrl_reg0_r: UART0 transmit/recv ctrl 0 = %x (PC=%x)\n", m_uart_ctrl_reg0[0], REG_PG | REG_PC);
	if (m_uart_trace)
		m_uart_trace(0, false, 0x34, m_uart_ctrl_reg0[0]);

	return m_uart_ctrl_reg0[0];
}

void m37710_cpu_device::uart0_ctrl_reg0_w(uint8_t data)
{
	LOGMASKED(LOG_UART, "uart0_ctrl_reg0_w %x: UART0 transmit/recv ctrl 0 = %x\n", data, m_uart_ctrl_reg0[0]);
	if (m_uart_trace)
		m_uart_trace(0, true, 0x34, data);

	// Tx empty flag is read-only
	m_uart_ctrl_reg0[0] = (data & ~8) | (m_uart_ctrl_reg0[0] & 8);
}

uint8_t m37710_cpu_device::uart1_ctrl_reg0_r()
{
	LOGMASKED(LOG_UART, "uart1_ctrl_reg0_r: UART1 transmit/recv ctrl 0 = %x (PC=%x)\n", m_uart_ctrl_reg0[1], REG_PG | REG_PC);
	if (m_uart_trace)
		m_uart_trace(1, false, 0x3c, m_uart_ctrl_reg0[1]);

	return m_uart_ctrl_reg0[1];
}

void m37710_cpu_device::uart1_ctrl_reg0_w(uint8_t data)
{
	LOGMASKED(LOG_UART, "uart1_ctrl_reg0_w %x: UART1 transmit/recv ctrl 0 = %x\n", data, m_uart_ctrl_reg0[1]);
	if (m_uart_trace)
		m_uart_trace(1, true, 0x3c, data);

	// Tx empty flag is read-only
	m_uart_ctrl_reg0[1] = (data & ~8) | (m_uart_ctrl_reg0[1] & 8);
}

uint8_t m37710_cpu_device::uart0_ctrl_reg1_r()
{
	LOGMASKED(LOG_UART, "uart0_ctrl_reg1_r: UART0 transmit/recv ctrl 1 = %x (PC=%x)\n", m_uart_ctrl_reg1[0], REG_PG | REG_PC);
	if (m_uart_trace)
		m_uart_trace(0, false, 0x35, m_uart_ctrl_reg1[0]);

	return m_uart_ctrl_reg1[0];
}

void m37710_cpu_device::uart0_ctrl_reg1_w(uint8_t data)
{
	LOGMASKED(LOG_UART, "uart0_ctrl_reg1_w %x: UART0 transmit/recv ctrl 1 = %x\n", data, m_uart_ctrl_reg1[0]);
	if (m_uart_trace)
		m_uart_trace(0, true, 0x35, data);

	m_uart_ctrl_reg1[0] = (m_uart_ctrl_reg1[0] & (BIT(data, 2) ? 0xfa : 0x0a)) | (data & 0x05);
}

uint8_t m37710_cpu_device::uart1_ctrl_reg1_r()
{
	LOGMASKED(LOG_UART, "uart1_ctrl_reg1_r: UART1 transmit/recv ctrl 1 = %x (PC=%x)\n", m_uart_ctrl_reg1[1], REG_PG | REG_PC);
	if (m_uart_trace)
		m_uart_trace(1, false, 0x3d, m_uart_ctrl_reg1[1]);

	return m_uart_ctrl_reg1[1];
}

void m37710_cpu_device::uart1_ctrl_reg1_w(uint8_t data)
{
	LOGMASKED(LOG_UART, "uart1_ctrl_reg1_w %x: UART1 transmit/recv ctrl 1 = %x\n", data, m_uart_ctrl_reg1[1]);
	if (m_uart_trace)
		m_uart_trace(1, true, 0x3d, data);

	m_uart_ctrl_reg1[1] = (m_uart_ctrl_reg1[1] & (BIT(data, 2) ? 0xfa : 0x0a)) | (data & 0x05);
}

uint16_t m37710_cpu_device::uart0_rbuf_r()
{
	const uint16_t value = m_uart_recv_buf[0];
	LOGMASKED(LOG_UART, "uart0_rbuf_r: UART0 recv buf = %x (PC=%x)\n", value, REG_PG | REG_PC);
	if (m_uart_trace)
		m_uart_trace(0, false, 0x36, value);

	// Reading the low-order byte (including a full word read) clears receive-complete and error status.
	m_uart_ctrl_reg1[0] &= 0x07;
	m37710_set_irq_line(M37710_LINE_UART0RECV, CLEAR_LINE);
	return value;
}

uint16_t m37710_cpu_device::uart1_rbuf_r()
{
	const uint16_t value = m_uart_recv_buf[1];
	LOGMASKED(LOG_UART, "uart1_rbuf_r: UART1 recv buf = %x (PC=%x)\n", value, REG_PG | REG_PC);
	if (m_uart_trace)
		m_uart_trace(1, false, 0x3e, value);

	// Reading the low-order byte (including a full word read) clears receive-complete and error status.
	m_uart_ctrl_reg1[1] &= 0x07;
	m37710_set_irq_line(M37710_LINE_UART1RECV, CLEAR_LINE);
	return value;
}

uint8_t m37710_cpu_device::count_start_r()
{
	LOGMASKED(LOG_TIMER, "count_start_r: Count start = %x (PC=%x)\n", m_count_start, REG_PG | REG_PC);

	return m_count_start;
}

void m37710_cpu_device::count_start_w(uint8_t data)
{
	uint8_t prevdata = m_count_start;
	m_count_start = data;

	LOGMASKED(LOG_TIMER, "count_start_w %x: Count start = %x\n", data, prevdata);

	for (int i = 0; i < 8; i++)
		if (BIT(data, i) && !BIT(prevdata, i))
			m37710_recalc_timer(i);
}

void m37710_cpu_device::one_shot_start_w(uint8_t data)
{
	LOGMASKED(LOG_TIMER, "one_shot_start_w %x: One-shot start = %x\n", data, m_one_shot_start);

	m_one_shot_start = data;
}

uint8_t m37710_cpu_device::up_down_r()
{
	LOGMASKED(LOG_TIMER, "up_down_r: Up-down register = %x (PC=%x)\n", m_up_down_reg, REG_PG | REG_PC);

	// bits 7-5 read back as 0
	return m_up_down_reg & 0x1f;
}

void m37710_cpu_device::up_down_w(uint8_t data)
{
	LOGMASKED(LOG_TIMER, "up_down_w %x: Up-down register = %x\n", data, m_up_down_reg);

	m_up_down_reg = data;
}

uint16_t m37710_cpu_device::timer_reg_r(uint32_t offset, uint16_t mem_mask)
{
	return m_timer_reg[offset] & mem_mask;
}

void m37710_cpu_device::timer_reg_w(uint32_t offset, uint16_t data, uint16_t mem_mask)
{
	LOGMASKED(LOG_TIMER, "timer_reg_w %04x & %04x to %02x: Timer %s = %04x\n", data, mem_mask, (int)(offset * 2) + 0x46, m37710_tnames[offset], m_timer_reg[offset]);

	m_timer_reg[offset] = (data & mem_mask) | (m_timer_reg[offset] & ~mem_mask);
}

uint8_t m37710_cpu_device::timer_mode_r(uint32_t offset)
{
	LOGMASKED(LOG_TIMER, "timer_mode_r from %02x: Timer %s mode = %x (PC=%x)\n", (int)offset + 0x56, m37710_tnames[offset], m_timer_mode[offset], REG_PG | REG_PC);

	return m_timer_mode[offset];
}

void m37710_cpu_device::timer_mode_w(uint32_t offset, uint8_t data)
{
	LOGMASKED(LOG_TIMER, "timer_mode_w %x to %02x: Timer %s mode = %x\n", data, (int)offset + 0x56, m37710_tnames[offset], m_timer_mode[offset]);

	m_timer_mode[offset] = data;
}

uint8_t m37710_cpu_device::proc_mode_r([[maybe_unused]] uint32_t offset)
{
	LOG("proc_mode_r: Processor mode = %x (PC=%x)\n", m_proc_mode, REG_PG | REG_PC);

	return m_proc_mode & 0xf7;
}

void m37710_cpu_device::proc_mode_w(uint8_t data)
{
	LOG("proc_mode_w %x: Processor mode = %x\n", data, m_proc_mode);

	m_proc_mode = data;
}

void m37710_cpu_device::watchdog_timer_w([[maybe_unused]] uint8_t data)
{
	// TODO: reset watchdog timer (data is irrelevant)
}

uint8_t m37710_cpu_device::watchdog_freq_r()
{
	return m_watchdog_freq;
}

void m37710_cpu_device::watchdog_freq_w(uint8_t data)
{
	LOG("watchdog_freq_w %x: Watchdog timer frequency = %x\n", data, m_watchdog_freq);

	m_watchdog_freq = data;
}

uint8_t m37710_cpu_device::waveform_mode_r()
{
	LOG("waveform_mode_r: Waveform output mode (PC=%x)\n", REG_PG | REG_PC);

	return 0;
}

void m37710_cpu_device::waveform_mode_w([[maybe_unused]] uint8_t data)
{
	LOG("waveform_mode_w %x: Waveform output mode\n", data);
}

uint8_t m37710_cpu_device::rto_control_r()
{
	LOG("rto_control_r: Real-time output control = %x (PC=%x)\n", m_rto_control, REG_PG | REG_PC);

	return m_rto_control;
}

void m37710_cpu_device::rto_control_w(uint8_t data)
{
	LOG("rto_control_w %x: Real-time output control = %x\n", data, m_rto_control);

	m_rto_control = data;
}

uint8_t m37710_cpu_device::dram_control_r()
{
	LOG("dram_control_r: DRAM control = %x (PC=%x)\n", m_dram_control, REG_PG | REG_PC);

	return m_dram_control;
}

void m37710_cpu_device::dram_control_w(uint8_t data)
{
	LOG("dram_control_w %x: DRAM control = %x\n", data, m_dram_control);

	m_dram_control = data;
}

void m37710_cpu_device::refresh_timer_w([[maybe_unused]] uint8_t data)
{
	LOG("refresh_timer_w %x: Set refresh timer\n", data);
}

uint16_t m37710_cpu_device::dmac_control_r([[maybe_unused]] uint32_t offset, uint16_t mem_mask)
{
	return m_dmac_control & mem_mask;
}

void m37710_cpu_device::dmac_control_w(uint32_t offset, uint16_t data, uint16_t mem_mask)
{
	LOG("dmac_control_w %04x & %04x: DMAC control = %04x\n", data, mem_mask, m_dmac_control);

	m_dmac_control = (data & mem_mask) | (m_timer_reg[offset] & ~mem_mask);
}

uint8_t m37710_cpu_device::get_int_control(int level)
{
	assert(level < M37710_MASKABLE_INTERRUPTS);

	//LOGMASKED(LOG_INT, "int_control_r: %s IRQ ctrl = %x (PC=%x)\n", m37710_intnames[level], m_int_control[level], REG_PG | REG_PC);

	uint8_t result = m_int_control[level];

	return result;
}

void m37710_cpu_device::set_int_control(int level, uint8_t data)
{
	assert(level < M37710_MASKABLE_INTERRUPTS);

	LOGMASKED(LOG_INT, "int_control_w %x: %s IRQ ctrl = %x\n", data, m37710_intnames[level], m_int_control[level]);

	m_int_control[level] = data;

	//m37710_set_irq_line(offset, (data & 8) ? HOLD_LINE : CLEAR_LINE);
	m37710i_update_irqs();

	// level-sense interrupts are not implemented yet
	if ((level == M37710_LINE_IRQ0 || level == M37710_LINE_IRQ1 || level == M37710_LINE_IRQ2) && BIT(data, 5))
		logerror("error M37710: INT%d level-sense\n", M37710_LINE_IRQ0 - level);
}

const m37710_cpu_device::opcode_func *const m37710_cpu_device::m37710i_opcodes[4] =
{
	m37710i_opcodes_M0X0,
	m37710i_opcodes_M0X1,
	m37710i_opcodes_M1X0,
	m37710i_opcodes_M1X1,
};

const m37710_cpu_device::opcode_func *const m37710_cpu_device::m37710i_opcodes2[4] =
{
	m37710i_opcodes42_M0X0,
	m37710i_opcodes42_M0X1,
	m37710i_opcodes42_M1X0,
	m37710i_opcodes42_M1X1,
};

const m37710_cpu_device::opcode_func *const m37710_cpu_device::m37710i_opcodes3[4] =
{
	m37710i_opcodes89_M0X0,
	m37710i_opcodes89_M0X1,
	m37710i_opcodes89_M1X0,
	m37710i_opcodes89_M1X1,
};

const m37710_cpu_device::get_reg_func m37710_cpu_device::m37710i_get_reg[4] =
{
	&m37710_cpu_device::m37710i_get_reg_M0X0,
	&m37710_cpu_device::m37710i_get_reg_M0X1,
	&m37710_cpu_device::m37710i_get_reg_M1X0,
	&m37710_cpu_device::m37710i_get_reg_M1X1,
};

const m37710_cpu_device::set_reg_func m37710_cpu_device::m37710i_set_reg[4] =
{
	&m37710_cpu_device::m37710i_set_reg_M0X0,
	&m37710_cpu_device::m37710i_set_reg_M0X1,
	&m37710_cpu_device::m37710i_set_reg_M1X0,
	&m37710_cpu_device::m37710i_set_reg_M1X1,
};

const m37710_cpu_device::execute_func m37710_cpu_device::m37710i_execute[4] =
{
	&m37710_cpu_device::m37710i_execute_M0X0,
	&m37710_cpu_device::m37710i_execute_M0X1,
	&m37710_cpu_device::m37710i_execute_M1X0,
	&m37710_cpu_device::m37710i_execute_M1X1,
};

/* internal functions */

void m37710_cpu_device::m37710i_update_irqs()
{
	int curirq, pending = LINE_IRQ;
	int wantedIRQ = -1;
	int curpri = 0;

	for (curirq = M37710_INTERRUPT_MAX - 1; curirq >= 0; curirq--)
	{
		if ((pending & (1 << curirq)))
		{
			// this IRQ is set
			if (curirq < M37710_MASKABLE_INTERRUPTS)
			{
				int control = m_int_control[curirq];
				int thispri = control & 7;
				// logerror("line %d set, level %x curpri %x IPL %x\n", curirq, thispri, curpri, m_ipl);
				// it's maskable, check if the level works, also make sure it's acceptable for the current CPU level
				if (!FLAG_I && thispri > curpri && static_cast<uint32_t>(thispri) > m_ipl)
				{
					// mark us as the best candidate
					LOGMASKED(LOG_INT, "%s interrupt active with priority %d (PC=%x)\n", m37710_intnames[curirq], thispri, REG_PG | REG_PC);
					wantedIRQ = curirq;
					curpri = thispri;
				}
			}
			else
			{
				// non-maskable
				LOGMASKED(LOG_INT, "%s interrupt active (PC=%x)\n", m37710_intnames[curirq], REG_PG | REG_PC);
				wantedIRQ = curirq;
				curpri = 7;
				break;  // no more processing, NMIs always win
			}
		}
	}

	if (wantedIRQ != -1)
	{
		// make sure we're running to service the interrupt
		CPU_STOPPED &= ~STOP_LEVEL_WAI;

		// auto-clear line
		m37710_set_irq_line(wantedIRQ, CLEAR_LINE);

		// let's do it...
		// push PB, then PC, then status
		CLK(13);
		m37710i_push_8(REG_PG>>16);
		m37710i_push_16(REG_PC);
		m37710i_push_8(m_ipl);
		m37710i_push_8(m37710i_get_reg_ps());

		// set I to 1, set IPL to the interrupt we're taking
		FLAG_I = IFLAG_SET;
		m_ipl = curpri;
		// then PG=0, PC=(vector)
		REG_PG = 0;
		REG_PC = m37710_read_16(m37710_irq_vectors[wantedIRQ]);
	}
}

/* external functions */

void m37710_cpu_device::reset()
{
	int i;

	/* Reset internal timers */
	for (i = 0; i < 8; i++)
	{
		m_timer_cycles[i] = 0;
		m_reload[i] = 0;
	}

	/* Start the CPU */
	CPU_STOPPED = 0;

	/* Reset internal registers */
	// port direction
	std::fill(std::begin(m_port_dir), std::end(m_port_dir), 0);

	// A-D
	m_ad_control &= 7;
	m_ad_sweep = (m_ad_sweep & ~0xdc) | 3;
	m_ad_cycles = 0;

	// UARTs
	for (i = 0; i < 2; i++)
	{
		m_uart_mode[i] = 0;
		m_uart_ctrl_reg0[i] = (m_uart_ctrl_reg0[i] & 0xe0) | 8;
		m_uart_ctrl_reg1[i] = 2;
		m_uart_recv_buf[i] = 0;
	}

	// timers
	m_count_start = 0;
	m_one_shot_start &= ~0x1f;
	m_up_down_reg = 0;
	for (i = 0; i < 8; i++)
		m_timer_mode[i] = 0;

	m_proc_mode = 0; // processor mode
	m_watchdog_freq &= ~1; // watchdog timer frequency
	m_rto_control &= ~3;
	m_dram_control &= ~0x8f;

	// interrupt control
	for (i = 0; i <= M37710_LINE_TIMERA0; i++)
		m_int_control[i] &= ~0xf;
	for (i = M37710_LINE_IRQ2; i <= M37710_LINE_IRQ0; i++)
		m_int_control[i] &= ~0x3f;

	/* Clear IPL, m, x, D and set I */
	m_ipl = 0;
	FLAG_M = MFLAG_CLEAR;
	FLAG_X = XFLAG_CLEAR;
	FLAG_D = DFLAG_CLEAR;
	FLAG_I = IFLAG_SET;

	/* Clear all pending interrupts (should we really do this?) */
	LINE_IRQ = 0;
	IRQ_DELAY = 0;

	/* 37710 boots in full native mode */
	REG_DPR = 0;
	REG_PG = 0;
	REG_DT = 0;
	REG_S = (REG_S & 0xff) | 0x100;
	REG_XH = REG_X & 0xff00; REG_X &= 0xff;
	REG_YH = REG_Y & 0xff00; REG_Y &= 0xff;
	REG_B = REG_A & 0xff00; REG_A &= 0xff;
	REG_BB = REG_BA & 0xff00; REG_BA &= 0xff;

	/* Set the function tables to emulation mode */
	m37710i_set_execution_mode(EXECUTION_MODE_M0X0);

	/* Fetch the reset vector */
	REG_PC = m37710_read_16(0xfffe);
}

/* Execute some instructions */
int m37710_cpu_device::execute(int cycles)
{
	if (!m_rom_loaded || cycles <= 0) return 0;

	int total_consumed = 0;
	while (total_consumed < cycles)
	{
		m37710i_update_irqs();

		// MAME schedules the internal timers and A/D converter independently of the
		// CPU. Stop at the next peripheral deadline so an interrupt can be taken
		// before the remainder of the caller's scheduler slice executes.
		int slice_cycles = cycles - total_consumed;
		for (int timer = 0; timer < 8; ++timer)
		{
			if (m_timer_cycles[timer] > 0 && BIT(m_count_start, timer))
				slice_cycles = static_cast<int>(std::min<int64_t>(slice_cycles, m_timer_cycles[timer]));
		}
		if (m_ad_cycles > 0)
			slice_cycles = static_cast<int>(std::min<int64_t>(slice_cycles, m_ad_cycles));

		slice_cycles = std::max(slice_cycles, 1);
		m_ICount = slice_cycles;
		const int consumed = (this->*m_execute)(slice_cycles);
		m_ICount = slice_cycles - consumed;
		if (consumed <= 0)
			break;

		advance_timers(consumed);
		total_consumed += consumed;
	}

	return total_consumed;
}

void m37710_cpu_device::advance_timers(int cycles)
{
	for (int timer = 0; timer < 8; ++timer) {
		if (m_timer_cycles[timer] <= 0 || !BIT(m_count_start, timer)) continue;
		m_timer_cycles[timer] -= cycles;
		if (m_timer_cycles[timer] <= 0) {
			// A CPU scheduler slice can span several C74 timer periods.  Keep
			// the timer phase by advancing through all elapsed periods; the IRQ
			// latch records the pending event without turning a short period into
			// a burst of one-cycle callbacks on following slices.
			const int64_t reload = std::max<int64_t>(1, m_reload[timer]);
			const int64_t elapsed_periods =
				(-m_timer_cycles[timer] / reload) + 1;
			m_timer_cycles[timer] += elapsed_periods * reload;
			m37710_timer_cb(timer);
		}
	}
	if (m_ad_cycles > 0) {
		m_ad_cycles -= cycles;
		if (m_ad_cycles <= 0) ad_timer_cb();
	}
}


/* Set the Program Counter */
void m37710_cpu_device::m37710_set_pc(unsigned val)
{
	REG_PC = MAKE_UINT_16(val);
}

/* Get the current Stack Pointer */
unsigned m37710_cpu_device::m37710_get_sp()
{
	return REG_S;
}

/* Set the Stack Pointer */
void m37710_cpu_device::m37710_set_sp(unsigned val)
{
	REG_S = MAKE_UINT_16(val);
}

/* Get a register */
unsigned m37710_cpu_device::m37710_get_reg(int regnum)
{
	return (this->*m_get_reg)(regnum);
}

/* Set a register */
void m37710_cpu_device::m37710_set_reg(int regnum, unsigned value)
{
	(this->*m_set_reg)(regnum, value);
}

/* Set an interrupt line */
void m37710_cpu_device::m37710_set_irq_line(int line, int state)
{
	switch(line)
	{
		// maskable interrupts
		case M37710_LINE_ADC:
		case M37710_LINE_UART1XMIT:
		case M37710_LINE_UART1RECV:
		case M37710_LINE_UART0XMIT:
		case M37710_LINE_UART0RECV:
		case M37710_LINE_TIMERB2:
		case M37710_LINE_TIMERB1:
		case M37710_LINE_TIMERB0:
		case M37710_LINE_TIMERA4:
		case M37710_LINE_TIMERA3:
		case M37710_LINE_TIMERA2:
		case M37710_LINE_TIMERA1:
		case M37710_LINE_TIMERA0:
		case M37710_LINE_IRQ2:
		case M37710_LINE_IRQ1:
		case M37710_LINE_IRQ0:
		case M37710_LINE_DMA0:
		case M37710_LINE_DMA1:
		case M37710_LINE_DMA2:
		case M37710_LINE_DMA3:
			switch(state)
			{
				case CLEAR_LINE:
					LINE_IRQ &= ~(1 << line);
					m_int_control[line] &= ~8;
					break;

				case ASSERT_LINE:
				case HOLD_LINE:
					LINE_IRQ |= (1 << line);
					m_int_control[line] |= 8;
					break;

				default: break;
			}
			break;

		default: break;
	}
}

#if 0
bool m37710_cpu_device::get_m_flag() const
{
	return FLAG_M;
}

bool m37710_cpu_device::get_x_flag() const
{
	return FLAG_X;
}

std::unique_ptr<util::disasm_interface> m37710_cpu_device::create_disassembler()
{
	return std::make_unique<m7700_disassembler>(this);
}

void m37710_cpu_device::m37710_restore_state()
{
	// restore proper function pointers
	m37710i_set_execution_mode((FLAG_M>>4) | (FLAG_X>>4));
}

void m37710_cpu_device::device_start()
{
	m_a = 0;
	m_b = 0;
	m_ba = 0;
	m_bb = 0;
	m_x = 0;
	m_y = 0;
	m_xh = 0;
	m_yh = 0;
	m_s = 0;
	m_pc = 0;
	m_ppc = 0;
	m_pg = 0;
	m_dt = 0;
	m_dpr = 0;
	m_flag_e = 0;
	m_flag_m = 0;
	m_flag_x = 0;
	m_flag_n = 0;
	m_flag_v = 0;
	m_flag_d = 0;
	m_flag_i = 0;
	m_flag_z = 0;
	m_flag_c = 0;
	m_line_irq = 0;
	m_ipl = 0;
	m_ir = 0;
	m_im = 0;
	m_im2 = 0;
	m_im3 = 0;
	m_im4 = 0;
	m_irq_delay = 0;
	m_stopped = 0;
	std::fill(std::begin(m_port_regs), std::end(m_port_regs), 0);
	std::fill(std::begin(m_port_dir), std::end(m_port_dir), 0);
	m_ad_control = 0;
	m_ad_sweep = 0;
	std::fill(std::begin(m_ad_result), std::end(m_ad_result), 0);
	std::fill(std::begin(m_uart_mode), std::end(m_uart_mode), 0);
	std::fill(std::begin(m_uart_baud), std::end(m_uart_baud), 0);
	std::fill(std::begin(m_uart_ctrl_reg0), std::end(m_uart_ctrl_reg0), 0);
	std::fill(std::begin(m_uart_ctrl_reg1), std::end(m_uart_ctrl_reg1), 0);
	m_count_start = 0;
	m_one_shot_start = 0;
	m_up_down_reg = 0;
	std::fill(std::begin(m_timer_reg), std::end(m_timer_reg), 0);
	std::fill(std::begin(m_timer_mode), std::end(m_timer_mode), 0);
	m_proc_mode = 0;
	m_watchdog_freq = 0;
	std::fill(std::begin(m_int_control), std::end(m_int_control), 0);

	space(AS_PROGRAM).cache(m_cache);
	space(AS_PROGRAM).specific(m_program);

	m_ICount = 0;

	m_source = 0;
	m_destination = 0;

	for (int i = 0; i < 8; i++)
	{
		m_timers[i] = timer_alloc(FUNC(m37710_cpu_device::m37710_timer_cb), this);
		m_reload[i] = attotime::never;
		m_timer_out[i] = 0;
	}

	m_ad_timer = timer_alloc(FUNC(m37710_cpu_device::ad_timer_cb), this);

	save_item(NAME(m_a));
	save_item(NAME(m_b));
	save_item(NAME(m_ba));
	save_item(NAME(m_bb));
	save_item(NAME(m_x));
	save_item(NAME(m_y));
	save_item(NAME(m_xh));
	save_item(NAME(m_yh));
	save_item(NAME(m_s));
	save_item(NAME(m_pc));
	save_item(NAME(m_ppc));
	save_item(NAME(m_pg));
	save_item(NAME(m_dt));
	save_item(NAME(m_dpr));
	save_item(NAME(m_flag_e));
	save_item(NAME(m_flag_m));
	save_item(NAME(m_flag_x));
	save_item(NAME(m_flag_n));
	save_item(NAME(m_flag_v));
	save_item(NAME(m_flag_d));
	save_item(NAME(m_flag_i));
	save_item(NAME(m_flag_z));
	save_item(NAME(m_flag_c));
	save_item(NAME(m_line_irq));
	save_item(NAME(m_ipl));
	save_item(NAME(m_ir));
	save_item(NAME(m_im));
	save_item(NAME(m_im2));
	save_item(NAME(m_im3));
	save_item(NAME(m_im4));
	save_item(NAME(m_irq_delay));
	save_item(NAME(m_stopped));
	save_item(NAME(m_port_regs));
	save_item(NAME(m_port_dir));
	save_item(NAME(m_ad_control));
	save_item(NAME(m_ad_sweep));
	save_item(NAME(m_ad_result));
	save_item(NAME(m_uart_mode));
	save_item(NAME(m_uart_baud));
	save_item(NAME(m_uart_ctrl_reg0));
	save_item(NAME(m_uart_ctrl_reg1));
	save_item(NAME(m_count_start));
	save_item(NAME(m_one_shot_start));
	save_item(NAME(m_up_down_reg));
	save_item(NAME(m_timer_reg));
	save_item(NAME(m_timer_mode));
	save_item(NAME(m_reload[0]));
	save_item(NAME(m_reload[1]));
	save_item(NAME(m_reload[2]));
	save_item(NAME(m_reload[3]));
	save_item(NAME(m_reload[4]));
	save_item(NAME(m_reload[5]));
	save_item(NAME(m_reload[6]));
	save_item(NAME(m_reload[7]));
	save_item(NAME(m_timer_out));
	save_item(NAME(m_proc_mode));
	save_item(NAME(m_watchdog_freq));
	save_item(NAME(m_int_control));

	machine().save().register_postload(save_prepost_delegate(save_prepost_delegate(FUNC(m37710_cpu_device::m37710_restore_state), this)));

	state_add( M37710_PC,        "PC",  m_pc).formatstr("%04X");
	state_add( M37710_PG,        "PG",  m_debugger_pg).callimport().callexport().formatstr("%02X");
	state_add( M37710_DT,        "DT",  m_debugger_dt).callimport().callexport().formatstr("%02X");
	state_add( M37710_DPR,       "DPR", m_dpr).formatstr("%04X");
	state_add( M37710_S,         "S",   m_s).formatstr("%04X");
	state_add( M37710_PS,        "PS", m_debugger_ps).callimport().callexport().formatstr("%04X");
	state_add( M37710_E,         "E",   m_flag_e).formatstr("%01X");
	state_add( M37710_A,         "A",   m_debugger_a).callimport().callexport().formatstr("%04X");
	state_add( M37710_B,         "B",   m_debugger_b).callimport().callexport().formatstr("%04X");
	state_add( M37710_X,         "X",   m_x).formatstr("%04X");
	state_add( M37710_Y,         "Y",   m_y).formatstr("%04X");
	state_add( M37710_IRQ_STATE, "IRQ", m_line_irq).formatstr("%01X");

	state_add( STATE_GENPC, "GENPC", m_debugger_pc ).callimport().callexport().noshow();
	state_add( STATE_GENPCBASE, "CURPC", m_debugger_pc ).callimport().callexport().noshow();
	state_add( STATE_GENFLAGS, "GENFLAGS", m_debugger_ps ).formatstr("%8s").noshow();

	set_icountptr(m_ICount);
}


void m37710_cpu_device::state_import(const device_state_entry &entry)
{
	switch (entry.index())
	{
		case M37710_PG:
			m37710_set_reg(M37710_PG, m_debugger_pg);
			break;

		case M37710_DT:
			m37710_set_reg(M37710_DT, m_debugger_dt);
			break;

		case M37710_PS:
			m37710_set_reg(M37710_PS, m_debugger_ps&0xff);
			m_ipl = (m_debugger_ps>>8)&0xff;
			break;

		case M37710_A:
			m37710_set_reg(M37710_A, m_debugger_a);
			break;

		case M37710_B:
			m37710_set_reg(M37710_B, m_debugger_b);
			break;

		case STATE_GENPC:
		case STATE_GENPCBASE:
			REG_PG = m_debugger_pc & 0xff0000;
			m37710_set_pc(m_debugger_pc & 0xffff);
			break;
	}
}


void m37710_cpu_device::state_export(const device_state_entry &entry)
{
	switch (entry.index())
	{
		case M37710_PG:
			m_debugger_pg = m_pg >> 16;
			break;

		case M37710_DT:
			m_debugger_dt = m_dt >> 16;
			break;

		case M37710_PS:
			m_debugger_ps = (m_flag_n&0x80) | ((m_flag_v>>1)&0x40) | m_flag_m | m_flag_x | m_flag_d | m_flag_i | ((!m_flag_z)<<1) | ((m_flag_c>>8)&1) | (m_ipl<<8);
			break;

		case M37710_A:
			m_debugger_a = m_a | m_b;
			break;

		case M37710_B:
			m_debugger_b = m_ba | m_bb;
			break;

		case STATE_GENPC:
		case STATE_GENPCBASE:
			m_debugger_pc = (REG_PG | REG_PC);
			break;
	}
}


void m37710_cpu_device::state_string_export(const device_state_entry &entry, std::string &str) const
{
	switch (entry.index())
	{
		case STATE_GENFLAGS:
			str = string_format("%c%c%c%c%c%c%c%c",
				m_flag_n & NFLAG_SET ? 'N':'.',
				m_flag_v & VFLAG_SET ? 'V':'.',
				m_flag_m & MFLAG_SET ? 'M':'.',
				m_flag_x & XFLAG_SET ? 'X':'.',
				m_flag_d & DFLAG_SET ? 'D':'.',
				m_flag_i & IFLAG_SET ? 'I':'.',
				m_flag_z == 0        ? 'Z':'.',
				m_flag_c & CFLAG_SET ? 'C':'.');
			break;
	}
}


void m37710_cpu_device::execute_set_input(int inputnum, int state)
{
	switch( inputnum )
	{
		case M37710_LINE_ADC:
		case M37710_LINE_IRQ0:
		case M37710_LINE_IRQ1:
		case M37710_LINE_IRQ2:
			m37710_set_irq_line(inputnum, state);
			break;

		case M37710_LINE_TIMERA0IN:
		case M37710_LINE_TIMERA1IN:
		case M37710_LINE_TIMERA2IN:
		case M37710_LINE_TIMERA3IN:
		case M37710_LINE_TIMERA4IN:
		case M37710_LINE_TIMERB0IN:
		case M37710_LINE_TIMERB1IN:
		case M37710_LINE_TIMERB2IN:
			m37710_external_tick(inputnum - M37710_LINE_TIMERA0IN, state);
			break;

		case M37710_LINE_TIMERA0OUT:
		case M37710_LINE_TIMERA1OUT:
		case M37710_LINE_TIMERA2OUT:
		case M37710_LINE_TIMERA3OUT:
		case M37710_LINE_TIMERA4OUT:
		case M37710_LINE_TIMERB0OUT:
		case M37710_LINE_TIMERB1OUT:
		case M37710_LINE_TIMERB2OUT:
			m_timer_out[inputnum - M37710_LINE_TIMERA0OUT] = state ? 1 : 0;
			break;
	}
}
#endif

void m37710_cpu_device::set_input(int inputnum, bool asserted)
{
	const int state = asserted ? ASSERT_LINE : CLEAR_LINE;
	switch (inputnum)
	{
		case M37710_LINE_ADC:
		case M37710_LINE_IRQ0:
		case M37710_LINE_IRQ1:
		case M37710_LINE_IRQ2:
			m37710_set_irq_line(inputnum, state);
			break;
		case M37710_LINE_TIMERA0IN:
		case M37710_LINE_TIMERA1IN:
		case M37710_LINE_TIMERA2IN:
		case M37710_LINE_TIMERA3IN:
		case M37710_LINE_TIMERA4IN:
		case M37710_LINE_TIMERB0IN:
		case M37710_LINE_TIMERB1IN:
		case M37710_LINE_TIMERB2IN:
			m37710_external_tick(inputnum - M37710_LINE_TIMERA0IN, state);
			break;
		default:
			break;
	}
}


void m37710_cpu_device::m37710i_set_execution_mode(uint32_t mode)
{
	m_opcodes = m37710i_opcodes[mode];
	m_opcodes42 = m37710i_opcodes2[mode];
	m_opcodes89 = m37710i_opcodes3[mode];
	FTABLE_GET_REG = m37710i_get_reg[mode];
	FTABLE_SET_REG = m37710i_set_reg[mode];
	m_execute = m37710i_execute[mode];
}


/* ======================================================================== */
/* =============================== INTERRUPTS ============================= */
/* ======================================================================== */

void m37710_cpu_device::m37710i_interrupt_software(uint32_t vector)
{
	CLK(13);
	m37710i_push_8(REG_PG>>16);
	m37710i_push_16(REG_PC);
	m37710i_push_8(m_ipl);
	m37710i_push_8(m37710i_get_reg_ps());
	FLAG_I = IFLAG_SET;
	REG_PG = 0;
	REG_PC = m37710_read_16(vector);
}



/* ======================================================================== */
/* ============================== END OF FILE ============================= */
/* ======================================================================== */
