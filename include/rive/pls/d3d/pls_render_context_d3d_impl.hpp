/*
 * Copyright 2023 Rive
 */

#pragma once

#include "rive/pls/d3d/d3d11.hpp"
#include "rive/pls/pls_render_context_buffer_ring_impl.hpp"
#include <map>

namespace rive::pls
{
class PLSRenderContextD3DImpl;

// D3D backend implementation of PLSRenderTarget.
class PLSRenderTargetD3D : public PLSRenderTarget
{
public:
    ~PLSRenderTargetD3D() override {}

    void setTargetTexture(ID3D11Device*, ComPtr<ID3D11Texture2D> tex);
    ID3D11Texture2D* targetTexture() const { return m_targetTexture.Get(); }

private:
    friend class PLSRenderContextD3DImpl;

    PLSRenderTargetD3D(ID3D11Device*, size_t width, size_t height);

    ComPtr<ID3D11Texture2D> m_targetTexture;
    ComPtr<ID3D11Texture2D> m_coverageTexture;
    ComPtr<ID3D11Texture2D> m_originalDstColorTexture;
    ComPtr<ID3D11Texture2D> m_clipTexture;

    ComPtr<ID3D11UnorderedAccessView> m_targetUAV;
    ComPtr<ID3D11UnorderedAccessView> m_coverageUAV;
    ComPtr<ID3D11UnorderedAccessView> m_originalDstColorUAV;
    ComPtr<ID3D11UnorderedAccessView> m_clipUAV;
};

// D3D backend implementation of PLSRenderContextImpl.
class PLSRenderContextD3DImpl : public PLSRenderContextBufferRingImpl
{
public:
    static std::unique_ptr<PLSRenderContext> MakeContext(ComPtr<ID3D11Device>,
                                                         ComPtr<ID3D11DeviceContext>,
                                                         bool isIntel);

    rcp<PLSRenderTargetD3D> makeRenderTarget(size_t width, size_t height);

private:
    PLSRenderContextD3DImpl(ComPtr<ID3D11Device>, ComPtr<ID3D11DeviceContext>, bool isIntel);

    rcp<PLSTexture> makeImageTexture(uint32_t width,
                                     uint32_t height,
                                     uint32_t mipLevelCount,
                                     const uint8_t imageDataRGBA[]) override;

    std::unique_ptr<TexelBufferRing> makeTexelBufferRing(TexelBufferRing::Format,
                                                         size_t widthInItems,
                                                         size_t height,
                                                         size_t texelsPerItem,
                                                         int textureIdx,
                                                         TexelBufferRing::Filter) override;

    std::unique_ptr<BufferRing> makeVertexBufferRing(size_t capacity,
                                                     size_t itemSizeInBytes) override;

    std::unique_ptr<BufferRing> makePixelUnpackBufferRing(size_t capacity,
                                                          size_t itemSizeInBytes) override
    {
        // It appears impossible to update a D3D texture from a GPU buffer; implement this resource
        // directly from the main interface instead of PLSRenderContextBufferRingImpl.
        RIVE_UNREACHABLE();
    }

    void resizeSimpleColorRampsBuffer(size_t sizeInBytes) override
    {
        m_simpleColorRampsBuffer.resize(sizeInBytes / sizeof(TwoTexelRamp));
    }

    void mapSimpleColorRampsBuffer(WriteOnlyMappedMemory<TwoTexelRamp>* data) override
    {
        data->reset(m_simpleColorRampsBuffer.data(), m_simpleColorRampsBuffer.size());
    }

    void unmapSimpleColorRampsBuffer(size_t bytesWritten) override {}

    std::unique_ptr<BufferRing> makeUniformBufferRing(size_t itemSizeInBytes) override;

    void resizeGradientTexture(size_t height) override;
    void resizeTessellationTexture(size_t height) override;

    void flush(const PLSRenderContext::FlushDescriptor&) override;

    void setPipelineLayoutAndShaders(DrawType, const ShaderFeatures&);

    const bool m_isIntel;

    ComPtr<ID3D11Device> m_gpu;
    ComPtr<ID3D11DeviceContext> m_gpuContext;

    ComPtr<ID3D11Texture2D> m_gradTexture;
    ComPtr<ID3D11ShaderResourceView> m_gradTextureSRV;
    ComPtr<ID3D11RenderTargetView> m_gradTextureRTV;
    // It appears impossible to update a D3D texture from a GPU buffer, so we just write out the
    // simple gradients to an intermediate CPU-side vector.
    std::vector<TwoTexelRamp> m_simpleColorRampsBuffer;

    ComPtr<ID3D11Texture2D> m_tessTexture;
    ComPtr<ID3D11ShaderResourceView> m_tessTextureSRV;
    ComPtr<ID3D11RenderTargetView> m_tessTextureRTV;

    ComPtr<ID3D11RasterizerState> m_rasterState;
    ComPtr<ID3D11RasterizerState> m_debugWireframeState;

    ComPtr<ID3D11InputLayout> m_colorRampLayout;
    ComPtr<ID3D11VertexShader> m_colorRampVertexShader;
    ComPtr<ID3D11PixelShader> m_colorRampPixelShader;

    ComPtr<ID3D11InputLayout> m_tessellateLayout;
    ComPtr<ID3D11VertexShader> m_tessellateVertexShader;
    ComPtr<ID3D11PixelShader> m_tessellatePixelShader;

    struct DrawVertexShader
    {
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11VertexShader> shader;
    };
    std::map<uint32_t, DrawVertexShader> m_drawVertexShaders;
    std::map<uint32_t, ComPtr<ID3D11PixelShader>> m_drawPixelShaders;

    ComPtr<ID3D11Buffer> m_patchVertexBuffer;
    ComPtr<ID3D11Buffer> m_patchIndexBuffer;

    struct PerDrawUniforms
    {
        PerDrawUniforms(uint32_t baseInstance_) : baseInstance(baseInstance_) {}
        uint32_t baseInstance;
        uint32_t pad0;
        uint32_t pad1;
        uint32_t pad2;
    };
    static_assert(sizeof(PerDrawUniforms) == 16);

    ComPtr<ID3D11Buffer> m_perDrawUniforms;

    ComPtr<ID3D11SamplerState> m_linearSampler;
    ComPtr<ID3D11SamplerState> m_mipmapSampler;
};
} // namespace rive::pls