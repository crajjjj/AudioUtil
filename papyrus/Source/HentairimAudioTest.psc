Scriptname HentairimAudioTest Hidden
{Console test harness. With ConsoleUtil: cgf "HentairimAudioTest.T1" etc.}

; basic file play, default flags
Function T1() global
    int h = HentairimAudio.DebugPlayFile("Sound\\fx\\IVDT\\M1\\Orgasm\\01.wav", Game.GetPlayer(), 0x1A, 128)
    Debug.Notification("T1 handle=" + h + " dur=" + HentairimAudio.GetHandleDuration(h))
EndFunction

; flags sweep — run repeatedly with different values via T2F
Function T2F(int flags, int priority) global
    int h = HentairimAudio.DebugPlayFile("Sound\\fx\\IVDT\\M1\\Orgasm\\01.wav", Game.GetPlayer(), flags, priority)
    Debug.Notification("flags=" + flags + " prio=" + priority + " handle=" + h)
EndFunction

; voice via slot resolution on the player
Function T3() global
    int h = HentairimAudio.PlayVoice(Game.GetPlayer(), "Orgasm")
    Debug.Notification("T3 PlayVoice handle=" + h + " slot=" + HentairimAudio.GetSlotForActor(Game.GetPlayer()))
EndFunction

; shuffle-bag check: play a category 5x back to back (watch the log for picked files)
Function T4() global
    int i = 0
    while i < 5
        HentairimAudio.PlayVoiceFromSlot("M3", "Aggressive", Game.GetPlayer())
        Utility.Wait(2.0)
        i += 1
    endwhile
EndFunction

; SFX table
Function T5() global
    HentairimAudio.PlaySFX("MediumClap", Game.GetPlayer())
EndFunction

; channel replacement: second play should cut the first
Function T6() global
    HentairimAudio.PlayVoiceFromSlot("M1", "Lovey Dovey", Game.GetPlayer(), 1.0, "", "test_chan")
    Utility.Wait(0.5)
    HentairimAudio.PlayVoiceFromSlot("M1", "Orgasm", Game.GetPlayer(), 1.0, "", "test_chan")
EndFunction

; group duck: start a line, duck it after a second, restore
Function T7() global
    HentairimAudio.PlayVoiceFromSlot("M1", "Aggressive", Game.GetPlayer(), 1.0, "pc_low")
    Utility.Wait(1.0)
    HentairimAudio.DuckGroup("pc_low")
    Utility.Wait(1.5)
    HentairimAudio.UnduckGroup("pc_low")
EndFunction

; PPA status
Function T8() global
    Debug.Notification("PPA connected=" + HentairimAudio.IsPPAConnected())
EndFunction

Function TReload() global
    Debug.Notification("reload=" + HentairimAudio.ReloadConfig())
EndFunction
