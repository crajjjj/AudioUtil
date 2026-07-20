set_xmakever("2.9.5")

-- Globals
PROJECT_NAME = "HentairimAudio"
PROJECT_VERSION = "1.0.0"

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
        author = "crajjjj",
        description = "Native folder-based audio player for Hentairim (voice + SFX) with PPA bridge."
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
            os.cp("dist/*", path.join(mod_folder, "HentairimAudio"))
        end
    end)
target_end()

-- Papyrus compile: xmake build papyrus
-- Compiles papyrus\Source\*.psc into dist\Scripts via Pyro (HentairimAudio.ppj)
-- and mirrors the sources into dist\Scripts\Source.
-- Overrides: PYRO_EXE (pyro.exe path), SKYRIM_GAME_PATH (game root).
target("papyrus")
    set_kind("phony")
    set_default(false)
    on_build(function (target)
        local pyro = os.getenv("PYRO_EXE")
        if not pyro or not os.isfile(pyro) then
            local home = os.getenv("USERPROFILE") or ""
            local candidates = os.files(path.join(home,
                ".vscode", "extensions", "joelday.papyrus-lang-vscode-*", "pyro", "pyro.exe"))
            pyro = candidates and candidates[1] or nil
        end
        assert(pyro and os.isfile(pyro),
            "pyro.exe not found - set the PYRO_EXE environment variable")

        local game = os.getenv("SKYRIM_GAME_PATH")
            or "C:\\SteamLibrary\\steamapps\\common\\Skyrim Special Edition"
        os.execv(pyro, {
            "-i", path.join(os.projectdir(), "HentairimAudio.ppj"),
            "--game-path", game
        })

        local src_out = path.join(os.projectdir(), "dist", "Scripts", "Source")
        if not os.isdir(src_out) then
            os.mkdir(src_out)
        end
        os.cp(path.join(os.projectdir(), "papyrus", "Source", "*.psc"), src_out)
    end)
target_end()

-- Release package: xmake build release
-- Builds the DLL and the Papyrus scripts, then zips dist\* into
-- Build\HentairimAudio-<version>.zip (a ready-to-install mod archive).
target("release")
    set_kind("phony")
    set_default(false)
    add_deps(PROJECT_NAME, "papyrus")
    on_build(function (target)
        import("core.project.project")
        -- NOT "Build": on Windows that is the same directory as xmake's own "build"
        local out_dir = path.join(os.projectdir(), "Release")
        if not os.isdir(out_dir) then
            os.mkdir(out_dir)
        end
        -- project.name() would report the included commonlibsse-ng subproject here
        local zip = path.join(out_dir,
            "HentairimAudio-" .. tostring(project.version()) .. ".zip")
        if os.isfile(zip) then
            os.rm(zip)
        end
        os.execv("powershell", {
            "-NoProfile", "-Command",
            string.format("Compress-Archive -Path '%s' -DestinationPath '%s' -Force",
                path.join(os.projectdir(), "dist", "*"), zip)
        })
        print("release written: " .. zip)
    end)
target_end()
