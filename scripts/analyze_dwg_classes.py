#!/usr/bin/env python3
"""Analyze DWG R2010+ classes section format."""
import struct
import io
import sys


def dwg_decompress(comp_data, decomp_size):
    src = io.BytesIO(comp_data)
    dst = bytearray(decomp_size)
    dst_pos = 0

    def read_byte():
        b = src.read(1)
        return b[0] if b else 0

    def read_literal_length(opcode):
        length = opcode & 0x0F
        if length == 0:
            b = 0
            while (b := read_byte()) == 0:
                length += 0xFF
            length += 0x0F + b
        return length + 3

    def read_comp_bytes(opcode, mask):
        nbytes = opcode & mask
        if nbytes == 0:
            b = 0
            while (b := read_byte()) == 0:
                nbytes += 0xFF
            nbytes += mask + b
        return int(nbytes) + 2

    def copy_literal(length):
        nonlocal dst_pos
        for _ in range(length):
            if dst_pos >= decomp_size:
                break
            dst[dst_pos] = read_byte()
            dst_pos += 1
        return read_byte()

    opcode1 = read_byte()
    if (opcode1 & 0xF0) == 0:
        lit_len = read_literal_length(opcode1)
        opcode1 = copy_literal(lit_len)

    while True:
        comp_bytes = 0
        comp_offset = 0

        if opcode1 < 0x10 or opcode1 >= 0x40:
            comp_bytes = (opcode1 >> 4) - 1
            opcode2 = read_byte()
            comp_offset = (((opcode1 >> 2) & 3) | (opcode2 << 2)) + 1
        elif opcode1 >= 0x20:
            comp_bytes = read_comp_bytes(opcode1, 0x1F)
            first = read_byte()
            second = read_byte()
            comp_offset = (first >> 2) | (second << 6)
            comp_offset += 1
            opcode1 = first
        else:
            comp_bytes = read_comp_bytes(opcode1, 7)
            comp_offset = (opcode1 & 8) << 11
            first = read_byte()
            second = read_byte()
            comp_offset |= (first >> 2)
            comp_offset |= (second << 6)
            comp_offset += 0x4000
            opcode1 = first

        if comp_offset > 0 and comp_bytes > 0:
            pos = dst_pos
            end = pos + comp_bytes
            if end > decomp_size:
                end = decomp_size
            if comp_offset > pos:
                for p in range(pos, end):
                    dst[p] = 0
            else:
                for p in range(pos, end):
                    dst[p] = dst[p - comp_offset]
            dst_pos = end

        lit_length = opcode1 & 3
        if lit_length == 0:
            opcode1 = read_byte()
            if opcode1 == 0:
                break
            if (opcode1 & 0xF0) == 0:
                llen = read_literal_length(opcode1)
                opcode1 = copy_literal(llen)
        else:
            opcode1 = copy_literal(lit_length)

    return bytes(dst[:dst_pos])


def decrypt_page_header(data, page_file_offset):
    mask = 0x4164536b ^ page_file_offset
    dec = bytearray(32)
    for i in range(32):
        dec[i] = data[page_file_offset + i] ^ ((mask >> (8 * (i % 4))) & 0xFF)
    return bytes(dec)


def main():
    with open(sys.argv[1], 'rb') as f:
        data = f.read()

    print('Version:', data[:6])

    # Decrypt R2004+ header
    seed = 1
    decrypted_hdr = bytearray(108)
    for i in range(108):
        seed = (seed * 0x343FD + 0x269EC3) & 0xFFFFFFFF
        decrypted_hdr[i] = data[0x80 + i] ^ ((seed >> 16) & 0xFF)

    section_map_address = struct.unpack('<Q', decrypted_hdr[0x54:0x54+8])[0]
    section_info_id = struct.unpack('<i', decrypted_hdr[0x5C:0x5C+4])[0]

    print(f'section_map_address: {hex(section_map_address)}')
    print(f'section_info_id: {section_info_id}')

    # Read section page map
    map_offset = section_map_address + 0x100
    section_type, decomp_size, comp_size = struct.unpack('<III', data[map_offset:map_offset+12])
    comp_data = data[map_offset+20:map_offset+20+comp_size]
    page_map = dwg_decompress(comp_data, decomp_size)

    page_map_entries = []
    running_address = 0x100
    pos = 0
    while pos + 8 <= len(page_map):
        number = struct.unpack('<i', page_map[pos:pos+4])[0]
        size = struct.unpack('<I', page_map[pos+4:pos+8])[0]
        pos += 8
        if number >= 0:
            page_map_entries.append((number, size, running_address))
        else:
            pos += 16
        running_address += size

    # Find section info page
    info_entry = None
    for number, size, address in page_map_entries:
        if number == section_info_id:
            info_entry = (number, size, address)
            break

    if not info_entry:
        print("ERROR: section info page not found!")
        print(f"Available page numbers: {[e[0] for e in page_map_entries]}")
        return

    info_offset = info_entry[2]
    section_type, decomp_size, comp_size = struct.unpack('<III', data[info_offset:info_offset+12])
    comp_data = data[info_offset+20:info_offset+20+comp_size]
    info_data = dwg_decompress(comp_data, decomp_size)

    num_desc = struct.unpack('<I', info_data[0:4])[0]
    print(f'Num descriptors: {num_desc}')

    pos = 20
    section_infos = []
    for i in range(num_desc):
        if pos + 96 > len(info_data):
            break
        sz = struct.unpack('<Q', info_data[pos:pos+8])[0]
        num_sections = struct.unpack('<I', info_data[pos+8:pos+12])[0]
        max_decomp = struct.unpack('<I', info_data[pos+12:pos+16])[0]
        compressed = struct.unpack('<I', info_data[pos+20:pos+24])[0]
        sect_type = struct.unpack('<I', info_data[pos+24:pos+28])[0]
        name = info_data[pos+32:pos+96].split(b'\x00')[0].decode('ascii', errors='ignore')
        pos += 96

        pages = []
        for p in range(num_sections):
            if pos + 16 > len(info_data):
                break
            page_num = struct.unpack('<i', info_data[pos:pos+4])[0]
            page_size = struct.unpack('<I', info_data[pos+4:pos+8])[0]
            page_addr = struct.unpack('<Q', info_data[pos+8:pos+16])[0]
            pages.append((page_num, page_size, page_addr))
            pos += 16

        section_infos.append((name, sz, num_sections, max_decomp, compressed, sect_type, pages))
        print(f'  Section {i}: {name} type={sect_type} pages={num_sections} max_decomp={max_decomp} compressed={compressed}')

    # Find Classes section
    classes_pages = None
    for name, sz, num_sections, max_decomp, compressed, sect_type, pages in section_infos:
        if name in ('Classes', 'AcDb:Classes'):
            classes_pages = pages
            classes_max_decomp = max_decomp
            classes_compressed = compressed
            break

    print(f'Classes pages: {len(classes_pages)}')

    # Decompress classes section
    classes_buf = bytearray()
    for page_num, page_size, page_addr in classes_pages:
        pentry = None
        for pm_num, pm_size, pm_addr in page_map_entries:
            if pm_num == page_num:
                pentry = (pm_num, pm_size, pm_addr)
                break
        if not pentry:
            continue

        page_file_offset = pentry[2]
        dec_header = decrypt_page_header(data, page_file_offset)

        page_type = struct.unpack('<I', dec_header[0:4])[0]
        data_size = struct.unpack('<I', dec_header[8:12])[0]
        page_dsize = struct.unpack('<I', dec_header[12:16])[0]
        start_offset = struct.unpack('<I', dec_header[16:20])[0]

        page_comp = data[page_file_offset+32:page_file_offset+32+data_size]
        target = page_dsize if page_dsize > 0 else classes_max_decomp

        if classes_compressed == 2 and data_size > 0:
            decomp = dwg_decompress(page_comp, target)
        else:
            decomp = page_comp

        needed = start_offset + len(decomp)
        if needed > len(classes_buf):
            classes_buf.extend(b'\x00' * (needed - len(classes_buf)))
        classes_buf[start_offset:start_offset+len(decomp)] = decomp

    classes_data = bytes(classes_buf)
    print(f'Classes section size: {len(classes_data)}')
    print(f'First 64 bytes: {classes_data[:64].hex()}')

    # Check for sentinel
    sentinel = b'Designed by the Intelligent Pencil'
    if sentinel in classes_data:
        idx = classes_data.find(sentinel)
        print(f'Sentinel found at offset {idx}')
    else:
        print('Sentinel NOT found')

    # Check for UTF-16 strings
    for i in range(0, len(classes_data)-20, 2):
        if classes_data[i:i+12] == b'A\x00c\x00D\x00b\x00':
            print(f'Found AcDb at offset {i}')
            break

    # Try to parse header
    print('\n--- Header parsing attempts ---')

    def read_bl_at(data, bit_pos):
        code = ((data[bit_pos >> 3] >> (6 - (bit_pos & 7))) & 3)
        bit_pos += 2
        if code == 0:
            val = struct.unpack('<I', bytes([data[(bit_pos >> 3)], data[(bit_pos >> 3)+1], data[(bit_pos >> 3)+2], data[(bit_pos >> 3)+3]]))[0]
            bit_pos += 32
            return val, bit_pos
        elif code == 1:
            val = data[bit_pos >> 3]
            bit_pos += 8
            return val, bit_pos
        elif code == 2:
            return 0, bit_pos
        else:
            return None, bit_pos  # invalid

    def read_bs_at(data, bit_pos):
        code = ((data[bit_pos >> 3] >> (6 - (bit_pos & 7))) & 3)
        bit_pos += 2
        if code == 0:
            val = struct.unpack('<H', bytes([data[(bit_pos >> 3)], data[(bit_pos >> 3)+1]]))[0]
            bit_pos += 16
            return val, bit_pos
        elif code == 1:
            val = data[bit_pos >> 3]
            bit_pos += 8
            return val, bit_pos
        elif code == 2:
            return 0, bit_pos
        else:
            return 256, bit_pos

    def read_b_at(data, bit_pos):
        val = (data[bit_pos >> 3] >> (7 - (bit_pos & 7))) & 1
        return val, bit_pos + 1

    def read_rc_at(data, bit_pos):
        return data[bit_pos >> 3], bit_pos + 8

    # Try reading at bit 128 (after 16-byte sentinel)
    bit_pos = 128

    # Try: RL size
    size, bit_pos = read_bl_at(classes_data, bit_pos)
    print(f'At bit 128: size={size} bit_pos={bit_pos}')

    # Try with hsize
    bit_pos2 = bit_pos
    hsize, bit_pos2 = read_bl_at(classes_data, bit_pos2)
    print(f'  then hsize={hsize} bit_pos={bit_pos2}')

    bitsize, bit_pos2 = read_bl_at(classes_data, bit_pos2)
    print(f'  then bitsize={bitsize} bit_pos={bit_pos2}')

    max_num, bit_pos2 = read_bs_at(classes_data, bit_pos2)
    print(f'  then max_num={max_num} bit_pos={bit_pos2}')

    rc1, bit_pos2 = read_rc_at(classes_data, bit_pos2)
    rc2, bit_pos2 = read_rc_at(classes_data, bit_pos2)
    b1, bit_pos2 = read_b_at(classes_data, bit_pos2)
    print(f'  rc1={hex(rc1)} rc2={hex(rc2)} b1={b1} bit_pos={bit_pos2}')

    # Try without hsize
    bit_pos3 = bit_pos
    bitsize2, bit_pos3 = read_bl_at(classes_data, bit_pos3)
    print(f'Without hsize: bitsize={bitsize2} bit_pos={bit_pos3}')

    max_num2, bit_pos3 = read_bs_at(classes_data, bit_pos3)
    print(f'  then max_num={max_num2} bit_pos={bit_pos3}')

    rc1_2, bit_pos3 = read_rc_at(classes_data, bit_pos3)
    rc2_2, bit_pos3 = read_rc_at(classes_data, bit_pos3)
    b1_2, bit_pos3 = read_b_at(classes_data, bit_pos3)
    print(f'  rc1={hex(rc1_2)} rc2={hex(rc2_2)} b1={b1_2} bit_pos={bit_pos3}')

    # Search for plausible max_num values in the first 256 bytes
    print('\n--- Searching for plausible max_num values ---')
    for start_bit in range(128, 128 + 128):
        mn, _ = read_bs_at(classes_data, start_bit)
        if 100 <= mn <= 5000:
            print(f'  Possible max_num={mn} at bit {start_bit}')


if __name__ == '__main__':
    main()
