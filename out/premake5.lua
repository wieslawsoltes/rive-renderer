workspace "rive"
configurations {"debug", "release"}

require 'setup_compiler'

dofile("premake5_pls_renderer.lua")
dofile(RIVE_RUNTIME_DIR .. "/build/premake5.lua")
dofile(RIVE_RUNTIME_DIR .. '/decoders/build/premake5.lua')

newoption {
    trigger = "with-skia",
    description = "use skia",
}
if _OPTIONS["with-skia"]
then
    dofile(RIVE_RUNTIME_DIR .. "/skia/renderer/build/premake5.lua")
end

if _OPTIONS["wasm"]
then
    dofile("premake5_canvas2d_renderer.lua")
end

project "path_fiddle"
do
    dependson 'rive'
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    exceptionhandling "Off"
    rtti "Off"
    targetdir "%{cfg.buildcfg}"
    objdir "obj/%{cfg.buildcfg}"
    includedirs {"../include",
                 RIVE_RUNTIME_DIR .. "/include",
                 "../glad",
                 "../include",
                 RIVE_RUNTIME_DIR .. "/skia/dependencies/glfw/include"}
    flags { "FatalWarnings" }

    files {
        "../path_fiddle/**.cpp",
    }

    links {
        "rive",
        "rive_pls_renderer",
        "rive_decoders",
        "libpng",
        "zlib",
        "rive_harfbuzz",
        "rive_sheenbidi"
    }

    filter "options:with-skia"
    do
        includedirs {RIVE_RUNTIME_DIR .. "/skia/renderer/include",
                     RIVE_RUNTIME_DIR .. "/skia/dependencies",
                     RIVE_RUNTIME_DIR .. "/skia/dependencies/skia"}
        defines {"RIVE_SKIA", "SK_GL"}
        libdirs {RIVE_RUNTIME_DIR .. "/skia/dependencies/skia/out/static"}
        links {"skia", "rive_skia_renderer"}
    end

    filter "system:windows"
    do
        architecture "x64"
        defines {"RIVE_WINDOWS", "_CRT_SECURE_NO_WARNINGS"}
        libdirs {RIVE_RUNTIME_DIR .. "/skia/dependencies/glfw_build/src/Release"}
        links {"glfw3", "opengl32", "d3d11", "dxgi", "d3dcompiler"}
    end

    filter "system:macosx"
    do
        files {"../path_fiddle/**.mm"}
        buildoptions {"-fobjc-arc"}
        links {"glfw3",
               "Cocoa.framework",
               "Metal.framework",
               "QuartzCore.framework",
               "IOKit.framework"}
        libdirs {RIVE_RUNTIME_DIR .. "/skia/dependencies/glfw_build/src"}
    end

    filter "options:with-dawn"
    do
        includedirs {
            "../dependencies/dawn/include",
            "../dependencies/dawn/out/release/gen/include",
        }
        libdirs {
            "../dependencies/dawn/out/release/obj/src/dawn",
            "../dependencies/dawn/out/release/obj/src/dawn/native",
            "../dependencies/dawn/out/release/obj/src/dawn/platform",
            "../dependencies/dawn/out/release/obj/src/dawn/platform",
        }
        links {
            "dawn_native_static",
            "webgpu_dawn",
            "dawn_platform_static",
            "dawn_proc_static",
        }
    end

    filter {"options:with-dawn", "system:windows"}
    do
        links {
            "dxguid",
        }
    end

    filter {"options:with-dawn", "system:macosx"}
    do
        links {
            "IOSurface.framework",
        }
    end

    filter "system:emscripten"
    do
        targetname "path_fiddle.js"
        targetdir(_OPTIONS['emsdk'] .. '_%{cfg.buildcfg}')
        objdir(_OPTIONS['emsdk'] .. '_%{cfg.buildcfg}')
        linkoptions {"-sUSE_GLFW=3",
                     "--preload-file ../../../gold/rivs@/"}
        files {"../path_fiddle/index.html"}
    end

    filter 'files:**.html'
    do
        buildmessage "Copying %{file.relpath} to %{cfg.targetdir}"
        buildcommands {"cp %{file.relpath} %{cfg.targetdir}/%{file.name}"}
        buildoutputs { "%{cfg.targetdir}/%{file.name}" }
    end

    filter "configurations:debug"
    do
        defines {"DEBUG"}
        symbols "On"
    end

    filter "configurations:release"
    do
        defines {"RELEASE"}
        defines {"NDEBUG"}
        optimize "On"
    end
end

project "webgpu_player"
do
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    exceptionhandling "Off"
    rtti "Off"
    targetdir "%{cfg.buildcfg}"
    objdir "obj/%{cfg.buildcfg}"
    includedirs {"../include",
                 RIVE_RUNTIME_DIR .. "/include",
                 "../glad",
                 "../include",
                 RIVE_RUNTIME_DIR .. "/skia/dependencies/glfw/include"}
    flags { "FatalWarnings" }

    files {
        "../webgpu_player/webgpu_player.cpp",
        "../webgpu_player/index.html",
        "../webgpu_player/rive.js",
        "../../../gold/rivs/LumberjackFinal.riv",
        "../../../gold/rivs/Santa_Claus.riv",
        "../../../gold/rivs/Coffee_Cup.riv",
        "../../../gold/rivs/skull_404.riv",
        "../../../gold/rivs/octopus_loop.riv",
        "../../../gold/rivs/planets.riv",
        "../../../gold/rivs/Timer.riv",
        "../../../gold/rivs/icons_vampire-aquamonster.riv",
        "../../../gold/rivs/towersDemo.riv",
        "../../../gold/rivs/skills_demov1.riv",
    }

    links {
        "rive",
        "rive_pls_renderer",
        "rive_decoders",
        "libpng",
        "zlib",
        "rive_harfbuzz",
        "rive_sheenbidi"
    }

    filter "system:windows"
    do
        architecture "x64"
        defines {"RIVE_WINDOWS", "_CRT_SECURE_NO_WARNINGS"}
        libdirs {RIVE_RUNTIME_DIR .. "/skia/dependencies/glfw_build/src/Release"}
        links {"glfw3", "opengl32", "d3d11", "dxgi", "d3dcompiler"}
    end

    filter "system:macosx"
    do
        files {"../path_fiddle/fiddle_context_dawn_helper.mm"}
        buildoptions {"-fobjc-arc"}
        links {"glfw3",
               "Cocoa.framework",
               "Metal.framework",
               "QuartzCore.framework",
               "IOKit.framework"}
        libdirs {RIVE_RUNTIME_DIR .. "/skia/dependencies/glfw_build/src"}
    end

    filter "options:with-dawn"
    do
        includedirs {
            "../dependencies/dawn/include",
            "../dependencies/dawn/out/release/gen/include",
        }
        libdirs {
            "../dependencies/dawn/out/release/obj/src/dawn",
            "../dependencies/dawn/out/release/obj/src/dawn/native",
            "../dependencies/dawn/out/release/obj/src/dawn/platform",
            "../dependencies/dawn/out/release/obj/src/dawn/platform",
        }
        links {
            "dawn_native_static",
            "webgpu_dawn",
            "dawn_platform_static",
            "dawn_proc_static",
        }
    end

    filter {"options:with-dawn", "system:windows"}
    do
        links {
            "dxguid",
        }
    end

    filter {"options:with-dawn", "system:macosx"}
    do
        links {
            "IOSurface.framework",
        }
    end

    filter "system:emscripten"
    do
        targetname "webgpu_player.js"
        targetdir(_OPTIONS['emsdk'] .. '_%{cfg.buildcfg}')
        objdir(_OPTIONS['emsdk'] .. '_%{cfg.buildcfg}')
        linkoptions {
            "-sEXPORTED_FUNCTIONS=_RiveInitialize,_RiveBeginRendering,_RiveFlushRendering,_RiveLoadFile,_File_artboardNamed,_File_artboardDefault,_File_destroy,_ArtboardInstance_width,_ArtboardInstance_height,_ArtboardInstance_stateMachineNamed,_ArtboardInstance_animationNamed,_ArtboardInstance_defaultScene,_ArtboardInstance_align,_ArtboardInstance_destroy,_Scene_advanceAndApply,_Scene_draw,_Scene_destroy,_Renderer_save,_Renderer_restore,_Renderer_translate,_Renderer_transform,_malloc,_free",
            "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap",
            "-sSINGLE_FILE",
            "-sUSE_WEBGPU",
            "-sENVIRONMENT=web,shell",
        }
    end

    filter 'files:**.html or **.riv or **.js'
    do
        buildmessage "Copying %{file.relpath} to %{cfg.targetdir}"
        buildcommands {"cp %{file.relpath} %{cfg.targetdir}/%{file.name}"}
        buildoutputs { "%{cfg.targetdir}/%{file.name}" }
    end

    filter "configurations:debug"
    do
        defines {"DEBUG"}
        symbols "On"
    end

    filter "configurations:release"
    do
        defines {"RELEASE"}
        defines {"NDEBUG"}
        optimize "On"
    end
end

project "bubbles"
do
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    exceptionhandling "Off"
    rtti "Off"
    targetdir "%{cfg.buildcfg}"
    objdir "obj/%{cfg.buildcfg}"
    includedirs {"../",
                 "../glad",
                 "../include",
                 RIVE_RUNTIME_DIR .. "/skia/dependencies/glfw/include"}
    flags { "FatalWarnings" }
    files {"../bubbles/bubbles.cpp",
           "../glad/glad.c",
           "../glad/glad_custom.c"}

    filter "system:windows"
    do
        architecture "x64"
        defines {"RIVE_WINDOWS"}
        links {"glfw3", "opengl32"}
        libdirs {RIVE_RUNTIME_DIR .. "/skia/dependencies/glfw_build/src/Release"}
    end

    filter "system:macosx"
    do
        links {'glfw3', 'Cocoa.framework', 'IOKit.framework'}
        libdirs {RIVE_RUNTIME_DIR .. "/skia/dependencies/glfw_build/src"}
    end

    filter "system:emscripten"
    do
        targetname "bubbles.js"
        targetdir(_OPTIONS['emsdk'] .. '_%{cfg.buildcfg}')
        objdir(_OPTIONS['emsdk'] .. '_%{cfg.buildcfg}')
        linkoptions {"-sUSE_GLFW=3"}
        files {"../bubbles/index.html"}
    end

    filter 'files:**.html'
    do
        buildmessage "Copying %{file.relpath} to %{cfg.targetdir}"
        buildcommands {"cp %{file.relpath} %{cfg.targetdir}/%{file.name}"}
        buildoutputs { "%{cfg.targetdir}/%{file.name}" }
    end

    filter "configurations:debug"
    do
        defines {"DEBUG"}
        symbols "On"
    end

    filter "configurations:release"
    do
        defines {"RELEASE"}
        defines {"NDEBUG"}
        optimize "On"
    end
end
