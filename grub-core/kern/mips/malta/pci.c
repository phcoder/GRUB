#include <grub/pci.h>
#include <grub/machine/pci.h>

static volatile grub_uint32_t *cfg_addr = (volatile grub_uint32_t *) 0xbbe00cf8;
static volatile grub_uint32_t *cfg_data = (volatile grub_uint32_t *) 0xbbe00cfc;

grub_uint32_t
grub_pci_read (grub_pci_address_t addr)
{
  *cfg_addr = addr & ~3;
  return *cfg_data;
}

grub_uint16_t
grub_pci_read_word (grub_pci_address_t addr)
{
  *cfg_addr = addr & ~3;
  return *((volatile grub_uint16_t *) cfg_data  + ((addr & 2) >> 1));
}

grub_uint8_t
grub_pci_read_byte (grub_pci_address_t addr)
{
  *cfg_addr = addr & ~3;
  return *((volatile grub_uint8_t *) cfg_data  + (addr & 3));
}

void
grub_pci_write (grub_pci_address_t addr, grub_uint32_t data)
{
  *cfg_addr = addr & ~3;
  *cfg_data = data;
}

void
grub_pci_write_word (grub_pci_address_t addr, grub_uint16_t data)
{
  *cfg_addr = addr & ~3;
  *((volatile grub_uint16_t *) cfg_data  + ((addr & 2) >> 1)) = data;
}

void
grub_pci_write_byte (grub_pci_address_t addr, grub_uint8_t data)
{
  *cfg_addr = addr & ~3;
  *((volatile grub_uint8_t *) cfg_data  + (addr & 3)) = data;
}

volatile void *
grub_pci_device_map_range (grub_pci_device_t dev __attribute__ ((unused)),
			   grub_addr_t base, grub_size_t size __attribute__ ((unused)))
{
  return (volatile void *)(0xa0000000 | base);
}

void *
grub_pci_device_map_range_cached (grub_pci_device_t dev,
				  grub_addr_t base, grub_size_t size)
{
  return (void *) (((grub_addr_t) grub_pci_device_map_range (dev, base, size))
		   & ~0x20000000);
}

void
grub_pci_device_unmap_range (grub_pci_device_t dev __attribute__ ((unused)),
			     volatile void *mem __attribute__ ((unused)),
			     grub_size_t size __attribute__ ((unused)))
{
}
