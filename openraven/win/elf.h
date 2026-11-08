// Minimal ELF64 definitions (MinGW has no <elf.h>).
#pragma once
#include <stdint.h>
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define EM_X86_64 62
#define ET_EXEC 2
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_TLS 7
#define PF_X 1
#define SHN_UNDEF 0
#define STB_LOCAL 0
#define STB_WEAK 2
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_R_SYM(i) ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)(i))
enum { DT_NULL=0, DT_NEEDED=1, DT_PLTRELSZ=2, DT_HASH=4, DT_STRTAB=5, DT_SYMTAB=6,
       DT_RELA=7, DT_RELASZ=8, DT_INIT=12, DT_JMPREL=23, DT_INIT_ARRAY=25,
       DT_INIT_ARRAYSZ=27, DT_GNU_HASH=0x6ffffef5, DT_VERSYM=0x6ffffff0,
       DT_VERNEED=0x6ffffffe };
enum { R_X86_64_NONE=0, R_X86_64_64=1, R_X86_64_COPY=5, R_X86_64_GLOB_DAT=6,
       R_X86_64_JUMP_SLOT=7, R_X86_64_RELATIVE=8, R_X86_64_DTPMOD64=16,
       R_X86_64_DTPOFF64=17, R_X86_64_TPOFF64=18 };
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
  uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
  p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint32_t st_name; unsigned char st_info, st_other; uint16_t st_shndx;
  uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
typedef struct { uint16_t vn_version, vn_cnt; uint32_t vn_file, vn_aux, vn_next; } Elf64_Verneed;
typedef struct { uint32_t vna_hash; uint16_t vna_flags, vna_other; uint32_t vna_name,
  vna_next; } Elf64_Vernaux;
