/*
 * (C) Copyright 2004-2012 Cavium, Inc. <support@cavium.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <i2c.h>
#include <asm/arch/octeon_boot.h>
#include <asm/arch/octeon_board_common.h>
#include <pci.h>
#include <miiphy.h>
#include <asm/arch/lib_octeon_shared.h>
#include <asm/arch/lib_octeon.h>
#include <asm/arch/cvmx-helper-jtag.h>

DECLARE_GLOBAL_DATA_PTR;

static const char *isp_voltage_labels[] = {
	NULL      ,		/* Chan 0  */
	"DCOK_QLM",		/* Chan 1  */
	NULL      ,		/* Chan 2  */
	"1.1V    ",		/* Chan 3  */
	"1.5V    ",		/* Chan 4  */
	"2.5V    ",		/* Chan 5  */
	NULL      ,		/* Chan 6  */
	"DDR 1.5V",		/* Chan 7  */
	"Core VDD",		/* Chan 8  */
	"3.3V    ",		/* Chan 9  */
	"5.0V    ",		/* Chan 10 */
	"12V / 3 ",		/* Chan 11 */
	"VCCA    ",		/* VCCA    */
	"VCCINP  ",		/* VCCINP  */
	0
};

extern void print_isp_volt(const char **labels, uint16_t isp_dev_addr, uint8_t adc_chan);

extern int read_ispPAC_mvolts(uint16_t isp_dev_addr, uint8_t adc_chan);

extern int read_ispPAC_mvolts_avg(int loops, uint16_t isp_dev_addr, uint8_t adc_chan);

int board_early_init_f(void)
{
	return 0;
}

/**
 * Modify the device tree to remove all unused interface types.
 */
int board_fixup_fdt(void)
{
	const char *fdt_key;
	cvmx_mio_qlmx_cfg_t mio_qlmx;
	char *eptr;
	int ret;
	uchar rv;

	/*
	 * If 'enable_spi_eeprom' is set in the environment, clear bit
	 * 2 of register 0x15 in the CPLD to enable the SPI EEPROM,
	 * then hijack octeon_fdt_patch to enable/disable the eeprom
	 * in the device tree.
	 */
	eptr = getenv("enable_spi_eeprom");
	fdt_key = "9,none";
	if (eptr) {
		ret = i2c_read(0x6c, 0x15, 1, &rv, 1);
		if (ret == 0) {
			if (rv == 0)
				fdt_key = "9,eeprom";
			rv &= ~4;
			ret = i2c_write(0x6c, 0x15, 1, &rv, 1);
		}
	}
	octeon_fdt_patch(working_fdt, fdt_key, NULL);

	return 0;
}

/* Raise an integer to a power */
static uint64_t ipow(uint64_t base, uint64_t exp)
{
	uint64_t result = 1;
	while (exp) {
		if (exp & 1)
			result *= base;
		exp >>= 1;
		base *= base;
	}
	return result;
}

int checkboard(void)
{
        int core_mVolts, dram_mVolts;

	if (octeon_show_info()) {

		int mcu_rev_maj = 0;
		int mcu_rev_min = 0;

		if (twsii_mcu_read(0x00) == 0xa5
		    && twsii_mcu_read(0x01) == 0x5a) {
			gd->ogd.mcu_rev_maj = mcu_rev_maj = twsii_mcu_read(2);
			gd->ogd.mcu_rev_min = mcu_rev_min = twsii_mcu_read(3);
		}
#if 0
		{
			int x;

			/* Enable this to display the voltages on all the channels. */
			printf("System Power Status:\n");
			for (x = 0; x < 14; x++) {
                            print_isp_volt(isp_voltage_labels, BOARD_ISP_TWSI_ADDR, x);
			}
		}
#endif

		core_mVolts = read_ispPAC_mvolts_avg(10, BOARD_ISP_TWSI_ADDR, 8);
		dram_mVolts = read_ispPAC_mvolts_avg(10, BOARD_ISP_TWSI_ADDR, 7);

		char mcu_ip_msg[64] = { 0 };

		if (twsii_mcu_read(0x14) == 1)
			sprintf(mcu_ip_msg, "MCU IPaddr: %d.%d.%d.%d, ",
				twsii_mcu_read(0x10),
				twsii_mcu_read(0x11),
				twsii_mcu_read(0x12), twsii_mcu_read(0x13));
		printf("MCU rev: %d.%02d, %sCPU voltage: %d.%03d DDR voltage: %d.%03d\n",
		       gd->ogd.mcu_rev_maj, gd->ogd.mcu_rev_min, mcu_ip_msg,
		       core_mVolts / 1000, core_mVolts % 1000,
		       dram_mVolts / 1000, dram_mVolts % 1000);

#define LED_CHARACTERS 8
		char tmp[10];
		int characters, idx = 0, value = core_mVolts;

		idx = sprintf(tmp, "%d ", gd->ogd.cpu_clock_mhz);
		characters = LED_CHARACTERS - idx;

		if (value / 1000) {
			idx += sprintf(tmp + idx, "%d", value / 1000);
			characters = LED_CHARACTERS - idx;
		}

		characters -= 1;	/* Account for decimal point */

		value %= 1000;
		value = divide_nint(value, ipow(10, max(3 - characters, 0)));

		idx += sprintf(tmp + idx, ".%0*d", min(3, characters), value);

		/* Display CPU speed and voltage on display */
		octeon_led_str_write(tmp);
	} else {
		octeon_led_str_write("Boot    ");
	}

	return 0;
}

int early_board_init(void)
{
	int cpu_ref = DEFAULT_CPU_REF_FREQUENCY_MHZ;

	{
		/* configure clk_out pin */
		cvmx_mio_fus_pll_t fus_pll;

		fus_pll.u64 = cvmx_read_csr(CVMX_MIO_FUS_PLL);
		fus_pll.cn63xx.c_cout_rst = 1;
		cvmx_write_csr(CVMX_MIO_FUS_PLL, fus_pll.u64);

		/* Sel::  0:rclk, 1:pllout 2:psout 3:gnd */
		fus_pll.cn63xx.c_cout_sel = 0;
		cvmx_write_csr(CVMX_MIO_FUS_PLL, fus_pll.u64);
		fus_pll.cn63xx.c_cout_rst = 0;
		cvmx_write_csr(CVMX_MIO_FUS_PLL, fus_pll.u64);
	}

	/* Configure the QLM modes.... */
	{
		cvmx_mio_qlmx_cfg_t qlm_cfg;

		qlm_cfg.u64= 0;
		qlm_cfg.s.qlm_spd = 0x3;
		qlm_cfg.s.qlm_cfg = 0x2;
		cvmx_write_csr(CVMX_MIO_QLMX_CFG(0), qlm_cfg.u64);

	}

	memset((void *)&(gd->ogd.mac_desc), 0x0,
	       sizeof(octeon_eeprom_mac_addr_t));
	memset((void *)&(gd->ogd.clock_desc), 0x0,
	       sizeof(octeon_eeprom_clock_desc_t));
	memset((void *)&(gd->ogd.board_desc), 0x0,
	       sizeof(octeon_eeprom_board_desc_t));

	/* NOTE: this is early in the init process, so the serial port is not
	 * yet configured
	 */

	/* Populate global data from eeprom */
	octeon_board_get_clock_info(EVB7100_DEF_DRAM_FREQ);

	octeon_board_get_descriptor(CVMX_BOARD_TYPE_EVB7100, 1, 0);

	/* CNF71XX has a fixed 50 MHz reference clock */
	gd->ogd.ddr_ref_hertz = 50000000;

	/* Even though the CPU ref freq is stored in the clock descriptor, we
	 * don't read it here.  The MCU reads it and configures the clock, and
	 * we read how the clock is actually configured.
	 * The bootloader does not need to read the clock descriptor tuple for
	 * normal operation on rev 2 and later boards.
	 */
	cpu_ref = octeon_mcu_read_cpu_ref();

	/* Some sanity checks */
	if (cpu_ref <= 0) {
		/* Default if cpu reference clock reading fails. */
		cpu_ref = DEFAULT_CPU_REF_FREQUENCY_MHZ;
	}
	if (gd->ogd.ddr_clock_mhz < 100 || gd->ogd.ddr_clock_mhz > 2000) {
		gd->ogd.ddr_clock_mhz = EVB7100_DEF_DRAM_FREQ;
	}

	octeon_board_get_mac_addr();

	/* Read CPU clock multiplier */
	gd->ogd.cpu_clock_mhz = octeon_get_cpu_multiplier() * cpu_ref;

	octeon_led_str_write("Booting.");
	return 0;
}
