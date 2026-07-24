#include "fat32.h"
#include "../drivers/disk.h"
#include "../drivers/screen.h"

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
// the first cluster that can hold data is cluster 2. See CLUSTER_TO_BLOCK below.
#define FAT32_FIRST_DATA_CLUSTER 2

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
