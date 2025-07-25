#include <stddef.h>
#include <stdint.h>
#include <lib/part.h>
#include <drivers/disk.h>
#include <lib/libc.h>
#include <lib/blib.h>
#include <lib/real.h>
#include <lib/print.h>
#include <mm/pmm.h>

#define NO_PARTITION  (-1)
#define INVALID_TABLE (-2)
#define END_OF_TABLE  (-3)

struct gpt_table_header {
    // the head
    char     signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t _reserved0;

    // the partitioning info
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;

    // the guid
    struct guid disk_guid;

    // entries related
    uint64_t partition_entry_lba;
    uint32_t number_of_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_array_crc32;
} __attribute__((packed));

struct gpt_entry {
    struct guid partition_type_guid;

    struct guid unique_partition_guid;

    uint64_t starting_lba;
    uint64_t ending_lba;

    uint64_t attributes;

    uint16_t partition_name[36];
} __attribute__((packed));

static int gpt_get_part(struct part *ret, int drive, int partition) {
    struct gpt_table_header header = {0};

    // read header, located after the first block
    read(drive, &header, 512, sizeof(header));

    // check the header
    // 'EFI PART'
    if (strncmp(header.signature, "EFI PART", 8))
        return INVALID_TABLE;
    if (header.revision != 0x00010000)
        return END_OF_TABLE;

    // parse the entries if reached here
    if ((uint32_t)partition >= header.number_of_partition_entries)
        return END_OF_TABLE;

    struct gpt_entry entry = {0};
    read(drive, &entry,
         (header.partition_entry_lba * 512) + (partition * sizeof(entry)),
         sizeof(entry));

    struct guid empty_guid = {0};
    if (!memcmp(&entry.unique_partition_guid, &empty_guid, sizeof(struct guid)))
        return NO_PARTITION;

    ret->first_sect = entry.starting_lba;
    ret->sect_count = (entry.ending_lba - entry.starting_lba) + 1;
    ret->guid       = entry.unique_partition_guid;

    return 0;
}

struct mbr_entry {
	uint8_t status;
	uint8_t chs_first_sect[3];
	uint8_t type;
	uint8_t chs_last_sect[3];
	uint32_t first_sect;
	uint32_t sect_count;
} __attribute__((packed));

static int mbr_get_part(struct part *ret, int drive, int partition) {
    // Check if actually valid mbr
    uint16_t hint;
    read(drive, &hint, 444, sizeof(uint16_t));
    if (hint && hint != 0x5a5a)
        return INVALID_TABLE;

    if (partition > 3)
        return END_OF_TABLE;

    uint32_t disk_signature;
    read(drive, &disk_signature, 440, sizeof(uint32_t));

    struct mbr_entry entry;
    size_t entry_offset = 0x1be + sizeof(struct mbr_entry) * partition;

    int r = read(drive, &entry, entry_offset, sizeof(struct mbr_entry));
    if (r)
        return r;

    if (entry.type == 0)
        return NO_PARTITION;

    ret->first_sect = entry.first_sect;
    ret->sect_count = entry.sect_count;

    // We use the same method of generating GUIDs for MBR partitions as Linux
    struct guid empty_guid = {0};
    ret->guid   = empty_guid;
    ret->guid.a = disk_signature;
    ret->guid.b = (partition + 1) << 8;

    return 0;
}

int get_part(struct part *part, int drive, int partition) {
    int ret;

    ret = gpt_get_part(part, drive, partition);
    if (ret != INVALID_TABLE)
        return ret;

    ret = mbr_get_part(part, drive, partition);
    if (ret != INVALID_TABLE)
        return ret;

    return -1;
}

static struct part *part_index = NULL;
static size_t part_index_i = 0;

void part_create_index(void) {
    part_index = conv_mem_alloc(0);

    for (uint8_t drive = 0x80; drive < 0x8f; drive++) {
        struct rm_regs r = {0};
        struct bios_drive_params drive_params;

        r.eax = 0x4800;
        r.edx = drive;
        r.ds  = rm_seg(&drive_params);
        r.esi = rm_off(&drive_params);

        drive_params.buf_size = sizeof(struct bios_drive_params);

        rm_int(0x13, &r, &r);

        if (r.eflags & EFLAGS_CF)
            continue;

        print("Found BIOS drive %x\n", drive);
        print(" ... %X total %u-byte sectors\n",
              drive_params.lba_count, drive_params.bytes_per_sect);

        size_t part_count = 0;
        for (int part = 0; ; part++) {
            struct part p;
            int ret = get_part(&p, drive, part);

            if (ret == END_OF_TABLE)
                break;
            if (ret == NO_PARTITION)
                continue;

            part_count++;
        }

        part_index = conv_mem_alloc(sizeof(struct part) * part_count);

        for (int part = 0; ; part++) {
            struct part p;
            int ret = get_part(&p, drive, part);

            if (ret == END_OF_TABLE)
                break;
            if (ret == NO_PARTITION)
                continue;

            part_index[part_index_i++] = p;
        }
    }
}
