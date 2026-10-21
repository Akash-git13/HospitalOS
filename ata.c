#include <stdint.h>

#include "ata.h"

/* Legacy IDE channel register maps. */
#define ATA_PRIMARY_IO 0x1f0u
#define ATA_PRIMARY_CONTROL 0x3f6u
#define ATA_SECONDARY_IO 0x170u
#define ATA_SECONDARY_CONTROL 0x376u

#define ATA_REG_DATA 0u
#define ATA_REG_ERROR 1u
#define ATA_REG_FEATURES 1u
#define ATA_REG_SECTOR_COUNT 2u
#define ATA_REG_LBA_LOW 3u
#define ATA_REG_LBA_MID 4u
#define ATA_REG_LBA_HIGH 5u
#define ATA_REG_DRIVE 6u
#define ATA_REG_STATUS 7u
#define ATA_REG_COMMAND 7u

#define ATA_STATUS_ERR 0x01u
#define ATA_STATUS_DRQ 0x08u
#define ATA_STATUS_DF 0x20u
#define ATA_STATUS_DRDY 0x40u
#define ATA_STATUS_BSY 0x80u

#define ATA_COMMAND_IDENTIFY 0xecu
#define ATA_COMMAND_READ_SECTORS 0x20u
#define ATA_COMMAND_WRITE_SECTORS 0x30u
#define ATA_COMMAND_CACHE_FLUSH 0xe7u

#define ATA_DEVICE_MASTER 0u
#define ATA_DEVICE_SLAVE 1u
#define ATA_LBA28_MAX 0x0fffffffu
#define ATA_POLL_LIMIT 1000000u

static struct ata_drive_info ata_drives[ATA_MAX_DRIVES];
static uint32_t ata_detected_drives;

static inline void ata_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t ata_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t ata_in16(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void ata_out16(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void ata_400ns_delay(uint16_t control) {
    (void)ata_in8(control);
    (void)ata_in8(control);
    (void)ata_in8(control);
    (void)ata_in8(control);
}

static uint8_t ata_status(uint16_t io) {
    return ata_in8((uint16_t)(io + ATA_REG_STATUS));
}

static int ata_wait_drq(uint16_t io) {
    for (uint32_t count = 0; count < ATA_POLL_LIMIT; count++) {
        uint8_t status = ata_status(io);
        if (status == 0) {
            return -1;
        }
        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0) {
            (void)ata_in8((uint16_t)(io + ATA_REG_ERROR));
            return -1;
        }
        if ((status & ATA_STATUS_BSY) == 0 && (status & ATA_STATUS_DRQ) != 0) {
            return 0;
        }
    }
    return -2;
}

static int ata_wait_idle(uint16_t io) {
    for (uint32_t count = 0; count < ATA_POLL_LIMIT; count++) {
        uint8_t status = ata_status(io);
        if (status == 0) {
            return -1;
        }
        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0) {
            (void)ata_in8((uint16_t)(io + ATA_REG_ERROR));
            return -1;
        }
        if ((status & ATA_STATUS_BSY) == 0) {
            return 0;
        }
    }
    return -2;
}

static void ata_select(uint16_t io, uint16_t control, uint8_t device,
                       uint32_t lba) {
    uint8_t drive = (uint8_t)(0xe0u | (device << 4) | ((lba >> 24) & 0x0fu));
    ata_out8((uint16_t)(io + ATA_REG_DRIVE), drive);
    ata_400ns_delay(control);
}

static int ata_identify(uint16_t io, uint16_t control, uint8_t device,
                        uint32_t *sector_count) {
    uint16_t identify[256];
    uint8_t status;

    ata_select(io, control, device, 0);
    status = ata_status(io);
    if (status == 0) {
        return -1;
    }

    ata_out8((uint16_t)(io + ATA_REG_SECTOR_COUNT), 0);
    ata_out8((uint16_t)(io + ATA_REG_LBA_MID), 0);
    ata_out8((uint16_t)(io + ATA_REG_LBA_HIGH), 0);
    ata_out8((uint16_t)(io + ATA_REG_COMMAND), ATA_COMMAND_IDENTIFY);

    if (ata_wait_drq(io) != 0) {
        return -1;
    }
    for (uint32_t word = 0; word < 256; word++) {
        identify[word] = ata_in16(io + ATA_REG_DATA);
    }

    uint32_t sectors = (uint32_t)identify[60] |
                       ((uint32_t)identify[61] << 16);
    if (sectors == 0 || sectors > ATA_LBA28_MAX) {
        return -1;
    }
    *sector_count = sectors;
    return 0;
}

static int ata_valid_drive(uint32_t drive_index) {
    return drive_index < ATA_MAX_DRIVES && ata_drives[drive_index].present;
}

void ata_initialize(void) {
    static const uint16_t io_bases[2] = {
        ATA_PRIMARY_IO,
        ATA_SECONDARY_IO,
    };
    static const uint16_t control_bases[2] = {
        ATA_PRIMARY_CONTROL,
        ATA_SECONDARY_CONTROL,
    };

    ata_detected_drives = 0;
    for (uint32_t index = 0; index < ATA_MAX_DRIVES; index++) {
        ata_drives[index].present = 0;
        ata_drives[index].channel = 0;
        ata_drives[index].device = 0;
        ata_drives[index].sector_count = 0;
    }

    for (uint32_t channel = 0; channel < 2; channel++) {
        for (uint32_t device = 0; device < 2; device++) {
            uint32_t index = channel * 2 + device;
            uint32_t sector_count;
            if (ata_identify(io_bases[channel], control_bases[channel],
                             (uint8_t)device, &sector_count) == 0) {
                ata_drives[index].present = 1;
                ata_drives[index].channel = (uint8_t)channel;
                ata_drives[index].device = (uint8_t)device;
                ata_drives[index].sector_count = sector_count;
                ata_detected_drives++;
            }
        }
    }
}

uint32_t ata_drive_count(void) {
    return ata_detected_drives;
}

const struct ata_drive_info *ata_drive_info(uint32_t drive_index) {
    if (!ata_valid_drive(drive_index)) {
        return (void *)0;
    }
    return &ata_drives[drive_index];
}

static int ata_transfer_sector(uint32_t drive_index, uint32_t lba,
                               void *buffer, uint8_t command) {
    static const uint16_t io_bases[2] = {
        ATA_PRIMARY_IO,
        ATA_SECONDARY_IO,
    };
    static const uint16_t control_bases[2] = {
        ATA_PRIMARY_CONTROL,
        ATA_SECONDARY_CONTROL,
    };
    if (!ata_valid_drive(drive_index) || buffer == (void *)0 ||
        lba > ATA_LBA28_MAX || lba >= ata_drives[drive_index].sector_count) {
        return -1;
    }

    uint16_t io = io_bases[ata_drives[drive_index].channel];
    uint16_t control = control_bases[ata_drives[drive_index].channel];
    ata_select(io, control, ata_drives[drive_index].device, lba);
    ata_out8((uint16_t)(io + ATA_REG_FEATURES), 0);
    ata_out8((uint16_t)(io + ATA_REG_SECTOR_COUNT), 1);
    ata_out8((uint16_t)(io + ATA_REG_LBA_LOW), (uint8_t)lba);
    ata_out8((uint16_t)(io + ATA_REG_LBA_MID), (uint8_t)(lba >> 8));
    ata_out8((uint16_t)(io + ATA_REG_LBA_HIGH), (uint8_t)(lba >> 16));
    ata_out8((uint16_t)(io + ATA_REG_COMMAND), command);

    if (ata_wait_drq(io) != 0) {
        return -2;
    }

    if (command == ATA_COMMAND_READ_SECTORS) {
        uint8_t *bytes = (uint8_t *)buffer;
        for (uint32_t word = 0; word < ATA_SECTOR_SIZE / 2; word++) {
            uint16_t value = ata_in16(io + ATA_REG_DATA);
            bytes[word * 2] = (uint8_t)value;
            bytes[word * 2 + 1] = (uint8_t)(value >> 8);
        }
        return 0;
    }

    const uint8_t *bytes = (const uint8_t *)buffer;
    for (uint32_t word = 0; word < ATA_SECTOR_SIZE / 2; word++) {
        uint16_t value = (uint16_t)bytes[word * 2] |
                         ((uint16_t)bytes[word * 2 + 1] << 8);
        ata_out16((uint16_t)(io + ATA_REG_DATA), value);
    }
    ata_out8((uint16_t)(io + ATA_REG_COMMAND), ATA_COMMAND_CACHE_FLUSH);
    return ata_wait_idle(io);
}

int ata_read_sector(uint32_t drive_index, uint32_t lba, void *buffer) {
    return ata_transfer_sector(drive_index, lba, buffer,
                               ATA_COMMAND_READ_SECTORS);
}

int ata_write_sector(uint32_t drive_index, uint32_t lba, const void *buffer) {
    return ata_transfer_sector(drive_index, lba, (void *)buffer,
                               ATA_COMMAND_WRITE_SECTORS);
}
