#pragma once
#include "VulkanPipelineCompiler.h"
#include <sstream>

namespace RevelationsPipelineReuse
{
// Audit the actual creation state, not register hashes or transient Vulkan handles.
inline std::string CreationState(const PipelineCompiler& c, const PipelineInfo& p, uint64 renderPassHash)
{
    std::ostringstream out;
    out.precision(9);
    const auto add = [&out](const auto&... values) { ((out << values << ','), ...); out << ';'; };
    for (const auto* shader : {p.vertexShader, p.geometryShader, p.pixelShader})
    {
        add(shader != nullptr);
        if (shader) add(shader->baseHash, shader->auxHash);
    }
    add(renderPassHash, c.m_requestRobustBufferAccess, static_cast<uint32>(p.primitiveMode));
    add(c.vertexInputInfo.flags, c.vertexInputBindingDescription.size(), c.vertexInputAttributeDescription.size());
    for (const auto& b : c.vertexInputBindingDescription) add(b.binding, b.stride, b.inputRate);
    for (const auto& a : c.vertexInputAttributeDescription) add(a.location, a.binding, a.format, a.offset);
    add(c.inputAssembly.flags, c.inputAssembly.topology, c.inputAssembly.primitiveRestartEnable);
    add(c.viewportState.flags, c.viewportState.viewportCount, c.viewportState.scissorCount);
    // Revelations uses dynamic viewport/scissor and no viewport extension.
    cemu_assert(c.viewportState.pNext == nullptr);
    const auto& r = c.rasterizer;
    add(r.flags, r.depthClampEnable, r.rasterizerDiscardEnable, r.polygonMode, r.cullMode, r.frontFace,
        r.depthBiasEnable, r.depthBiasConstantFactor, r.depthBiasClamp, r.depthBiasSlopeFactor, r.lineWidth);
    add(r.pNext != nullptr);
    if (r.pNext) add(c.rasterizerExt.flags, c.rasterizerExt.depthClipEnable);
    const auto& m = c.multisampling;
    add(m.flags, m.rasterizationSamples, m.sampleShadingEnable, m.minSampleShading,
        m.alphaToCoverageEnable, m.alphaToOneEnable, m.pSampleMask != nullptr);
    if (m.pSampleMask)
        for (uint32 i = 0; i < (static_cast<uint32>(m.rasterizationSamples) + 31) / 32; ++i) add(m.pSampleMask[i]);
    add(c.colorBlending.flags, c.colorBlending.logicOpEnable, c.colorBlending.logicOp, c.colorBlending.attachmentCount);
    for (const auto& a : c.colorBlendAttachments)
        add(a.blendEnable, a.srcColorBlendFactor, a.dstColorBlendFactor, a.colorBlendOp,
            a.srcAlphaBlendFactor, a.dstAlphaBlendFactor, a.alphaBlendOp, a.colorWriteMask);
    for (const auto value : c.colorBlending.blendConstants) add(value);
    const auto& d = c.depthStencilState;
    add(d.flags, d.depthTestEnable, d.depthWriteEnable, d.depthCompareOp, d.depthBoundsTestEnable,
        d.stencilTestEnable, d.minDepthBounds, d.maxDepthBounds);
    for (const auto& s : {d.front, d.back})
        add(s.failOp, s.passOp, s.depthFailOp, s.compareOp, s.compareMask, s.writeMask, s.reference);
    add(c.dynamicState.flags, c.dynamicStates.size());
    for (const auto state : c.dynamicStates) add(state);
    return out.str();
}
}
