/**
 * @file    syscalls.c
 * @brief   Minimal newlib retargeting.
 *
 * --specs=nosys.specs already supplies weak stubs for the POSIX layer; the only
 * ones worth overriding here are _write (so anything that does reach printf
 * lands on the log UART) and _sbrk (so a stray malloc cannot silently eat the
 * stack). FreeRTOS allocates from its own heap_4 pool, not from this one.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "main.h"

/* Provided by the linker script. */
extern char end;           /* first free byte after .bss */
extern char _estack;
extern char _Min_Stack_Size;

int _write(int file, char *ptr, int len)
{
    (void)file;
    if (ptr == NULL || len <= 0)
    {
        return 0;
    }
    board_uart_write_blocking(ptr, (uint16_t)len);
    return len;
}

int _read(int file, char *ptr, int len)
{
    (void)file; (void)ptr; (void)len;
    return 0;                       /* stdin is not connected */
}

caddr_t _sbrk(int incr)
{
    static char *heap_end = NULL;

    if (heap_end == NULL)
    {
        heap_end = &end;
    }

    /* Refuse to grow into the region reserved for the main stack. */
    const char *const limit = &_estack - (uintptr_t)&_Min_Stack_Size;

    if ((heap_end + incr) > limit)
    {
        errno = ENOMEM;
        return (caddr_t)-1;
    }

    char *const prev = heap_end;
    heap_end += incr;
    return (caddr_t)prev;
}

int _close(int file)                        { (void)file; return -1; }
int _fstat(int file, struct stat *st)       { (void)file; st->st_mode = S_IFCHR; return 0; }
int _isatty(int file)                       { (void)file; return 1; }
int _lseek(int file, int ptr, int dir)      { (void)file; (void)ptr; (void)dir; return 0; }
int _getpid(void)                           { return 1; }
int _kill(int pid, int sig)                 { (void)pid; (void)sig; errno = EINVAL; return -1; }

void _exit(int status)
{
    (void)status;
    for (;;) { __NOP(); }
}
