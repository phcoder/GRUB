#define MKIMAGE_ELF32 1

# define SUFFIX(x)	x ## 32
# define ELFCLASSXX	ELFCLASS32
# define Elf_Ehdr	Elf32_Ehdr
# define Elf_Phdr	Elf32_Phdr
# define Elf_Nhdr	Elf32_Nhdr
# define Elf_Addr	Elf32_Addr
# define Elf_Sym	Elf32_Sym
# define Elf_Off	Elf32_Off
# define Elf_Shdr	Elf32_Shdr
# define Elf_Rela       Elf32_Rela
# define Elf_Rel        Elf32_Rel
# define Elf_Word       Elf32_Word
# define Elf_Half       Elf32_Half
# define Elf_Section    Elf32_Section
# define ELF_R_SYM(val)		ELF32_R_SYM(val)
# define ELF_R_TYPE(val)		ELF32_R_TYPE(val)
# define ELF_ST_TYPE(val)		ELF32_ST_TYPE(val)
# define grub_riscvXX_dl_get_tramp_got_size grub_riscv32_dl_get_tramp_got_size
# define grub_le_to_cpuXX grub_le_to_cpu32
# define grub_target_addr_t grub_uint32_t

#define XEN_NOTE_SIZE		132
#define XEN_PVH_NOTE_SIZE	20

#ifndef GRUB_MKIMAGEXX
#include "grub-mkimagexx.c"
#endif
