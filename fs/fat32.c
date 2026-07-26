#include "fat32.h"
#include "../drivers/disk.h"
#include "../drivers/screen.h"
#include "../kernel/heap.h"
#include "../libc/mem.h"

// Read-only FAT32. See fat32.h for the scope and docs/reference/fat32.md for the
// on-disk layout this parses.

// The boot sector is block 0 of the volume. The image is formatted as a
// "superfloppy" (the FAT32 volume starts at block 0, no partition table), so
// there is no partition entry to walk first.
#define FAT32_BOOT_BLOCK 0

// Offset 510 of the boot sector holds 0xAA55, little-endian. Every FAT volume
// has it; its absence means block 0 is not a boot sector at all.
#define FAT32_SIGNATURE_OFFSET 510
#define FAT32_SIGNATURE        0xAA55

// A cluster is a run of consecutive blocks treated as one unit. disk_read takes
// a uint8_t count, so one call moves at most 255 blocks, and a cluster read has
// to respect that. It does so by construction: sectors_per_cluster is itself a
// single byte on disk, so it cannot ask for more blocks than one disk_read can
// carry. The guard below pins that reasoning down at compile time, because it is
// the only thing keeping every cluster read to a single call.
#define FAT32_MAX_SECTORS_PER_CLUSTER 255

// FAT slots 0 and 1 are reserved by the format and never describe file data, so
// the first cluster that can hold data is cluster 2. See cluster_to_block below.
#define FAT32_FIRST_DATA_CLUSTER 2

// ---------------------------------------------------------------------------
// FAT entries.
// ---------------------------------------------------------------------------
// One 32-bit slot per cluster, holding the number of the next cluster in the
// file. A slot holding zero means the cluster is free, so the same table is both
// the chain map and the free list.
#define FAT32_ENTRY_BYTES 4

// Only the low 28 bits of an entry are meaningful; the top 4 are reserved and
// may hold anything. Every entry must be masked before it is compared against
// anything below. Skipping the mask is the classic FAT32 bug: end-of-chain
// detection then fails intermittently, depending on what the formatter happened
// to leave in the reserved bits.
#define FAT32_ENTRY_MASK 0x0FFFFFFF

#define FAT32_CLUSTER_FREE 0x00000000
#define FAT32_CLUSTER_BAD  0x0FFFFFF7
// Any masked value at or above this marks the last cluster of a file. It is a
// range, not a single value, because different formatters write different
// end-of-chain markers (0x0FFFFFF8 and 0x0FFFFFFF are both common).
#define FAT32_CLUSTER_END  0x0FFFFFF8

// fat32_next_cluster's three outcomes, kept distinct because "the chain ended"
// and "the read failed" are different facts and callers act on them differently.
#define FAT32_CHAIN_NEXT  0
#define FAT32_CHAIN_END   1
#define FAT32_CHAIN_ERROR (-1)

// ---------------------------------------------------------------------------
// Directory entries.
// ---------------------------------------------------------------------------
#define FAT32_DIRENT_BYTES 32

// The first byte of an entry doubles as a marker.
#define FAT32_DIRENT_FREE    0x00  // never used: no more entries in this directory
#define FAT32_DIRENT_DELETED 0xE5  // was used, since deleted: skip it

// Attribute byte bits.
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20

// An entry whose whole attribute byte equals this is not a file at all: it is
// one fragment of a long filename, bolted onto the format later and deliberately
// given an attribute combination old software would ignore. Out of scope here,
// so skip these; the 8.3 entry the fragments describe follows them.
#define FAT32_ATTR_LONG_NAME 0x0F

// The 8.3 name is 11 bytes with no dot stored: 8 bytes of base name then 3 of
// extension, both space-padded, uppercase. HELLO.TXT is stored as "HELLO   TXT".
#define FAT32_NAME_LENGTH 11
#define FAT32_BASE_LENGTH 8
#define FAT32_EXT_LENGTH  3
// Longest display form: 8 base + '.' + 3 extension + terminator.
#define FAT32_DISPLAY_NAME_MAX 13

// ---------------------------------------------------------------------------
// The BIOS Parameter Block: the filesystem describing its own shape.
// ---------------------------------------------------------------------------
// __attribute__((packed)) is load-bearing, not decoration. These fields sit at
// unaligned offsets on disk (bytes_per_sector is at offset 11, a 16-bit field at
// an odd address). Without packed, the compiler inserts padding to align them
// and every field after the first misalignment silently reads the wrong bytes.
// This is the same trap as the Multiboot mmap entry. If every parsed field after
// the first looks like garbage, check that this attribute is still here.
//
// The struct stops at root_cluster because nothing past it is needed for
// reading. The trailing boot code and the 0xAA55 signature are checked straight
// out of the raw block instead.
struct fat32_bpb {
    uint8_t  jump[3];                  // 0:  jump over the BPB to the boot code
    uint8_t  oem_name[8];              // 3:  formatter's name, informational
    uint16_t bytes_per_sector;         // 11: block size, 512 on this platform
    uint8_t  sectors_per_cluster;      // 13: blocks glued into one cluster
    uint16_t reserved_sector_count;    // 14: blocks before the first FAT
    uint8_t  num_fats;                 // 16: how many copies of the FAT follow
    uint16_t root_entry_count;         // 17: FAT12/16 only, 0 here
    uint16_t total_sectors_16;         // 19: FAT12/16 only, 0 here
    uint8_t  media;                    // 21: media descriptor byte
    uint16_t sectors_per_fat_16;       // 22: FAT12/16 only, 0 here (see below)
    uint16_t sectors_per_track;        // 24: legacy CHS geometry, unused
    uint16_t num_heads;                // 26: legacy CHS geometry, unused
    uint32_t hidden_sectors;           // 28: blocks before this volume
    uint32_t total_sectors_32;         // 32: size of the volume in blocks
    uint32_t sectors_per_fat_32;       // 36: blocks in one FAT copy
    uint16_t ext_flags;                // 40: FAT mirroring flags
    uint16_t fs_version;               // 42: format version
    uint32_t root_cluster;             // 44: first cluster of the root directory
} __attribute__((packed));

// Compile-time guard on the above. If padding ever creeps back in, the array
// size goes negative and the build fails here rather than at runtime with
// nonsense geometry. 48 is where root_cluster ends (offset 44 plus 4 bytes).
#define FAT32_BPB_SIZE 48
typedef char fat32_bpb_is_packed[(sizeof(struct fat32_bpb) == FAT32_BPB_SIZE) ? 1 : -1];

// One cluster is always one disk_read: an 8-bit block count can hold any value
// an 8-bit sectors_per_cluster field can produce. Widen either and the cluster
// read must be split into several calls, so fail the build here instead.
typedef char fat32_cluster_fits_one_read[
    (sizeof(((struct fat32_bpb *)0)->sectors_per_cluster) == 1 &&
     FAT32_MAX_SECTORS_PER_CLUSTER >= 255) ? 1 : -1];

// ---------------------------------------------------------------------------
// A directory entry: the mapping from a name to a starting cluster and a size.
// ---------------------------------------------------------------------------
// A directory is just a file whose contents are a run of these, 32 bytes each.
// Packed for the same reason as the BPB: these offsets are fixed by the format,
// not by the compiler's alignment preferences.
//
// first_cluster is split across two 16-bit fields at opposite ends of the entry
// (a FAT16 layout with the high half bolted into a gap later). They must be
// recombined as (high << 16) | low. Reading only the low half is easy to do by
// accident and yields a wildly wrong cluster number on any volume large enough
// for the high half to be non-zero.
struct fat32_dirent {
    uint8_t  name[FAT32_NAME_LENGTH];  // 0:  8.3, space-padded, no dot stored
    uint8_t  attr;                     // 11: the attribute bits above
    uint8_t  nt_reserved;              // 12: reserved
    uint8_t  create_time_tenths;       // 13: creation time, tenths of a second
    uint16_t create_time;              // 14: creation time
    uint16_t create_date;              // 16: creation date
    uint16_t access_date;              // 18: last access date
    uint16_t first_cluster_high;       // 20: high 16 bits of the start cluster
    uint16_t write_time;               // 22: last write time
    uint16_t write_date;               // 24: last write date
    uint16_t first_cluster_low;        // 26: low 16 bits of the start cluster
    uint32_t size;                     // 28: file length in bytes
} __attribute__((packed));

typedef char fat32_dirent_is_packed[
    (sizeof(struct fat32_dirent) == FAT32_DIRENT_BYTES) ? 1 : -1];

// ---------------------------------------------------------------------------
// Cached volume geometry, filled in by fat32_init.
// ---------------------------------------------------------------------------
// Parsed once from the boot sector because every read needs it and re-reading
// block 0 per operation would be pointless I/O.
static uint32_t fs_bytes_per_sector;
static uint32_t fs_sectors_per_cluster;
static uint32_t fs_bytes_per_cluster;
static uint32_t fs_first_fat_block;    // block of the first FAT copy
static uint32_t fs_sectors_per_fat;
static uint32_t fs_first_data_block;   // block where cluster 2 begins
static uint32_t fs_root_cluster;
static uint32_t fs_total_clusters;     // count of data clusters on the volume
static int      fs_ready;              // 0 until a successful fat32_init

// A power of two has exactly one bit set, so n & (n - 1) clears it to zero.
static int is_power_of_two(uint32_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

int fat32_init(void) {
    fs_ready = 0;

    // The boot sector is one block, small enough for the stack.
    uint8_t block[DISK_SECTOR_SIZE];
    if (disk_read(FAT32_BOOT_BLOCK, 1, block) != 0) {
        print_string("FAT32: cannot read boot sector\n");
        return -1;
    }

    // Read the signature straight out of the raw block, little-endian. This is
    // the cheapest "is this even a filesystem" check, so do it before trusting
    // any other field.
    uint16_t signature = (uint16_t)(block[FAT32_SIGNATURE_OFFSET] |
                                    (block[FAT32_SIGNATURE_OFFSET + 1] << 8));
    if (signature != FAT32_SIGNATURE) {
        print_string("FAT32: no 0xAA55 boot signature, not a FAT volume\n");
        return -1;
    }

    struct fat32_bpb *bpb = (struct fat32_bpb *)block;

    // Sanity-check the geometry before computing anything from it. Proceeding
    // with garbage here would produce block numbers pointing anywhere on the
    // disk, and the failure would surface later as unreadable file contents
    // rather than as the parse error it actually is.
    if (bpb->bytes_per_sector != DISK_SECTOR_SIZE) {
        print_string("FAT32: bytes per sector is not 512\n");
        return -1;
    }
    if (!is_power_of_two(bpb->sectors_per_cluster)) {
        print_string("FAT32: sectors per cluster is not a power of two\n");
        return -1;
    }
    if (bpb->num_fats == 0 || bpb->reserved_sector_count == 0) {
        print_string("FAT32: no FAT copies or no reserved area\n");
        return -1;
    }
    // sectors_per_fat_16 is zero on FAT32 and the 32-bit field carries the real
    // value. Reading the 16-bit one is a classic mistake: it yields a FAT length
    // of zero, so the data area appears to start on top of the FAT.
    if (bpb->sectors_per_fat_32 == 0) {
        print_string("FAT32: FAT length is zero, volume is not FAT32\n");
        return -1;
    }
    if (bpb->root_cluster < FAT32_FIRST_DATA_CLUSTER) {
        print_string("FAT32: root cluster below 2\n");
        return -1;
    }

    fs_bytes_per_sector    = bpb->bytes_per_sector;
    fs_sectors_per_cluster = bpb->sectors_per_cluster;
    fs_bytes_per_cluster   = fs_bytes_per_sector * fs_sectors_per_cluster;
    fs_sectors_per_fat     = bpb->sectors_per_fat_32;
    fs_root_cluster        = bpb->root_cluster;

    // The volume is laid out as: reserved area, then num_fats copies of the FAT
    // back to back, then the data area. So the first FAT starts right after the
    // reserved area, and the data starts right after the last FAT copy.
    fs_first_fat_block  = bpb->reserved_sector_count;
    fs_first_data_block = bpb->reserved_sector_count +
                          (uint32_t)bpb->num_fats * fs_sectors_per_fat;

    if (bpb->total_sectors_32 <= fs_first_data_block) {
        print_string("FAT32: volume smaller than its own metadata\n");
        return -1;
    }
    // Data clusters are whatever is left after the metadata. This count bounds
    // chain following (see fat32_next_cluster's callers): a cluster number past
    // the end of the volume is corruption, not data.
    fs_total_clusters = (bpb->total_sectors_32 - fs_first_data_block) /
                        fs_sectors_per_cluster;

    fs_ready = 1;

    print_string("FAT32: ");
    print_int(fs_bytes_per_sector);
    print_string(" B/sector, ");
    print_int(fs_sectors_per_cluster);
    print_string(" sectors/cluster, first data block ");
    print_int(fs_first_data_block);
    print_string(", root cluster ");
    print_int(fs_root_cluster);
    print_string("\n");

    return 0;
}

// ---------------------------------------------------------------------------
// Clusters and chains.
// ---------------------------------------------------------------------------

// Where a cluster's blocks actually live.
//
// The `- 2` is not a bug. FAT slots 0 and 1 are reserved by the format and never
// describe data, so the first cluster that can hold file contents is cluster 2,
// and cluster 2 sits at offset 0 of the data area. Cluster 3 sits one cluster
// in, and so on. If file contents come back shifted by exactly two clusters'
// worth of bytes, this subtraction is missing or wrong.
static uint32_t cluster_to_block(uint32_t cluster) {
    return fs_first_data_block +
           (cluster - FAT32_FIRST_DATA_CLUSTER) * fs_sectors_per_cluster;
}

// Is this a cluster number the volume could actually contain? Data clusters are
// numbered from 2 up to total_clusters + 1. Anything else is corruption, and
// following it would read blocks belonging to something else, or off the end of
// the disk entirely.
static int cluster_in_range(uint32_t cluster) {
    return cluster >= FAT32_FIRST_DATA_CLUSTER &&
           cluster < fs_total_clusters + FAT32_FIRST_DATA_CLUSTER;
}

// Look up one cluster's FAT entry and report what follows it.
//
// The FAT is a flat array of 32-bit entries starting at the first FAT block, so
// finding an entry is a division: which block of the table holds it, and where
// in that block it sits.
//
// Only the first FAT copy is consulted. The format keeps num_fats identical
// copies for redundancy, and reading needs just one of them. A writer would have
// to update every copy, or they disagree and the volume is corrupt.
//
// Returns FAT32_CHAIN_NEXT with *next set, FAT32_CHAIN_END at the end of the
// chain, or FAT32_CHAIN_ERROR on a read failure or a corrupt entry.
static int fat32_next_cluster(uint32_t cluster, uint32_t *next) {
    uint32_t entries_per_block = fs_bytes_per_sector / FAT32_ENTRY_BYTES;
    uint32_t block  = fs_first_fat_block + (cluster / entries_per_block);
    uint32_t offset = (cluster % entries_per_block) * FAT32_ENTRY_BYTES;

    // One block, so this stays on the stack. Cluster buffers are heap-allocated
    // (see fat32_read_file) because a cluster can be far larger than a block.
    uint8_t block_buf[DISK_SECTOR_SIZE];
    if (disk_read(block, 1, block_buf) != 0) {
        return FAT32_CHAIN_ERROR;
    }

    // Assembled byte by byte rather than cast through a uint32_t pointer: the
    // entry is little-endian on disk and its offset need not be 4-byte aligned.
    uint32_t entry = (uint32_t)block_buf[offset] |
                     ((uint32_t)block_buf[offset + 1] << 8) |
                     ((uint32_t)block_buf[offset + 2] << 16) |
                     ((uint32_t)block_buf[offset + 3] << 24);

    entry &= FAT32_ENTRY_MASK;   // the top 4 bits are reserved, never compared

    if (entry >= FAT32_CLUSTER_END) {
        return FAT32_CHAIN_END;
    }
    if (entry == FAT32_CLUSTER_FREE || entry == FAT32_CLUSTER_BAD ||
        !cluster_in_range(entry)) {
        // A live chain never points at a free or bad cluster, and never off the
        // volume. Refuse it rather than read whatever happens to be there.
        return FAT32_CHAIN_ERROR;
    }

    *next = entry;
    return FAT32_CHAIN_NEXT;
}

// Read one whole cluster into buf, which must hold bytes_per_cluster bytes.
// A single disk_read call by construction; see FAT32_MAX_SECTORS_PER_CLUSTER.
static int read_cluster(uint32_t cluster, uint8_t *buf) {
    if (!cluster_in_range(cluster)) {
        return -1;
    }
    return disk_read(cluster_to_block(cluster),
                     (uint8_t)fs_sectors_per_cluster, buf);
}

// ---------------------------------------------------------------------------
// 8.3 names.
// ---------------------------------------------------------------------------

static char to_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// Convert a caller's name ("HELLO.TXT") into the 11-byte on-disk form
// ("HELLO   TXT"): base name padded to 8 with spaces, extension padded to 3, no
// dot stored, uppercase. Returns 0 on success, -1 if the name cannot be
// expressed in 8.3 (too long a base or extension, or empty). A name this
// rejects is one MiniOS cannot see at all, since long-filename entries are
// skipped when a directory is walked.
static int name_to_83(const char *name, char out[FAT32_NAME_LENGTH]) {
    for (int i = 0; i < FAT32_NAME_LENGTH; i++) {
        out[i] = ' ';
    }

    int i = 0;
    int written = 0;
    while (name[i] != '\0' && name[i] != '.') {
        if (written >= FAT32_BASE_LENGTH) {
            return -1;   // base name too long for 8.3
        }
        out[written++] = to_upper(name[i]);
        i++;
    }
    if (written == 0) {
        return -1;       // no base name at all
    }

    if (name[i] == '.') {
        i++;
        written = 0;
        while (name[i] != '\0') {
            if (written >= FAT32_EXT_LENGTH) {
                return -1;   // extension too long for 8.3
            }
            out[FAT32_BASE_LENGTH + written] = to_upper(name[i]);
            written++;
            i++;
        }
    }
    return 0;
}

// Format an on-disk 11-byte name back for display: trim the padding from each
// half and put the dot back. A name with an empty extension (directories usually
// have one) gets no trailing dot.
static void name_from_83(const uint8_t raw[FAT32_NAME_LENGTH],
                         char out[FAT32_DISPLAY_NAME_MAX]) {
    int written = 0;

    for (int i = 0; i < FAT32_BASE_LENGTH && raw[i] != ' '; i++) {
        out[written++] = (char)raw[i];
    }
    if (raw[FAT32_BASE_LENGTH] != ' ') {
        out[written++] = '.';
        for (int i = 0; i < FAT32_EXT_LENGTH; i++) {
            uint8_t c = raw[FAT32_BASE_LENGTH + i];
            if (c == ' ') {
                break;
            }
            out[written++] = (char)c;
        }
    }
    out[written] = '\0';
}

static int name_equals_83(const uint8_t raw[FAT32_NAME_LENGTH],
                          const char wanted[FAT32_NAME_LENGTH]) {
    // Fixed-length and not NUL-terminated, so strcmp does not apply.
    for (int i = 0; i < FAT32_NAME_LENGTH; i++) {
        if ((char)raw[i] != wanted[i]) {
            return 0;
        }
    }
    return 1;
}

// The start cluster is split across two 16-bit fields at opposite ends of the
// entry. Recombine them; using only the low half silently works on small volumes
// and then breaks on large ones.
static uint32_t dirent_first_cluster(const struct fat32_dirent *entry) {
    return ((uint32_t)entry->first_cluster_high << 16) |
           (uint32_t)entry->first_cluster_low;
}

// Which entries are real files or directories, as opposed to format bookkeeping.
// The long-filename check must come first: an LFN entry's attribute byte has the
// volume-id bit set too, so testing that bit first would misclassify it.
static int dirent_is_usable(const struct fat32_dirent *entry) {
    if (entry->attr == FAT32_ATTR_LONG_NAME) {
        return 0;   // one fragment of a long filename, out of scope
    }
    if (entry->attr & FAT32_ATTR_VOLUME_ID) {
        return 0;   // the volume label, not a file
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Walking a directory.
// ---------------------------------------------------------------------------
// A directory is a file, so reading one means following its cluster chain like
// any other. Its contents are a run of 32-byte entries.
//
// A sink for collecting file names during a directory walk, used by
// fat32_list_names (which backs SYS_LIST). When walk_directory is handed one of
// these, each usable entry's display name is appended followed by '\n' and the
// buffer is kept NUL-terminated, instead of being printed. Truncation is silent
// but safe: once a name plus its separator and terminator would not fit, that name
// and every one after it is dropped, and `count` reflects only the names written.
struct fat32_name_sink {
    char    *buf;
    uint32_t size;    // capacity of buf in bytes, including the terminating NUL
    uint32_t used;    // bytes written so far, not counting the terminating NUL
    uint32_t count;   // names actually written into the buffer
};

// Append one name plus a '\n' to the sink, keeping it NUL-terminated. Drops the
// name if it would not fit with its separator and terminator (see the sink above).
static void fat32_name_sink_add(struct fat32_name_sink *sink, const char *name) {
    uint32_t len = 0;
    while (name[len] != '\0') {
        len++;
    }
    // Room needed: len name bytes, one '\n', one '\0'. Reject rather than overrun.
    if (sink->used + len + 2 > sink->size) {
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        sink->buf[sink->used++] = name[i];
    }
    sink->buf[sink->used++] = '\n';
    sink->buf[sink->used] = '\0';   // valid C string after every append
    sink->count++;
}

// Both callers below (listing and lookup) share this walk. When wanted is NULL it
// visits every usable entry: appending each name to `sink` if one is given, else
// printing it. When wanted is non-NULL it stops at the first entry whose name
// matches and copies it to found. Returns 0 on success (for a lookup, 0 means
// found), -1 on a read error or a corrupt chain, and 1 when a lookup completed
// without finding the name.
#define FAT32_DIR_NOT_FOUND 1

static int walk_directory(uint32_t dir_cluster,
                          const char *wanted,
                          struct fat32_dirent *found,
                          struct fat32_name_sink *sink) {
    uint8_t *cluster_buf = (uint8_t *)kmalloc(fs_bytes_per_cluster);
    if (cluster_buf == NULL) {
        return -1;
    }

    uint32_t cluster = dir_cluster;
    int result = wanted ? FAT32_DIR_NOT_FOUND : 0;

    // Bounded, deliberately. A corrupt or self-referential chain would otherwise
    // spin here forever, and a filesystem that hangs the machine on bad data is
    // worse than one that reports an error. No valid chain can be longer than
    // the number of data clusters on the volume, so running past that bound is
    // proof of corruption (checked after the loop). Same reasoning as the disk
    // driver's bounded polling loops.
    uint32_t steps;
    for (steps = 0; steps <= fs_total_clusters; steps++) {
        if (read_cluster(cluster, cluster_buf) != 0) {
            result = -1;
            break;
        }

        uint32_t entries = fs_bytes_per_cluster / FAT32_DIRENT_BYTES;
        int done = 0;

        for (uint32_t i = 0; i < entries; i++) {
            struct fat32_dirent *entry =
                (struct fat32_dirent *)(cluster_buf + i * FAT32_DIRENT_BYTES);

            if (entry->name[0] == FAT32_DIRENT_FREE) {
                // Never-used entry: everything after it is unused too, so this
                // is the end of the directory, not just a gap.
                done = 1;
                break;
            }
            if (entry->name[0] == FAT32_DIRENT_DELETED) {
                continue;   // a hole left by a deleted file, keep scanning
            }
            if (!dirent_is_usable(entry)) {
                continue;
            }

            if (wanted) {
                if (name_equals_83(entry->name, wanted)) {
                    *found = *entry;
                    result = 0;
                    done = 1;
                    break;
                }
                continue;
            }

            char display[FAT32_DISPLAY_NAME_MAX];
            name_from_83(entry->name, display);
            if (sink) {
                fat32_name_sink_add(sink, display);
            } else {
                print_string(display);
                if (entry->attr & FAT32_ATTR_DIRECTORY) {
                    print_string("  <DIR>\n");
                } else {
                    print_string("  ");
                    print_int(entry->size);
                    print_string(" bytes\n");
                }
            }
        }

        if (done) {
            break;
        }

        uint32_t next;
        int step = fat32_next_cluster(cluster, &next);
        if (step == FAT32_CHAIN_END) {
            break;                  // directory ended on a cluster boundary
        }
        if (step == FAT32_CHAIN_ERROR) {
            result = -1;
            break;
        }
        cluster = next;
    }

    if (steps > fs_total_clusters) {
        // Followed more clusters than the volume holds, so the chain loops.
        print_string("FAT32: directory chain exceeds volume size\n");
        result = -1;
    }

    kfree(cluster_buf);
    return result;
}

int fat32_list_root(void) {
    if (!fs_ready) {
        print_string("FAT32: not initialised\n");
        return -1;
    }
    return walk_directory(fs_root_cluster, NULL, NULL, NULL);
}

// Buffer-filling sibling of fat32_list_root: instead of printing, collect the root
// directory's names into buf, one per line, NUL-terminated. This is what SYS_LIST
// hands back to a ring-3 program, which cannot see the screen the print path
// writes to. Names that do not all fit are dropped from the end (see the sink).
int fat32_list_names(char *buf, uint32_t bufsize, uint32_t *out_count) {
    if (!fs_ready || buf == NULL || bufsize == 0) {
        return -1;
    }
    struct fat32_name_sink sink = { buf, bufsize, 0, 0 };
    buf[0] = '\0';   // an empty directory yields an empty string, not garbage
    if (walk_directory(fs_root_cluster, NULL, NULL, &sink) != 0) {
        return -1;
    }
    if (out_count) {
        *out_count = sink.count;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Finding a file.
// ---------------------------------------------------------------------------

// Look up one name in the root directory and copy its entry out. Shared by
// fat32_stat and fat32_read_file so there is exactly one lookup path.
//
// Root directory only. The walk itself takes any starting cluster and would read
// a subdirectory's entries just as happily, but this interface takes a bare name
// with no path to split, so there is no way to say which directory. Path lookup
// is future work alongside TODO(fat32-write).
//
// Returns 0 on success, -1 if the filesystem is not ready, the name is not
// expressible in 8.3, the name is not present, or the entry is a directory
// rather than a file.
static int lookup_root_file(const char *name, struct fat32_dirent *out) {
    if (!fs_ready) {
        return -1;
    }

    char wanted[FAT32_NAME_LENGTH];
    if (name_to_83(name, wanted) != 0) {
        return -1;   // not expressible in 8.3, so nothing on disk can match it
    }

    if (walk_directory(fs_root_cluster, wanted, out, NULL) != 0) {
        return -1;   // not found, or the directory could not be read
    }
    if (out->attr & FAT32_ATTR_DIRECTORY) {
        return -1;   // a directory's contents are entries, not file bytes
    }
    return 0;
}

int fat32_stat(const char *name, uint32_t *out_size) {
    // Answers "how big is this file" without reading a byte of it. A caller that
    // wants a file's contents has to allocate a buffer first, which means knowing
    // the size first, and the only honest alternative (a fixed buffer that is
    // assumed to be big enough) is a size limit waiting to be exceeded quietly.
    // The size lives in the directory entry, so this walk costs the same as a
    // lookup and touches no file data at all.
    struct fat32_dirent entry;
    if (lookup_root_file(name, &entry) != 0) {
        return -1;
    }
    if (out_size) {
        *out_size = entry.size;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Reading a file.
// ---------------------------------------------------------------------------

int fat32_read_file(const char *name, void *buf, uint32_t bufsize,
                    uint32_t *out_size) {
    struct fat32_dirent entry;
    if (lookup_root_file(name, &entry) != 0) {
        return -1;
    }

    // The chain says which clusters hold the file; the directory entry's size is
    // what says where the real data ends inside the last one.
    uint32_t size = entry.size;
    if (size > bufsize) {
        return -1;
    }
    if (out_size) {
        *out_size = size;
    }
    if (size == 0) {
        return 0;    // an empty file may not have a start cluster at all
    }

    uint32_t cluster = dirent_first_cluster(&entry);
    if (!cluster_in_range(cluster)) {
        return -1;
    }

    // Heap, not stack: a cluster can be far larger than a block (up to 128KB at
    // the format's maximum), and the kernel stack is not big.
    uint8_t *cluster_buf = (uint8_t *)kmalloc(fs_bytes_per_cluster);
    if (cluster_buf == NULL) {
        return -1;
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t remaining = size;
    int result = 0;

    // Bounded for the same reason walk_directory is: a looping chain must fail,
    // not hang. Delivering `size` bytes normally ends this loop long before the
    // bound is reached.
    uint32_t steps;
    for (steps = 0; steps <= fs_total_clusters && remaining > 0; steps++) {
        if (read_cluster(cluster, cluster_buf) != 0) {
            result = -1;
            break;
        }

        // Copy whole clusters until the last one, which is usually only partly
        // real data. Trimming to `remaining` is what keeps the stale bytes after
        // the end of the file (whatever occupied those blocks before) out of the
        // caller's buffer.
        uint32_t chunk = remaining < fs_bytes_per_cluster ? remaining
                                                          : fs_bytes_per_cluster;
        memcpy(out, cluster_buf, chunk);
        out += chunk;
        remaining -= chunk;

        if (remaining == 0) {
            break;   // satisfied the size, so the rest of the chain is padding
        }

        uint32_t next;
        int step = fat32_next_cluster(cluster, &next);
        if (step != FAT32_CHAIN_NEXT) {
            // Either the chain ended early (size claims more data than the
            // chain holds) or an entry was unreadable. Both mean the volume
            // disagrees with itself, so refuse rather than return a short read
            // the caller would mistake for the whole file.
            result = -1;
            break;
        }
        cluster = next;
    }

    if (remaining > 0 && result == 0) {
        result = -1;   // ran past the cluster bound: the chain loops
    }

    kfree(cluster_buf);
    return result;
}
