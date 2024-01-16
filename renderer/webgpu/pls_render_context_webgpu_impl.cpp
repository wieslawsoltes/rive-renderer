/*
 * Copyright 2023 Rive
 */

#include "rive/pls/webgpu/pls_render_context_webgpu_impl.hpp"

#include "rive/pls/pls_image.hpp"
#include "shaders/constants.glsl"

#include "../out/obj/generated/spirv/color_ramp.vert.h"
#include "../out/obj/generated/spirv/color_ramp.frag.h"
#include "../out/obj/generated/spirv/tessellate.vert.h"
#include "../out/obj/generated/spirv/tessellate.frag.h"
#include "../out/obj/generated/spirv/draw_path.vert.h"
#include "../out/obj/generated/spirv/draw_path.frag.h"
#include "../out/obj/generated/spirv/draw_interior_triangles.vert.h"
#include "../out/obj/generated/spirv/draw_interior_triangles.frag.h"
#include "../out/obj/generated/spirv/draw_image_mesh.vert.h"
#include "../out/obj/generated/spirv/draw_image_mesh.frag.h"

#include "../out/obj/generated/glsl.glsl.hpp"
#include "../out/obj/generated/constants.glsl.hpp"
#include "../out/obj/generated/common.glsl.hpp"
#include "../out/obj/generated/advanced_blend.glsl.hpp"
#include "../out/obj/generated/draw_path.glsl.hpp"
#include "../out/obj/generated/draw_image_mesh.glsl.hpp"

#include <sstream>
#include <string>

#ifdef RIVE_DAWN
#include <dawn/webgpu_cpp.h>

static void enable_shader_pixel_local_storage_ext(wgpu::RenderPassEncoder, bool enabled)
{
    RIVE_UNREACHABLE();
}

static void write_texture(wgpu::Queue queue,
                          wgpu::Texture texture,
                          uint32_t bytesPerRow,
                          uint32_t width,
                          uint32_t height,
                          const void* data,
                          size_t dataSize)
{
    wgpu::ImageCopyTexture dest = {
        .texture = texture,
        .mipLevel = 0,
    };
    wgpu::TextureDataLayout layout = {
        .bytesPerRow = bytesPerRow,
    };
    wgpu::Extent3D extent = {
        .width = width,
        .height = height,
    };
    queue.WriteTexture(&dest, data, dataSize, &layout, &extent);
}

static void write_buffer(wgpu::Queue queue, wgpu::Buffer buffer, const void* data, size_t dataSize)
{
    queue.WriteBuffer(buffer, 0, data, dataSize);
}
#endif

#ifdef RIVE_WEBGPU
#include <webgpu/webgpu_cpp.h>
#include <emscripten.h>
#include <emscripten/html5_webgpu.h>

EM_JS(void, enable_shader_pixel_local_storage_ext_js, (int renderPass, bool enabled), {
    renderPass = JsValStore.get(renderPass);
    renderPass.setShaderPixelLocalStorageEnabled(Boolean(enabled));
});

static void enable_shader_pixel_local_storage_ext(wgpu::RenderPassEncoder renderPass, bool enabled)
{
    enable_shader_pixel_local_storage_ext_js(
        emscripten_webgpu_export_render_pass_encoder(renderPass.Get()),
        enabled);
}

EM_JS(void,
      write_texture_js,
      (int queue,
       int texture,
       uint32_t bytesPerRow,
       uint32_t width,
       uint32_t height,
       uintptr_t indexU8,
       size_t dataSize),
      {
          queue = JsValStore.get(queue);
          texture = JsValStore.get(texture);
          // Copy data off the WASM heap before sending it to WebGPU bindings.
          const data = new Uint8Array(dataSize);
          data.set(Module.HEAPU8.subarray(indexU8, indexU8 + dataSize));
          queue.writeTexture({texture},
                             data,
                             {bytesPerRow : bytesPerRow},
                             {width : width, height : height});
      });

static void write_texture(wgpu::Queue queue,
                          wgpu::Texture texture,
                          uint32_t bytesPerRow,
                          uint32_t width,
                          uint32_t height,
                          const void* data,
                          size_t dataSize)
{
    write_texture_js(emscripten_webgpu_export_queue(queue.Get()),
                     emscripten_webgpu_export_texture(texture.Get()),
                     bytesPerRow,
                     width,
                     height,
                     reinterpret_cast<uintptr_t>(data),
                     dataSize);
}

EM_JS(void, write_buffer_js, (int queue, int buffer, uintptr_t indexU8, size_t dataSize), {
    queue = JsValStore.get(queue);
    buffer = JsValStore.get(buffer);
    // Copy data off the WASM heap before sending it to WebGPU bindings.
    const data = new Uint8Array(dataSize);
    data.set(Module.HEAPU8.subarray(indexU8, indexU8 + dataSize));
    queue.writeBuffer(buffer, 0, data, 0, dataSize);
});

static void write_buffer(wgpu::Queue queue, wgpu::Buffer buffer, const void* data, size_t dataSize)
{
    write_buffer_js(emscripten_webgpu_export_queue(queue.Get()),
                    emscripten_webgpu_export_buffer(buffer.Get()),
                    reinterpret_cast<uintptr_t>(data),
                    dataSize);
}
#endif

namespace rive::pls
{
// Draws emulated render-pass load/store actions for EXT_shader_pixel_local_storage.
class PLSRenderContextWebGPUImpl::LoadStoreEXTPipeline
{
public:
    LoadStoreEXTPipeline(PLSRenderContextWebGPUImpl* context,
                         LoadStoreActionsEXT actions,
                         wgpu::TextureFormat framebufferFormat) :
        m_framebufferFormat(framebufferFormat)
    {
        wgpu::PipelineLayoutDescriptor pipelineLayoutDesc;
        if (actions & LoadStoreActionsEXT::clearColor)
        {
            // Create a uniform buffer binding for the clear color.
            wgpu::BindGroupLayoutEntry bindingLayouts[] = {
                {
                    .binding = 0,
                    .visibility = wgpu::ShaderStage::Fragment,
                    .buffer =
                        {
                            .type = wgpu::BufferBindingType::Uniform,
                        },
                },
            };

            wgpu::BindGroupLayoutDescriptor bindingsDesc = {
                .entryCount = std::size(bindingLayouts),
                .entries = bindingLayouts,
            };

            m_bindGroupLayout = context->m_device.CreateBindGroupLayout(&bindingsDesc);

            pipelineLayoutDesc = {
                .bindGroupLayoutCount = 1,
                .bindGroupLayouts = &m_bindGroupLayout,
            };
        }
        else
        {
            pipelineLayoutDesc = {
                .bindGroupLayoutCount = 0,
                .bindGroupLayouts = nullptr,
            };
        }

        wgpu::PipelineLayout pipelineLayout =
            context->m_device.CreatePipelineLayout(&pipelineLayoutDesc);

        wgpu::ShaderModule fragmentShader;
        std::ostringstream glsl;
        glsl << "#version 310 es\n";
        glsl << "#pragma shader_stage(fragment)\n";
        glsl << "#define " GLSL_FRAGMENT "\n";
        BuildLoadStoreEXTGLSL(glsl, actions);
        fragmentShader = m_fragmentShaderHandle.compileShaderModule(context->m_device,
                                                                    glsl.str().c_str(),
                                                                    "glsl-raw");

        wgpu::ColorTargetState colorTargetState = {
            .format = framebufferFormat,
        };

        wgpu::FragmentState fragmentState = {
            .module = fragmentShader,
            .entryPoint = "main",
            .targetCount = 1,
            .targets = &colorTargetState,
        };

        wgpu::RenderPipelineDescriptor desc = {
            .layout = pipelineLayout,
            .vertex =
                {
                    .module = context->m_loadStoreEXTVertexShader,
                    .entryPoint = "main",
                    .bufferCount = 0,
                    .buffers = nullptr,
                },
            .primitive =
                {
                    .topology = wgpu::PrimitiveTopology::TriangleStrip,
                    .frontFace = context->m_frontFaceForOnScreenDraws,
                    .cullMode = wgpu::CullMode::Back,
                },
            .fragment = &fragmentState,
        };

        m_renderPipeline = context->m_device.CreateRenderPipeline(&desc);
    }

    const wgpu::BindGroupLayout& bindGroupLayout() const
    {
        assert(m_bindGroupLayout); // We only have a bind group if there is a clear color.
        return m_bindGroupLayout;
    }

    wgpu::RenderPipeline renderPipeline(wgpu::TextureFormat framebufferFormat) const
    {
        assert(framebufferFormat == m_framebufferFormat);
        return m_renderPipeline;
    }

private:
    const wgpu::TextureFormat m_framebufferFormat RIVE_MAYBE_UNUSED;
    wgpu::BindGroupLayout m_bindGroupLayout;
    EmJsHandle m_fragmentShaderHandle;
    wgpu::RenderPipeline m_renderPipeline;
};

// Renders color ramps to the gradient texture.
class PLSRenderContextWebGPUImpl::ColorRampPipeline
{
public:
    ColorRampPipeline(wgpu::Device device)
    {
        wgpu::BindGroupLayoutEntry bindingLayouts[] = {
            {
                .binding = FLUSH_UNIFORM_BUFFER_IDX,
                .visibility = wgpu::ShaderStage::Vertex,
                .buffer =
                    {
                        .type = wgpu::BufferBindingType::Uniform,
                    },
            },
        };

        wgpu::BindGroupLayoutDescriptor bindingsDesc = {
            .entryCount = std::size(bindingLayouts),
            .entries = bindingLayouts,
        };

        m_bindGroupLayout = device.CreateBindGroupLayout(&bindingsDesc);

        wgpu::PipelineLayoutDescriptor pipelineLayoutDesc = {
            .bindGroupLayoutCount = 1,
            .bindGroupLayouts = &m_bindGroupLayout,
        };

        wgpu::PipelineLayout pipelineLayout = device.CreatePipelineLayout(&pipelineLayoutDesc);

        wgpu::ShaderModule vertexShader =
            m_vertexShaderHandle.compileSPIRVShaderModule(device,
                                                          color_ramp_vert,
                                                          std::size(color_ramp_vert));

        wgpu::VertexAttribute attrs[] = {
            {
                .format = wgpu::VertexFormat::Uint32x4,
                .offset = 0,
                .shaderLocation = 0,
            },
        };

        wgpu::VertexBufferLayout vertexBufferLayout = {
            .arrayStride = sizeof(pls::GradientSpan),
            .stepMode = wgpu::VertexStepMode::Instance,
            .attributeCount = std::size(attrs),
            .attributes = attrs,
        };

        wgpu::ShaderModule fragmentShader =
            m_fragmentShaderHandle.compileSPIRVShaderModule(device,
                                                            color_ramp_frag,
                                                            std::size(color_ramp_frag));

        wgpu::ColorTargetState colorTargetState = {
            .format = wgpu::TextureFormat::RGBA8Unorm,
        };

        wgpu::FragmentState fragmentState = {
            .module = fragmentShader,
            .entryPoint = "main",
            .targetCount = 1,
            .targets = &colorTargetState,
        };

        wgpu::RenderPipelineDescriptor desc = {
            .layout = pipelineLayout,
            .vertex =
                {
                    .module = vertexShader,
                    .entryPoint = "main",
                    .bufferCount = 1,
                    .buffers = &vertexBufferLayout,
                },
            .primitive =
                {
                    .topology = wgpu::PrimitiveTopology::TriangleStrip,
                    .frontFace = kFrontFaceForOffscreenDraws,
                    .cullMode = wgpu::CullMode::Back,
                },
            .fragment = &fragmentState,
        };

        m_renderPipeline = device.CreateRenderPipeline(&desc);
    }

    const wgpu::BindGroupLayout& bindGroupLayout() const { return m_bindGroupLayout; }
    const wgpu::RenderPipeline& renderPipeline() const { return m_renderPipeline; }

private:
    wgpu::BindGroupLayout m_bindGroupLayout;
    EmJsHandle m_vertexShaderHandle;
    EmJsHandle m_fragmentShaderHandle;
    wgpu::RenderPipeline m_renderPipeline;
};

// Renders tessellated vertices to the tessellation texture.
class PLSRenderContextWebGPUImpl::TessellatePipeline
{
public:
    TessellatePipeline(wgpu::Device device)
    {
        wgpu::BindGroupLayoutEntry bindingLayouts[] = {
            {
                .binding = PATH_BUFFER_IDX,
                .visibility = wgpu::ShaderStage::Vertex,
                .buffer =
                    {
                        .type = wgpu::BufferBindingType::ReadOnlyStorage,
                    },
            },
            {
                .binding = CONTOUR_BUFFER_IDX,
                .visibility = wgpu::ShaderStage::Vertex,
                .buffer =
                    {
                        .type = wgpu::BufferBindingType::ReadOnlyStorage,
                    },
            },
            {
                .binding = FLUSH_UNIFORM_BUFFER_IDX,
                .visibility = wgpu::ShaderStage::Vertex,
                .buffer =
                    {
                        .type = wgpu::BufferBindingType::Uniform,
                    },
            },
        };

        wgpu::BindGroupLayoutDescriptor bindingsDesc = {
            .entryCount = std::size(bindingLayouts),
            .entries = bindingLayouts,
        };

        m_bindGroupLayout = device.CreateBindGroupLayout(&bindingsDesc);

        wgpu::PipelineLayoutDescriptor pipelineLayoutDesc = {
            .bindGroupLayoutCount = 1,
            .bindGroupLayouts = &m_bindGroupLayout,
        };

        wgpu::PipelineLayout pipelineLayout = device.CreatePipelineLayout(&pipelineLayoutDesc);

        wgpu::ShaderModule vertexShader =
            m_vertexShaderHandle.compileSPIRVShaderModule(device,
                                                          tessellate_vert,
                                                          std::size(tessellate_vert));

        wgpu::VertexAttribute attrs[] = {
            {
                .format = wgpu::VertexFormat::Float32x4,
                .offset = 0,
                .shaderLocation = 0,
            },
            {
                .format = wgpu::VertexFormat::Float32x4,
                .offset = 4 * sizeof(float),
                .shaderLocation = 1,
            },
            {
                .format = wgpu::VertexFormat::Float32x4,
                .offset = 8 * sizeof(float),
                .shaderLocation = 2,
            },
            {
                .format = wgpu::VertexFormat::Uint32x4,
                .offset = 12 * sizeof(float),
                .shaderLocation = 3,
            },
        };

        wgpu::VertexBufferLayout vertexBufferLayout = {
            .arrayStride = sizeof(pls::TessVertexSpan),
            .stepMode = wgpu::VertexStepMode::Instance,
            .attributeCount = std::size(attrs),
            .attributes = attrs,
        };

        wgpu::ShaderModule fragmentShader =
            m_fragmentShaderHandle.compileSPIRVShaderModule(device,
                                                            tessellate_frag,
                                                            std::size(tessellate_frag));

        wgpu::ColorTargetState colorTargetState = {
            .format = wgpu::TextureFormat::RGBA32Uint,
        };

        wgpu::FragmentState fragmentState = {
            .module = fragmentShader,
            .entryPoint = "main",
            .targetCount = 1,
            .targets = &colorTargetState,
        };

        wgpu::RenderPipelineDescriptor desc = {
            .layout = pipelineLayout,
            .vertex =
                {
                    .module = vertexShader,
                    .entryPoint = "main",
                    .bufferCount = 1,
                    .buffers = &vertexBufferLayout,
                },
            .primitive =
                {
                    .topology = wgpu::PrimitiveTopology::TriangleList,
                    .frontFace = kFrontFaceForOffscreenDraws,
                    .cullMode = wgpu::CullMode::Back,
                },
            .fragment = &fragmentState,
        };

        m_renderPipeline = device.CreateRenderPipeline(&desc);
    }

    const wgpu::BindGroupLayout& bindGroupLayout() const { return m_bindGroupLayout; }
    const wgpu::RenderPipeline renderPipeline() const { return m_renderPipeline; }

private:
    wgpu::BindGroupLayout m_bindGroupLayout;
    EmJsHandle m_vertexShaderHandle;
    EmJsHandle m_fragmentShaderHandle;
    wgpu::RenderPipeline m_renderPipeline;
};

// Draw paths and image meshes using the gradient and tessellation textures.
class PLSRenderContextWebGPUImpl::DrawPipeline
{
public:
    DrawPipeline(PLSRenderContextWebGPUImpl* context,
                 DrawType drawType,
                 pls::ShaderFeatures shaderFeatures)
    {
        PixelLocalStorageType plsType = context->m_pixelLocalStorageType;
        wgpu::ShaderModule vertexShader, fragmentShader;
        if (plsType == PixelLocalStorageType::subpassLoad ||
            plsType == PixelLocalStorageType::EXT_shader_pixel_local_storage)
        {
            const char* language;
            const char* versionString;
            if (plsType == PixelLocalStorageType::EXT_shader_pixel_local_storage)
            {
                language = "glsl-raw";
                versionString = "#version 310 es";
            }
            else
            {
                language = "glsl";
                versionString = "#version 460";
            }

            std::ostringstream glsl;
            auto addDefine = [&glsl](const char* name) { glsl << "#define " << name << "\n"; };
            if (plsType == PixelLocalStorageType::EXT_shader_pixel_local_storage)
            {
                glsl << "#ifdef GL_EXT_shader_pixel_local_storage\n";
                addDefine(GLSL_PLS_IMPL_EXT_NATIVE);
                glsl << "#else\n";
                glsl << "#extension GL_EXT_samplerless_texture_functions : enable\n";
                addDefine(GLSL_TARGET_VULKAN);
                // If we are being compiled by SPIRV transpiler for introspection,
                // GL_EXT_shader_pixel_local_storage will not be defined.
                addDefine(GLSL_PLS_IMPL_NONE);
                glsl << "#endif\n";
            }
            else
            {
                glsl << "#extension GL_EXT_samplerless_texture_functions : enable\n";
                addDefine(GLSL_TARGET_VULKAN);
                addDefine(GLSL_PLS_IMPL_SUBPASS_LOAD);
            }
            switch (drawType)
            {
                case DrawType::midpointFanPatches:
                case DrawType::outerCurvePatches:
                    addDefine(GLSL_ENABLE_INSTANCE_INDEX);
                    if (plsType == PixelLocalStorageType::EXT_shader_pixel_local_storage)
                    {
                        // The WebGPU layer automatically searches for a uniform named
                        // "SPIRV_Cross_BaseInstance" and manages it for us.
                        addDefine(GLSL_ENABLE_SPIRV_CROSS_BASE_INSTANCE);
                    }
                    break;
                case DrawType::interiorTriangulation:
                    addDefine(GLSL_DRAW_INTERIOR_TRIANGLES);
                    break;
                case DrawType::imageRect:
                    RIVE_UNREACHABLE();
                case DrawType::imageMesh:
                    break;
                case DrawType::plsAtomicResolve:
                    RIVE_UNREACHABLE();
            }
            for (size_t i = 0; i < pls::kShaderFeatureCount; ++i)
            {
                ShaderFeatures feature = static_cast<ShaderFeatures>(1 << i);
                if (shaderFeatures & feature)
                {
                    addDefine(GetShaderFeatureGLSLName(feature));
                }
            }
            glsl << pls::glsl::glsl << '\n';
            glsl << pls::glsl::constants << '\n';
            glsl << pls::glsl::common << '\n';
            if (shaderFeatures & ShaderFeatures::ENABLE_ADVANCED_BLEND)
            {
                glsl << pls::glsl::advanced_blend << '\n';
            }
            if (context->platformFeatures().avoidFlatVaryings)
            {
                addDefine(GLSL_OPTIONALLY_FLAT);
            }
            else
            {
                glsl << "#define " GLSL_OPTIONALLY_FLAT " flat\n";
            }
            switch (drawType)
            {
                case DrawType::midpointFanPatches:
                case DrawType::outerCurvePatches:
                case DrawType::interiorTriangulation:
                    glsl << pls::glsl::draw_path << '\n';
                    break;
                case DrawType::imageRect:
                    RIVE_UNREACHABLE();
                case DrawType::imageMesh:
                    glsl << pls::glsl::draw_image_mesh << '\n';
                    break;
                case DrawType::plsAtomicResolve:
                    RIVE_UNREACHABLE();
            }

            std::ostringstream vertexGLSL;
            vertexGLSL << versionString << "\n";
            vertexGLSL << "#pragma shader_stage(vertex)\n";
            vertexGLSL << "#define " GLSL_VERTEX "\n";
            vertexGLSL << glsl.str();
            vertexShader = m_vertexShaderHandle.compileShaderModule(context->m_device,
                                                                    vertexGLSL.str().c_str(),
                                                                    language);

            std::ostringstream fragmentGLSL;
            fragmentGLSL << versionString << "\n";
            fragmentGLSL << "#pragma shader_stage(fragment)\n";
            fragmentGLSL << "#define " GLSL_FRAGMENT "\n";
            fragmentGLSL << glsl.str();
            fragmentShader = m_fragmentShaderHandle.compileShaderModule(context->m_device,
                                                                        fragmentGLSL.str().c_str(),
                                                                        language);
        }
        else
        {
            switch (drawType)
            {
                case DrawType::midpointFanPatches:
                case DrawType::outerCurvePatches:
                    vertexShader =
                        m_vertexShaderHandle.compileSPIRVShaderModule(context->m_device,
                                                                      draw_path_vert,
                                                                      std::size(draw_path_vert));
                    fragmentShader =
                        m_fragmentShaderHandle.compileSPIRVShaderModule(context->m_device,
                                                                        draw_path_frag,
                                                                        std::size(draw_path_frag));
                    break;
                case DrawType::interiorTriangulation:
                    vertexShader = m_vertexShaderHandle.compileSPIRVShaderModule(
                        context->m_device,
                        draw_interior_triangles_vert,
                        std::size(draw_interior_triangles_vert));
                    fragmentShader = m_fragmentShaderHandle.compileSPIRVShaderModule(
                        context->m_device,
                        draw_interior_triangles_frag,
                        std::size(draw_interior_triangles_frag));
                    break;
                case DrawType::imageRect:
                    RIVE_UNREACHABLE();
                case DrawType::imageMesh:
                    vertexShader = m_vertexShaderHandle.compileSPIRVShaderModule(
                        context->m_device,
                        draw_image_mesh_vert,
                        std::size(draw_image_mesh_vert));
                    fragmentShader = m_fragmentShaderHandle.compileSPIRVShaderModule(
                        context->m_device,
                        draw_image_mesh_frag,
                        std::size(draw_image_mesh_frag));
                    break;
                case DrawType::plsAtomicResolve:
                    RIVE_UNREACHABLE();
            }
        }

        for (auto framebufferFormat :
             {wgpu::TextureFormat::BGRA8Unorm, wgpu::TextureFormat::RGBA8Unorm})
        {
            int pipelineIdx = RenderPipelineIdx(framebufferFormat);
            m_renderPipelines[pipelineIdx] =
                context->makePLSDrawPipeline(drawType,
                                             framebufferFormat,
                                             vertexShader,
                                             fragmentShader,
                                             &m_renderPipelineHandles[pipelineIdx]);
        }
    }

    const wgpu::RenderPipeline renderPipeline(wgpu::TextureFormat framebufferFormat) const
    {
        return m_renderPipelines[RenderPipelineIdx(framebufferFormat)];
    }

private:
    static int RenderPipelineIdx(wgpu::TextureFormat framebufferFormat)
    {
        assert(framebufferFormat == wgpu::TextureFormat::BGRA8Unorm ||
               framebufferFormat == wgpu::TextureFormat::RGBA8Unorm);
        return framebufferFormat == wgpu::TextureFormat::BGRA8Unorm ? 1 : 0;
    }

    EmJsHandle m_vertexShaderHandle;
    EmJsHandle m_fragmentShaderHandle;
    wgpu::RenderPipeline m_renderPipelines[2];
    EmJsHandle m_renderPipelineHandles[2];
};

PLSRenderContextWebGPUImpl::PLSRenderContextWebGPUImpl(
    wgpu::Device device,
    wgpu::Queue queue,
    const PlatformFeatures& baselinePlatformFeatures,
    PixelLocalStorageType pixelLocalStorageType) :
    m_device(device),
    m_queue(queue),
    m_pixelLocalStorageType(pixelLocalStorageType),
    m_frontFaceForOnScreenDraws(wgpu::FrontFace::CW),
    m_colorRampPipeline(std::make_unique<ColorRampPipeline>(m_device)),
    m_tessellatePipeline(std::make_unique<TessellatePipeline>(m_device))
{
    m_platformFeatures = baselinePlatformFeatures;
    m_platformFeatures.invertOffscreenY = true;

    if (m_pixelLocalStorageType == PixelLocalStorageType::EXT_shader_pixel_local_storage &&
        baselinePlatformFeatures.uninvertOnScreenY)
    {
        // We will use "glsl-raw" in order to access EXT_shader_pixel_local_storage, so the WebGPU
        // layer won't actually get a chance to negate Y like it thinks it will.
        m_platformFeatures.uninvertOnScreenY = false;
        // PLS always expects CW, but in this case, we need to specify CCW. This is because the
        // WebGPU layer thinks it's going to negate Y in our shader, and will therefore also flip
        // our frontFace. However, since we will use raw-glsl shaders, the WebGPU layer won't
        // actually get a chance to negate Y like it thinks it will. Therefore, we emit the wrong
        // frontFace, in anticipation of it getting flipped into the correct frontFace on its way to
        // the driver.
        m_frontFaceForOnScreenDraws = wgpu::FrontFace::CCW;
    }
}

void PLSRenderContextWebGPUImpl::initGPUObjects()
{
    wgpu::BindGroupLayoutEntry drawBindingLayouts[] = {
        {
            .binding = TESS_VERTEX_TEXTURE_IDX,
            .visibility = wgpu::ShaderStage::Vertex,
            .texture =
                {
                    .sampleType = wgpu::TextureSampleType::Uint,
                    .viewDimension = wgpu::TextureViewDimension::e2D,
                },
        },
        {
            .binding = GRAD_TEXTURE_IDX,
            .visibility = wgpu::ShaderStage::Fragment,
            .texture =
                {
                    .sampleType = wgpu::TextureSampleType::Float,
                    .viewDimension = wgpu::TextureViewDimension::e2D,
                },
        },
        {
            .binding = IMAGE_TEXTURE_IDX,
            .visibility = wgpu::ShaderStage::Fragment,
            .texture =
                {
                    .sampleType = wgpu::TextureSampleType::Float,
                    .viewDimension = wgpu::TextureViewDimension::e2D,
                },
        },
        {
            .binding = PATH_BUFFER_IDX,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                {
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
        {
            .binding = PAINT_BUFFER_IDX,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                {
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
        {
            .binding = PAINT_AUX_BUFFER_IDX,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                {
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
        {
            .binding = CONTOUR_BUFFER_IDX,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                {
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
        {
            .binding = FLUSH_UNIFORM_BUFFER_IDX,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                {
                    .type = wgpu::BufferBindingType::Uniform,
                },
        },
        {
            .binding = IMAGE_DRAW_UNIFORM_BUFFER_IDX,
            .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
            .buffer =
                {
                    .type = wgpu::BufferBindingType::Uniform,
                    .hasDynamicOffset = true,
                    .minBindingSize = sizeof(pls::ImageDrawUniforms),
                },
        },
    };

    wgpu::BindGroupLayoutDescriptor drawBindingsDesc = {
        .entryCount = std::size(drawBindingLayouts),
        .entries = drawBindingLayouts,
    };

    m_drawBindGroupLayouts[0] = m_device.CreateBindGroupLayout(&drawBindingsDesc);

    wgpu::BindGroupLayoutEntry drawBindingSamplerLayouts[] = {
        {
            .binding = GRAD_TEXTURE_IDX,
            .visibility = wgpu::ShaderStage::Fragment,
            .sampler =
                {
                    .type = wgpu::SamplerBindingType::Filtering,
                },
        },
        {
            .binding = IMAGE_TEXTURE_IDX,
            .visibility = wgpu::ShaderStage::Fragment,
            .sampler =
                {
                    .type = wgpu::SamplerBindingType::Filtering,
                },
        },
    };

    wgpu::BindGroupLayoutDescriptor samplerBindingsDesc = {
        .entryCount = std::size(drawBindingSamplerLayouts),
        .entries = drawBindingSamplerLayouts,
    };

    m_drawBindGroupLayouts[SAMPLER_BINDINGS_SET] =
        m_device.CreateBindGroupLayout(&samplerBindingsDesc);

    wgpu::SamplerDescriptor linearSamplerDesc = {
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
        .mipmapFilter = wgpu::MipmapFilterMode::Nearest,
    };

    m_linearSampler = m_device.CreateSampler(&linearSamplerDesc);

    wgpu::SamplerDescriptor mipmapSamplerDesc = {
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
        .mipmapFilter = wgpu::MipmapFilterMode::Nearest,
    };

    m_mipmapSampler = m_device.CreateSampler(&mipmapSamplerDesc);

    wgpu::BindGroupEntry samplerBindingEntries[] = {
        {
            .binding = GRAD_TEXTURE_IDX,
            .sampler = m_linearSampler,
        },
        {
            .binding = IMAGE_TEXTURE_IDX,
            .sampler = m_mipmapSampler,
        },
    };

    wgpu::BindGroupDescriptor samplerBindGroupDesc = {
        .layout = m_drawBindGroupLayouts[SAMPLER_BINDINGS_SET],
        .entryCount = std::size(samplerBindingEntries),
        .entries = samplerBindingEntries,
    };

    m_samplerBindings = m_device.CreateBindGroup(&samplerBindGroupDesc);

    bool needsPLSTextureBindings = m_pixelLocalStorageType == PixelLocalStorageType::subpassLoad;
    if (needsPLSTextureBindings)
    {
        m_drawBindGroupLayouts[PLS_TEXTURE_BINDINGS_SET] = initPLSTextureBindGroup();
    }

    wgpu::PipelineLayoutDescriptor drawPipelineLayoutDesc = {
        .bindGroupLayoutCount = static_cast<size_t>(needsPLSTextureBindings ? 3 : 2),
        .bindGroupLayouts = m_drawBindGroupLayouts,
    };

    m_drawPipelineLayout = m_device.CreatePipelineLayout(&drawPipelineLayoutDesc);

    if (m_pixelLocalStorageType == PixelLocalStorageType::EXT_shader_pixel_local_storage)
    {
        // We have to manually implement load/store operations from a shader when using
        // EXT_shader_pixel_local_storage.
        std::ostringstream glsl;
        glsl << "#version 310 es\n";
        glsl << "#pragma shader_stage(vertex)\n";
        glsl << "#define " GLSL_VERTEX "\n";
        // If we are being compiled by SPIRV transpiler for introspection, use gl_VertexIndex
        // instead of gl_VertexID.
        glsl << "#ifndef GL_EXT_shader_pixel_local_storage\n";
        glsl << "#define gl_VertexID gl_VertexIndex\n";
        glsl << "#endif\n";
        BuildLoadStoreEXTGLSL(glsl, LoadStoreActionsEXT::none);
        m_loadStoreEXTVertexShader =
            m_loadStoreEXTVertexShaderHandle.compileShaderModule(m_device,
                                                                 glsl.str().c_str(),
                                                                 "glsl-raw");
        m_loadStoreEXTUniforms = makeUniformBufferRing(sizeof(float) * 4);
    }

    wgpu::BufferDescriptor tessSpanIndexBufferDesc = {
        .usage = wgpu::BufferUsage::Index,
        .size = sizeof(pls::kTessSpanIndices),
        .mappedAtCreation = true,
    };
    m_tessSpanIndexBuffer = m_device.CreateBuffer(&tessSpanIndexBufferDesc);
    memcpy(m_tessSpanIndexBuffer.GetMappedRange(),
           pls::kTessSpanIndices,
           sizeof(pls::kTessSpanIndices));
    m_tessSpanIndexBuffer.Unmap();

    wgpu::BufferDescriptor patchBufferDesc = {
        .usage = wgpu::BufferUsage::Vertex,
        .size = kPatchVertexBufferCount * sizeof(PatchVertex),
        .mappedAtCreation = true,
    };
    m_pathPatchVertexBuffer = m_device.CreateBuffer(&patchBufferDesc);

    patchBufferDesc.size = (kPatchIndexBufferCount * sizeof(uint16_t));
    // WebGPU buffer sizes must be multiples of 4.
    patchBufferDesc.size = math::round_up_to_multiple_of<4>(patchBufferDesc.size);
    patchBufferDesc.usage = wgpu::BufferUsage::Index;
    m_pathPatchIndexBuffer = m_device.CreateBuffer(&patchBufferDesc);

    GeneratePatchBufferData(
        reinterpret_cast<PatchVertex*>(m_pathPatchVertexBuffer.GetMappedRange()),
        reinterpret_cast<uint16_t*>(m_pathPatchIndexBuffer.GetMappedRange()));

    m_pathPatchVertexBuffer.Unmap();
    m_pathPatchIndexBuffer.Unmap();

    wgpu::TextureDescriptor nullImagePaintTextureDesc = {
        .usage = wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {1, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
    };

    m_nullImagePaintTexture = m_device.CreateTexture(&nullImagePaintTextureDesc);
    m_nullImagePaintTextureView = m_nullImagePaintTexture.CreateView();
}

PLSRenderContextWebGPUImpl::~PLSRenderContextWebGPUImpl() {}

PLSRenderTargetWebGPU::PLSRenderTargetWebGPU(wgpu::Device device,
                                             wgpu::TextureFormat framebufferFormat,
                                             uint32_t width,
                                             uint32_t height,
                                             wgpu::TextureUsage additionalTextureFlags) :
    PLSRenderTarget(width, height), m_framebufferFormat(framebufferFormat)
{
    wgpu::TextureDescriptor desc = {
        .usage = wgpu::TextureUsage::RenderAttachment | additionalTextureFlags,
        .size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
    };

    desc.format = m_framebufferFormat;
    m_originalDstColorTexture = device.CreateTexture(&desc);

    desc.format = wgpu::TextureFormat::R32Uint;
    m_coverageTexture = device.CreateTexture(&desc);
    m_clipTexture = device.CreateTexture(&desc);

    m_targetTextureView = {}; // Will be configured later by setTargetTexture().
    m_coverageTextureView = m_coverageTexture.CreateView();
    m_originalDstColorTextureView = m_originalDstColorTexture.CreateView();
    m_clipTextureView = m_clipTexture.CreateView();
}

void PLSRenderTargetWebGPU::setTargetTextureView(wgpu::TextureView textureView)
{
    m_targetTextureView = textureView;
}

rcp<PLSRenderTargetWebGPU> PLSRenderContextWebGPUImpl::makeRenderTarget(
    wgpu::TextureFormat framebufferFormat,
    uint32_t width,
    uint32_t height)
{
    return rcp(new PLSRenderTargetWebGPU(m_device,
                                         framebufferFormat,
                                         width,
                                         height,
                                         wgpu::TextureUsage::None));
}

class RenderBufferWebGPUImpl : public RenderBuffer
{
public:
    RenderBufferWebGPUImpl(wgpu::Device device,
                           wgpu::Queue queue,
                           RenderBufferType renderBufferType,
                           RenderBufferFlags renderBufferFlags,
                           size_t sizeInBytes) :
        RenderBuffer(renderBufferType, renderBufferFlags, sizeInBytes),
        m_device(device),
        m_queue(queue)
    {
        bool mappedOnceAtInitialization = flags() & RenderBufferFlags::mappedOnceAtInitialization;
        int bufferCount = mappedOnceAtInitialization ? 1 : pls::kBufferRingSize;
        wgpu::BufferDescriptor desc = {
            .usage = type() == RenderBufferType::index ? wgpu::BufferUsage::Index
                                                       : wgpu::BufferUsage::Vertex,
            // WebGPU buffer sizes must be multiples of 4.
            .size = math::round_up_to_multiple_of<4>(sizeInBytes),
            .mappedAtCreation = mappedOnceAtInitialization,
        };
        if (!mappedOnceAtInitialization)
        {
            desc.usage |= wgpu::BufferUsage::CopyDst;
        }
        for (int i = 0; i < bufferCount; ++i)
        {
            m_buffers[i] = device.CreateBuffer(&desc);
        }
    }

    wgpu::Buffer submittedBuffer() const { return m_buffers[m_submittedBufferIdx]; }

protected:
    void* onMap() override
    {
        m_submittedBufferIdx = (m_submittedBufferIdx + 1) % pls::kBufferRingSize;
        assert(m_buffers[m_submittedBufferIdx] != nullptr);
        if (flags() & RenderBufferFlags::mappedOnceAtInitialization)
        {
            return m_buffers[m_submittedBufferIdx].GetMappedRange();
        }
        else
        {
            if (m_stagingBuffer == nullptr)
            {
                m_stagingBuffer.reset(new uint8_t[sizeInBytes()]);
            }
            return m_stagingBuffer.get();
        }
    }

    void onUnmap() override
    {
        if (flags() & RenderBufferFlags::mappedOnceAtInitialization)
        {
            m_buffers[m_submittedBufferIdx].Unmap();
        }
        else
        {
            write_buffer(m_queue,
                         m_buffers[m_submittedBufferIdx],
                         m_stagingBuffer.get(),
                         sizeInBytes());
        }
    }

private:
    const wgpu::Device m_device;
    const wgpu::Queue m_queue;
    wgpu::Buffer m_buffers[pls::kBufferRingSize];
    int m_submittedBufferIdx = -1;
    std::unique_ptr<uint8_t[]> m_stagingBuffer;
};

rcp<RenderBuffer> PLSRenderContextWebGPUImpl::makeRenderBuffer(RenderBufferType type,
                                                               RenderBufferFlags flags,
                                                               size_t sizeInBytes)
{
    return make_rcp<RenderBufferWebGPUImpl>(m_device, m_queue, type, flags, sizeInBytes);
}

class PLSTextureWebGPUImpl : public PLSTexture
{
public:
    PLSTextureWebGPUImpl(wgpu::Device device,
                         wgpu::Queue queue,
                         uint32_t width,
                         uint32_t height,
                         uint32_t mipLevelCount,
                         const uint8_t imageDataRGBA[]) :
        PLSTexture(width, height)
    {
        wgpu::TextureDescriptor desc = {
            .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
            .dimension = wgpu::TextureDimension::e2D,
            .size = {width, height},
            .format = wgpu::TextureFormat::RGBA8Unorm,
            // TODO: implement mipmap generation.
            .mipLevelCount = 1, // mipLevelCount,
        };

        m_texture = device.CreateTexture(&desc);
        m_textureView = m_texture.CreateView();

        // Specify the top-level image in the mipmap chain.
        // TODO: implement mipmap generation.
        write_texture(queue,
                      m_texture,
                      width * 4,
                      width,
                      height,
                      imageDataRGBA,
                      height * width * 4);
    }

    wgpu::TextureView textureView() const { return m_textureView; }

private:
    wgpu::Texture m_texture;
    wgpu::TextureView m_textureView;
};

rcp<PLSTexture> PLSRenderContextWebGPUImpl::makeImageTexture(uint32_t width,
                                                             uint32_t height,
                                                             uint32_t mipLevelCount,
                                                             const uint8_t imageDataRGBA[])
{
    return make_rcp<PLSTextureWebGPUImpl>(m_device,
                                          m_queue,
                                          width,
                                          height,
                                          mipLevelCount,
                                          imageDataRGBA);
}

class BufferWebGPU : public BufferRing
{
public:
    static std::unique_ptr<BufferWebGPU> Make(wgpu::Device device,
                                              wgpu::Queue queue,
                                              size_t capacityInBytes,
                                              wgpu::BufferUsage usage)
    {
        capacityInBytes = std::max<size_t>(capacityInBytes, 1);
        return std::make_unique<BufferWebGPU>(device, queue, capacityInBytes, usage);
    }

    BufferWebGPU(wgpu::Device device,
                 wgpu::Queue queue,
                 size_t capacityInBytes,
                 wgpu::BufferUsage usage) :
        BufferRing(capacityInBytes), m_queue(queue)
    {
        wgpu::BufferDescriptor desc = {
            .usage = wgpu::BufferUsage::CopyDst | usage,
            .size = capacityInBytes,
        };
        for (int i = 0; i < pls::kBufferRingSize; ++i)
        {
            m_buffers[i] = device.CreateBuffer(&desc);
        }
    }

    wgpu::Buffer submittedBuffer() const { return m_buffers[submittedBufferIdx()]; }

protected:
    void* onMapBuffer(int bufferIdx, size_t mapSizeInBytes) override { return shadowBuffer(); }

    void onUnmapAndSubmitBuffer(int bufferIdx, size_t mapSizeInBytes) override
    {
        write_buffer(m_queue, m_buffers[bufferIdx], shadowBuffer(), mapSizeInBytes);
    }

private:
    const wgpu::Queue m_queue;
    wgpu::Buffer m_buffers[kBufferRingSize];
};

std::unique_ptr<BufferRing> PLSRenderContextWebGPUImpl::makeUniformBufferRing(
    size_t capacityInBytes)
{
    // Uniform blocks must be multiples of 256 bytes in size.
    capacityInBytes = std::max<size_t>(capacityInBytes, 256);
    assert(capacityInBytes % 256 == 0);
    return BufferWebGPU::Make(m_device, m_queue, capacityInBytes, wgpu::BufferUsage::Uniform);
}

std::unique_ptr<BufferRing> PLSRenderContextWebGPUImpl::makeStorageBufferRing(
    size_t capacityInBytes,
    pls::StorageBufferStructure)
{
    return BufferWebGPU::Make(m_device, m_queue, capacityInBytes, wgpu::BufferUsage::Storage);
}

std::unique_ptr<BufferRing> PLSRenderContextWebGPUImpl::makeVertexBufferRing(size_t capacityInBytes)
{
    return BufferWebGPU::Make(m_device, m_queue, capacityInBytes, wgpu::BufferUsage::Vertex);
}

std::unique_ptr<BufferRing> PLSRenderContextWebGPUImpl::makeTextureTransferBufferRing(
    size_t capacityInBytes)
{
    return BufferWebGPU::Make(m_device, m_queue, capacityInBytes, wgpu::BufferUsage::CopySrc);
}

void PLSRenderContextWebGPUImpl::resizeGradientTexture(uint32_t width, uint32_t height)
{
    width = std::max(width, 1u);
    height = std::max(height, 1u);

    wgpu::TextureDescriptor desc{
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                 wgpu::TextureUsage::CopyDst,
        .size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
        .format = wgpu::TextureFormat::RGBA8Unorm,
    };

    m_gradientTexture = m_device.CreateTexture(&desc);
    m_gradientTextureView = m_gradientTexture.CreateView();
}

void PLSRenderContextWebGPUImpl::resizeTessellationTexture(uint32_t width, uint32_t height)
{
    width = std::max(width, 1u);
    height = std::max(height, 1u);

    wgpu::TextureDescriptor desc{
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
        .format = wgpu::TextureFormat::RGBA32Uint,
    };

    m_tessVertexTexture = m_device.CreateTexture(&desc);
    m_tessVertexTextureView = m_tessVertexTexture.CreateView();
}

wgpu::RenderPipeline PLSRenderContextWebGPUImpl::makePLSDrawPipeline(
    rive::pls::DrawType drawType,
    wgpu::TextureFormat framebufferFormat,
    wgpu::ShaderModule vertexShader,
    wgpu::ShaderModule fragmentShader,
    EmJsHandle* pipelineJSHandleIfNeeded)
{
    std::vector<wgpu::VertexAttribute> attrs;
    std::vector<wgpu::VertexBufferLayout> vertexBufferLayouts;
    switch (drawType)
    {
        case DrawType::midpointFanPatches:
        case DrawType::outerCurvePatches:
        {
            attrs = {
                {
                    .format = wgpu::VertexFormat::Float32x4,
                    .offset = 0,
                    .shaderLocation = 0,
                },
                {
                    .format = wgpu::VertexFormat::Float32x4,
                    .offset = 4 * sizeof(float),
                    .shaderLocation = 1,
                },
            };

            vertexBufferLayouts = {
                {
                    .arrayStride = sizeof(pls::PatchVertex),
                    .stepMode = wgpu::VertexStepMode::Vertex,
                    .attributeCount = std::size(attrs),
                    .attributes = attrs.data(),
                },
            };
            break;
        }
        case DrawType::interiorTriangulation:
        {
            attrs = {
                {
                    .format = wgpu::VertexFormat::Float32x3,
                    .offset = 0,
                    .shaderLocation = 0,
                },
            };

            vertexBufferLayouts = {
                {
                    .arrayStride = sizeof(pls::TriangleVertex),
                    .stepMode = wgpu::VertexStepMode::Vertex,
                    .attributeCount = std::size(attrs),
                    .attributes = attrs.data(),
                },
            };
            break;
        }
        case DrawType::imageRect:
            RIVE_UNREACHABLE();
        case DrawType::imageMesh:
            attrs = {
                {
                    .format = wgpu::VertexFormat::Float32x2,
                    .offset = 0,
                    .shaderLocation = 0,
                },
                {
                    .format = wgpu::VertexFormat::Float32x2,
                    .offset = 0,
                    .shaderLocation = 1,
                },
            };

            vertexBufferLayouts = {
                {
                    .arrayStride = sizeof(float) * 2,
                    .stepMode = wgpu::VertexStepMode::Vertex,
                    .attributeCount = 1,
                    .attributes = &attrs[0],
                },
                {
                    .arrayStride = sizeof(float) * 2,
                    .stepMode = wgpu::VertexStepMode::Vertex,
                    .attributeCount = 1,
                    .attributes = &attrs[1],
                },
            };
            break;
        case DrawType::plsAtomicResolve:
            RIVE_UNREACHABLE();
    }

    wgpu::ColorTargetState colorTargets[] = {
        {.format = framebufferFormat},
        {.format = wgpu::TextureFormat::R32Uint},
        {.format = framebufferFormat},
        {.format = wgpu::TextureFormat::R32Uint},
    };

    wgpu::FragmentState fragmentState = {
        .module = fragmentShader,
        .entryPoint = "main",
        .targetCount = static_cast<size_t>(
            m_pixelLocalStorageType == PixelLocalStorageType::EXT_shader_pixel_local_storage ? 1
                                                                                             : 4),
        .targets = colorTargets,
    };

    wgpu::RenderPipelineDescriptor desc = {
        .layout = m_drawPipelineLayout,
        .vertex =
            {
                .module = vertexShader,
                .entryPoint = "main",
                .bufferCount = std::size(vertexBufferLayouts),
                .buffers = vertexBufferLayouts.data(),
            },
        .primitive =
            {
                .topology = wgpu::PrimitiveTopology::TriangleList,
                .frontFace = m_frontFaceForOnScreenDraws,
                .cullMode =
                    drawType != DrawType::imageMesh ? wgpu::CullMode::Back : wgpu::CullMode::None,
            },
        .fragment = &fragmentState,
    };

    return m_device.CreateRenderPipeline(&desc);
}

wgpu::RenderPassEncoder PLSRenderContextWebGPUImpl::makePLSRenderPass(
    wgpu::CommandEncoder encoder,
    const PLSRenderTargetWebGPU* renderTarget,
    wgpu::LoadOp loadOp,
    const wgpu::Color& clearColor,
    EmJsHandle* renderPassJSHandleIfNeeded)
{
    wgpu::RenderPassColorAttachment plsAttachments[4] = {
        {
            // framebuffer
            .view = renderTarget->m_targetTextureView,
            .loadOp = loadOp,
            .storeOp = wgpu::StoreOp::Store,
            .clearValue = clearColor,
        },
        {
            // coverage
            .view = renderTarget->m_coverageTextureView,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Discard,
            .clearValue = {},
        },
        {
            // originalDstColor
            .view = renderTarget->m_originalDstColorTextureView,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Discard,
            .clearValue = {},
        },
        {
            // clip
            .view = renderTarget->m_clipTextureView,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Discard,
            .clearValue = {},
        },
    };

    wgpu::RenderPassDescriptor passDesc = {
        .colorAttachmentCount = static_cast<size_t>(
            m_pixelLocalStorageType == PixelLocalStorageType::EXT_shader_pixel_local_storage ? 1
                                                                                             : 4),
        .colorAttachments = plsAttachments,
    };

    return encoder.BeginRenderPass(&passDesc);
}

static wgpu::Buffer webgpu_buffer(const BufferRing* bufferRing)
{
    assert(bufferRing != nullptr);
    return static_cast<const BufferWebGPU*>(bufferRing)->submittedBuffer();
}

void PLSRenderContextWebGPUImpl::flush(const FlushDescriptor& desc)
{
    auto* renderTarget = static_cast<const PLSRenderTargetWebGPU*>(desc.renderTarget);
    wgpu::CommandEncoder encoder = m_device.CreateCommandEncoder();

    // Render the complex color ramps to the gradient texture.
    if (desc.complexGradSpanCount > 0)
    {
        wgpu::BindGroupEntry bindingEntries[] = {
            {
                .binding = FLUSH_UNIFORM_BUFFER_IDX,
                .buffer = webgpu_buffer(flushUniformBufferRing()),
                .offset = desc.flushUniformDataOffsetInBytes,
            },
        };

        wgpu::BindGroupDescriptor bindGroupDesc = {
            .layout = m_colorRampPipeline->bindGroupLayout(),
            .entryCount = std::size(bindingEntries),
            .entries = bindingEntries,
        };

        wgpu::BindGroup bindings = m_device.CreateBindGroup(&bindGroupDesc);

        wgpu::RenderPassColorAttachment attachment = {
            .view = m_gradientTextureView,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Store,
            .clearValue = {},
        };

        wgpu::RenderPassDescriptor gradPassDesc = {
            .colorAttachmentCount = 1,
            .colorAttachments = &attachment,
        };

        wgpu::RenderPassEncoder gradPass = encoder.BeginRenderPass(&gradPassDesc);
        gradPass.SetViewport(0.f,
                             static_cast<double>(desc.complexGradRowsTop),
                             pls::kGradTextureWidth,
                             static_cast<float>(desc.complexGradRowsHeight),
                             0.0,
                             1.0);
        gradPass.SetPipeline(m_colorRampPipeline->renderPipeline());
        gradPass.SetVertexBuffer(0, webgpu_buffer(gradSpanBufferRing()));
        gradPass.SetBindGroup(0, bindings);
        gradPass.Draw(4, desc.complexGradSpanCount, 0, desc.firstComplexGradSpan);
        gradPass.End();
    }

    // Copy the simple color ramps to the gradient texture.
    if (desc.simpleGradTexelsHeight > 0)
    {
        wgpu::ImageCopyBuffer srcBuffer = {
            .layout =
                {
                    .offset = desc.simpleGradDataOffsetInBytes,
                    .bytesPerRow = pls::kGradTextureWidth * 4,
                },
            .buffer = webgpu_buffer(simpleColorRampsBufferRing()),
        };
        wgpu::ImageCopyTexture dstTexture = {
            .texture = m_gradientTexture,
            .origin = {0, 0, 0},
        };
        wgpu::Extent3D copySize = {
            .width = desc.simpleGradTexelsWidth,
            .height = desc.simpleGradTexelsHeight,
        };
        encoder.CopyBufferToTexture(&srcBuffer, &dstTexture, &copySize);
    }

    // Tessellate all curves into vertices in the tessellation texture.
    if (desc.tessVertexSpanCount > 0)
    {
        wgpu::BindGroupEntry bindingEntries[] = {
            {
                .binding = PATH_BUFFER_IDX,
                .buffer = webgpu_buffer(pathBufferRing()),
            },
            {
                .binding = CONTOUR_BUFFER_IDX,
                .buffer = webgpu_buffer(contourBufferRing()),
            },
            {
                .binding = FLUSH_UNIFORM_BUFFER_IDX,
                .buffer = webgpu_buffer(flushUniformBufferRing()),
                .offset = desc.flushUniformDataOffsetInBytes,
            },
        };

        wgpu::BindGroupDescriptor bindGroupDesc = {
            .layout = m_tessellatePipeline->bindGroupLayout(),
            .entryCount = std::size(bindingEntries),
            .entries = bindingEntries,
        };

        wgpu::BindGroup bindings = m_device.CreateBindGroup(&bindGroupDesc);

        wgpu::RenderPassColorAttachment attachment{
            .view = m_tessVertexTextureView,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Store,
            .clearValue = {},
        };

        wgpu::RenderPassDescriptor tessPassDesc = {
            .colorAttachmentCount = 1,
            .colorAttachments = &attachment,
        };

        wgpu::RenderPassEncoder tessPass = encoder.BeginRenderPass(&tessPassDesc);
        tessPass.SetViewport(0.f,
                             0.f,
                             pls::kTessTextureWidth,
                             static_cast<float>(desc.tessDataHeight),
                             0.0,
                             1.0);
        tessPass.SetPipeline(m_tessellatePipeline->renderPipeline());
        tessPass.SetVertexBuffer(0, webgpu_buffer(tessSpanBufferRing()));
        tessPass.SetIndexBuffer(m_tessSpanIndexBuffer, wgpu::IndexFormat::Uint16);
        tessPass.SetBindGroup(0, bindings);
        tessPass.DrawIndexed(std::size(pls::kTessSpanIndices),
                             desc.tessVertexSpanCount,
                             0,
                             0,
                             desc.firstTessVertexSpan);
        tessPass.End();
    }

    wgpu::LoadOp loadOp;
    wgpu::Color clearColor;
    if (desc.loadAction == LoadAction::clear)
    {
        loadOp = wgpu::LoadOp::Clear;
        float cc[4];
        UnpackColorToRGBA32F(desc.clearColor, cc);
        clearColor = {cc[0], cc[1], cc[2], cc[3]};
    }
    else
    {
        loadOp = wgpu::LoadOp::Load;
    }

    EmJsHandle drawPassJSHandle;
    wgpu::RenderPassEncoder drawPass =
        makePLSRenderPass(encoder, renderTarget, loadOp, clearColor, &drawPassJSHandle);
    drawPass.SetViewport(0.f, 0.f, renderTarget->width(), renderTarget->height(), 0.0, 1.0);

    bool needsPLSTextureBindings = m_pixelLocalStorageType == PixelLocalStorageType::subpassLoad;
    if (needsPLSTextureBindings)
    {
        wgpu::BindGroupEntry plsTextureBindingEntries[] = {
            {
                .binding = FRAMEBUFFER_PLANE_IDX,
                .textureView = renderTarget->m_targetTextureView,
            },
            {
                .binding = COVERAGE_PLANE_IDX,
                .textureView = renderTarget->m_coverageTextureView,
            },
            {
                .binding = ORIGINAL_DST_COLOR_PLANE_IDX,
                .textureView = renderTarget->m_originalDstColorTextureView,
            },
            {
                .binding = CLIP_PLANE_IDX,
                .textureView = renderTarget->m_clipTextureView,
            },
        };

        wgpu::BindGroupDescriptor plsTextureBindingsGroupDesc = {
            .layout = m_drawBindGroupLayouts[PLS_TEXTURE_BINDINGS_SET],
            .entryCount = std::size(plsTextureBindingEntries),
            .entries = plsTextureBindingEntries,
        };

        wgpu::BindGroup plsTextureBindings = m_device.CreateBindGroup(&plsTextureBindingsGroupDesc);
        drawPass.SetBindGroup(PLS_TEXTURE_BINDINGS_SET, plsTextureBindings);
    }

    if (m_pixelLocalStorageType == PixelLocalStorageType::EXT_shader_pixel_local_storage)
    {
        enable_shader_pixel_local_storage_ext(drawPass, true);

        // Draw the load action for EXT_shader_pixel_local_storage.
        std::array<float, 4> clearColor;
        LoadStoreActionsEXT loadActions = pls::BuildLoadActionsEXT(desc, &clearColor);
        const LoadStoreEXTPipeline& loadPipeline =
            m_loadStoreEXTPipelines
                .try_emplace(loadActions, this, loadActions, renderTarget->framebufferFormat())
                .first->second;

        if (loadActions & LoadStoreActionsEXT::clearColor)
        {
            void* uniformData = m_loadStoreEXTUniforms->mapBuffer(sizeof(clearColor));
            memcpy(uniformData, clearColor.data(), sizeof(clearColor));
            m_loadStoreEXTUniforms->unmapAndSubmitBuffer();

            wgpu::BindGroupEntry uniformBindingEntries[] = {
                {
                    .binding = 0,
                    .buffer = webgpu_buffer(m_loadStoreEXTUniforms.get()),
                },
            };

            wgpu::BindGroupDescriptor uniformBindGroupDesc = {
                .layout = loadPipeline.bindGroupLayout(),
                .entryCount = std::size(uniformBindingEntries),
                .entries = uniformBindingEntries,
            };

            wgpu::BindGroup uniformBindings = m_device.CreateBindGroup(&uniformBindGroupDesc);
            drawPass.SetBindGroup(0, uniformBindings);
        }

        drawPass.SetPipeline(loadPipeline.renderPipeline(renderTarget->framebufferFormat()));
        drawPass.Draw(4);
    }

    drawPass.SetBindGroup(SAMPLER_BINDINGS_SET, m_samplerBindings);

    // Execute the DrawList.
    wgpu::TextureView currentImageTextureView = m_nullImagePaintTextureView;
    wgpu::BindGroup bindings;
    bool needsNewBindings = true;
    for (const DrawBatch& batch : *desc.drawList)
    {
        if (batch.elementCount == 0)
        {
            continue;
        }

        DrawType drawType = batch.drawType;

        // Bind the appropriate image texture, if any.
        if (auto imageTexture = static_cast<const PLSTextureWebGPUImpl*>(batch.imageTexture))
        {
            currentImageTextureView = imageTexture->textureView();
            needsNewBindings = true;
        }

        if (needsNewBindings)
        {
            wgpu::BindGroupEntry bindingEntries[] = {
                {
                    .binding = TESS_VERTEX_TEXTURE_IDX,
                    .textureView = m_tessVertexTextureView,
                },
                {
                    .binding = GRAD_TEXTURE_IDX,
                    .textureView = m_gradientTextureView,
                },
                {
                    .binding = IMAGE_TEXTURE_IDX,
                    .textureView = currentImageTextureView,
                },
                {
                    .binding = PATH_BUFFER_IDX,
                    .buffer = webgpu_buffer(pathBufferRing()),
                    .offset = desc.firstPath * sizeof(pls::PathData),
                },
                {
                    .binding = PAINT_BUFFER_IDX,
                    .buffer = webgpu_buffer(paintBufferRing()),
                    .offset = desc.firstPaint * sizeof(pls::PaintData),
                },
                {
                    .binding = PAINT_AUX_BUFFER_IDX,
                    .buffer = webgpu_buffer(paintAuxBufferRing()),
                    .offset = desc.firstPaintAux * sizeof(pls::PaintAuxData),
                },
                {
                    .binding = CONTOUR_BUFFER_IDX,
                    .buffer = webgpu_buffer(contourBufferRing()),
                    .offset = desc.firstContour * sizeof(pls::ContourData),
                },
                {
                    .binding = FLUSH_UNIFORM_BUFFER_IDX,
                    .buffer = webgpu_buffer(flushUniformBufferRing()),
                    .offset = desc.flushUniformDataOffsetInBytes,
                },
                {
                    .binding = IMAGE_DRAW_UNIFORM_BUFFER_IDX,
                    .buffer = webgpu_buffer(imageDrawUniformBufferRing()),
                    .size = sizeof(pls::ImageDrawUniforms),
                },
            };

            wgpu::BindGroupDescriptor bindGroupDesc = {
                .layout = m_drawBindGroupLayouts[0],
                .entryCount = std::size(bindingEntries),
                .entries = bindingEntries,
            };

            bindings = m_device.CreateBindGroup(&bindGroupDesc);
        }

        if (needsNewBindings ||
            // Image draws always re-bind because they update the dynamic offset to their uniforms.
            drawType == DrawType::imageRect || drawType == DrawType::imageMesh)
        {
            drawPass.SetBindGroup(0, bindings, 1, &batch.imageDrawDataOffset);
            needsNewBindings = false;
        }

        // Setup the pipeline for this specific drawType and shaderFeatures.
        const DrawPipeline& drawPipeline =
            m_drawPipelines
                .try_emplace(pls::ShaderUniqueKey(drawType,
                                                  batch.shaderFeatures,
                                                  pls::InterlockMode::rasterOrdered),
                             this,
                             drawType,
                             batch.shaderFeatures)
                .first->second;
        drawPass.SetPipeline(drawPipeline.renderPipeline(renderTarget->framebufferFormat()));

        switch (drawType)
        {
            case DrawType::midpointFanPatches:
            case DrawType::outerCurvePatches:
            {
                // Draw PLS patches that connect the tessellation vertices.
                drawPass.SetVertexBuffer(0, m_pathPatchVertexBuffer);
                drawPass.SetIndexBuffer(m_pathPatchIndexBuffer, wgpu::IndexFormat::Uint16);
                drawPass.DrawIndexed(pls::PatchIndexCount(drawType),
                                     batch.elementCount,
                                     pls::PatchBaseIndex(drawType),
                                     0,
                                     batch.baseElement);
                break;
            }
            case DrawType::interiorTriangulation:
            {
                drawPass.SetVertexBuffer(0, webgpu_buffer(triangleBufferRing()));
                drawPass.Draw(batch.elementCount, 1, batch.baseElement);
                break;
            }
            case DrawType::imageRect:
                RIVE_UNREACHABLE();
            case DrawType::imageMesh:
            {
                auto vertexBuffer = static_cast<const RenderBufferWebGPUImpl*>(batch.vertexBuffer);
                auto uvBuffer = static_cast<const RenderBufferWebGPUImpl*>(batch.uvBuffer);
                auto indexBuffer = static_cast<const RenderBufferWebGPUImpl*>(batch.indexBuffer);
                drawPass.SetVertexBuffer(0, vertexBuffer->submittedBuffer());
                drawPass.SetVertexBuffer(1, uvBuffer->submittedBuffer());
                drawPass.SetIndexBuffer(indexBuffer->submittedBuffer(), wgpu::IndexFormat::Uint16);
                drawPass.DrawIndexed(batch.elementCount, 1, batch.baseElement);
                break;
            }
            case DrawType::plsAtomicResolve:
                RIVE_UNREACHABLE();
        }
    }

    if (m_pixelLocalStorageType == PixelLocalStorageType::EXT_shader_pixel_local_storage)
    {
        // Draw the store action for EXT_shader_pixel_local_storage.
        LoadStoreActionsEXT actions = LoadStoreActionsEXT::storeColor;
        auto it = m_loadStoreEXTPipelines.try_emplace(actions,
                                                      this,
                                                      actions,
                                                      renderTarget->framebufferFormat());
        LoadStoreEXTPipeline* storePipeline = &it.first->second;
        drawPass.SetPipeline(storePipeline->renderPipeline(renderTarget->framebufferFormat()));
        drawPass.Draw(4);

        enable_shader_pixel_local_storage_ext(drawPass, false);
    }

    drawPass.End();

    wgpu::CommandBuffer commands = encoder.Finish();
    m_queue.Submit(1, &commands);
}

std::unique_ptr<PLSRenderContext> PLSRenderContextWebGPUImpl::MakeContext(
    wgpu::Device device,
    wgpu::Queue queue,
    const pls::PlatformFeatures& baselinePlatformFeatures,
    PixelLocalStorageType pixelLocalStorageType)
{
    std::unique_ptr<PLSRenderContextWebGPUImpl> impl;
    switch (pixelLocalStorageType)
    {
        case PixelLocalStorageType::subpassLoad:
#ifdef RIVE_WEBGPU
            impl = std::unique_ptr<PLSRenderContextWebGPUImpl>(
                new PLSRenderContextWebGPUVulkan(device, queue, baselinePlatformFeatures));
            break;
#endif
        case PixelLocalStorageType::EXT_shader_pixel_local_storage:
        case PixelLocalStorageType::none:
            impl = std::unique_ptr<PLSRenderContextWebGPUImpl>(
                new PLSRenderContextWebGPUImpl(device,
                                               queue,
                                               baselinePlatformFeatures,
                                               pixelLocalStorageType));
            break;
    }
    impl->initGPUObjects();
    return std::make_unique<PLSRenderContext>(std::move(impl));
}
} // namespace rive::pls
