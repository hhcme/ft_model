#!/usr/bin/env python3
"""Dump DWG R2004+ section info and object map structure."""
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

    print(f'Page map decompressed: {len(page_map)} bytes (expected {decomp_size})')

    page_map_entries = {}
    running_address = 0x100
    pos = 0
    while pos + 8 <= len(page_map):
        number = struct.unpack('<i', page_map[pos:pos+4])[0]
        size = struct.unpack('<I', page_map[pos+4:pos+8])[0]
        pos += 8
        if number >= 0:
            page_map_entries[number] = (size, running_address)
        else:
            pos += 16
        running_address += size

    print(f'Page map entries: {len(page_map_entries)}')
    print(f'Max page number: {max(page_map_entries.keys())}')

    # Find section info page - if not found by exact match, scan remaining
    info_entry = None
    for number, (size, address) in sorted(page_map_entries.items()):
        if number == section_info_id:
            info_entry = (number, size, address)
            break

    if not info_entry:
        # Try scanning all entries
        print(f"section_info_id {section_info_id} not found, scanning all...")
        for number, (size, address) in sorted(page_map_entries.items()):
            print(f"  page {number}: size={size} addr={hex(address)}")
        return

    info_offset = info_entry[2]
    print(f'Info page at file offset: {hex(info_offset)}')
    section_type, decomp_size, comp_size = struct.unpack('<III', data[info_offset:info_offset+12])
    comp_data = data[info_offset+20:info_offset+20+comp_size]
    info_data = dwg_decompress(comp_data, decomp_size)

    print(f'Info data decompressed: {len(info_data)} bytes (expected {decomp_size})')

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
        print(f'  Section {i}: "{name}" type={sect_type} pages={num_sections} max_decomp={max_decomp} compressed={compressed} size={sz}')

    # Decompress and dump handles section
    for name, sz, num_sections, max_decomp, compressed, sect_type, pages in section_infos:
        if name not in ('Handles', 'AcDb:Handles'):
            continue

        print(f'\n=== Handles Section: {num_sections} pages ===')
        handles_buf = bytearray()
        for page_num, page_size, page_addr in pages:
            if page_num not in page_map_entries:
                print(f'  Page {page_num} not in page map!')
                continue
            pentry_size, pentry_addr = page_map_entries[page_num]
            page_file_offset = pentry_addr

            dec_header = decrypt_page_header(data, page_file_offset)
            pg_type = struct.unpack('<I', dec_header[0:4])[0]
            data_size = struct.unpack('<I', dec_header[8:12])[0]
            page_dsize = struct.unpack('<I', dec_header[12:16])[0]
            start_offset = struct.unpack('<I', dec_header[16:20])[0]

            page_comp = data[page_file_offset+32:page_file_offset+32+data_size]
            target = page_dsize if page_dsize > 0 else max_decomp

            if compressed == 2 and data_size > 0:
                decomp = dwg_decompress(page_comp, target)
            else:
                decomp = page_comp

            needed = start_offset + len(decomp)
            if needed > len(handles_buf):
                handles_buf.extend(b'\x00' * (needed - len(handles_buf)))
            handles_buf[start_offset:start_offset+len(decomp)] = decomp

            print(f'  Page {page_num}: file_offset={hex(page_file_offset)} start_offset={start_offset} data_size={data_size} decomp_len={len(decomp)}')

        handles_data = bytes(handles_buf)
        print(f'Handles section total size: {len(handles_data)}')
        print(f'First 32 bytes: {handles_data[:32].hex()}')

        # Parse first few entries
        pos = 0
        page_idx = 0
        while pos + 2 < len(handles_data) and page_idx < 3:
            section_size = (handles_data[pos] << 8) | handles_data[pos + 1]
            pos += 2
            if section_size <= 2:
                break
            if pos + section_size > len(handles_data):
                break
            section_data_end = pos + section_size - 2

            print(f'\n  Sub-section {page_idx}: section_size={section_size} data_end={section_data_end}')
            entry_idx = 0
            last_handle = 0
            last_offset = 0
            while pos < section_data_end and entry_idx < 10:
                # UMC handleoff
                h_delta = 0
                shift = 0
                h_bytes = 0
                while h_bytes < 4 and pos < section_data_end:
                    b = handles_data[pos]
                    pos += 1
                    h_delta |= (b & 0x7F) << shift
                    shift += 7
                    h_bytes += 1
                    if not (b & 0x80):
                        break

                if pos >= section_data_end:
                    break

                # MC offset
                o_delta = 0
                o_bytes = 0
                o_shift = 0
                negative = False
                while o_bytes < 4 and pos < section_data_end:
                    b = handles_data[pos]
                    pos += 1
                    if b & 0x80:
                        o_delta |= (b & 0x7F) << o_shift
                        o_shift += 7
                        o_bytes += 1
                    else:
                        if b & 0x40:
                            negative = True
                            o_delta |= (b & 0x3F) << o_shift
                        else:
                            o_delta |= b << o_shift
                        o_bytes += 1
                        break

                if negative:
                    o_delta = -o_delta

                last_handle += h_delta
                last_offset += o_delta

                print(f'    entry {entry_idx}: handle={last_handle} offset={last_offset} h_delta={h_delta} o_delta={o_delta}')
                entry_idx += 1

            # Skip to end of section + CRC
            pos = section_data_end + 2
            page_idx += 1


if __name__ == '__main__':
    main()
