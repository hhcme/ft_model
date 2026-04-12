#pragma once
#include <cstddef>
#include <cstdint>

namespace cad {

// Arena allocator — all allocations from a contiguous memory block
// Used during DXF/DWG parsing for zero per-entity malloc overhead
// TODO: Phase 3 implementation
class ArenaAllocator {
public:
    explicit ArenaAllocator(size_t block_size = 64 * 1024 * 1024); // 64MB default
    ~ArenaAllocator();

    void* allocate(size_t size, size_t alignment = 8);
    void reset();  // Reset to beginning (all previous allocations invalidated)
    size_t used() const;
    size_t capacity() const;

private:
    uint8_t* m_block = nullptr;
    size_t m_block_size = 0;
    size_t m_offset = 0;
};

} // namespace cad
