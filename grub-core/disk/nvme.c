/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2007, 2008, 2009, 2010  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/dl.h>
#include <grub/disk.h>
#include <grub/mm.h>
#include <grub/time.h>
#include <grub/pci.h>
#include <grub/misc.h>
#include <grub/list.h>
#include <grub/loader.h>

GRUB_MOD_LICENSE ("GPLv3+");

#define NVME_CC_EN	(1 <<  0)
#define NVME_CC_CSS	(0 <<  4)
#define NVME_CC_MPS	(0 <<  7)
#define NVME_CC_AMS	(0 << 11)
#define NVME_CC_SHN	(0 << 14)
#define NVME_CC_IOSQES	(6 << 16)
#define NVME_CC_IOCQES	(4 << 20)

struct grub_nvme_mmio_reg
{
  /*  0 */ grub_uint64_t cap;
  /*  8 */ grub_uint32_t fill1[3];
  /* 14 */ grub_uint32_t controller_config;
  /* 18 */ grub_uint32_t fill2;
  /* 1c */ grub_uint32_t controller_status;
};

struct grub_nvme_device
{
  struct grub_nvme_device *next;
  struct grub_nvme_device **prev;
  volatile struct grub_nvme_mmio_reg *regs;
  struct grub_pci_dma_chunk *prp_list_chunk;
};

static struct grub_nvme_device *grub_nvme_device;
static int numdevs;

static int
grub_nvme_pciinit (grub_pci_device_t dev,
		   grub_pci_id_t pciid __attribute__ ((unused)),
		   void *data __attribute__ ((unused)))
{
  grub_pci_address_t addr;
  grub_uint32_t class;
  grub_uint64_t bar;
  unsigned i, nports;
  volatile volatile struct grub_nvme_mmio_reg *mmio_reg;

  /* Read class.  */
  addr = grub_pci_make_address (dev, GRUB_PCI_REG_CLASS);
  class = grub_pci_read (addr);

  /* Check if this class ID matches that of a PCI NVMe Controller.  */
  if (class >> 8 != 0x010802)
    return 0;

  addr = grub_pci_make_address (dev, GRUB_PCI_REG_ADDRESS_REG0);

  bar = grub_pci_read (addr);

  if ((bar & (GRUB_PCI_ADDR_SPACE_MASK | GRUB_PCI_ADDR_MEM_TYPE_MASK
	      | GRUB_PCI_ADDR_MEM_PREFETCH))
      != (GRUB_PCI_ADDR_SPACE_MEMORY | GRUB_PCI_ADDR_MEM_TYPE_64))
    return 0;

  addr = grub_pci_make_address (dev, GRUB_PCI_REG_ADDRESS_REG1);

  bar |= ((grub_uint64_t) grub_pci_read (addr)) << 32;

  addr = grub_pci_make_address (dev, GRUB_PCI_REG_COMMAND);
  grub_pci_write_word (addr, grub_pci_read_word (addr)
		    | GRUB_PCI_COMMAND_MEM_ENABLED);

  mmio_reg = grub_pci_device_map_range (dev, bar & ~0xfULL,
				   sizeof (*mmio_reg));
  grub_dprintf ("nvme", "dev: %x:%x.%x\n", dev.bus, dev.device, dev.function);

  if (!(mmio_reg->cap & (1LL << 37))) {
    grub_dprintf ("nvme", "nvme command set not supported\n");
    return 0;
  }

  mmio_reg->controller_config = 0;
  grub_int32_t max_timeout_ms = ((mmio_reg->cap >> 24) & 0xff) * 500;
  grub_int32_t timeout_ms = max_timeout_ms;
  while (1)
    {
      grub_uint8_t status = mmio_reg->controller_status & 0x3;
      if (status == 0x2)
	{
	  grub_dprintf("nvme", "NVMe ERROR: Failed to disable controller. FATAL ERROR\n");
	  return 0;
	}
      if (status == 0)
	break;
      if (timeout_ms < 0)
	{
	  grub_dprintf("nvme", "NVMe ERROR: Failed to disable controller. Timeout.\n");
	  return 0;
	}
      timeout_ms -= 10;
      grub_millisleep(10);
    }

  timeout_ms = max_timeout_ms;
  if (create_admin_queues(nvme))
    goto _free_abort;

  mmio_reg->controller_config = NVME_CC_EN | NVME_CC_CSS | NVME_CC_MPS | NVME_CC_AMS | NVME_CC_SHN
    | NVME_CC_IOSQES | NVME_CC_IOCQES;
  while (1)
    {
      status = mmio_reg->controller_status & 0x3;
      if (status == 0x2) {
	grub_dprintf("nvme", "NVMe ERROR: Failed to enable controller. FATAL ERROR\n");
	mmio_reg->controller_config = 0;
	return 0;
      }
      if (status == 1)
	break;
      if (timeout_ms < 0) {
	grub_dprintf("nvme", "NVMe ERROR: Failed to enable controller. Timeout.\n");
	mmio_reg->controller_config = 0;
	return 0;
      }
      timeout_ms -= 10;
      grub_millisleep(10);
    }

  struct grub_nvme_device *nvmedev = grub_malloc (sizeof (*nvmedev));
  if (!nvmedev) {
    mmio_reg->controller_config = 0;
    return 0;
  }

  nvmedev->prp_list_chunk = grub_memalign_dma32(0x1000, 0x1000);
  if (!nvmedev->prp_list_chunk) {
    mmio_reg->controller_config = 0;
    grub_free (nvmedev);
    return 0;
  }

  nvmedev->regs = mmio_reg;

  addr = grub_pci_make_address (dev, GRUB_PCI_REG_COMMAND);
  grub_pci_write_word (addr, grub_pci_read_word (addr) | GRUB_PCI_COMMAND_BUS_MASTER);
  
  grub_list_push (GRUB_AS_LIST_P (&grub_nvme_devices),
		  GRUB_AS_LIST (nvmedev));
  return 0;
}

static grub_err_t
grub_nvme_initialize (void)
{
  grub_pci_iterate (grub_nvme_pciinit, NULL);
  return grub_errno;
}

static grub_err_t
grub_nvme_fini_hw (int noreturn __attribute__ ((unused)))
{
  struct grub_nvme_device *dev;

  for (dev = grub_nvme_devices; dev; dev = dev->next)
    {
      dev->regs->controller_config = 0;
      grub_dma_free (dev->prp_list_chunk);
      /* TODO: wait for completition.  */
    }
  return GRUB_ERR_NONE;
}


static grub_err_t
grub_nvme_restore_hw (void)
{
  struct grub_nvme_device **pdev;

  for (pdev = &grub_nvme_devices; *pdev; pdev = &((*pdev)->next))
    {
      (*pdev)->prp_list_chunk = grub_memalign_dma32(0x1000, 0x1000);
      (*pdev)->regs->controller_config = NVME_CC_EN | NVME_CC_CSS | NVME_CC_MPS | NVME_CC_AMS | NVME_CC_SHN
	| NVME_CC_IOSQES | NVME_CC_IOCQES;
      /* TODO: Error handling.  */
    }
  return GRUB_ERR_NONE;
}




static int
grub_ahci_iterate (grub_ata_dev_iterate_hook_t hook, void *hook_data,
		   grub_disk_pull_t pull)
{
  struct grub_ahci_device *dev;

  if (pull != GRUB_DISK_PULL_NONE)
    return 0;

  FOR_LIST_ELEMENTS(dev, grub_ahci_devices)
    if (hook (GRUB_SCSI_SUBSYSTEM_AHCI, dev->num, hook_data))
      return 1;

  return 0;
}

#if 0
static int
find_free_cmd_slot (struct grub_ahci_device *dev)
{
  int i;
  for (i = 0; i < 32; i++)
    {
      if (dev->hda->ports[dev->port].command_issue & (1 << i))
	continue;
      if (dev->hda->ports[dev->port].sata_active & (1 << i))
	continue;
      return i;
    }
  return -1;
}
#endif

enum
  {
    GRUB_AHCI_FIS_REG_H2D = 0x27
  };

static const int register_map[11] = { 3 /* Features */,
				      12 /* Sectors */,
				      4 /* LBA low */,
				      5 /* LBA mid */,
				      6 /* LBA high */,
				      7 /* Device */,
				      2 /* CMD register */,
				      13 /* Sectors 48  */,
				      8 /* LBA48 low */,
				      9 /* LBA48 mid */,
				      10 /* LBA48 high */ };

static grub_err_t
grub_ahci_reset_port (struct grub_ahci_device *dev, int force)
{
  grub_uint64_t endtime;

  dev->hba->ports[dev->port].sata_error = dev->hba->ports[dev->port].sata_error;

  if (force || (dev->hba->ports[dev->port].command_issue & 1)
      || (dev->hba->ports[dev->port].task_file_data & 0x80))
    {
      struct grub_disk_ata_pass_through_parms parms2;
      dev->hba->ports[dev->port].command &= ~GRUB_AHCI_HBA_PORT_CMD_ST;
      dev->hba->ports[dev->port].command_issue = 0;
      dev->command_list[0].config = 0;
      dev->command_table[0].prdt[0].unused = 0;
      dev->command_table[0].prdt[0].size = 0;
      dev->command_table[0].prdt[0].data_base = 0;

      endtime = grub_get_time_ms () + 1000;
      while ((dev->hba->ports[dev->port].command & GRUB_AHCI_HBA_PORT_CMD_CR))
	if (grub_get_time_ms () > endtime)
	  {
	    grub_dprintf ("ahci", "couldn't stop CR");
	    return grub_error (GRUB_ERR_IO, "couldn't stop CR");
	  }
      dev->hba->ports[dev->port].command |= 8;
      while (dev->hba->ports[dev->port].command & 8)
	if (grub_get_time_ms () > endtime)
	  {
	    grub_dprintf ("ahci", "couldn't set CLO\n");
	    dev->hba->ports[dev->port].command &= ~GRUB_AHCI_HBA_PORT_CMD_FRE;
	    return grub_error (GRUB_ERR_IO, "couldn't set CLO");
	  }

      dev->hba->ports[dev->port].command |= GRUB_AHCI_HBA_PORT_CMD_ST;
      while (!(dev->hba->ports[dev->port].command & GRUB_AHCI_HBA_PORT_CMD_CR))
	if (grub_get_time_ms () > endtime)
	  {
	    grub_dprintf ("ahci", "couldn't stop CR");
	    dev->hba->ports[dev->port].command &= ~GRUB_AHCI_HBA_PORT_CMD_ST;
	    return grub_error (GRUB_ERR_IO, "couldn't stop CR");
	  }
      dev->hba->ports[dev->port].sata_error = dev->hba->ports[dev->port].sata_error;
      grub_memset (&parms2, 0, sizeof (parms2));
      parms2.taskfile.cmd = 8;
      return grub_ahci_readwrite_real (dev, &parms2, 1, 1);
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_ahci_readwrite_real (struct grub_ahci_device *dev,
			  struct grub_disk_ata_pass_through_parms *parms,
			  int spinup, int reset)
{
  struct grub_pci_dma_chunk *bufc;
  grub_uint64_t endtime;
  unsigned i;
  grub_err_t err = GRUB_ERR_NONE;

  grub_dprintf ("ahci", "AHCI tfd = %x\n",
		dev->hba->ports[dev->port].task_file_data);

  if (!reset)
    grub_ahci_reset_port (dev, 0);

  grub_dprintf ("ahci", "AHCI tfd = %x\n",
		dev->hba->ports[dev->port].task_file_data);
  dev->hba->ports[dev->port].task_file_data = 0;
  dev->hba->ports[dev->port].command_issue = 0;
  grub_dprintf ("ahci", "AHCI tfd = %x\n",
		dev->hba->ports[dev->port].task_file_data);

  dev->hba->ports[dev->port].sata_error = dev->hba->ports[dev->port].sata_error;

  grub_dprintf("ahci", "grub_ahci_read (size=%llu, cmdsize = %llu)\n",
	       (unsigned long long) parms->size,
	       (unsigned long long) parms->cmdsize);

  if (parms->cmdsize != 0 && parms->cmdsize != 12 && parms->cmdsize != 16)
    return grub_error (GRUB_ERR_BUG, "incorrect ATAPI command size");

  if (parms->size > GRUB_AHCI_PRDT_MAX_CHUNK_LENGTH)
    return grub_error (GRUB_ERR_BUG, "too big data buffer");

  if (parms->size)
    bufc = grub_memalign_dma32 (1024, parms->size + (parms->size & 1));
  else
    bufc = grub_memalign_dma32 (1024, 512);

  grub_dprintf ("ahci", "AHCI tfd = %x, CL=%p\n",
		dev->hba->ports[dev->port].task_file_data,
		dev->command_list);
  /* FIXME: support port multipliers.  */
  dev->command_list[0].config
    = (5 << GRUB_AHCI_CONFIG_CFIS_LENGTH_SHIFT)
    //    | GRUB_AHCI_CONFIG_CLEAR_R_OK
    | (0 << GRUB_AHCI_CONFIG_PMP_SHIFT)
    | ((parms->size ? 1 : 0) << GRUB_AHCI_CONFIG_PRDT_LENGTH_SHIFT)
    | (parms->cmdsize ? GRUB_AHCI_CONFIG_ATAPI : 0)
    | (parms->write ? GRUB_AHCI_CONFIG_WRITE : GRUB_AHCI_CONFIG_READ)
    | (parms->taskfile.cmd == 8 ? (1 << 8) : 0);
  grub_dprintf ("ahci", "AHCI tfd = %x\n",
		dev->hba->ports[dev->port].task_file_data);

  dev->command_list[0].transferred = 0;
  dev->command_list[0].command_table_base
    = grub_dma_get_phys (dev->command_table_chunk);

  grub_memset ((char *) dev->command_list[0].unused, 0,
	       sizeof (dev->command_list[0].unused));

  grub_memset ((char *) &dev->command_table[0], 0,
	       sizeof (dev->command_table[0]));
  grub_dprintf ("ahci", "AHCI tfd = %x\n",
		dev->hba->ports[dev->port].task_file_data);

  if (parms->cmdsize)
    grub_memcpy ((char *) dev->command_table[0].command, parms->cmd,
		 parms->cmdsize);

  grub_dprintf ("ahci", "AHCI tfd = %x\n",
		dev->hba->ports[dev->port].task_file_data);

  dev->command_table[0].cfis[0] = GRUB_AHCI_FIS_REG_H2D;
  dev->command_table[0].cfis[1] = 0x80;
  for (i = 0; i < sizeof (parms->taskfile.raw); i++)
    dev->command_table[0].cfis[register_map[i]] = parms->taskfile.raw[i];

  grub_dprintf ("ahci", "cfis: %02x %02x %02x %02x %02x %02x %02x %02x\n",
		dev->command_table[0].cfis[0], dev->command_table[0].cfis[1],
		dev->command_table[0].cfis[2], dev->command_table[0].cfis[3],
		dev->command_table[0].cfis[4], dev->command_table[0].cfis[5],
		dev->command_table[0].cfis[6], dev->command_table[0].cfis[7]);
  grub_dprintf ("ahci", "cfis: %02x %02x %02x %02x %02x %02x %02x %02x\n",
		dev->command_table[0].cfis[8], dev->command_table[0].cfis[9],
		dev->command_table[0].cfis[10], dev->command_table[0].cfis[11],
		dev->command_table[0].cfis[12], dev->command_table[0].cfis[13],
		dev->command_table[0].cfis[14], dev->command_table[0].cfis[15]);

  dev->command_table[0].prdt[0].data_base = grub_dma_get_phys (bufc);
  dev->command_table[0].prdt[0].unused = 0;
  dev->command_table[0].prdt[0].size = (parms->size - 1);

  grub_dprintf ("ahci", "PRDT = %" PRIxGRUB_UINT64_T ", %x, %x (%"
		PRIuGRUB_SIZE ")\n",
		dev->command_table[0].prdt[0].data_base,
		dev->command_table[0].prdt[0].unused,
		dev->command_table[0].prdt[0].size,
		(grub_size_t) ((char *) &dev->command_table[0].prdt[0]
			       - (char *) &dev->command_table[0]));

  if (parms->write)
    grub_memcpy ((char *) grub_dma_get_virt (bufc), parms->buffer, parms->size);

  grub_dprintf ("ahci", "AHCI command scheduled\n");
  grub_dprintf ("ahci", "AHCI tfd = %x\n",
		dev->hba->ports[dev->port].task_file_data);
  grub_dprintf ("ahci", "AHCI inten = %x\n",
		dev->hba->ports[dev->port].inten);
  grub_dprintf ("ahci", "AHCI intstatus = %x\n",
		dev->hba->ports[dev->port].intstatus);

  dev->hba->ports[dev->port].inten = 0xffffffff;//(1 << 2) | (1 << 5);
  dev->hba->ports[dev->port].intstatus = 0xffffffff;//(1 << 2) | (1 << 5);
  grub_dprintf ("ahci", "AHCI inten = %x\n",
		dev->hba->ports[dev->port].inten);
  grub_dprintf ("ahci", "AHCI tfd = %x\n",
		dev->hba->ports[dev->port].task_file_data);
  dev->hba->ports[dev->port].sata_active = 1;
  dev->hba->ports[dev->port].command_issue = 1;
  grub_dprintf ("ahci", "AHCI sig = %x\n", dev->hba->ports[dev->port].sig);
  grub_dprintf ("ahci", "AHCI tfd = %x\n",
		dev->hba->ports[dev->port].task_file_data);

  endtime = grub_get_time_ms () + (spinup ? 20000 : 20000);
  while ((dev->hba->ports[dev->port].command_issue & 1))
    if (grub_get_time_ms () > endtime ||
	(dev->hba->ports[dev->port].intstatus & GRUB_AHCI_HBA_PORT_IS_FATAL_MASK))
      {
	grub_dprintf ("ahci", "AHCI status <%x %x %x %x>\n",
		      dev->hba->ports[dev->port].command_issue,
		      dev->hba->ports[dev->port].sata_active,
		      dev->hba->ports[dev->port].intstatus,
		      dev->hba->ports[dev->port].task_file_data);
	dev->hba->ports[dev->port].command_issue = 0;
	if (dev->hba->ports[dev->port].intstatus & GRUB_AHCI_HBA_PORT_IS_FATAL_MASK)
	  err = grub_error (GRUB_ERR_IO, "AHCI transfer error");
	else
	  err = grub_error (GRUB_ERR_IO, "AHCI transfer timed out");
	if (!reset)
	  grub_ahci_reset_port (dev, 1);
	break;
      }

  grub_dprintf ("ahci", "AHCI command completed <%x %x %x %x %x, %x %x>\n",
		dev->hba->ports[dev->port].command_issue,
		dev->hba->ports[dev->port].intstatus,
		dev->hba->ports[dev->port].task_file_data,
		dev->command_list[0].transferred,
		dev->hba->ports[dev->port].sata_error,
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x00],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x18]);
  grub_dprintf ("ahci",
		"last PIO FIS %08x %08x %08x %08x %08x %08x %08x %08x\n",
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x08],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x09],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x0a],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x0b],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x0c],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x0d],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x0e],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x0f]);
  grub_dprintf ("ahci",
		"last REG FIS %08x %08x %08x %08x %08x %08x %08x %08x\n",
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x10],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x11],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x12],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x13],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x14],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x15],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x16],
		((grub_uint32_t *) grub_dma_get_virt (dev->rfis))[0x17]);

  if (!parms->write)
    grub_memcpy (parms->buffer, (char *) grub_dma_get_virt (bufc), parms->size);
  grub_dma_free (bufc);

  return err;
}

static grub_err_t
grub_ahci_readwrite (grub_ata_t disk,
		     struct grub_disk_ata_pass_through_parms *parms,
		     int spinup)
{
  return grub_ahci_readwrite_real (disk->data, parms, spinup, 0);
}

static grub_err_t
grub_ahci_open (int id, int devnum, struct grub_ata *ata)
{
  struct grub_ahci_device *dev;

  if (id != GRUB_SCSI_SUBSYSTEM_AHCI)
    return grub_error (GRUB_ERR_UNKNOWN_DEVICE, "not an AHCI device");

  FOR_LIST_ELEMENTS(dev, grub_ahci_devices)
    if (dev->num == devnum)
      break;

  if (! dev)
    return grub_error (GRUB_ERR_UNKNOWN_DEVICE, "no such AHCI device");

  grub_dprintf ("ahci", "opening AHCI dev `ahci%d'\n", dev->num);

  ata->data = dev;
  ata->dma = 1;
  ata->atapi = dev->atapi;
  ata->maxbuffer = GRUB_AHCI_PRDT_MAX_CHUNK_LENGTH;
  ata->present = &dev->present;

  return GRUB_ERR_NONE;
}

static struct grub_disk_dev grub_nvme_dev =
  {
    .name = "nvme",
    .id = GRUB_DISK_DEVICE_NVME_ID,
    .disk_iterate = grub_nvme_iterate,
    .disk_open = grub_nvme_open,
    .disk_close = grub_nvme_close,
    .disk_read = grub_nvme_read,
    .disk_write = grub_nvme_write,
    .next = 0
  };



static struct grub_preboot *fini_hnd;

GRUB_MOD_INIT(nvme)
{
  grub_stop_disk_firmware ();

  /* NVMe initialization.  */
  grub_nvme_initialize ();

  grub_disk_dev_register (&grub_nvme_dev);

  fini_hnd = grub_loader_register_preboot_hook (grub_nvme_fini_hw,
						grub_nvme_restore_hw,
						GRUB_LOADER_PREBOOT_HOOK_PRIO_DISK);
}

GRUB_MOD_FINI(nvme)
{
  grub_nvme_fini_hw (0);
  grub_loader_unregister_preboot_hook (fini_hnd);

  grub_disk_dev_unregister (&grub_nvme_dev);
}
