#ifndef DISK_H
#define DISK_H

#include "../include/types.h"

// A disk is a flat array of 512-byte blocks addressed by LBA (0, 1, 2, ...).
// This driver names an exact block for every transfer. It does not track names
// or free space; that is a filesystem's job, a layer above this one.
#define DISK_SECTOR_SIZE 512

// Probe the primary ATA bus master and print whether a disk was detected.
void disk_init(void);

// Read/write `count` contiguous 512-byte blocks starting at `lba` into/from buf.
// Return 0 on success, -1 on error or timeout. buf must hold count*512 bytes.
int disk_read(uint32_t lba, uint8_t count, void *buf);
int disk_write(uint32_t lba, uint8_t count, const void *buf);

#endif
