#include "disk.h"
#include "ports.h"
#include "screen.h"

// Polled PIO driver for the primary ATA bus master, LBA28 addressing.
//
// Deliberately the simplest correct disk driver: no interrupts, no DMA. The CPU
// personally copies every 16-bit word and spins waiting on the status port. This
// means a transfer blocks the whole machine (the scheduler cannot run until it
// finishes). That is slow and it freezes preemption during a transfer. It is an
// accepted limitation of PIO, not a bug. Interrupt-driven DMA is the real fix,
// left as a later layer. See docs/reference/disk.md and ADR 0013.

// Primary ATA bus I/O ports.
#define ATA_DATA         0x1F0  // 16-bit data register (read/write 256 words per block)
#define ATA_ERROR        0x1F1  // error register (read) / features (write)
#define ATA_SECTOR_COUNT 0x1F2  // number of blocks to transfer
#define ATA_LBA_LOW      0x1F3  // LBA bits 0-7
#define ATA_LBA_MID      0x1F4  // LBA bits 8-15
#define ATA_LBA_HIGH     0x1F5  // LBA bits 16-23
#define ATA_DRIVE_HEAD   0x1F6  // top 4 LBA bits + master/slave select + LBA mode
#define ATA_COMMAND      0x1F7  // command (write)
#define ATA_STATUS       0x1F7  // status (read); same port, different direction
#define ATA_ALT_STATUS   0x3F6  // alternate status (read); no side effects on read
#define ATA_DEV_CONTROL  0x3F6  // device control (write); same port, different direction

// Commands.
#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH   0xE7

// Status register bits.
#define ATA_STATUS_BSY 0x80  // busy: the drive owns the registers, do not touch them
#define ATA_STATUS_DRQ 0x08  // data request: a block is ready to move
#define ATA_STATUS_ERR 0x01  // error: the last command failed

// Device control register bit. nIEN set means "do not raise INTRQ". Because this
// driver polls the status port, it never wants the drive to assert IRQ14; setting
// nIEN keeps the interrupt line quiet so no disk IRQ ever fires.
#define ATA_CTRL_NIEN 0x02

// Drive/head base value: bit 6 sets LBA mode, bit 4 clear selects the master.
// The low 4 bits carry LBA bits 24-27, OR'd in per transfer.
#define ATA_DRIVE_MASTER_LBA 0xE0

// 256 sixteen-bit words move one 512-byte block. Getting this wrong (512 instead
// of 256) is the classic ATA PIO bug: you would read/write twice the data.
#define ATA_WORDS_PER_SECTOR (DISK_SECTOR_SIZE / 2)

// A floating (empty) ATA bus reads back 0xFF on the status port.
#define ATA_FLOATING_BUS 0xFF

// Every poll loop is bounded, never infinite. A missing or wrong-bus disk must
// fail loudly with -1, not hang the machine forever. This cap is generous enough
// for a real transfer yet still terminates. This bound is deliberate.
#define ATA_POLL_TIMEOUT 100000000

// The ATA spec requires a ~400ns settle after selecting a drive before the status
// bits are meaningful. Reading the alternate-status port four times and discarding
// the result burns exactly that long without side effects.
static void ata_400ns_delay(void) {
    for (int i = 0; i < 4; i++) {
        (void)port_byte_in(ATA_ALT_STATUS);
    }
}

// Spin until BSY clears. Return 0 on success, -1 on timeout.
static int ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < ATA_POLL_TIMEOUT; i++) {
        if ((port_byte_in(ATA_STATUS) & ATA_STATUS_BSY) == 0) {
            return 0;
        }
    }
    return -1;
}

// Spin until BSY clears and DRQ sets (a block is ready). Return 0 when ready,
// -1 on timeout or if the drive raised ERR.
static int ata_wait_for_data(void) {
    for (uint32_t i = 0; i < ATA_POLL_TIMEOUT; i++) {
        uint8_t status = port_byte_in(ATA_STATUS);
        if (status & ATA_STATUS_ERR) {
            return -1;
        }
        if ((status & ATA_STATUS_BSY) == 0 && (status & ATA_STATUS_DRQ)) {
            return 0;
        }
    }
    return -1;
}

// Program the block address and count into the ATA registers, then issue a
// read or write command. Shared setup for disk_read and disk_write.
static int ata_prepare(uint32_t lba, uint8_t count, uint8_t command) {
    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    // Select master, enable LBA mode, and load LBA bits 24-27 into the low nibble.
    port_byte_out(ATA_DRIVE_HEAD, ATA_DRIVE_MASTER_LBA | ((lba >> 24) & 0x0F));
    ata_400ns_delay();  // let the drive-select settle before the status bits matter

    port_byte_out(ATA_SECTOR_COUNT, count);

    // Split the 28-bit LBA across three 8-bit ports (bits 0-7, 8-15, 16-23).
    // The top bits 24-27 already went into the drive/head port above.
    port_byte_out(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    port_byte_out(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    port_byte_out(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));

    port_byte_out(ATA_COMMAND, command);
    return 0;
}

int disk_read(uint32_t lba, uint8_t count, void *buf) {
    if (ata_prepare(lba, count, ATA_CMD_READ_SECTORS) != 0) {
        return -1;
    }

    uint16_t *out = (uint16_t *)buf;
    for (uint8_t block = 0; block < count; block++) {
        // Each block is fetched separately: wait until this one is ready.
        if (ata_wait_for_data() != 0) {
            return -1;
        }
        for (int w = 0; w < ATA_WORDS_PER_SECTOR; w++) {
            *out++ = port_word_in(ATA_DATA);
        }
    }
    return 0;
}

int disk_write(uint32_t lba, uint8_t count, const void *buf) {
    if (ata_prepare(lba, count, ATA_CMD_WRITE_SECTORS) != 0) {
        return -1;
    }

    const uint16_t *in = (const uint16_t *)buf;
    for (uint8_t block = 0; block < count; block++) {
        if (ata_wait_for_data() != 0) {
            return -1;
        }
        for (int w = 0; w < ATA_WORDS_PER_SECTOR; w++) {
            port_word_out(ATA_DATA, *in++);
        }
    }

    // A write is not durable until the drive commits its internal buffer to the
    // platter, so flush and wait for it. A read needs no flush because reading
    // changes nothing on the disk.
    port_byte_out(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    return 0;
}

void disk_init(void) {
    // This is a polling driver, so silence the drive's interrupt line up front by
    // setting nIEN in the device control register. Without this, every completed
    // command would assert IRQ14 for no one to handle.
    port_byte_out(ATA_DEV_CONTROL, ATA_CTRL_NIEN);

    // Minimal presence check: select the master, let it settle, and read status.
    // A floating bus with no drive reads back 0xFF. A real drive reports something
    // else. A full IDENTIFY is not needed just to know a disk is there.
    port_byte_out(ATA_DRIVE_HEAD, ATA_DRIVE_MASTER_LBA);
    ata_400ns_delay();

    uint8_t status = port_byte_in(ATA_STATUS);
    if (status == ATA_FLOATING_BUS) {
        print_string("Disk: no drive on primary ATA bus\n");
        return;
    }
    print_string("Disk: primary ATA master detected\n");
}
