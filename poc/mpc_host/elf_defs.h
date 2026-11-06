// The part of the ELF format the loader uses (names as in glibc's <elf.h>, which Windows and macOS do not have). Layouts follow the ELF specification
// and the ARM / AArch64 ELF ABI supplements. Only little-endian 32-bit ARM and 64-bit AArch64 objects are read.
#pragma once
#include <cstdint>

#define EI_NIDENT 16
#define EI_CLASS 4
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ET_DYN 3
#define EM_ARM 40
#define EM_AARCH64 183

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_ARM_EXIDX 0x70000001

#define DT_NULL 0
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_INIT 12
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_PLTRELSZ 2
#define DT_RELR 36

#define SHT_DYNSYM 11
#define SHN_UNDEF 0
#define STB_LOCAL 0
#define STB_WEAK 2
#define STT_TLS 6

#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((i) & 0xff)
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)

#define R_ARM_NONE 0
#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23
#define R_ARM_TLS_DTPMOD32 17
#define R_ARM_TLS_DTPOFF32 18
#define R_ARM_TLS_TPOFF32 19
#define R_AARCH64_NONE 0
#define R_AARCH64_ABS64 257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027
#define R_AARCH64_TLS_DTPMOD 1028
#define R_AARCH64_TLS_DTPREL 1029
#define R_AARCH64_TLS_TPREL 1030
#define R_AARCH64_TLSDESC 1031

typedef struct { unsigned char e_ident[EI_NIDENT]; uint16_t e_type, e_machine; uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf32_Ehdr;
typedef struct { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align; } Elf32_Phdr;
typedef struct { int32_t d_tag; union { uint32_t d_val; uint32_t d_ptr; } d_un; } Elf32_Dyn;
typedef struct { uint32_t st_name, st_value, st_size; unsigned char st_info, st_other; uint16_t st_shndx; } Elf32_Sym;
typedef struct { uint32_t r_offset, r_info; } Elf32_Rel;
typedef struct { uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize; } Elf32_Shdr;

typedef struct { unsigned char e_ident[EI_NIDENT]; uint16_t e_type, e_machine; uint32_t e_version; uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { int64_t d_tag; union { uint64_t d_val; uint64_t d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint32_t st_name; unsigned char st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset, sh_size; uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize; } Elf64_Shdr;
