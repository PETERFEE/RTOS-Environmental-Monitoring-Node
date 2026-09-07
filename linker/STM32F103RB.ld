/* -----------------------------------------------------------------------------
 * STM32F103RBT6 linker script  --  128 KB Flash / 20 KB SRAM
 *
 * Written from scratch for the rtos-iot-sensor-node project (MIT licensed with
 * the rest of the application code) so that the repository carries no
 * redistribution-restricted vendor linker script.
 *
 * Memory map
 *   FLASH : 0x0800_0000 .. 0x0802_0000   (128 KB)
 *   SRAM  : 0x2000_0000 .. 0x2000_5000   ( 20 KB)
 *
 * The main stack (MSP) grows downward from the top of SRAM. Under FreeRTOS the
 * MSP is only used by the startup code, by interrupt handlers and by the
 * scheduler itself; every task runs on its own stack allocated out of the
 * FreeRTOS heap (heap_4). _Min_Stack_Size therefore only has to cover the
 * deepest ISR nesting, not application code.
 * ---------------------------------------------------------------------------*/

ENTRY(Reset_Handler)

_Min_Heap_Size  = 0x000;   /* newlib heap: unused, FreeRTOS owns all dynamic memory */
_Min_Stack_Size = 0x400;   /* 1 KB of MSP for interrupt/handler mode              */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* top of stack */

SECTIONS
{
  /* Vector table must sit at the start of flash: the CPU fetches the initial
     MSP value and reset vector from 0x08000000 out of reset.                 */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)          /* ARM -> Thumb interworking veneers */
    *(.glue_7t)         /* Thumb -> ARM interworking veneers */
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM       : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >FLASH

  /* Load address of .data in flash, used by Reset_Handler to copy it to SRAM */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* Fails the link (rather than the board) if RAM is over-committed. */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);          /* newlib _sbrk break start */
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
