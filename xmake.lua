-- include subprojects
includes("lib/commonlibsse-ng")

-- set project constants
set_project("skyrimcameradisocclusion")
set_version("0.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- define targets
target("skyrimcameradisocclusion")
    add_deps("commonlibsse-ng")

    -- The commonlibsse-ng.plugin rule is the canonical home for plugin
    -- metadata. It uses the values below to generate, at build time:
    --   - build/.gens/.../rules/commonlibsse-ng/plugin/plugin.cpp
    --     (contains SKSEPluginInfo(...) — what SKSE reads to register
    --     the plugin and pick the log file name)
    --   - a Windows resource file (.rc) stamping the DLL's File Properties
    --     with author / version / description
    -- It also sets the build kind to "shared" (DLL), wires up `xmake install`
    -- to drop into a Data/SKSE/Plugins layout, and `xmake package` to build a
    -- Nexus-ready zip. Treat this block as the manifest, not as configuration —
    -- there's no plugin.cpp in src/ on purpose.
    add_rules("commonlibsse-ng.plugin", {
        name = "skyrimcameradisocclusion",
        author = "anon",
        description = "Third-person camera disocclusion: see-through walls when the camera floats out of view of the player."
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
