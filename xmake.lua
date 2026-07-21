set_xmakever("2.9.5")

-- Globals
PROJECT_NAME = "AudioUtil"
PROJECT_VERSION = "1.0.0"
PROJECT_AUTHOR = "crajjjj"

-- Project
set_project(PROJECT_NAME)
set_version(PROJECT_VERSION)
set_languages("cxx23")
set_license("gplv3")
set_warnings("allextra")

-- Options
option("copy_to_mod")
    set_default(false)
    set_description("Copy dist/* to a mod folder (XSE_TES5_MODS_PATH)")
option_end()

-- Dependencies & Includes
includes("lib/commonlibsse-ng")

add_requires("toml++")

-- policies
set_policy("package.requires_lock", true)

-- rules
add_rules("mode.debug", "mode.release")

if is_mode("debug") then
    add_defines("DEBUG")
    set_optimize("none")
    set_runtimes("MTd")
elseif is_mode("release") then
    add_defines("NDEBUG")
    set_optimize("fastest")
    set_symbols("debug")
    set_runtimes("MT")
end

add_defines("_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING")

-- Target
target(PROJECT_NAME)
    set_kind("shared")

    -- CommonLibSSE-NG
    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = PROJECT_NAME,
        author = PROJECT_AUTHOR,
        description = "Native folder-based audio player (voice + SFX) with an optional PPA bridge."
    })

    -- Packages
    add_packages("toml++")

    -- Source files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src", "include")
    set_pcxxheader("src/PCH.h")

    -- Exports
    add_ldflags("/DEF:exports.def", { force = true })

    -- flags
    add_cxxflags(
        "cl::/diagnostics:caret",
        "cl::/wd4200",
        "cl::/wd4201",
        "cl::/Zc:preprocessor",
        "cl::/utf-8"
    )

    if is_mode("debug") then
        add_cxxflags("cl::/bigobj")
    end

    -- Post Build
    after_build(function (target)
        local plugin_folder = path.join(os.projectdir(), "dist", "SKSE", "Plugins")
        if not os.isdir(plugin_folder) then
            os.mkdir(plugin_folder)
        end
        os.cp(target:targetfile(), plugin_folder)
        if is_mode("debug") then
            local pdb = target:symbolfile()
            if pdb then
                os.cp(pdb, plugin_folder)
            end
        end

        local mod_folder = os.getenv("XSE_TES5_MODS_PATH")
        if mod_folder and has_config("copy_to_mod") then
            os.cp("dist/*", path.join(mod_folder, "AudioUtil"))
        end
    end)
target_end()

-- Papyrus compile: xmake build papyrus
-- Runs Pyro on AudioUtil.ppj; also refreshes Release\AudioUtil.zip
-- (see scripts\pyro.lua; overrides: PYRO_EXE, SKYRIM_GAME_PATH)
target("papyrus")
    set_kind("phony")
    set_default(false)
    on_build(function (target)
        import("scripts.pyro")
        pyro()
    end)
target_end()

-- Release package: xmake build release
-- Same as `papyrus` but rebuilds the DLL first so the ppj's <ZipFiles> archive
-- (Release\AudioUtil.zip) always carries a fresh DLL.
target("release")
    set_kind("phony")
    set_default(false)
    add_deps(PROJECT_NAME)
    on_build(function (target)
        import("scripts.pyro")
        pyro()
    end)
target_end()
