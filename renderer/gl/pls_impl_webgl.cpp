/*
 * Copyright 2022 Rive
 */

#include "rive/pls/gl/pls_render_context_gl.hpp"

#include <stdio.h>

#ifdef RIVE_WASM
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

#include "../out/obj/generated/glsl.exports.h"

namespace rive::pls
{
class PLSRenderContextGL::PLSImplWebGL : public PLSRenderContextGL::PLSImpl
{
    rcp<PLSRenderTargetGL> wrapGLRenderTarget(GLuint framebufferID,
                                              size_t width,
                                              size_t height,
                                              const PlatformFeatures&) override
    {
        // WEBGL_shader_pixel_local_storage can't load or store to framebuffers.
        return nullptr;
    }

    rcp<PLSRenderTargetGL> makeOffscreenRenderTarget(
        size_t width,
        size_t height,
        const PlatformFeatures& platformFeatures) override
    {
        auto renderTarget = rcp(new PLSRenderTargetGL(width, height, platformFeatures));
        renderTarget->allocateCoverageBackingTextures();
        glFramebufferTexturePixelLocalStorageWEBGL(kFramebufferPlaneIdx,
                                                   renderTarget->m_offscreenTextureID,
                                                   0,
                                                   0);
        glFramebufferTexturePixelLocalStorageWEBGL(kCoveragePlaneIdx,
                                                   renderTarget->m_coverageTextureID,
                                                   0,
                                                   0);
        glFramebufferTexturePixelLocalStorageWEBGL(kOriginalDstColorPlaneIdx,
                                                   renderTarget->m_originalDstColorTextureID,
                                                   0,
                                                   0);
        glFramebufferTexturePixelLocalStorageWEBGL(kClipPlaneIdx,
                                                   renderTarget->m_clipTextureID,
                                                   0,
                                                   0);
        renderTarget->createSideFramebuffer();
        return renderTarget;
    }

    void activatePixelLocalStorage(PLSRenderContextGL* context,
                                   const PLSRenderTargetGL* renderTarget,
                                   LoadAction loadAction,
                                   bool needsClipBuffer) override
    {
        glBindFramebuffer(GL_FRAMEBUFFER, renderTarget->drawFramebufferID());

        if (loadAction == LoadAction::clear)
        {
            float clearColor4f[4];
            UnpackColorToRGBA32F(context->frameDescriptor().clearColor, clearColor4f);
            glFramebufferPixelLocalClearValuefvWEBGL(kFramebufferPlaneIdx, clearColor4f);
        }

        GLenum loadOps[4] = {(GLenum)(loadAction == LoadAction::clear ? GL_LOAD_OP_CLEAR_WEBGL
                                                                      : GL_LOAD_OP_LOAD_WEBGL),
                             GL_LOAD_OP_ZERO_WEBGL,
                             GL_DONT_CARE,
                             (GLenum)(needsClipBuffer ? GL_LOAD_OP_ZERO_WEBGL : GL_DONT_CARE)};

        glBeginPixelLocalStorageWEBGL(4, loadOps);
    }

    void deactivatePixelLocalStorage(PLSRenderContextGL*) override
    {
        constexpr static GLenum kStoreOps[4] = {GL_STORE_OP_STORE_WEBGL,
                                                GL_DONT_CARE,
                                                GL_DONT_CARE,
                                                GL_DONT_CARE};
        glEndPixelLocalStorageWEBGL(4, kStoreOps);
    }

    const char* shaderDefineName() const override { return GLSL_PLS_IMPL_WEBGL; }
};

std::unique_ptr<PLSRenderContextGL::PLSImpl> PLSRenderContextGL::MakePLSImplWebGL()
{
    return std::make_unique<PLSImplWebGL>();
}
} // namespace rive::pls
