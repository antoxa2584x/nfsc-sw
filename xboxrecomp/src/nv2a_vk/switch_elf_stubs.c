/*
 * libelf for NVK's CUBIN reader (nv_cubin.c), which the static libvulkan.a
 * references. devkitPro has no libelf, and the reader never runs on the
 * Switch; these only satisfy the linker. elf_version() reporting no version
 * makes a caller give up at once if one ever did run.
 */
#include <stddef.h>

unsigned int elf_version(unsigned int v) { (void)v; return 0; }
void *elf_memory(char *image, size_t size) { (void)image; (void)size; return NULL; }
int elf_end(void *e) { (void)e; return 0; }
int elf_kind(void *e) { (void)e; return 0; }
void *elf64_getehdr(void *e) { (void)e; return NULL; }
void *elf64_getshdr(void *scn) { (void)scn; return NULL; }
void *elf_getscn(void *e, size_t i) { (void)e; (void)i; return NULL; }
void *elf_nextscn(void *e, void *scn) { (void)e; (void)scn; return NULL; }
void *elf_getdata(void *scn, void *d) { (void)scn; (void)d; return NULL; }
int elf_getshdrstrndx(void *e, size_t *dst) { (void)e; if (dst) *dst = 0; return -1; }
char *elf_strptr(void *e, size_t s, size_t o) { (void)e; (void)s; (void)o; return NULL; }
int elf_errno(void) { return 0; }
const char *elf_errmsg(int err) { (void)err; return "no libelf on the Switch"; }
