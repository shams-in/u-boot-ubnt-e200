/*
 * Copyright (C) Freescale Semiconductor, Inc. 2006.
 * Author: Jason Jin<Jason.jin@freescale.com>
 *         Zhang Wei<wei.zhang@freescale.com>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
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
 *
 * with the reference on libata and ahci drvier in kernel
 *
 */
/* define DEBUG */
#include <common.h>

#include <command.h>
#include <pci.h>
#include <asm/processor.h>
#include <asm/errno.h>
#include <asm/io.h>
#include <asm/bitops.h>
#include <malloc.h>
#include <scsi.h>
#include <ata.h>
#include <linux/ctype.h>
#include <ahci.h>
#include <fis.h>

struct ahci_probe_ent *probe_ent = NULL;
hd_driveid_t *ataid[AHCI_MAX_PORTS];

#define writel_with_flush(a,b)	do { writel(a,b); readl(b); } while (0)

#define virt_to_bus(devno, v)	pci_virt_to_mem(devno, (void *) (v))

static void ahci_sata_dump_fis(struct sata_fis_d2h *s)
{
	printf("---------------------\n");
	printf("Status FIS dump:\n");
	printf("fis_type:		%02x\n", s->fis_type);
	printf("pm_port_i:		%02x\n", s->pm_port_i);
	printf("status:			%02x\n", s->status);
	printf("error:			%02x\n", s->error);
	printf("lba_low:		%02x\n", s->lba_low);
	printf("lba_mid:		%02x\n", s->lba_mid);
	printf("lba_high:		%02x\n", s->lba_high);
	printf("device:			%02x\n", s->device);
	printf("lba_low_exp:		%02x\n", s->lba_low_exp);
	printf("lba_mid_exp:		%02x\n", s->lba_mid_exp);
	printf("lba_high_exp:		%02x\n", s->lba_high_exp);
	printf("res1:			%02x\n", s->res1);
	printf("sector_count:		%02x\n", s->sector_count);
	printf("sector_count_exp:	%02x\n", s->sector_count_exp);
	printf("---------------------\n");
}


static inline u32 ahci_port_base(u32 base, u32 port)
{
	return base + 0x100 + (port * 0x80);
}


static void ahci_setup_port(struct ahci_ioports *port, unsigned long base,
			    unsigned int port_idx)
{
	base = ahci_port_base(base, port_idx);

	port->cmd_addr = base;
	port->cmd_addr_dma = virt_to_bus(probe_ent->dev,
					 (void *)port->cmd_addr);
	port->scr_addr = base + PORT_SCR;
	port->scr_addr_dma = virt_to_bus(probe_ent->dev,
					 (void *)port->scr_addr);
}


#define msleep(a) udelay(a * 1000)
#define ssleep(a) msleep(a * 1000)

static int waiting_for_cmd_completed(volatile u8 *offset,
				     int timeout_msec,
				     u32 sign)
{
	int i;
	u32 status;

	for (i = 0; ((status = readl(offset)) & sign) && i < timeout_msec; i++)
		msleep(1);

	return (i < timeout_msec) ? 0 : -1;
}


static int ahci_host_init(struct ahci_probe_ent *probe_ent)
{
#ifndef CONFIG_SCSI_AHCI_PLAT
	pci_dev_t pdev = probe_ent->dev;
	u16 tmp16;
	unsigned short vendor;
#endif
	volatile u8 *mmio = (volatile u8 *)probe_ent->mmio_base;
	u32 tmp, cap_save;
	int i, j;
	volatile u8 *port_mmio;

	cap_save = readl(mmio + HOST_CAP);
	cap_save &= ((1 << 28) | (1 << 17));
	cap_save |= (1 << 27);

	debug("In ahci_host_init, dev=%d\n", pdev);

	/* global controller reset */
	tmp = readl(mmio + HOST_CTL);
	if ((tmp & HOST_RESET) == 0)
		writel_with_flush(tmp | HOST_RESET, mmio + HOST_CTL);

	/* reset must complete within 1 second, or
	 * the hardware should be considered fried.
	 */
	ssleep(1);

	tmp = readl(mmio + HOST_CTL);
	if (tmp & HOST_RESET) {
		debug("controller reset failed (0x%x)\n", tmp);
		return -1;
	}

	writel_with_flush(HOST_AHCI_EN, mmio + HOST_CTL);
	writel(cap_save, mmio + HOST_CAP);
	writel_with_flush(0xf, mmio + HOST_PORTS_IMPL);

#ifndef CONFIG_SCSI_AHCI_PLAT
	pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor);

	if (vendor == PCI_VENDOR_ID_INTEL) {
		u16 tmp16;
		pci_read_config_word(pdev, 0x92, &tmp16);
		tmp16 |= 0xf;
		pci_write_config_word(pdev, 0x92, tmp16);
	}
#endif
	probe_ent->cap = readl(mmio + HOST_CAP);
	probe_ent->port_map = readl(mmio + HOST_PORTS_IMPL);
	probe_ent->n_ports = (probe_ent->cap & 0x1f) + 1;

	debug("cap 0x%x  port_map 0x%x  n_ports %d\n",
	      probe_ent->cap, probe_ent->port_map, probe_ent->n_ports);

	for (i = 0; i < probe_ent->n_ports; i++) {
		probe_ent->port[i].port_mmio = ahci_port_base((u32) mmio, i);
		port_mmio = (u8 *) probe_ent->port[i].port_mmio;
		ahci_setup_port(&probe_ent->port[i], (unsigned long)mmio, i);

		/* make sure port is not active */
		tmp = readl(port_mmio + PORT_CMD);
		if (tmp & (PORT_CMD_LIST_ON | PORT_CMD_FIS_ON |
			   PORT_CMD_FIS_RX | PORT_CMD_START)) {
			tmp &= ~(PORT_CMD_LIST_ON | PORT_CMD_FIS_ON |
				 PORT_CMD_FIS_RX | PORT_CMD_START);
			writel_with_flush(tmp, port_mmio + PORT_CMD);

			/* spec says 500 msecs for each bit, so
			 * this is slightly incorrect.
			 */
			msleep(1000);
		}

		writel(PORT_CMD_SPIN_UP, port_mmio + PORT_CMD);

		j = 0;
		while (j < 100) {
			msleep(10);
			tmp = readl(port_mmio + PORT_SCR_STAT);
			if ((tmp & 0xf) == 0x3)
				break;
			j++;
		}

		tmp = readl(port_mmio + PORT_SCR_ERR);
		debug("PORT_SCR_ERR 0x%x\n", tmp);
		writel(tmp, port_mmio + PORT_SCR_ERR);

		/* ack any pending irq events for this port */
		tmp = readl(port_mmio + PORT_IRQ_STAT);
		debug("PORT_IRQ_STAT 0x%x\n", tmp);
		if (tmp)
			writel(tmp, port_mmio + PORT_IRQ_STAT);

		writel(1 << i, mmio + HOST_IRQ_STAT);

		/* set irq mask (enables interrupts) */
		writel(DEF_PORT_IRQ, port_mmio + PORT_IRQ_MASK);

		/*register linkup ports */
		tmp = readl(port_mmio + PORT_SCR_STAT);
		debug("Port %d status: 0x%x\n", i, tmp);
		if ((tmp & 0xf) == 0x03)
			probe_ent->link_port_map |= (0x01 << i);
	}

	tmp = readl(mmio + HOST_CTL);
	debug("HOST_CTL 0x%x\n", tmp);
	writel(tmp | HOST_IRQ_EN, mmio + HOST_CTL);
	tmp = readl(mmio + HOST_CTL);
	debug("HOST_CTL 0x%x\n", tmp);
#ifndef CONFIG_SCSI_AHCI_PLAT
	pci_read_config_word(pdev, PCI_COMMAND, &tmp16);
	tmp |= PCI_COMMAND_MASTER;
	pci_write_config_word(pdev, PCI_COMMAND, tmp16);
#endif
	return 0;
}


static void ahci_print_info(struct ahci_probe_ent *probe_ent)
{
#ifndef CONFIG_SCSI_AHCI_PLAT
	pci_dev_t pdev = probe_ent->dev;
	u16 cc;
#endif
	volatile u8 *mmio = (volatile u8 *)probe_ent->mmio_base;
	u32 vers, cap, impl, speed;
	const char *speed_s;
	const char *scc_s;

	vers = readl(mmio + HOST_VERSION);
	cap = probe_ent->cap;
	impl = probe_ent->port_map;

	speed = (cap >> 20) & 0xf;
	if (speed == 1)
		speed_s = "1.5";
	else if (speed == 2)
		speed_s = "3";
	else
		speed_s = "?";

#ifdef CONFIG_SCSI_AHCI_PLAT
	scc_s = "SATA";
#else
	pci_read_config_word(pdev, 0x0a, &cc);
	if (cc == 0x0101)
		scc_s = "IDE";
	else if (cc == 0x0106)
		scc_s = "SATA";
	else if (cc == 0x0104)
		scc_s = "RAID";
	else if (cc == 0x0180)
		scc_s = "Other";
	else
		scc_s = "unknown";
#endif
	printf("AHCI %02x%02x.%02x%02x "
	       "%u slots %u ports %s Gbps 0x%x impl %s mode\n",
	       (vers >> 24) & 0xff,
	       (vers >> 16) & 0xff,
	       (vers >> 8) & 0xff,
	       vers & 0xff,
	       ((cap >> 8) & 0x1f) + 1, (cap & 0x1f) + 1, speed_s, impl, scc_s);

	printf("flags: "
	       "%s%s%s%s%s%s"
	       "%s%s%s%s%s%s%s\n",
	       cap & (1 << 31) ? "64bit " : "",
	       cap & (1 << 30) ? "ncq " : "",
	       cap & (1 << 28) ? "ilck " : "",
	       cap & (1 << 27) ? "stag " : "",
	       cap & (1 << 26) ? "pm " : "",
	       cap & (1 << 25) ? "led " : "",
	       cap & (1 << 24) ? "clo " : "",
	       cap & (1 << 19) ? "nz " : "",
	       cap & (1 << 18) ? "only " : "",
	       cap & (1 << 17) ? "pmp " : "",
	       cap & (1 << 15) ? "pio " : "",
	       cap & (1 << 14) ? "slum " : "",
	       cap & (1 << 13) ? "part " : "");
}

#ifndef CONFIG_SCSI_AHCI_PLAT
static int ahci_init_one(pci_dev_t pdev)
{
	u16 vendor;
	int rc;

	memset((void *)ataid, 0, sizeof(hd_driveid_t *) * AHCI_MAX_PORTS);

	probe_ent = malloc(sizeof(struct ahci_probe_ent));
	memset(probe_ent, 0, sizeof(struct ahci_probe_ent));
	probe_ent->dev = pdev;

	probe_ent->host_flags = ATA_FLAG_SATA
				| ATA_FLAG_NO_LEGACY
				| ATA_FLAG_MMIO
				| ATA_FLAG_PIO_DMA
				| ATA_FLAG_NO_ATAPI;
	probe_ent->pio_mask = 0x1f;
	probe_ent->udma_mask = 0x7f;	/*Fixme,assume to support UDMA6 */

	probe_ent->mmio_base = (u32)pci_map_bar(pdev, AHCI_PCI_BAR,
						PCI_REGION_MEM);

	/* Take from kernel:
	 * JMicron-specific fixup:
	 * make sure we're in AHCI mode
	 */
	pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor);
	if (vendor == 0x197b) {
		u32 conf1, conf5, class;
		u16 id;
		u8 hdr;

		pci_write_config_byte(pdev, 0x41, 0xa1);
		conf1 = pci_read_config_dword(pdev, 0x40, &conf1);
		conf5 = pci_read_config_dword(pdev, 0x80, &conf5);
		conf1 &= ~0x00CFF302;	/* Clear bit 1, 8, 9, 12-19, 22, 23 */
		conf5 &= ~(1 << 24);	/* Clear bit 24 */
		pci_read_config_word(pdev, PCI_DEVICE_ID, &id);
		switch (id) {
		case 0x2360:	/* Single SATA port */
		case 0x2362:	/* Dual SATA port */
			/* The controller should be in single function ahci mode */
			conf1 |= 0x002A100;	/* Set 8, 13, 15, 17 */
			break;
		case 0x2361:
		case 0x2363:
			/* Redirect IDE second PATA port to the right spot */
			conf1 |= 0x00C2A1B3;	/* Set 0, 1, 4, 5, 7, 8, 13, 15, 17, 22, 23 */
			break;
		}
		pci_write_config_dword(pdev, 0x40, conf1);
		pci_write_config_dword(pdev, 0x80, conf5);
	}

	/* initialize adapter */
	rc = ahci_host_init(probe_ent);
	if (rc)
		goto err_out;

	ahci_print_info(probe_ent);

	return 0;

      err_out:
	return rc;
}
#endif

#define MAX_DATA_BYTE_COUNT  (4*1024*1024)

static int ahci_fill_sg(u8 port, unsigned char *buf, int buf_len)
{
	struct ahci_ioports *pp = &(probe_ent->port[port]);
	struct ahci_sg *ahci_sg = pp->cmd_tbl_sg;
	u32 sg_count;
	int i;
	phys_addr_t paddr;

	sg_count = ((buf_len - 1) / MAX_DATA_BYTE_COUNT) + 1;
	if (sg_count > AHCI_MAX_SG) {
		printf("Error:Too much sg!\n");
		return -1;
	}

	for (i = 0; i < sg_count; i++) {
		paddr = virt_to_bus(probe_ent->dev,
				    ((u8 *)buf + i * MAX_DATA_BYTE_COUNT));
		ahci_sg->addr =
		    cpu_to_le32((u32)paddr);
		ahci_sg->addr_hi = cpu_to_le32((paddr >> 16) >> 16);
		ahci_sg->reserved = 0;
		ahci_sg->flags_size = cpu_to_le32(0x3fffff &
					  (buf_len < MAX_DATA_BYTE_COUNT
					   ? (buf_len - 1)
					   : (MAX_DATA_BYTE_COUNT - 1)));
		debug("ahci_sg at 0x%p\n", ahci_sg);
		debug("ahci_sg addr=0x%08x, addr_hi=0x%08x, flags/size=0x%08x\n",
		      ahci_sg->addr, ahci_sg->addr_hi, ahci_sg->flags_size);
		ahci_sg++;
		buf_len -= MAX_DATA_BYTE_COUNT;
	}

	return sg_count;
}


static void ahci_fill_cmd_slot(struct ahci_ioports *pp, u32 opts)
{
	phys_addr_t paddr = pp->cmd_tbl_dma;
	pp->cmd_slot->opts = cpu_to_le32(opts);
	pp->cmd_slot->status = 0;
	pp->cmd_slot->tbl_addr = cpu_to_le32((u32)paddr);
	pp->cmd_slot->tbl_addr_hi = cpu_to_le32((u32)((paddr >> 16) >> 16));
	debug("%s: pp at 0x%llx (0x%p), opts=0x%x\n",
	      __func__, pp->cmd_tbl_dma, pp->cmd_slot, opts);
}


static int ahci_set_feature(u8 port)
{
	struct ahci_ioports *pp = &(probe_ent->port[port]);
	volatile u8 *port_mmio = (volatile u8 *)pp->port_mmio;
	u32 cmd_fis_len = 5;	/* five dwords */
	u8 fis[20];

	/*set feature */
	memset(fis, 0, 20);
	fis[0] = 0x27;
	fis[1] = 1 << 7;
	fis[2] = ATA_CMD_SETF;
	fis[3] = SETFEATURES_XFER;
	switch (probe_ent->udma_mask) {
	case ATA_UDMA7:
		fis[12] = XFER_UDMA_7;
		break;
	case ATA_UDMA6:
		fis[12] = XFER_UDMA_6;
		break;
	case ATA_UDMA5:
		fis[12] = XFER_UDMA_5;
		break;
	case ATA_UDMA4:
		fis[12] = XFER_UDMA_4;
		break;
	case ATA_UDMA3:
		fis[12] = XFER_UDMA_3;
		break;
	}

	memcpy((unsigned char *)pp->cmd_tbl, fis, 20);
	ahci_fill_cmd_slot(pp, cmd_fis_len);
	writel(1, port_mmio + PORT_CMD_ISSUE);
	readl(port_mmio + PORT_CMD_ISSUE);

	if (waiting_for_cmd_completed(port_mmio + PORT_CMD_ISSUE, 150, 0x1)) {
		printf("set feature error (port %d)!\n", port);
		debug("port interrupt status: 0x%x, cmd issue: 0x%x\n",
		      readl(port_mmio + PORT_IRQ_STAT),
		      readl(port_mmio + PORT_CMD_ISSUE));
		debug("phy error: 0x%x\n",
		      readl(port_mmio + PORT_SCR_ERR));
		debug("udma mask: 0x%x\n", probe_ent->udma_mask);
		ahci_sata_dump_fis((struct sata_fis_d2h *)pp->cmd_tbl);
		return -1;
	}
	return 0;
}


static int ahci_port_start(u8 port)
{
	struct ahci_ioports *pp = &(probe_ent->port[port]);
	volatile u8 *port_mmio = (volatile u8 *)pp->port_mmio;
	u32 port_status;
	u32 mem;

	debug("Enter start port: %d\n", port);
	port_status = readl(port_mmio + PORT_SCR_STAT);
	debug("Port %d status: %x\n", port, port_status);
	if ((port_status & 0xf) != 0x03) {
		printf("No Link on this port!\n");
		return -1;
	}

	mem = (u32) memalign(2048, AHCI_PORT_PRIV_DMA_SZ);
	if (!mem) {
		free(pp);
		printf("No mem for table!\n");
		return -ENOMEM;
	}

	memset((u8 *) mem, 0, AHCI_PORT_PRIV_DMA_SZ);

	/*
	 * First item in chunk of DMA memory: 32-slot command table,
	 * 32 bytes each in size
	 */
	pp->cmd_slot = (struct ahci_cmd_hdr *)mem;
	pp->cmd_slot_dma = virt_to_bus(probe_ent->dev, pp->cmd_slot);
	debug("cmd_slot = 0x%llx (0x%p)\n", pp->cmd_slot_dma, pp->cmd_slot);
	mem += (AHCI_CMD_SLOT_SZ + 224);

	/*
	 * Second item: Received-FIS area
	 */
	pp->rx_fis = mem;
	pp->rx_fis_dma = virt_to_bus(probe_ent->dev, pp->rx_fis);
	mem += AHCI_RX_FIS_SZ;

	/*
	 * Third item: data area for storing a single command
	 * and its scatter-gather table
	 */
	pp->cmd_tbl = mem;
	pp->cmd_tbl_dma = virt_to_bus(probe_ent->dev, pp->cmd_tbl);
	debug("cmd_tbl_dma = 0x%llx (0x%x)\n", pp->cmd_tbl_dma, pp->cmd_tbl);

	mem += AHCI_CMD_TBL_HDR;
	pp->cmd_tbl_sg = (struct ahci_sg *)mem;

	writel((u32) pp->cmd_slot_dma, port_mmio + PORT_LST_ADDR);
	writel_with_flush((u32) ((pp->cmd_slot_dma >> 16) >> 16),
			  port_mmio + PORT_LST_ADDR_HI);

	writel((u32) pp->rx_fis_dma, port_mmio + PORT_FIS_ADDR);
	writel_with_flush((u32) ((pp->rx_fis_dma >> 16) >> 16),
			  port_mmio + PORT_FIS_ADDR_HI);

	writel_with_flush(PORT_CMD_ICC_ACTIVE | PORT_CMD_FIS_RX |
			  PORT_CMD_POWER_ON | PORT_CMD_SPIN_UP |
			  PORT_CMD_START, port_mmio + PORT_CMD);

	debug("Exit start port %d\n", port);

	return 0;
}


static int get_ahci_device_data(u8 port, u8 *fis, int fis_len, u8 *buf,
				int buf_len)
{

	struct ahci_ioports *pp = &(probe_ent->port[port]);
	volatile u8 *port_mmio = (volatile u8 *)pp->port_mmio;
	u32 opts;
	u32 port_status;
	int sg_count;

	debug("Enter get_ahci_device_data: for port %d\n", port);
	debug("buf at 0x%p, length 0x%x\n", buf, buf_len);

	if (port > probe_ent->n_ports) {
		debug("Invaild port number %d\n", port);
		return -1;
	}

	port_status = readl(port_mmio + PORT_SCR_STAT);
	if ((port_status & 0xf) != 0x03) {
		debug("No Link on port %d!\n", port);
		return -1;
	}

	memcpy((unsigned char *)pp->cmd_tbl, fis, fis_len);

	sg_count = ahci_fill_sg(port, buf, buf_len);
	opts = (fis_len >> 2) | (sg_count << 16);
	ahci_fill_cmd_slot(pp, opts);

	writel_with_flush(1, port_mmio + PORT_CMD_ISSUE);

	if (waiting_for_cmd_completed(port_mmio + PORT_CMD_ISSUE, 150, 0x1)) {
		printf("timeout exit!\n");
		return -1;
	}
	debug("get_ahci_device_data: 0x%x byte transferred.\n",
	      le32_to_cpu(pp->cmd_slot->status));

	return 0;
}


static char *ata_id_strcpy(u16 *target, u16 *src, int len)
{
	int i;
	for (i = 0; i < len / 2; i++)
		target[i] = swab16(src[i]);
	return (char *)target;
}


static void dump_ataid(hd_driveid_t *ataid)
{
	u8 product[ATA_ID_PROD_LEN+1];
	u8 revision[ATA_ID_FW_REV_LEN+1];
	u8 serial[ATA_ID_SERNO_LEN + 1];
	u8 firmware[ATA_ID_FW_REV_LEN + 1];
	debug("(49)ataid->capability = 0x%x\n", ataid->capability);
	debug("(53)ataid->field_valid =0x%x\n", ataid->field_valid);
	debug("(63)ataid->dma_mword = 0x%x\n", ataid->dma_mword);
	debug("(64)ataid->eide_pio_modes = 0x%x\n", ataid->eide_pio_modes);
	debug("(75)ataid->queue_depth = 0x%x\n", ataid->queue_depth);
	debug("(80)ataid->major_rev_num = 0x%x\n", ataid->major_rev_num);
	debug("(81)ataid->minor_rev_num = 0x%x\n", ataid->minor_rev_num);
	debug("(82)ataid->command_set_1 = 0x%x\n", ataid->command_set_1);
	debug("(83)ataid->command_set_2 = 0x%x\n", ataid->command_set_2);
	debug("(84)ataid->cfsse = 0x%x\n", ataid->cfsse);
	debug("(85)ataid->cfs_enable_1 = 0x%x\n", ataid->cfs_enable_1);
	debug("(86)ataid->cfs_enable_2 = 0x%x\n", ataid->cfs_enable_2);
	debug("(87)ataid->csf_default = 0x%x\n", ataid->csf_default);
	debug("(88)ataid->dma_ultra = 0x%x\n", ataid->dma_ultra);
	debug("(93)ataid->hw_config = 0x%x\n", ataid->hw_config);
	debug("(%d)ataid->capacity = 0x%x\n", ATA_ID_LBA_SECTORS,
	      ata_id_u32((u16 *)ataid, ATA_ID_LBA_SECTORS));
	debug("(%d)ataid->capacity_lba48 = 0x%x\n", ATA_ID_LBA48_SECTORS,
	      ata_id_u32((u16 *)ataid, ATA_ID_LBA48_SECTORS));
	ata_id_c_string((u16 *)ataid, serial, ATA_ID_SERNO, sizeof(serial));
	debug("(%d) serial = %s\n", ATA_ID_SERNO, serial);
	ata_id_c_string((u16 *)ataid, firmware, ATA_ID_FW_REV, sizeof(firmware));
	debug("(%d) firmware = %s\n", ATA_ID_FW_REV, firmware);
	ata_id_c_string((u16 *)ataid, product, ATA_ID_PROD, sizeof(product));
	debug("(%d) product = %s\n", ATA_ID_PROD, product);
	ata_id_c_string((u16 *)ataid, revision, ATA_ID_FW_REV, sizeof(revision));
	debug("(%d) revision = %s\n", ATA_ID_FW_REV, revision);
}


/*
 * SCSI INQUIRY command operation.
 */
static int ata_scsiop_inquiry(ccb *pccb)
{
	u8 hdr[] = {
		0,
		0,
		0x5,		/* claim SPC-3 version compatibility */
		2,
		95 - 4,
	};
	u8 fis[20];
	u8 *tmpid;
	u8 port;

	/* Clean ccb data buffer */
	memset(pccb->pdata, 0, pccb->datalen);

	memcpy(pccb->pdata, hdr, sizeof(hdr));

	if (pccb->datalen <= 35)
		return 0;

	memset(fis, 0, 20);
	/* Construct the FIS */
	fis[0] = 0x27;		/* Host to device FIS. */
	fis[1] = 1 << 7;	/* Command FIS. */
	fis[2] = ATA_CMD_IDENT;	/* Command byte. */

	/* Read id from sata */
	port = pccb->target;
	if (!(tmpid = malloc(sizeof(hd_driveid_t))))
		return -ENOMEM;
	memset(tmpid, 0, sizeof(hd_driveid_t));

	if (get_ahci_device_data(port, (u8 *) & fis, 20,
				 tmpid, sizeof(hd_driveid_t))) {
		debug("scsi_ahci: SCSI inquiry command failure.\n");
		return -EIO;
	}

	if (ataid[port])
		free(ataid[port]);
	ataid[port] = (hd_driveid_t *) tmpid;

	memcpy(&pccb->pdata[8], "ATA     ", 8);
#if 0
	ata_id_strcpy((u16 *) &pccb->pdata[16], (u16 *)ataid[port]->model, 16);
	ata_id_strcpy((u16 *) &pccb->pdata[32], (u16 *)ataid[port]->fw_rev, 4);
#endif
	ata_swap_buf_le16((u16 *)ataid[port], ATA_ID_WORDS);
	memcpy(&pccb->pdata[16], ataid[port]->model, 16);
	memcpy(&pccb->pdata[32], ataid[port]->fw_rev, 4);
	dump_ataid(ataid[port]);
	return 0;
}


/*
 * SCSI READ10 command operation.
 */
static int ata_scsiop_read10(ccb * pccb)
{
	u64 lba = 0;
	u32 len = 0;
	u8 fis[20];

	lba = (((u64) pccb->cmd[2]) << 24) | (((u64) pccb->cmd[3]) << 16)
	    | (((u64) pccb->cmd[4]) << 8) | ((u64) pccb->cmd[5]);
	len = (((u32) pccb->cmd[7]) << 8) | ((u32) pccb->cmd[8]);

	/* For 10-byte and 16-byte SCSI R/W commands, transfer
	 * length 0 means transfer 0 block of data.
	 * However, for ATA R/W commands, sector count 0 means
	 * 256 or 65536 sectors, not 0 sectors as in SCSI.
	 *
	 * WARNING: one or two older ATA drives treat 0 as 0...
	 */
	if (!len)
		return 0;
	memset(fis, 0, 20);

	/* Construct the FIS */
	fis[0] = 0x27;		/* Host to device FIS. */
	fis[1] = 1 << 7;	/* Command FIS. */
	fis[2] = ATA_CMD_RD_DMA;	/* Command byte. */

	/* LBA address, only support LBA28 in this driver */
	fis[4] = pccb->cmd[5];
	fis[5] = pccb->cmd[4];
	fis[6] = pccb->cmd[3];
	fis[7] = (pccb->cmd[2] & 0x0f) | 0xe0;

	/* Sector Count */
	fis[12] = pccb->cmd[8];
	fis[13] = pccb->cmd[7];

	/* Read from ahci */
	if (get_ahci_device_data(pccb->target, (u8 *) & fis, 20,
				 pccb->pdata, pccb->datalen)) {
		debug("scsi_ahci: SCSI READ10 command failure.\n");
		return -EIO;
	}

	return 0;
}


/*
 * SCSI READ CAPACITY10 command operation.
 */
static int ata_scsiop_read_capacity10(ccb *pccb)
{
	u64 cap;

	if (!ataid[pccb->target]) {
		printf("scsi_ahci: SCSI READ CAPACITY10 command failure. "
		       "\tNo ATA info!\n"
		       "\tPlease run SCSI commmand INQUIRY firstly!\n");
		return -EPERM;
	}

	cap = ata_id_n_sectors((u16 *)ataid[pccb->target]);
	pccb->pdata[0] = (cap >> 24) & 0xff;
	pccb->pdata[1] = (cap >> 16) & 0xff;
	pccb->pdata[2] = (cap >> 8) & 0xff;
	pccb->pdata[3] = cap & 0xff;
	
	pccb->pdata[4] = pccb->pdata[5] = 0;
	pccb->pdata[6] = 512 >> 8;
	pccb->pdata[7] = 512 & 0xff;
	debug("AHCI read capacity: %u sectors, sector size %u bytes\n",
	      cap, 512);

	return 0;
}

/*
 * SCSI TEST UNIT READY command operation.
 */
static int ata_scsiop_test_unit_ready(ccb *pccb)
{
	return (ataid[pccb->target]) ? 0 : -EPERM;
}


int scsi_exec(ccb *pccb)
{
	int ret;

	switch (pccb->cmd[0]) {
	case SCSI_READ10:
		ret = ata_scsiop_read10(pccb);
		break;
	case SCSI_RD_CAPAC:
		ret = ata_scsiop_read_capacity10(pccb);
		break;
	case SCSI_TST_U_RDY:
		ret = ata_scsiop_test_unit_ready(pccb);
		break;
	case SCSI_INQUIRY:
		ret = ata_scsiop_inquiry(pccb);
		break;
	default:
		printf("Unsupport SCSI command 0x%02x\n", pccb->cmd[0]);
		return FALSE;
	}

	if (ret) {
		debug("SCSI command 0x%02x ret errno %d\n", pccb->cmd[0], ret);
		return FALSE;
	}
	return TRUE;

}


void scsi_low_level_init(int busdevfunc)
{
	int i;
	u32 linkmap;

#ifndef CONFIG_SCSI_AHCI_PLAT
	ahci_init_one(busdevfunc);
#endif

	linkmap = probe_ent->link_port_map;

	for (i = 0; i < CONFIG_SYS_SCSI_MAX_SCSI_ID; i++) {
		if (((linkmap >> i) & 0x01)) {
			if (ahci_port_start((u8) i)) {
				printf("Can not start port %d\n", i);
				continue;
			}
			ahci_set_feature((u8) i);
		}
	}
}

#ifdef CONFIG_SCSI_AHCI_PLAT
int ahci_init(u32 base)
{
	int i, rc = 0;
	u32 linkmap;

	memset(ataid, 0, sizeof(ataid));

	probe_ent = malloc(sizeof(struct ahci_probe_ent));
	memset(probe_ent, 0, sizeof(struct ahci_probe_ent));

	probe_ent->host_flags = ATA_FLAG_SATA
				| ATA_FLAG_NO_LEGACY
				| ATA_FLAG_MMIO
				| ATA_FLAG_PIO_DMA
				| ATA_FLAG_NO_ATAPI;
	probe_ent->pio_mask = 0x1f;
	probe_ent->udma_mask = 0x7f;	/*Fixme,assume to support UDMA6 */

	probe_ent->mmio_base = base;

	/* initialize adapter */
	rc = ahci_host_init(probe_ent);
	if (rc)
		goto err_out;

	ahci_print_info(probe_ent);

	linkmap = probe_ent->link_port_map;

	for (i = 0; i < CONFIG_SYS_SCSI_MAX_SCSI_ID; i++) {
		if (((linkmap >> i) & 0x01)) {
			if (ahci_port_start((u8) i)) {
				printf("Can not start port %d\n", i);
				continue;
			}
			ahci_set_feature((u8) i);
		}
	}
err_out:
	return rc;
}
#endif

void scsi_bus_reset(void)
{
	/*Not implement*/
}


void scsi_print_error(ccb * pccb)
{
	/*The ahci error info can be read in the ahci driver*/
}
