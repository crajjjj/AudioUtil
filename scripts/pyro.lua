-- Runs Pyro on AudioUtil.ppj: compiles papyrus\Source -> dist\Scripts, mirrors
-- the .psc sources into dist, and (Zip="true" in the ppj) writes the <ZipFiles>
-- archive to Release\ — BFNG-style: pyro owns the release packaging.
-- The ppj writes Release\AudioUtil.zip (from @ModName); this script then renames
-- it to Release\AudioUtil-<version>.zip so the artifact is self-labeling. The
-- version comes from set_version in xmake.lua (PROJECT_VERSION), read here via
-- project.version() — single source of truth. (It can't be reached as a plain
-- global: that lives in description scope, this runs in script scope.)
-- Overrides: PYRO_EXE (pyro.exe path), SKYRIM_GAME_PATH (game root).

function main()
    import("core.project.project")
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

    -- mirror sources into dist before zipping so the archive carries them
    local src_out = path.join(os.projectdir(), "dist", "Scripts", "Source")
    if not os.isdir(src_out) then
        os.mkdir(src_out)
    end
    os.cp(path.join(os.projectdir(), "papyrus", "Source", "*.psc"), src_out)

    os.execv(pyro, {
        "-i", path.join(os.projectdir(), "AudioUtil.ppj"),
        "--game-path", game
    })

    -- stamp the version into the archive name (Pyro can't; the ppj is static).
    -- Release\AudioUtil.zip -> Release\AudioUtil-<version>.zip.
    local version = project.version()
    if version and version ~= "" then
        local base = path.join(os.projectdir(), "Release", "AudioUtil")
        local src_zip = base .. ".zip"
        local dst_zip = base .. "-" .. version .. ".zip"
        if os.isfile(src_zip) then
            os.mv(src_zip, dst_zip)
            print("pyro: release archive -> " .. dst_zip)
        end
    end
end
