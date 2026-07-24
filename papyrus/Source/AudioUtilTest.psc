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

; Play an SFX by name at the player.  e.g.  autest sfx MediumClap
Function Sfx(string name) global
    int h = AudioUtil.PlaySFX(name, Game.GetPlayer())
    Debug.Notification("sfx " + name + " handle=" + h)
EndFunction
