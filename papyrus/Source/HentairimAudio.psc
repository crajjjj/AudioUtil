Scriptname HentairimAudio Hidden
{Native folder-based audio player for Hentairim. Plays loose WAV files by
 slot/category folder with no sound descriptor forms. See HentairimAudio.toml.}

; ===================== NATIVE — core =====================

int Function GetAPIVersion() global native
bool Function ReloadConfig() global native

; Play a voice line for an actor. Slot resolved from npc_overrides ->
; voicetype_remap -> voicetype_map -> default slot for the actor's sex.
; Returns instance handle (> 0) or 0 on failure (logged, never throws).
; channel != "" stops the channel's previous line first.
int Function PlayVoice(Actor akActor, string category, float volume = 1.0, string group = "", string channel = "") global native

; Play from an explicit slot ("F1".."M8"), following akFollow's position.
int Function PlayVoiceFromSlot(string slot, string category, Actor akFollow, float volume = 1.0, string group = "", string channel = "") global native

; Play a named SFX from the [sfx] table in HentairimAudio.toml.
int Function PlaySFX(string sfxName, Actor akFollow, float volume = 1.0, string group = "sfx", string channel = "") global native

; Play a specific file / random file from a folder (paths relative to Data\).
int Function PlayFile(string dataRelativePath, Actor akFollow, float volume = 1.0, string group = "", string channel = "") global native
int Function PlayFolder(string dataRelativeFolder, Actor akFollow, float volume = 1.0, string group = "", string channel = "") global native

; ===================== NATIVE — handles =====================

bool Function IsHandlePlaying(int handle) global native
bool Function StopHandle(int handle) global native
float Function GetHandleDuration(int handle) global native
Function SetHandleVolume(int handle, float volume) global native

; ===================== NATIVE — groups & channels =====================
; Groups: pc_high / pc_low / partner_high / partner_low / sfx / oneshot / custom

Function SetGroupVolume(string group, float volume) global native
Function DuckGroup(string group, float factor = 0.0) global native
Function UnduckGroup(string group) global native
Function StopGroup(string group) global native
Function StopAllAudio() global native
Function StopChannel(string channel) global native

; ===================== NATIVE — introspection =====================

string Function GetSlotForActor(Actor akActor) global native
int Function GetCategoryFileCount(string slot, string category) global native
bool Function CategoryExists(string slot, string category) global native

; ===================== NATIVE — PPA bridge =====================
; Mod events: "HentairimPPA_Update" / "HentairimPPA_End"
;   sender = receiver Actor, numArg = penetration depth, strArg = context bitmask (decimal)
; Context bits: 1=Vaginal 2=Anal 4=Oral 8=Aggressive 16=FemDom 32=Loving
;               64=Dirty 128=Boobjob 256=Handjob 512=Footjob 1024=Masturbation

bool Function IsPPAConnected() global native
Function SetPPAEventRate(int milliseconds) global native
int Function GetPPAContext(Actor akReceiver) global native
float Function GetPPADepth(Actor akReceiver) global native
float Function GetPPAVaginalOpening(Actor akReceiver) global native
float Function GetPPAAnalOpening(Actor akReceiver) global native

; ===================== NATIVE — debug =====================

int Function DebugPlayFile(string dataRelativePath, Actor akFollow, int flags, int priority) global native

; ===================== WRAPPERS =====================
; SKSE natives cannot be latent, so PlayAndWait semantics live here.

Function WaitForHandle(int handle) global
    if handle <= 0
        return
    endif
    float timeout = GetHandleDuration(handle) + 1.0
    if timeout < 1.5
        timeout = 30.0 ; duration may be 0 while the sound is still being built
    endif
    float waited = 0.0
    while IsHandlePlaying(handle) && waited < timeout
        Utility.Wait(0.1)
        waited += 0.1
    endwhile
EndFunction

Function PlayVoiceAndWait(Actor akActor, string category, float volume = 1.0, string group = "", string channel = "") global
    int h = PlayVoice(akActor, category, volume, group, channel)
    WaitForHandle(h)
EndFunction

Function PlaySFXAndWait(string sfxName, Actor akFollow, float volume = 1.0, string group = "sfx", string channel = "") global
    int h = PlaySFX(sfxName, akFollow, volume, group, channel)
    WaitForHandle(h)
EndFunction

; Drop-in equivalent of the old IVDTControllerScript.PlaySound(Sound, Actor, wait)
Function Play(string category, Actor akActor, bool waitForCompletion = true, float volume = 1.0, string group = "", string channel = "") global
    if waitForCompletion
        PlayVoiceAndWait(akActor, category, volume, group, channel)
    else
        PlayVoice(akActor, category, volume, group, channel)
    endif
EndFunction
