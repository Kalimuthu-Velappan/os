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
#include <signal.h>

unsigned long base_virtual_address;
Elf64_Ehdr *elfHeader;
Elf64_Phdr *phHeader;
int fd;
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


void *map_single_page(unsigned long v_addr, int first_address) {
  long pg_size;

  ASSERT_I( (pg_size = sysconf(_SC_PAGE_SIZE)), "page size" );
  int i, prot;
  unsigned long offsetInPg; // we need to add this number to tatal size
  unsigned long alignedPgAddr; // pg_size - 1 == 000000 => will give rounded up bits
  unsigned long mapSize; // total map size. includes bits wasted due to allignment
  unsigned long offsetInFile; // offset in file
  Elf64_Addr p_vaddr;
  int flags = MAP_PRIVATE | MAP_DENYWRITE;
  unsigned long k_bss;
  int base_address_set = 0;
  int someAddressAssigned = 0;

  for (i = 0; i < elfHeader->e_phnum; ++i) {
    if (phHeader[i].p_type != PT_LOAD)
      continue;

    p_vaddr = phHeader[i].p_vaddr;

    // check that if it's the first mapping.
    if (first_address)
      goto assign_mem;
    else if  ((unsigned long)(p_vaddr <= v_addr) &&               // check that it's lies between the start
        v_addr <= (unsigned long)(p_vaddr + phHeader[i].p_memsz)) // and end of virtual memory add. from file.
      goto assign_mem;
    else
      continue; // continue and check the next index

  // page mapping code
assign_mem:
    someAddressAssigned = 1;
    offsetInPg    = p_vaddr & (pg_size - 1);


    // page size will always work since anything less than page size
    // will be rounded off to page size.
    //mapSize       = phHeader[i].p_filesz + offsetInPg;
    mapSize = pg_size;

    if (first_address) {
      alignedPgAddr = p_vaddr & ~(pg_size - 1);
      offsetInFile  = phHeader[i].p_offset - offsetInPg;
    } else {
      alignedPgAddr = v_addr & ~(pg_size - 1);
      // explanation at http://i.imgur.com/Fh2eF08.jpg?1
      // 1. Calculate the offset of page boundary of start address
      // 2. Calulate the number of pages in between the seeked virtual
      // address and the start address of this program header's virtual
      // address range
      offsetInFile = (unsigned long)phHeader[i].p_offset - (unsigned long)(p_vaddr &
          (pg_size - 1)) + (v_addr & ~(pg_size - 1)) - (p_vaddr & ~(pg_size - 1));
    }

    prot          = getProt(phHeader[i].p_flags);

    if (elfHeader->e_type == ET_EXEC)
      flags |= MAP_FIXED;

    // for bss segment, we still need to map anonymous region
    if (v_addr > (unsigned long)(p_vaddr + phHeader[i].p_filesz)) {
      flags |= MAP_ANONYMOUS;
      fprintf(stderr, "[BSS]");
    }

    char *m_map;
    ASSERT_I( (m_map = mmap((caddr_t)alignedPgAddr, mapSize, prot,
            flags, fd, offsetInFile)), "mmap");
    totalMemoryMapped += mapSize;
    fprintf(stderr, "Mapping aligned  virtual address at %li and TMP: %li\n", alignedPgAddr, totalMemoryMapped);
    CMP_AND_FAIL(m_map, (char *)alignedPgAddr, "Couldn't assign asked virtual address");
    // we shift the base address using static link.
    if (first_address == 1 && base_address_set == 0) {
      base_virtual_address = (unsigned long)m_map;
      base_address_set = 1;
    }
    break;
  }

  if (someAddressAssigned == 0) {
    // raise the segmenation fault again.
    // not sure why this is not getting caught.
    raise(SIGSEGV);
  }
}

/*
 * This method sets the elfheader and program header
 * And loads the initial page into memory.
 *
 * */
void *load_elf_binary(char* buf, int fdt)
{
  /*
   * ELFheader is at a fixed location and has paramters
   * required to understand the other components in the
   * ELF file
   *
   * */

  unsigned char *e_ident;
  long pg_size;

  elfHeader = (Elf64_Ehdr *)buf;
  phHeader  = (Elf64_Phdr *)(buf + elfHeader->e_phoff);
  e_ident   = elfHeader->e_ident;

  // validate elf
  validate_elf(e_ident);
  fd = fdt;

  // void *map_single_page(char *buf, int fd, unsigned long v_addr, int first_address) {
  map_single_page(0, 1);
  return (void *)elfHeader->e_entry;
}

/*
 * Reusing the stack. Modifying to accomodate changes
 * for new program.
 *
 * */
void modify_auxv(char *envp[], char *buf, void *argv) {
  Elf64_Ehdr *elfHeader;
  elfHeader = (Elf64_Ehdr *)buf;
  size_t pg_size;
  ASSERT_I( (pg_size = sysconf(_SC_PAGE_SIZE)), "page size" );
  Elf64_auxv_t *auxv;

  while(*envp++ != NULL);

  for (auxv = (Elf64_auxv_t *)envp; auxv->a_type != AT_NULL; auxv++) {
    switch (auxv->a_type) {
      case AT_PHDR:
        {
          auxv->a_un.a_val = (uint64_t)(buf + elfHeader->e_phoff);
          break;
        }
      case AT_PHENT:
        {
          auxv->a_un.a_val = (uint64_t)(buf + elfHeader->e_phentsize);
          break;
        }
      case AT_PHNUM:
        {
          auxv->a_un.a_val =(uint64_t)(buf + elfHeader->e_phnum);
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
 * Segfault handler. On each page fault we allocate a memory page
 * */
static void segfault_handler(int sig, siginfo_t *info, void *uap) {
  ASSERT_P(info, "Trying to derefence a null pointer");
  map_single_page((unsigned long)info->si_addr, 0);
}

/*
 * Reference:
 * http://www.emoticode.net/c/custom-sigsegv-handler-with-backtrace-reporting.html
 *
 * */
void install_segfault_handler() {
  struct sigaction act;
  sigemptyset(&act.sa_mask);
  // we want the signinfo and we don't want to defer it.
  // handle it now!
  act.sa_flags = SA_SIGINFO | SA_NODEFER;
  act.sa_handler = (void *)segfault_handler;
  ASSERT_I((sigaction( SIGSEGV, &act, NULL)), "segfault handler");
}

/**
 *
 * Text Segment should be shifted.
 * Load program under test on top of
 * loader program leads to assertion failure
 *
 * Note: stack creation code in header file. loader.h
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
    printf("Usage: %s <Valid Object/Executable ELF file>\n", argv[0]);
    exit(1);
  }

  unsigned long *loader_stack = (unsigned long*)(&argv[0]);
  size_t stack_size =  loaderStackSize(envp, loader_stack);

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
  // the new argc is argv[0].
  *((int*) stack_ptr) = argc - 1;

  // check that it is created correctly
  char **filename = (char **)((stack_ptr+1));
  DEBUG("stack copied: %d, %s\n", stack_size, filename[0]);

  modify_auxv((char **)(stack_ptr+argc+1), buf, (stack_ptr+1)); // stack_ptr + 1 points to new filename
  DEBUG("Modified auxillary vector\n");

  e_entry = load_elf_binary(buf, fd);
  DEBUG("Loaded the first page of elf binary.\n");

  // installing the segfault handler.
  install_segfault_handler();

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
  DEBUG("Set the stack pointer. Going to start executing now.\n");

  // jump to the entry of program
  asm("jmp %0"
      :
      :"a"(e_entry)
      :
     );

  // close the file.
  ASSERT_I(fclose(fh), "fclose");
  return 0;
}

