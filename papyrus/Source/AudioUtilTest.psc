Scriptname AudioUtilTest Hidden
{Console test harness. Reachable as console commands via ConsoleUtil Extended -
 see dist\SKSE\CustomConsole\AudioUtilTest.yaml (e.g. autest play <path>).}

; basic file play, default flags
Function T1() global
    int h = AudioUtil.DebugPlayFile("Sound\\fx\\IVDT\\M1\\Orgasm\\01.wav", Game.GetPlayer(), 0x1A, 128)
    Debug.Notification("T1 handle=" + h + " dur=" + AudioUtil.GetHandleDuration(h))
EndFunction

; flags sweep — run repeatedly with different values via T2F
Function T2F(int flags, int priority) global
    int h = AudioUtil.DebugPlayFile("Sound\\fx\\IVDT\\M1\\Orgasm\\01.wav", Game.GetPlayer(), flags, priority)
    Debug.Notification("flags=" + flags + " prio=" + priority + " handle=" + h)
EndFunction

; voice via slot resolution on the player
Function T3() global
    int h = AudioUtil.PlayVoice(Game.GetPlayer(), "Orgasm")
    Debug.Notification("T3 PlayVoice handle=" + h + " slot=" + AudioUtil.GetSlotForActor(Game.GetPlayer()))
EndFunction

; shuffle-bag check: play a category 5x back to back (watch the log for picked files)
Function T4() global
    int i = 0
    while i < 5
        AudioUtil.PlayVoiceFromSlot("M3", "Aggressive", Game.GetPlayer())
        Utility.Wait(2.0)
        i += 1
    endwhile
EndFunction

; SFX table
Function T5() global
    AudioUtil.PlaySFX("MediumClap", Game.GetPlayer())
EndFunction

; channel replacement: second play should cut the first
Function T6() global
    AudioUtil.PlayVoiceFromSlot("M1", "Lovey Dovey", Game.GetPlayer(), 1.0, "", "test_chan")
    Utility.Wait(0.5)
    AudioUtil.PlayVoiceFromSlot("M1", "Orgasm", Game.GetPlayer(), 1.0, "", "test_chan")
EndFunction

; group duck: start a line, duck it after a second, restore
Function T7() global
    AudioUtil.PlayVoiceFromSlot("M1", "Aggressive", Game.GetPlayer(), 1.0, "pc_low")
    Utility.Wait(1.0)
    AudioUtil.DuckGroup("pc_low")
    Utility.Wait(1.5)
    AudioUtil.UnduckGroup("pc_low")
EndFunction

; PPA status
Function T8() global
    Debug.Notification("PPA connected=" + AudioUtilPPA.IsConnected())
EndFunction

Function TReload() global
    Debug.Notification("reload=" + AudioUtil.ReloadConfig())
EndFunction

; --- parameterized helpers: content-agnostic, driven from console args (via CUE) ---
; Unlike T1-T7 (which hardcode IVDT paths/slots), these take what to play as
; arguments, so they work on any install by pointing at content you actually have.

; Play any loose wav by Data-relative path at the player.  e.g.  autest play "Sound\fx\foo\bar.wav"
Function PlayPath(string path) global
    int h = AudioUtil.PlayFile(path, Game.GetPlayer())
    if h > 0
        Debug.Notification("play '" + path + "' handle=" + h + " dur=" + AudioUtil.GetHandleDuration(h))
    else
        Debug.Notification("play '" + path + "' FAILED (0) - missing or BSA-packed? see AudioUtil.log")
    endif
EndFunction

; Spoken-line variant of `play`: same path, but drives the player's mouth
; (PlayFileWithLipSync - authored .lip/fuz lip when present, else envelope/
; pseudo).  e.g.  autest playlip "Sound\Voice\Skyrim.esm\FemaleEvenToned\dialoguewhiterun__0008f148_1.fuz"
Function PlayLipPath(string path) global
    int h = AudioUtil.PlayFileWithLipSync(path, Game.GetPlayer())
    if h > 0
        Debug.Notification("playlip '" + path + "' handle=" + h + " dur=" + AudioUtil.GetHandleDuration(h))
    else
        Debug.Notification("playlip '" + path + "' FAILED (0) - see AudioUtil.log")
    endif
EndFunction

; Shuffle-bag play a whole folder as a pool (no-repeat until the deck empties),
; spoken variant so the picked clip drives the player's mouth. Data-relative
; folder path, e.g.  autest playfolder "Sound\TMSDynamicDialogue\VoiceBella\Base"
; Re-run to hear the next pick.
Function PlayFolderPath(string folder) global
    int h = AudioUtil.PlayFolderWithLipSync(folder, Game.GetPlayer())
    if h > 0
        Debug.Notification("playfolder '" + folder + "' -> " + AudioUtil.GetHandlePath(h) + " handle=" + h + " dur=" + AudioUtil.GetHandleDuration(h))
    else
        Debug.Notification("playfolder '" + folder + "' FAILED (0) - empty/missing folder? see AudioUtil.log")
    endif
EndFunction

; DebugPlayFile on any path with explicit flags/priority. RAW path - a .fuz is
; fed to the engine as-is (no payload extraction), so this can probe the
; engine's native fuz handling.  e.g.  autest playf "Sound\x\y.xwm" 26 128
Function PlayFlags(string path, int flags, int priority) global
    int h = AudioUtil.DebugPlayFile(path, Game.GetPlayer(), flags, priority)
    Debug.Notification("playf flags=" + flags + " prio=" + priority + " handle=" + h + " dur=" + AudioUtil.GetHandleDuration(h))
EndFunction

; Auto-sweep a curated list of sound_flags values over one path, ~2.5s apart,
; each announced via notification with its handle + duration. Run it, CLOSE THE
; CONSOLE (the game must be unpaused for Utility.Wait / audio), listen, and note
; which "sweep N: flags=F" notifications coincide with audible sound.
; duration > 0 alone already means the decoder parsed the file.
; RAW path like PlayFlags - point it at an extracted .xwm or a .fuz directly.
Function Sweep(string path) global
    int[] f = new int[13]
    f[0] = 26   ; 0x1A - the shipped default
    f[1] = 10   ; 0x0A
    f[2] = 18   ; 0x12
    f[3] = 2
    f[4] = 8
    f[5] = 16
    f[6] = 24
    f[7] = 27
    f[8] = 30
    f[9] = 0
    f[10] = 58  ; 0x3A
    f[11] = 90  ; 0x5A
    f[12] = 154 ; 0x9A
    Debug.Notification("sweep '" + path + "' - 13 combos, ~2.5s apart")
    int i = 0
    while i < f.length
        int h = AudioUtil.DebugPlayFile(path, Game.GetPlayer(), f[i], 128)
        Utility.Wait(1.0)
        Debug.Notification("sweep " + i + ": flags=" + f[i] + " handle=" + h + " dur=" + AudioUtil.GetHandleDuration(h) + " playing=" + AudioUtil.IsHandlePlaying(h))
        Utility.Wait(1.5)
        AudioUtil.StopHandle(h)
        i += 1
    endwhile
    Debug.Notification("sweep done")
EndFunction

; Play a voice category from an explicit slot at the player.  e.g.  autest voice M1 Orgasm
Function Voice(string slot, string category) global
    int n = AudioUtil.GetCategoryFileCount(slot, category)
    int h = AudioUtil.PlayVoiceFromSlot(slot, category, Game.GetPlayer())
    Debug.Notification("voice " + slot + "/" + category + " files=" + n + " handle=" + h)
EndFunction

; Resolve the player's slot and play a category through it.  e.g.  autest voicepc Orgasm
Function VoicePC(string category) global
    string slot = AudioUtil.GetSlotForActor(Game.GetPlayer())
    int h = AudioUtil.PlayVoice(Game.GetPlayer(), category)
    Debug.Notification("voicepc " + category + " slot=" + slot + " handle=" + h)
EndFunction

; Tag-scored play from an explicit slot at the player, quoting the fact
; string.  e.g.  autest voicetag F1 Greeting "angry intense"
Function VoiceTag(string slot, string category, string tags) global
    int n = AudioUtil.GetCategoryFileCount(slot, category)
    int h = AudioUtil.PlayVoiceFromSlotTagged(slot, category, tags, Game.GetPlayer())
    Debug.Notification("voicetag " + slot + "/" + category + " [" + tags + "] files=" + n + " handle=" + h)
EndFunction

; Tag-scored play through the player's resolved slot.
; e.g.  autest voicetagpc Greeting "angry intense"
Function VoiceTagPC(string category, string tags) global
    string slot = AudioUtil.GetSlotForActor(Game.GetPlayer())
    int h = AudioUtil.PlayVoiceTagged(Game.GetPlayer(), category, tags)
    Debug.Notification("voicetagpc " + category + " [" + tags + "] slot=" + slot + " handle=" + h)
EndFunction

; Play an SFX by name at the player.  e.g.  autest sfx MediumClap
Function Sfx(string name) global
    int h = AudioUtil.PlaySFX(name, Game.GetPlayer())
    Debug.Notification("sfx " + name + " handle=" + h)
EndFunction

; --- lip-calibration capture (debug-only natives, registered on this script) ---
; Records the DIALOGUE SPEAKER's 16 MFG phoneme channels at ~30 Hz — i.e. what
; the engine's own .lip playback writes to the face — into a timestamped CSV
; under Data\SKSE\Plugins\AudioUtil\. Used offline to map .lip grid slots to
; MFG phonemes.  Usage:  autest lipcap start   ...talk to NPCs...   autest lipcap stop
bool Function StartLipCapture() global native
string Function StopLipCapture() global native
bool Function IsLipCapturing() global native
int Function PrewarmFolder(string asFolder) global native
bool Function BOverwriteFile(string asDst, string asSrc) global native
string Function BCacheFile(int aiIndex) global native
Function SetLipFilesMode(bool abEnabled) global native
bool Function GetLipFilesMode() global native
Function SetPseudoLipMode(bool abEnabled) global native
bool Function GetPseudoLipMode() global native
Function SetLipLeadMs(int aiMs) global native
int Function GetLipLeadMs() global native

; Toggle .lip-driven phoneme lipsync at runtime (vs the amplitude envelope).
; Affects newly started lines.  Usage:  autest lipfiles on|off|status
Function LipFiles(string mode) global
    if mode == "on"
        SetLipFilesMode(true)
        Debug.Notification("lipsync: .lip phoneme mode ON (lines with lip data use authored curves)")
    elseif mode == "off"
        SetLipFilesMode(false)
        Debug.Notification("lipsync: .lip phoneme mode OFF (amplitude envelope for everything)")
    else
        Debug.Notification("lipsync: .lip phoneme mode = " + GetLipFilesMode())
    endif
EndFunction

; Toggle pseudo-phoneme synthesis for lines WITHOUT lip data (vowel variety +
; lip closures from the envelope, vs the plain Aah jaw-flap).
; Affects newly started lines.  Usage:  autest pseudolip on|off|status
Function PseudoLip(string mode) global
    if mode == "on"
        SetPseudoLipMode(true)
        Debug.Notification("lipsync: pseudo-phoneme mode ON (envelope lines get vowels + closures)")
    elseif mode == "off"
        SetPseudoLipMode(false)
        Debug.Notification("lipsync: pseudo-phoneme mode OFF (plain envelope jaw-flap)")
    else
        Debug.Notification("lipsync: pseudo-phoneme mode = " + GetPseudoLipMode())
    endif
EndFunction

; Calibrate the mouth timing lead in ms (positive = mouth earlier, compensates
; the mouth trailing the sound). Applies IMMEDIATELY, including lines already
; playing, so tune it live against a looping voice.
; Usage:  autest liplead 80   |   autest liplead status
Function LipLead(string arg) global
    if arg == "status" || arg == ""
        Debug.Notification("lipsync: lead = " + GetLipLeadMs() + " ms")
    else
        SetLipLeadMs(arg as int)
        Debug.Notification("lipsync: lead set to " + GetLipLeadMs() + " ms (config default needs [lipsync] lead_ms)")
    endif
EndFunction

; Decode-only prewarm of a fuz folder. Under MO2 a cache wav written mid-session
; is invisible to the engine until the next launch, so a first-time fuz plays
; silent. Run this once over a fuz folder, RESTART the game, then folder/file
; play of those lines works first try.  autest warmfolder "Sound\...\Base"
Function WarmFolder(string folder) global
    int n = PrewarmFolder(folder)
    Debug.Notification("warmfolder: " + n + " fuz decoded in '" + folder + "' - now RESTART Skyrim, then play")
EndFunction

; B-experiment: overwrite dst wav's bytes with src's, to learn whether the engine
; re-reads a file per play or caches decoded audio by resource id.
;   autest play <dst>            ; hear dst's line
;   autest boverwrite <dst> <src>
;   autest play <dst>            ; src's line = re-read (B viable); dst's = cached
Function BOverwrite(string dst, string src) global
    if BOverwriteFile(dst, src)
        Debug.Notification("boverwrite OK - now: autest play '" + dst + "' - CHANGED=re-read (B viable), SAME=cached")
    else
        Debug.Notification("boverwrite FAILED - see AudioUtil.log")
    endif
EndFunction

; No-arg B-experiment (Skyrim's console caps input length, so no long paths).
; Uses the first two wavs in the fuz cache: A = [0], B = [1].
;   autest bbase   ; plays A - note what A says (its filename shows in the notice)
;   autest bswap   ; overwrites A's bytes with B, replays A:
;                  ;   hear B's line = engine re-reads  -> B VIABLE
;                  ;   hear A's line = cached/USVFS      -> B DEAD
Function BBase() global
    string a = BCacheFile(0)
    if a == ""
        Debug.Notification("bbase: no cache wavs (play/warm a fuz first)")
        return
    endif
    AudioUtil.PlayFile(a, Game.GetPlayer())
    Debug.Notification("bbase: A = " + a + " (close console, listen)")
EndFunction

Function BSwap() global
    string a = BCacheFile(0)
    string b = BCacheFile(1)
    if a == "" || b == ""
        Debug.Notification("bswap: need 2 cache wavs (warm more fuz first)")
        return
    endif
    if !BOverwriteFile(a, b)
        Debug.Notification("bswap: overwrite FAILED - see AudioUtil.log")
        return
    endif
    AudioUtil.PlayFile(a, Game.GetPlayer())
    Debug.Notification("bswap: replayed A after writing B=" + b + " -> hear B=re-read(B OK), A=cached")
EndFunction

Function LipCap(string mode) global
    if mode == "start"
        if StartLipCapture()
            Debug.Notification("lipcap: recording - talk to NPCs, then 'autest lipcap stop'")
        else
            Debug.Notification("lipcap: already recording")
        endif
    elseif mode == "stop"
        string file = StopLipCapture()
        if file != ""
            Debug.Notification("lipcap: saved Data\\" + file)
        else
            Debug.Notification("lipcap: nothing captured (was it running? did anyone talk?)")
        endif
    else
        Debug.Notification("lipcap: running=" + IsLipCapturing() + " (use start|stop)")
    endif
EndFunction
