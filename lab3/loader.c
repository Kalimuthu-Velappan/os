/*
 *
 * A simple program loader.
 *
 * References:
 * http://www.skyfree.org/linux/references/ELF_Format.pdf
 * http://linux.die.net/man/5/elf
 *
 * Aux vector type definitions
 * http://lxr.free-electrons.com/source/include/uapi/linux/auxvec.h#L33
 *
 * Stack Reference from http://articles.manugarg.com/aboutelfauxiliaryvectors.html
 *
position            content                     size (bytes) + comment
  ------------------------------------------------------------------------
  stack pointer ->  [ argc = number of args ]     4
                    [ argv[0] (pointer) ]         4   (program name)
                    [ argv[1] (pointer) ]         4
                    [ argv[..] (pointer) ]        4 * x
                    [ argv[n - 1] (pointer) ]     4
                    [ argv[n] (pointer) ]         4   (= NULL)

                    [ envp[0] (pointer) ]         4
                    [ envp[1] (pointer) ]         4
                    [ envp[..] (pointer) ]        4
                    [ envp[term] (pointer) ]      4   (= NULL)

                    [ auxv[0] (Elf32_auxv_t) ]    8
                    [ auxv[1] (Elf32_auxv_t) ]    8
                    [ auxv[..] (Elf32_auxv_t) ]   8
                    [ auxv[term] (Elf32_auxv_t) ] 8   (= AT_NULL vector)

                    [ padding ]                   0 - 16

                    [ argument ASCIIZ strings ]   >= 0
                    [ environment ASCIIZ str. ]   >= 0

  (0xbffffffc)      [ end marker ]                4   (= NULL)

  (0xc0000000)      < bottom of stack >           0   (virtual)
  ------------------------------------------------------------------------
 *
 *
 * Author: Ankit Goyal
 * */
#include <stdio.h>
#include <stdlib.h>
#include <elf.h>
#include <sys/stat.h>
#include "loader.h"
#include <unistd.h> // page size
#include <sys/types.h>
#include <sys/mman.h>
#include <assert.h>
#include <string.h>

unsigned long base_virtual_address;
unsigned long totalMemoryMapped = 0;

// basic validation checks
void validate_elf(unsigned char *e_ident) {
  assert(e_ident[EI_MAG0] == ELFMAG0); // magic number 0x7f
  assert(e_ident[EI_MAG1] == ELFMAG1); // 'E'
  assert(e_ident[EI_MAG2] == ELFMAG2); // 'L'
  assert(e_ident[EI_MAG3] == ELFMAG3); // 'F'
  assert(e_ident[EI_CLASS] == ELFCLASS64); // 64 bit
  assert(e_ident[EI_DATA] == ELFDATA2LSB); // least significant byte -> lowest address(intel)
}

inline int getProt(Elf64_Word p_flags) {
  int prot = PROT_NONE;

  if (p_flags & PF_W)
    prot |= PROT_READ;
  if (p_flags & PF_R)
    prot |= PROT_WRITE;
  if (p_flags & PF_X)
    prot |= PROT_EXEC;

  return prot;
}

void *load_elf_binary(char* buf, int fd)
{
  /*
   * ELFheader is at a fixed location and has paramters
   * required to understand the other components in the
   * ELF file
   *
   * */
  Elf64_Ehdr *elfHeader;
  Elf64_Phdr *phHeader;
  unsigned char *e_ident;
  long pg_size;

  elfHeader = (Elf64_Ehdr *)buf;
  e_ident   = elfHeader->e_ident;

  // validate elf
  validate_elf(e_ident);

  // get program header that has information about parts that
  // needs to be loaded in memory.
  phHeader  = (Elf64_Phdr *)(buf + elfHeader->e_phoff);

  // get the page size
  ASSERT_I( (pg_size = sysconf(_SC_PAGE_SIZE)), "page size" );
  DEBUG("Page size: %ld\n", pg_size);

  int i, prot;
  unsigned long offsetInPg; // we need to add this number to tatal size
  unsigned long alignedPgAddr; // pg_size - 1 == 000000 => will give rounded up bits
  unsigned long mapSize; // total map size. includes bits wasted due to allignment
  unsigned long offsetInFile; // offset in file
  Elf64_Addr p_vaddr;
  int flags = MAP_PRIVATE | MAP_DENYWRITE;
  unsigned long k_bss;
  int base_address_set = 0;

  for (i = 0; i < elfHeader->e_phnum; ++i) {
    // skip if it's not a load
    if (phHeader[i].p_type != PT_LOAD)
      continue;
    p_vaddr       = phHeader[i].p_vaddr;
    offsetInPg    = p_vaddr & (pg_size - 1);
    alignedPgAddr = p_vaddr & ~(pg_size - 1);
    mapSize       = phHeader[i].p_filesz + offsetInPg;
    offsetInFile  = phHeader[i].p_offset - offsetInPg;
    prot          = getProt(phHeader[i].p_flags);

    if (elfHeader->e_type == ET_EXEC)
      flags |= MAP_FIXED;

    char *m_map;
    ASSERT_I( (m_map = mmap((caddr_t)alignedPgAddr, mapSize, prot,
            flags, fd, offsetInFile)), "mmap");
    totalMemoryMapped += mapSize;
    fprintf(stderr, "Mapping aligned virtual address at %li and TMP: %li\n", alignedPgAddr, totalMemoryMapped);

    CMP_AND_FAIL(m_map, (char *)alignedPgAddr, "Couldn't assign asked virtual address");

    // we shift the base address using statuc link.
    if (base_address_set == 0) {
      base_virtual_address = (unsigned long)m_map;
      base_address_set = 1;
    }

    // adjust the bss segment.
    // bss doesn't occupy any space in file where as it does in memory
    // Note that it is actually present as zero bytes in elf too.
    if (phHeader[i].p_memsz > phHeader[i].p_filesz) {
      alignedPgAddr = phHeader[i].p_vaddr + phHeader[i].p_filesz;
      // move to next page and find the boundary
      alignedPgAddr = (alignedPgAddr + pg_size - 1) & ~(pg_size - 1);
      mapSize = phHeader[i].p_memsz - phHeader[i].p_filesz;

      flags |= MAP_ANONYMOUS;
      // map this region to Anonymous which is "zeroed" out
      ASSERT_I( (m_map = mmap((caddr_t)alignedPgAddr, mapSize, prot,
              flags, -1, 0)), "mmap");
      totalMemoryMapped += mapSize;
      fprintf(stderr, "[BSS]Mapping aligned virtual address at %li and TMP: %li\n", alignedPgAddr, totalMemoryMapped);
    }

  }

  // the entry point for program to execute
  DEBUG("Entry Point: %p\n", (void *)elfHeader->e_entry);
  DEBUG("Base aaddress: %li\n", base_virtual_address);

  return (void *)elfHeader->e_entry;
}

void modify_auxv(char **envp, char *buf, void *argv) {
  Elf64_Ehdr *elfHeader;
  elfHeader = (Elf64_Ehdr *)buf;
  size_t pg_size;
  ASSERT_I( (pg_size = sysconf(_SC_PAGE_SIZE)), "page size" );

  Elf64_auxv_t *auxv;
  DEBUG("Got the env pointer %s\n", envp[0]);

  while(*envp++ != NULL);

  for (auxv = (Elf64_auxv_t *)envp; auxv->a_type != AT_NULL; auxv++) {
    switch (auxv->a_type) {
      case AT_PHDR:
        {
          DEBUG("[AUXV] changed the phoff\n");
          auxv->a_un.a_val = (uint64_t)(buf + elfHeader->e_phoff);
          break;
        }
      case AT_PHENT:
        {
          auxv->a_un.a_val = (uint64_t)(elfHeader->e_phentsize);
          break;
        }
      case AT_PHNUM:
        {
          auxv->a_un.a_val =(uint64_t)(elfHeader->e_phnum);
          break;
        }

      case AT_PAGESZ:
        {
          auxv->a_un.a_val = pg_size;
          break;
        }

      case AT_BASE:
        {
          auxv->a_un.a_val = base_virtual_address;
          break;
        }

      case AT_ENTRY:
        {
          // Check that memory addresses don't clash with each other.
          // Not sure if this is exhaustive enough
          if((auxv->a_un.a_val & ~(pg_size - 1)) ==
              (((uint64_t)elfHeader->e_entry) & ~(pg_size - 1))){
            fprintf(stderr,"-----\nPossible memory overlap \
                \nExiting...\nGood bye!\n-----\n");
            exit(-1);
          }
          auxv->a_un.a_val = elfHeader->e_entry;
          break;
        }
      case AT_EXECFN:
        {
          auxv->a_un.a_val = *(unsigned long *)argv;
          break;
        }
      default:
        {
          break;
        }
    }
  }
}

/*

unsigned long *create_new_stack(unsigned long *loader_stack, size_t stack_size) {
  unsigned long *stack;
  int stack_prot = PROT_READ | PROT_WRITE | PROT_EXEC;
  int flags = (MAP_GROWSDOWN | MAP_ANONYMOUS | MAP_PRIVATE);
  ASSERT_I((stack = mmap(NULL, STACK_SIZE, stack_prot, flags, -1, 0)), "mmap");
  CMP_AND_FAIL(memcpy(stack, loader_stack, stack_size), stack, "memcpy");
  return stack;
}

size_t loaderStackSize(char** envp, void *top_stack) {
  while(*envp++ != NULL);
  Elf64_auxv_t *aux_v = (Elf64_auxv_t *)envp;
  for (; aux_v->a_type != AT_NULL; aux_v++);
  aux_v++;
  int *padding = (int *)aux_v;
  padding++;
  char *asciiz = (char *)padding;
  asciiz++;
  void *bottom_stack = asciiz;
  return ((unsigned long)(bottom_stack) - (unsigned long)(top_stack)) ;
}
*/

/**
 *
 * Text Segment should be shifted.
 * Load program under test on top of
 * loader program leads to assertion failure
 *
 **/
  int
main(int argc, char* argv[], char* envp[])
{
  struct stat fstat;
  int size;
  FILE *fh;
  int fd;
  char *buf;
  void *e_entry;
  unsigned long *stack_ptr;

  if (argc < 2) {
    printf("Usage: %s <Valid Executable ELF file>\n", argv[0]);
    exit(1);
  }

  unsigned long *loader_stack = (unsigned long*)(&argv[0]);
  size_t stack_size =  loaderStackSize(envp, loader_stack);
  //printf("Stack size: %d\n", stack_size);

  // open the binary file
  ASSERT_P( (fh = fopen(argv[1], "rb")), "open" );
  DEBUG("Opened the file\n");

  // stat the file to get size
  ASSERT_I( stat(argv[1], &fstat), "fstat" );
  size = fstat.st_size;
  DEBUG("File Size: %d\n", size);

  // allocate memory for file buffer
  ASSERT_P( (buf = (char *)malloc(sizeof(char) * size)), "malloc" );

  // read from file
  CMP_AND_FAIL(fread(buf, 1, size, fh), size, "fread");

  ASSERT_I((fd = fileno(fh)), "fileno");

  // create new stack
  stack_ptr = create_new_stack(loader_stack, stack_size);

  // set the argc pointer to argv[0]
  // set the value of argv[0] to argc - 1
  // get the pointer to new argc which is also the stack pointer
  //stack_ptr = (unsigned long *)(stack_ptr + 1);
  // the new argc is argv[0].
  *((int*) stack_ptr) = argc - 1;
  char **filename = (char **)((stack_ptr+1));
  DEBUG("stack copied: %d, %s\n", stack_size, filename[0]);

  modify_auxv((char **)(stack_ptr+argc+1), buf, (stack_ptr+1)); // stack_ptr + 1 points to new filename
  e_entry = load_elf_binary(buf, fd);
  DEBUG("Created the stack\n");

  // zero out all the registers and make them invalid by putting them in clobbered list.
  asm("xor %%rax, %%rax;"
      "xor %%rbx, %%rbx;"
      "xor %%rcx, %%rcx;"
      "xor %%rdx, %%rdx;"
      "xor %%rsi, %%rsi;"
      "xor %%rdi, %%rdi;"
      "xor %%r8, %%r8;"
      "xor %%r9, %%r9;"
      "xor %%r10, %%r10;"
      "xor %%r11, %%r11;"
      "xor %%r12, %%r12;"
      "xor %%r13, %%r13;"
      "xor %%r14, %%r14;"
      "xor %%r15, %%r15;"
      :
      :
      :"%rax", "%rbx", "%rcx", "%rdx", "%rsi", "%rdi", "%rsp", "%r8", "%r9", "%r10", "%r11", "%r12", "%r13", "%r14", "%r15"
     );


  // https://gcc.gnu.org/onlinedocs/gcc-4.5.1/gcc/Machine-Constraints.html
  // "a" - General purpose register.
  // update the stack pointer
  asm("movq %0, %%rsp;"
      :
      :"a"(stack_ptr)
      :"%rsp"
     );

  // jump to the entry of program
  asm("jmp %0"
      :
      :"a"(e_entry)
      :
     );

  // close the file
    ASSERT_I(fclose(fh), "fclose");
  return 0;
}

