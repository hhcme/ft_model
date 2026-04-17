// String pool — deduplicates and stores unique strings in contiguous memory
#include "cad/memory/string_pool.h"

#include <cstring>

namespace cad {

std::string_view StringPool::intern(const std::string& str) {
    return intern(std::string_view(str));
}

std::string_view StringPool::intern(std::string_view str) {
    if (str.empty()) {
        return std::string_view{};
    }

    // Check if already interned
    size_t existing = find(str);
    if (existing != npos) {
        return std::string_view(m_buffer.data() + existing, str.size());
    }

    // Not found — append a new copy
    return append(str);
}

size_t StringPool::size() const {
    return m_index.size();
}

size_t StringPool::memory_used() const {
    return m_buffer.size();
}

void StringPool::reset() {
    m_buffer.clear();
    m_index.clear();
}

size_t StringPool::find(std::string_view str) const {
    auto it = m_index.find(str);
    if (it != m_index.end()) {
        return it->second;
    }
    return npos;
}

std::string_view StringPool::append(std::string_view str) {
    size_t offset = m_buffer.size();

    // Reserve space: string content + null terminator
    m_buffer.resize(offset + str.size() + 1);
    std::memcpy(m_buffer.data() + offset, str.data(), str.size());
    m_buffer[offset + str.size()] = '\0';

    // Create the string_view pointing into our buffer
    std::string_view sv(m_buffer.data() + offset, str.size());

    // Insert into the index
    m_index.emplace(sv, offset);

    return sv;
}

} // namespace cad
