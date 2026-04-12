#pragma once
#include <cstddef>
#include <vector>

namespace cad {

// Typed object pool — reuse objects of the same type to avoid repeated allocation
// TODO: Phase 3 implementation
template<typename T>
class ObjectPool {
public:
    ObjectPool() = default;
    T* acquire();
    void release(T* obj);
    void reserve(size_t count);
    size_t size() const;

private:
    std::vector<T> m_pool;
    std::vector<T*> m_free_list;
};

} // namespace cad
