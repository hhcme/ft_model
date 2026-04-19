#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <utility>

namespace cad {

enum class DwgVersion : uint8_t;

// ============================================================
// Bit-level reader for DWG binary format
// ============================================================
class DwgBitReader {
public:
    DwgBitReader(const uint8_t* data, size_t size);

    // Basic bit operations
    uint8_t read_bit();
    uint8_t read_bits(int count);           // Read N bits as unsigned (MSB first)
    void align_to_byte();                    // Advance to next byte boundary

    // Position
    size_t bit_offset() const;
    void set_bit_offset(size_t offset);
    size_t remaining_bits() const;

    // Optional hard limit on readable bits (e.g. to stop before handle stream)
    void set_bit_limit(size_t limit);
    size_t bit_limit() const;

    // Fixed-width reads (bit-packed, MSB first)
    uint8_t  read_u8();
    uint16_t read_u16();
    uint32_t read_u32();
    int16_t  read_s16();
    int32_t  read_s32();
    float    read_float();                   // 32-bit IEEE 754
    double   read_double();                  // 64-bit IEEE 754
    uint8_t  read_raw_char();               // 8 bits, no bit packing

    // DWG variable-length encodings

    // BOT (Bit Object Type): R2010+ object type encoding per libredwg
    //   BB=00: type = read RC (1 raw byte, 0-255)
    //   BB=01: type = read RC + 0x1F0 (0x1F0-0x2EF)
    //   BB=10 or 11: type = read RS (2 raw bytes LE, 0-0x7FFF)
    uint16_t read_bot();

    // RS (Raw Short): byte-aligned 16-bit LE value
    uint16_t read_rs();

    // RL (Raw Long): byte-aligned 32-bit LE value
    uint32_t read_rl();

    // BS (BitShort): 2-bit code then value
    //   00 = read 16-bit value directly
    //   01 = read 8-bit unsigned value
    //   10 = value is 0
    //   11 = value is 256
    uint16_t read_bs();

    // BL (BitLong): 2-bit code then value
    //   00 = read 32-bit value directly
    //   01 = read 8-bit unsigned value
    //   10 = value is 0
    //   11 = NOT USED
    uint32_t read_bl();

    // BLL (BitLongLong): 3-bit length code then bytes
    //   len = (BB << 1) | B
    //   len=0 → 0, len=1 → RC, len=2 → RS, len=4 → RL
    //   other len → len bytes LE
    uint64_t read_bll();

    // BD (BitDouble): 2-bit code then value
    //   00 = read 64-bit double directly
    //   01 = value is 1.0
    //   10 = value is 0.0
    //   11 = NOT USED
    double read_bd();

    // B (single bit as bool)
    bool read_b();

    // BT (BitThickness): 2-bit code
    //   00 = read double, 01 = 0.0, 10 = 1.0, 11 = NOT USED
    double read_bt();

    // RD (Raw Double) - just a 64-bit IEEE double, no bit encoding
    double read_rd();

    // RF (Raw Float) - 16-bit IEEE 754 half-float, no bit encoding.
    // Used for R2010+ entity data where 16-bit precision suffices.
    float read_rf();

    // TV (Text Value): BL length, then single-byte chars
    std::string read_tv();

    // TU (Unicode Text): BL length, then UTF-16LE chars
    std::string read_tu();

    // T (Text): same as TV (single-byte) for pre-R2007 compatibility
    std::string read_t();

    // H (Handle / Object Reference):
    // 4-bit counter (number of bytes), then 4-bit code (reference type),
    // then counter bytes of handle value (big-endian)
    struct HandleRef {
        uint8_t code = 0;
        uint64_t value = 0;
    };
    HandleRef read_h();

    // CMC (Color): BS color index for now (simplified)
    uint16_t read_cmc();

    // Decoded CMC/ENC color with optional RGB True Color
    struct CmcColor {
        uint16_t index = 0;      // ACI color index
        uint32_t rgb = 0;        // 0x00BBGGRR, 0 if no RGB True Color
        bool has_rgb = false;
    };

    // R2004+ Encoded Color (CMC) per libredwg spec:
    //   BS(raw), where flag = raw >> 8, index = raw & 0x1ff
    //   if flag & 0x20: BL(alpha_raw)
    //   if flag & 0x40: H(handle)
    //   else if flag & 0x80: BL(rgb)
    //   if (flag & 0x41) == 0x41: T(name)
    //   if (flag & 0x42) == 0x42: T(book_name)
    // For R2007+, name/book_name are in the string stream and skipped here.
    CmcColor read_cmc_r2004(DwgVersion version);

    // BE (BitExtrusion): 3 BD values, but if first bit is 1, it's (0,0,1)
    void read_be(double& x, double& y, double& z);

    // DD (BitDouble with Default): 2-bit code
    //   00 = read double, 01 = use default value, 10 = use default value 0.0
    double read_dd(double default_value);

    // Modular encoding (used for sizes/offsets)
    // Variable-length: high bit of each byte = more bytes follow
    uint32_t read_modular_char();
    uint32_t read_modular_short();

    // 2D/3D point convenience reads
    void read_2d_point(double& x, double& y);
    void read_3d_point(double& x, double& y, double& z);

    // R2007+ string stream: extract from entity data.
    // bitsize: total entity data bits (from prepare_object's bit_limit).
    // When active, read_tv/read_tu/read_t read from string stream.
    void setup_string_stream(uint32_t bitsize);
    void clear_string_stream();

    // Accessors for transferring string stream state between readers
    bool has_string_stream() const { return m_use_string_stream; }
    bool is_r2007_plus() const { return m_is_r2007_plus; }
    size_t string_stream_bit_pos() const { return m_str_bit_pos; }
    void set_r2007_plus(bool v) { m_is_r2007_plus = v; }
    void restore_string_stream(const uint8_t* data, size_t size, size_t bit_pos) {
        m_str_data = data;
        m_str_size = size;
        m_str_bit_pos = bit_pos;
        m_use_string_stream = true;
    }

    // Error state
    bool has_error() const;

    const uint8_t* data() const { return m_data; }
    size_t data_size() const { return m_size; }

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_bit_pos = 0;
    size_t m_bit_limit = 0; // 0 means no limit (use m_size * 8)
    bool m_error = false;

    // String stream state (R2007+)
    // When m_use_string_stream is true, read_tv/read_tu/read_t
    // read text data from a separate region of the entity data.
    const uint8_t* m_str_data = nullptr;
    size_t m_str_size = 0;
    size_t m_str_bit_pos = 0;
    bool m_use_string_stream = false;
    bool m_is_r2007_plus = false;  // Set by setup_string_stream

    // String stream read helpers (operate on m_str_data/m_str_bit_pos)
    uint8_t  str_read_raw_char();
    uint16_t str_read_rs();
    uint16_t str_read_bs();
    uint32_t str_read_bl();
};

// ============================================================
// CRC utilities for DWG format
// ============================================================

// CRC-8 lookup table builder / computation
uint8_t  crc8(const uint8_t* data, size_t len);

// CRC-16/ARC as used in DWG (polynomial 0x8005, init 0xC0C1)
uint16_t crc16(const uint8_t* data, size_t len);

// ============================================================
// LZ77 decompression for R2004+ DWG sections
// ============================================================

// Decompress LZ77-compressed DWG section data.
// Returns {decompressed_data, actual_bytes_produced}.
// actual_bytes_produced <= decompressed_size. Trailing bytes are zero-padded.
std::pair<std::vector<uint8_t>, size_t> dwg_decompress(const uint8_t* data,
                                     size_t compressed_size,
                                     size_t decompressed_size);

// Decompress LZ77-compressed DWG section data into a pre-allocated buffer.
// Writes decompressed bytes starting at write_offset within output.
// Returns actual bytes written. Cross-page back-references can resolve
// against data already present in output[0..write_offset).
size_t dwg_decompress_into(const uint8_t* data,
                           size_t compressed_size,
                           uint8_t* output,
                           size_t output_size,
                           size_t write_offset);

// ============================================================
// R2004+ header metadata decryption
// ============================================================

// Decrypt header metadata at offset 0x80 in R2004+ files.
// XOR-encrypted with a key derived from the first 16 file bytes.
void dwg_decrypt_header(const uint8_t* encrypted,
                         uint8_t* decrypted,
                         size_t len,
                         const uint8_t key[16]);

} // namespace cad
