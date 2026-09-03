set_xmakever("3.0.0")  -- the commonlibsse-ng submodule requires 3.0

-- Globals
PROJECT_NAME = "AudioUtil"
PROJECT_VERSION = "0.9.17"
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
add_defines("NOMINMAX")  -- CommonLib v7 headers pull in Windows.h

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
    add_syslinks("ole32") -- CoCreateInstance for the WMA decoder DMO (FuzCache xWMA->PCM)
    set_pcxxheader("src/PCH.h")

    -- Exports (shared targets link via shflags; ldflags would be silently ignored)
    add_shflags("/DEF:exports.def", { force = true })

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

-- LipSim tool package: xmake build lipsim
-- Zips tools\lipsim (simulator page + server + launcher + fuz2wav) into
-- Release\AudioUtil-LipSim-<version>.zip — a standalone download for
-- voicepack authors, separate from the mod archive.
target("lipsim")
    set_kind("phony")
    set_default(false)
    on_build(function (target)
        import("core.project.project")
        import("lib.detect.find_tool")
        local proj = os.projectdir()
        local lipsim = path.join(proj, "tools", "lipsim")
        -- LipSim.exe embeds lipsim.html, so rebuild it whenever we package
        -- (needs pyinstaller: pip install pyinstaller). Skipped if absent.
        local pyinstaller = find_tool("pyinstaller")
        if pyinstaller then
            local work = path.join(os.tmpdir(), "lipsim-pybuild")
            local args = { "--onefile", "--name", "LipSim",
                "--add-data", path.join(lipsim, "lipsim.html") .. ";." }
            -- ship the ffmpeg.wasm bundle inside the exe so fuz xWMA decode is
            -- fully offline (no CDN). Skipped if the vendor dir isn't present.
            local vendor = path.join(lipsim, "vendor")
            if os.isdir(vendor) then
                table.insert(args, "--add-data")
                table.insert(args, vendor .. ";vendor")
            end
            table.insert(args, "--distpath"); table.insert(args, lipsim)
            table.insert(args, "--workpath"); table.insert(args, work)
            table.insert(args, "--specpath"); table.insert(args, work)
            table.insert(args, "--log-level"); table.insert(args, "WARN")
            table.insert(args, path.join(lipsim, "lipsim_server.py"))
            os.execv(pyinstaller.program, args)
        else
            print("lipsim: pyinstaller not found — packaging without a fresh LipSim.exe")
        end
        local staging = path.join(os.tmpdir(), "lipsim-pack")
        os.rm(staging)
        os.mkdir(staging)
        -- head bundles (generated locally by tri2head.py from installed game/mod
        -- assets) ship when present so the tool opens with heads preloaded
        for _, f in ipairs({ "lipsim.html", "lipsim_server.py", "fuz2wav.py",
                             "tri2head.py", "LipSim.bat", "README.md", "LipSim.exe",
                             "female.head.json", "male.head.json" }) do
            local src = path.join(lipsim, f)
            if os.isfile(src) then
                os.cp(src, staging)
            end
        end
        -- vendored ffmpeg.wasm bundle, for the Python-server path (LipSim.bat /
        -- lipsim_server.py serve it from vendor/ next to the script; the exe has
        -- its own copy baked in via --add-data above)
        local vendor = path.join(lipsim, "vendor")
        if os.isdir(vendor) then
            os.cp(vendor, staging)
        end
        local rel = path.join(proj, "Release")
        if not os.isdir(rel) then
            os.mkdir(rel)
        end
        local out = path.join(rel,
            "AudioUtil-LipSim-" .. (project.version() or "dev") .. ".zip")
        os.rm(out)
        os.execv("powershell", { "-NoProfile", "-Command",
            string.format("Compress-Archive -Path '%s\\*' -DestinationPath '%s' -Force",
                staging, out) })
        os.rm(staging)
        print("lipsim package -> " .. out)
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
