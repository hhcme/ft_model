#include "null_device.h"

namespace cad::gfx {

bool NullDevice::initialize() {
    return true;
}

void NullDevice::shutdown() {}

void NullDevice::resize(uint32_t /*width*/, uint32_t /*height*/) {}

GfxBuffer* NullDevice::create_buffer(const BufferDesc& desc) {
    auto buf = std::make_unique<NullBuffer>(desc.size);
    auto* ptr = buf.get();
    m_buffers.push_back(std::move(buf));
    return ptr;
}

GfxPipeline* NullDevice::create_pipeline(const PipelineDesc& /*desc*/) {
    auto pipe = std::make_unique<NullPipeline>();
    auto* ptr = pipe.get();
    m_pipelines.push_back(std::move(pipe));
    return ptr;
}

void NullDevice::destroy_buffer(GfxBuffer* buffer) {
    // In a real backend this would free GPU resources.
    // NullBackend just leaks — acceptable for testing.
    (void)buffer;
}

void NullDevice::destroy_pipeline(GfxPipeline* pipeline) {
    (void)pipeline;
}

void NullDevice::begin_frame() {}
void NullDevice::end_frame() {}
void NullDevice::present() {}

} // namespace cad::gfx
