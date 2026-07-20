Scriptname AudioUtilTest Hidden
{Console test harness. With ConsoleUtil: cgf "AudioUtilTest.T1" etc.}

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
    Debug.Notification("PPA connected=" + AudioUtil.IsPPAConnected())
EndFunction

Function TReload() global
    Debug.Notification("reload=" + AudioUtil.ReloadConfig())
EndFunction
