SECTIONS
{
  .text :
  {
    *(.text)
  }
  .data :
  {
    *(.data)
    *(.rdata)
    *(.pdata)
  }
  .bss :
  {
    *(.bss)
    *(COMMON)
  }
  .edata :
  {
    *(.edata)
  }
  .stab :
  {
    *(.stab)
  }
  .stabstr :
  {
    *(.stabstr)
  }

  /DISCARD/ :
  {
     *(.dynamic)
  }
}
