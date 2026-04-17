#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cad {

// String pool — deduplicates and stores unique strings in contiguous memory.
// Useful for layer names, block names, linetype names etc. to avoid
// per-entity std::string allocations during DXF/DWG parsing.
//
// Usage:
//   StringPool pool;
//   std::string_view a = pool.intern("Layer0");
//   std::string_view b = pool.intern("Layer0");  // same view, no extra storage
//   assert(a.data() == b.data());
//
class StringPool {
public:
    StringPool() = default;
    ~StringPool() = default;

    // Non-copyable (owns raw memory), movable
    StringPool(const StringPool&) = delete;
    StringPool& operator=(const StringPool&) = delete;
    StringPool(StringPool&&) noexcept = default;
    StringPool& operator=(StringPool&&) noexcept = default;

    // Intern a string: returns a string_view to the stored copy.
    // If the string already exists, returns the existing view (deduplication).
    // The returned view remains valid until reset() or pool destruction.
    std::string_view intern(const std::string& str);
    std::string_view intern(std::string_view str);

    // Number of unique strings stored.
    size_t size() const;

    // Total bytes used for string storage (excluding overhead).
    size_t memory_used() const;

    // Clear all stored strings.
    void reset();

private:
    // Contiguous character storage. Each interned string is copied into
    // this buffer with a null terminator so string_view users can also
    // treat the data as C strings if needed.
    std::vector<char> m_buffer;

    // Hash map from string_view -> offset into m_buffer.
    // This gives O(1) lookup for deduplication.
    std::unordered_map<std::string_view, size_t> m_index;

    // Find an existing interned string, or npos if not found.
    static constexpr size_t npos = static_cast<size_t>(-1);
    size_t find(std::string_view str) const;

    // Append a new string to the buffer and return its string_view.
    std::string_view append(std::string_view str);
};

} // namespace cad
