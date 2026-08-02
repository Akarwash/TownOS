#include "elf.h"
#include "memory.h"
#include "paging.h"
#include "usermode.h"
#include "heap.h"
#include "../drivers/screen.h"
#include "../fs/fat32.h"
#include "../libc/mem.h"

// ELF64 parsing. See elf.h for what this is for and docs/reference/elf-loading.md
// for the format walkthrough. Only the ELF header and the program headers are
// read; section headers describe the file for linkers and debuggers and say
// nothing a loader needs.

// ---------------------------------------------------------------------------
// Format constants. Every one of these is a magic number in the spec, so every
// one of them gets a name.
// ---------------------------------------------------------------------------

// e_ident is a 16-byte identification block at offset 0, the same in ELF32 and
// ELF64, which is what lets a reader work out which of the two it is holding
// before it has committed to a structure layout.
#define EI_NIDENT     16
#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4    // 32-bit or 64-bit
#define EI_DATA       5    // endianness
#define EI_VERSION    6    // ELF version

#define ELF_MAG0      0x7F
#define ELF_MAG1      'E'
#define ELF_MAG2      'L'
#define ELF_MAG3      'F'

#define ELFCLASS64    2    // 64-bit objects
#define ELFDATA2LSB   1    // little-endian
#define EV_CURRENT    1    // the only ELF version there has ever been

#define ET_EXEC       2    // an executable, linked at a fixed address
#define EM_X86_64     62   // AMD x86-64

#define PT_LOAD       1    // a segment to copy into memory; the only kind we load

// Program header flag bits. Only PF_W is enforceable in TownOS's page tables
// (there is no NX bit enabled), so PF_R and PF_X are read and reported but
// cannot be honoured by the hardware here.
#define PF_X          0x1
#define PF_W          0x2
#define PF_R          0x4

// The page-alignment contract with user/user.ld: every loadable segment starts
// on a page boundary and is a whole number of pages long. The loader maps whole
// pages, so this is what keeps two segments from sharing one page.
#define ELF_PAGE_SIZE FRAME_SIZE

// A generous cap on the program header count. A real program has a handful; a
// file claiming thousands is either corrupt or hostile, and the count is used to
// size a loop over untrusted data.
#define ELF_MAX_PHNUM 32

// The window a loaded segment is allowed to occupy: from the bottom of the
// ring-3 region up to the bottom of the fixed user stack. The stack is mapped
// separately at a known address, so a segment reaching into it would have the
// two fighting over the same pages.
#define ELF_LOAD_MIN_VADDR  USER_REGION_START
#define ELF_LOAD_MAX_VADDR  USER_STACK_BASE

// ---------------------------------------------------------------------------
// The two structures a loader reads.
// ---------------------------------------------------------------------------
// Packed for the same reason as the Multiboot mmap entry and the FAT32 BPB:
// these offsets are fixed by the format, not by the compiler's preferences. The
// fields here happen to be naturally aligned, so packed changes nothing today,
// which is exactly why it is easy to leave off and exactly why it is written
// down: the moment a field moves or a smaller one is added, padding would shift
// every field after it and the parse would silently read the wrong bytes.

struct elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];  // 0:  magic, class, endianness, version
    uint16_t e_type;              // 16: object file type (ET_EXEC here)
    uint16_t e_machine;           // 18: architecture (EM_X86_64 here)
    uint32_t e_version;           // 20: format version
    uint64_t e_entry;             // 24: virtual address of the first instruction
    uint64_t e_phoff;             // 32: file offset of the program header table
    uint64_t e_shoff;             // 40: section headers, ignored by a loader
    uint32_t e_flags;             // 48: processor-specific flags
    uint16_t e_ehsize;            // 52: size of this header
    uint16_t e_phentsize;         // 54: size of one program header
    uint16_t e_phnum;             // 56: how many program headers
    uint16_t e_shentsize;         // 58: ignored
    uint16_t e_shnum;             // 60: ignored
    uint16_t e_shstrndx;          // 62: ignored
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;              // 0:  PT_LOAD or something we skip
    uint32_t p_flags;             // 4:  PF_R / PF_W / PF_X
    uint64_t p_offset;            // 8:  where the bytes are in the file
    uint64_t p_vaddr;             // 16: where they go in memory
    uint64_t p_paddr;             // 24: physical address, unused here
    uint64_t p_filesz;            // 32: how many bytes are IN THE FILE
    uint64_t p_memsz;             // 40: how many bytes the segment OCCUPIES
    uint64_t p_align;             // 48: required alignment
} __attribute__((packed));

// Guards on the two structs above. The ELF spec fixes both sizes, so if padding
// ever appears the build fails here rather than at runtime with a manifest that
// reads like noise.
#define ELF64_EHDR_SIZE 64
#define ELF64_PHDR_SIZE 56
typedef char elf64_ehdr_is_packed[(sizeof(struct elf64_ehdr) == ELF64_EHDR_SIZE) ? 1 : -1];
typedef char elf64_phdr_is_packed[(sizeof(struct elf64_phdr) == ELF64_PHDR_SIZE) ? 1 : -1];

// ---------------------------------------------------------------------------
// Validation.
// ---------------------------------------------------------------------------

static void reject(const char *name, char *reason) {
    print_string("elf: ");
    print_string((char *)name);
    print_string(": ");
    print_string(reason);
    print_string("\n");
}

// Does [offset, offset + length) lie entirely inside a file of file_size bytes?
//
// Written as two subtractions rather than the obvious `offset + length <= size`
// because offset and length come from the file and are 64-bit: their sum can
// wrap, and a wrapped sum compares as comfortably small. Subtracting from the
// known-good size cannot overflow.
static int range_in_file(uint64_t offset, uint64_t length, uint32_t file_size) {
    if (offset > file_size) {
        return 0;
    }
    return length <= (uint64_t)file_size - offset;
}

static int is_page_aligned(uint64_t value) {
    return (value & (ELF_PAGE_SIZE - 1)) == 0;
}

// Validate the ELF header, then the whole program header table.
//
// VALIDATE FIRST, PARSE SECOND. Nothing below reads a field out of a structure
// it has not already confirmed is that kind of structure: the magic is checked
// before the class, the class before anything 64-bit-shaped is dereferenced, and
// the program header table's own bounds before a single header is read out of
// it. A file that fails any check is rejected with a reason, never interpreted
// on the assumption that the rest of it is probably fine.
static int elf_validate(const void *file, uint32_t file_size, const char *name,
                        const struct elf64_ehdr **out_header,
                        const struct elf64_phdr **out_phdrs) {
    // Before the header can be read AS a header, the file has to be at least as
    // long as one.
    if (file_size < sizeof(struct elf64_ehdr)) {
        reject(name, "file is smaller than an ELF header");
        return -1;
    }

    const struct elf64_ehdr *header = (const struct elf64_ehdr *)file;

    if (header->e_ident[EI_MAG0] != ELF_MAG0 ||
        header->e_ident[EI_MAG1] != ELF_MAG1 ||
        header->e_ident[EI_MAG2] != ELF_MAG2 ||
        header->e_ident[EI_MAG3] != ELF_MAG3) {
        reject(name, "not an ELF file (bad magic)");
        return -1;
    }
    if (header->e_ident[EI_CLASS] != ELFCLASS64) {
        reject(name, "not a 64-bit ELF file");
        return -1;
    }
    if (header->e_ident[EI_DATA] != ELFDATA2LSB) {
        reject(name, "not little-endian");
        return -1;
    }
    if (header->e_ident[EI_VERSION] != EV_CURRENT) {
        reject(name, "unknown ELF version");
        return -1;
    }
    if (header->e_machine != EM_X86_64) {
        reject(name, "not an x86-64 binary");
        return -1;
    }
    // ET_EXEC specifically, not ET_DYN. A shared object would need its
    // relocations applied and this loader does not relocate: a program runs at
    // the address it was linked at or not at all.
    if (header->e_type != ET_EXEC) {
        reject(name, "not an executable (ET_EXEC)");
        return -1;
    }

    // Now the program header table. Its entry size must match what this code
    // believes a program header is, or the stride through the table is wrong and
    // every entry after the first is read from the wrong offset.
    if (header->e_phentsize != sizeof(struct elf64_phdr)) {
        reject(name, "unexpected program header size");
        return -1;
    }
    if (header->e_phnum == 0) {
        reject(name, "no program headers");
        return -1;
    }
    if (header->e_phnum > ELF_MAX_PHNUM) {
        reject(name, "implausible program header count");
        return -1;
    }

    // The table itself must lie inside the file before any entry is read.
    uint64_t table_bytes = (uint64_t)header->e_phnum * header->e_phentsize;
    if (!range_in_file(header->e_phoff, table_bytes, file_size)) {
        reject(name, "program header table runs past the end of the file");
        return -1;
    }

    const struct elf64_phdr *phdrs =
        (const struct elf64_phdr *)((const uint8_t *)file + header->e_phoff);

    int loadable = 0;
    for (uint16_t i = 0; i < header->e_phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;   // dynamic info, notes, stack markers: not our business
        }
        loadable++;

        // The bytes this segment claims must actually be in the file. Without
        // this, the copy below would read past the end of the buffer.
        if (!range_in_file(ph->p_offset, ph->p_filesz, file_size)) {
            reject(name, "segment contents run past the end of the file");
            return -1;
        }
        // A segment can occupy more memory than it stores (that is .bss), but
        // never less, or the copy would run past the segment's own mapping.
        if (ph->p_memsz < ph->p_filesz) {
            reject(name, "segment memory size is less than its file size");
            return -1;
        }
        // The contract with user/user.ld. The loader maps whole pages, so a
        // segment starting or ending mid-page would put two segments in one
        // page and the second mapping would have to merge with the first
        // rather than replace it. We control the linker script, so this is
        // guaranteed there and merely checked here.
        if (!is_page_aligned(ph->p_vaddr) || !is_page_aligned(ph->p_memsz)) {
            reject(name, "segment is not page-aligned");
            return -1;
        }
    }

    if (loadable == 0) {
        reject(name, "no loadable segments");
        return -1;
    }

    *out_header = header;
    *out_phdrs = phdrs;
    return 0;
}

// ---------------------------------------------------------------------------
// Loading.
// ---------------------------------------------------------------------------

// Map one segment into `as` and fill it.
//
// Returns 0 on success, -1 if the destination is out of bounds or memory runs
// out. On failure the pages already mapped leak, the same accepted tradeoff as
// everywhere else here: it only happens on out-of-memory, and tasks are never
// destroyed.
static int load_segment(const struct elf64_phdr *ph, const void *file,
                        address_space_t *as, const char *name) {
    // (1) THE BOUNDS CHECK. This is the security boundary of the whole loader.
    //
    // Everything in a program header is an instruction from an untrusted file,
    // and this one reads "write these bytes to this address". Without this
    // check, a crafted ELF names any address it likes and the kernel dutifully
    // maps a page there and copies attacker-chosen bytes into it, including
    // over the kernel's own code. The file would not even need to be malicious
    // on purpose; a corrupt vaddr does the same damage.
    //
    // So the destination is checked against the window a user program is
    // allowed to occupy BEFORE a single frame is allocated. Same spirit as the
    // syscall pointer check in kernel/syscall.c, and stricter: this one bounds
    // the whole [start, end) range rather than just the start.
    uint64_t start = ph->p_vaddr;
    uint64_t end   = ph->p_vaddr + ph->p_memsz;   // memsz is page-aligned, checked
    if (end < start) {
        reject(name, "segment address range wraps");
        return -1;
    }
    if (start < ELF_LOAD_MIN_VADDR || end > ELF_LOAD_MAX_VADDR) {
        reject(name, "segment lands outside the region a program may occupy");
        return -1;
    }

    // (2) Honour the segment's flags. Only the write bit is enforceable: TownOS
    // does not enable NX, so a segment marked R+X and one marked R are mapped
    // identically. Leaving the writable bit OFF for the text segment is real
    // though, and means a program cannot overwrite its own code.
    uint64_t flags = PG_PRESENT | PG_USER;
    if (ph->p_flags & PF_W) {
        flags |= PG_WRITABLE;
    }

    const uint8_t *src = (const uint8_t *)file + ph->p_offset;

    for (uint64_t page = 0; page < ph->p_memsz; page += FRAME_SIZE) {
        uint64_t frame = alloc_frame();
        if (frame == 0) {
            reject(name, "out of physical frames");
            return -1;
        }

        // (3) and (4) in one step: zero the whole frame, then copy in whatever
        // part of it the file actually supplies.
        //
        // THE ZERO-FILL IS NOT OPTIONAL. A segment's memory size can exceed its
        // file size, and the gap is uninitialised data: globals that start at
        // zero, which the format deliberately does not store because storing
        // thousands of zero bytes is a waste. alloc_frame hands back a frame
        // holding whatever the last user of it left there. Skip the zeroing and
        // a program's globals come up holding that garbage, so the program works
        // or fails depending on what ran before it, which is the single most
        // confusing class of loader bug. Zeroing the whole frame first (rather
        // than only the tail past file size) makes it impossible to get the
        // boundary case wrong at the cost of writing some bytes twice.
        memset((void *)frame, 0, FRAME_SIZE);

        if (page < ph->p_filesz) {
            uint64_t remaining = ph->p_filesz - page;
            uint64_t chunk = remaining < FRAME_SIZE ? remaining : FRAME_SIZE;
            // alloc_frame returns an identity-mapped frame, so `frame` is a
            // writable kernel pointer right now. The copy therefore does not
            // care that the page may be mapped read-only into `as`.
            memcpy((void *)frame, src + page, chunk);
        }

        if (paging_map_page(as, ph->p_vaddr + page, frame, flags) != 0) {
            reject(name, "out of frames for page tables");
            return -1;
        }
    }

    return 0;
}

int elf_load(const void *file, uint32_t file_size, address_space_t *as,
             const char *name, uint64_t *out_entry) {
    const struct elf64_ehdr *header;
    const struct elf64_phdr *phdrs;

    if (elf_validate(file, file_size, name, &header, &phdrs) != 0) {
        return -1;
    }

    for (uint16_t i = 0; i < header->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) {
            continue;   // not a segment to bring into memory
        }
        if (load_segment(&phdrs[i], file, as, name) != 0) {
            return -1;
        }
    }

    // The entry point comes from the header, not from any symbol the kernel
    // knows. This is what makes the program a file rather than part of the
    // kernel: nothing here was decided at kernel link time.
    if (out_entry) {
        *out_entry = header->e_entry;
    }
    return 0;
}

int elf_load_file(const char *name, address_space_t *as, uint64_t *out_entry) {
    // Ask the size first, then allocate exactly that. The alternative, a fixed
    // buffer assumed to be big enough, is a limit that fails quietly the day a
    // program outgrows it.
    uint32_t size = 0;
    if (fat32_stat(name, &size) != 0) {
        reject(name, "not found on the disk");
        return -1;
    }
    if (size == 0) {
        reject(name, "file is empty");
        return -1;
    }

    // The whole file is read before anything runs: there is no demand paging, so
    // a program cannot be faulted in a page at a time.
    void *buf = kmalloc(size);
    if (buf == NULL) {
        reject(name, "out of memory to read the file");
        return -1;
    }

    uint32_t read_size = 0;
    if (fat32_read_file(name, buf, size, &read_size) != 0) {
        reject(name, "could not be read");
        kfree(buf);
        return -1;
    }

    int result = elf_load(buf, read_size, as, name, out_entry);

    // The file buffer is scratch. Its contents are now in the task's own frames,
    // so nothing needs the copy afterward.
    kfree(buf);
    return result;
}
