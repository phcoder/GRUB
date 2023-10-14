struct trampoline
{
  grub_uint32_t auipc; /* auipc   t0,0x0 */
  grub_uint32_t ld; /* ld      t0,16(t0) */
  grub_uint16_t jr; /* jr t0 */
  grub_uint16_t nops[3]; /* nop */
  grub_uint64_t call_address;
};


static const struct trampoline trampoline_template =
  {
    0x297, /* auipc   t0,0x0 */
    0x0102b283, /* ld      t0,16(t0) */
    0x8282, /* jr t0 */
    { 0x1, 0x1, 0x1 },
    0
  };

grub_err_t
grub_riscvXX_dl_get_tramp_got_size (const void *ehdr, grub_size_t *tramp,
				    grub_size_t *got)
{
  const Elf_Ehdr *e = ehdr;
  const Elf_Shdr *s;
  unsigned i;

  *tramp = 0;
  *got = 0;

  for (i = 0, s = (Elf_Shdr *) ((char *) e + grub_le_to_cpuXX (e->e_shoff));
       i < grub_le_to_cpu16 (e->e_shnum);
       i++, s = (Elf_Shdr *) ((char *) s + grub_le_to_cpu16 (e->e_shentsize)))
    if (s->sh_type == grub_cpu_to_le32_compile_time (SHT_RELA) || s->sh_type == grub_cpu_to_le32_compile_time (SHT_REL))
      {
	const Elf_Rela *rel, *max;

	for (rel = (const Elf_Rela *) ((const char *) e + grub_le_to_cpuXX (s->sh_offset)),
	       max = rel + grub_le_to_cpuXX (s->sh_size) / grub_le_to_cpu64 (s->sh_entsize);
	     rel < max;
	     rel++)
	  switch (ELF_R_TYPE (grub_le_to_cpuXX (rel->r_info)))
	    {
	    case R_RISCV_BRANCH:
	    case R_RISCV_JAL:
	    case R_RISCV_CALL:
	    case R_RISCV_CALL_PLT:
	    case R_RISCV_RVC_BRANCH:
	    case R_RISCV_RVC_JUMP:
	      (*tramp)++;
	      break;
	    case R_RISCV_GOT_HI20:
	      (*got)++;
	    }
      }

  *tramp *= sizeof (struct trampoline);
  *got *= sizeof (grub_target_addr_t);

  return GRUB_ERR_NONE;
}
