-- Runs Pyro on AudioUtil.ppj: compiles papyrus\Source -> dist\Scripts, mirrors
-- the .psc sources into dist, and (Zip="true" in the ppj) writes the <ZipFiles>
-- archive to Release\ — BFNG-style: pyro owns the release packaging.
-- The ppj writes Release\AudioUtil.zip (from @ModName); this script then renames
-- it to Release\AudioUtil-<version>.zip so the artifact is self-labeling. The
-- version comes from set_version in xmake.lua (PROJECT_VERSION), read here via
-- project.version() — single source of truth. (It can't be reached as a plain
-- global: that lives in description scope, this runs in script scope.)
-- Overrides: PYRO_EXE (pyro.exe path), SKYRIM_GAME_PATH (game root).

-- Ship the FuzSlots placeholder wavs so they're in the loose-file resource index
-- at a fresh install's first launch (FuzSlots::Configure only self-heals from the
-- NEXT launch — a mid-session-created slot isn't indexed, so first-session fuz
-- would be silent until a restart). Count mirrors [general] fuz_slots default 24;
-- names match FuzSlots::SlotName ("_au_slotNN.wav", NN = %02d). A tiny valid mono
-- 44.1k/16-bit silent wav — its bytes are overwritten per play; it only has to be
-- a parseable file present at launch. Extra slots (higher fuz_slots) bootstrap one
-- launch via Configure; that's fine. Written into dist here, then injected into
-- the built zip by inject_fuz_slots (the ppj is machine-local/untracked, so the
-- archiving lives in this tracked script, not in the ppj's ZipFiles).
function generate_fuz_slots()
    local count = 24
    local dir = path.join(os.projectdir(), "dist", "Sound", "AudioUtilFuzCache")
    if not os.isdir(dir) then
        os.mkdir(dir)
    end
    local function le(v, n)
        local s = ""
        for _ = 1, n do
            s = s .. string.char(v % 256)
            v = math.floor(v / 256)
        end
        return s
    end
    local dataBytes = 2000
    local wav = "RIFF" .. le(36 + dataBytes, 4) .. "WAVE"
        .. "fmt " .. le(16, 4) .. le(1, 2) .. le(1, 2)
        .. le(44100, 4) .. le(88200, 4) .. le(2, 2) .. le(16, 2)
        .. "data" .. le(dataBytes, 4) .. string.rep("\0", dataBytes)
    for i = 0, count - 1 do
        local name = string.format("_au_slot%02d.wav", i)
        local f = io.open(path.join(dir, name), "wb")
        if f then
            f:write(wav)
            f:close()
        end
    end
    print(string.format("pyro: %d FuzSlots placeholder wavs -> %s", count, dir))
end

-- Add dist\Sound (the FuzSlots placeholders) into an existing zip, nested as
-- Sound\AudioUtilFuzCache\*.wav. Done here rather than via the ppj's <Match>
-- because the ppj is gitignored (machine-local paths) — keeping the fix in this
-- tracked script makes it reproducible on any checkout.
function inject_fuz_slots(zip)
    local sound = path.join(os.projectdir(), "dist", "Sound")
    if not os.isdir(sound) or not os.isfile(zip) then
        return
    end
    os.execv("powershell", { "-NoProfile", "-Command", string.format(
        "Compress-Archive -Update -Path '%s' -DestinationPath '%s'", sound, zip) })
    print("pyro: injected Sound\\AudioUtilFuzCache placeholders into " .. path.filename(zip))
end

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

    -- put the FuzSlots placeholder wavs into dist so the ppj's Sound match zips them
    generate_fuz_slots()

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
            inject_fuz_slots(dst_zip)
            print("pyro: release archive -> " .. dst_zip)
        end
    else
        -- unversioned fallback: still inject into the ppj's default archive name
        inject_fuz_slots(path.join(os.projectdir(), "Release", "AudioUtil.zip"))
    end
end
