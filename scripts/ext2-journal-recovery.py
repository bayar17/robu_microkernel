import os
import struct
import sys


JOURNAL_NAME = b".robu-journal"
JOURNAL_SECTORS = 2048
JOURNAL_MAGIC = b"RBJNL001"
JOURNAL_STATE_COMMITTED = 2
TARGET_SECTOR = 2
TARGET_OFFSET = 48
TARGET_VALUE = 0x5EADBEEF


def get32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def get16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def put32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def read_at(fd, offset, length):
    data = os.pread(fd, length, offset)
    if len(data) != length:
        raise RuntimeError("short image read")
    return data


def inode_blocks(fd, inode, block_size, needed):
    entries = block_size // 4
    result = []

    def append_block(block):
        if block == 0:
            raise RuntimeError("sparse journal file")
        result.append(block)

    for index in range(12):
        if len(result) == needed:
            return result
        append_block(get32(inode, 40 + index * 4))
    indirect = get32(inode, 88)
    if indirect == 0:
        raise RuntimeError("journal indirect block is missing")
    indirect_data = read_at(fd, indirect * block_size, block_size)
    for index in range(entries):
        if len(result) == needed:
            return result
        append_block(get32(indirect_data, index * 4))
    double_indirect = get32(inode, 92)
    if double_indirect == 0:
        raise RuntimeError("journal double-indirect block is missing")
    double_data = read_at(fd, double_indirect * block_size, block_size)
    for outer in range(entries):
        if len(result) == needed:
            return result
        indirect = get32(double_data, outer * 4)
        if indirect == 0:
            raise RuntimeError("journal double-indirect entry is missing")
        indirect_data = read_at(fd, indirect * block_size, block_size)
        for inner in range(entries):
            if len(result) == needed:
                return result
            append_block(get32(indirect_data, inner * 4))
    raise RuntimeError("journal block map is too short")


def load_inode(fd, inode_number, block_size, inode_size, inodes_per_group, gdt_start):
    group = (inode_number - 1) // inodes_per_group
    index = (inode_number - 1) % inodes_per_group
    group_desc = read_at(fd, gdt_start + group * 32, 32)
    table = get32(group_desc, 8)
    return read_at(fd, table * block_size + index * inode_size, inode_size)


def find_journal_inode(fd, block_size, inode_size, inodes_per_group, gdt_start):
    root = load_inode(fd, 2, block_size, inode_size, inodes_per_group, gdt_start)
    size = get32(root, 4)
    blocks = inode_blocks(fd, root, block_size, (size + block_size - 1) // block_size)
    for block in blocks:
        data = read_at(fd, block * block_size, block_size)
        offset = 0
        while offset + 8 <= block_size:
            inode = get32(data, offset)
            record_length = get16(data, offset + 4)
            name_length = data[offset + 6]
            if record_length < 8 or offset + record_length > block_size:
                raise RuntimeError("invalid root directory entry")
            if inode and data[offset + 8:offset + 8 + name_length] == JOURNAL_NAME:
                return inode
            offset += record_length
    raise RuntimeError("journal file is missing")


def journal_layout(fd):
    superblock = read_at(fd, 1024, 1024)
    if get16(superblock, 56) != 0xEF53:
        raise RuntimeError("not an ext2 filesystem")
    block_size = 1024 << get32(superblock, 24)
    inode_size = get16(superblock, 88) if get32(superblock, 76) >= 1 else 128
    inodes_per_group = get32(superblock, 40)
    first_data_block = get32(superblock, 20)
    gdt_start = (first_data_block + 1) * block_size
    inode_number = find_journal_inode(fd, block_size, inode_size, inodes_per_group, gdt_start)
    inode = load_inode(fd, inode_number, block_size, inode_size, inodes_per_group, gdt_start)
    journal_size = get32(inode, 4)
    if journal_size < JOURNAL_SECTORS * 512:
        raise RuntimeError("journal file is too small")
    needed = (JOURNAL_SECTORS * 512 + block_size - 1) // block_size
    blocks = inode_blocks(fd, inode, block_size, needed)
    sectors_per_block = block_size // 512

    def map_sector(index):
        return blocks[index // sectors_per_block] * sectors_per_block + index % sectors_per_block

    return map_sector


def seed(path):
    with open(path, "r+b", buffering=0) as image:
        fd = image.fileno()
        map_sector = journal_layout(fd)
        payload = bytearray(read_at(fd, TARGET_SECTOR * 512, 512))
        put32(payload, TARGET_OFFSET, TARGET_VALUE)
        descriptors = bytearray(1024)
        struct.pack_into("<Q", descriptors, 0, TARGET_SECTOR)
        put32(descriptors, 8, fnv1a(payload))
        header = bytearray(512)
        header[0:8] = JOURNAL_MAGIC
        put32(header, 8, 1)
        put32(header, 12, JOURNAL_STATE_COMMITTED)
        put32(header, 16, 1)
        put32(header, 20, 1)
        put32(header, 28, fnv1a(descriptors))
        put32(header, 24, fnv1a(header))
        os.pwrite(fd, descriptors[0:512], map_sector(1) * 512)
        os.pwrite(fd, descriptors[512:1024], map_sector(2) * 512)
        os.pwrite(fd, payload, map_sector(3) * 512)
        os.pwrite(fd, header, map_sector(0) * 512)
        os.fsync(fd)


def verify(path):
    with open(path, "rb", buffering=0) as image:
        fd = image.fileno()
        map_sector = journal_layout(fd)
        payload = read_at(fd, TARGET_SECTOR * 512, 512)
        header = read_at(fd, map_sector(0) * 512, 512)
        if get32(payload, TARGET_OFFSET) != TARGET_VALUE:
            raise RuntimeError("committed journal record was not replayed")
        if any(header):
            raise RuntimeError("journal was not cleared after replay")


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("seed", "verify"):
        raise SystemExit("usage: ext2-journal-recovery.py seed|verify image")
    if sys.argv[1] == "seed":
        seed(sys.argv[2])
    else:
        verify(sys.argv[2])


if __name__ == "__main__":
    main()
