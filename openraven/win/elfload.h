#pragma once
#include <stdio.h>
#include "elf.h"
typedef struct {
    const char *path;
    unsigned char *base;
    uint64_t bias, size, entry, init;
    Elf64_Dyn *dyn;
    Elf64_Phdr tls;
    char *strtab;
    Elf64_Sym *symtab;
    uint32_t *hash, *gnuhash;
    Elf64_Rela *rela, *jmprel;
    uint64_t relasz, pltrelsz;
    uint16_t *versym;
    Elf64_Verneed *verneed;
    uint64_t *init_array;
    uint64_t init_arraysz;
    int tls_relocs, fs_hits, syscall_hits;
    int tls_id;  // 0 = no TLS
    Elf64_Phdr *phdr;  // copy of the program headers (for dl_iterate_phdr)
    uint16_t phnum;
} elf_module;
__attribute__((sysv_abi)) int elf_dl_iterate_phdr(void *cb, void *data);
const char *elf_module_name(void *addr);
int elf_emulate_copy(elf_module *exe, FILE *log);
__attribute__((sysv_abi)) void *elf_tls_get_addr(void *tls_index);
typedef void *(*shim_lookup_fn)(const char *name);
elf_module *elf_load(const char *path);
void *elf_find_export(const char *name);
int elf_relocate_all(shim_lookup_fn shims, FILE *report);
void elf_report(FILE *f);
