Scriptname AudioUtilPPA Hidden
{Optional bridge to the third-party PPA plugin. Everything here requires that
 plugin to be installed and [ppa] enable = true in AudioUtil.toml; without it
 IsConnected() returns false and every getter returns 0. The core player API
 lives in AudioUtil.psc and works independently of this script.}

; =============================================================================
; While connected, AudioUtil listens to the PPA plugin's interaction events,
; keeps a per-receiver snapshot (depth, context bitmask, opening values), and
; fires throttled mod events:
;
;   "AudioUtilPPA_Update"  - state for a receiver changed / periodic tick
;   "AudioUtilPPA_End"     - all interactions on a receiver stopped
;   sender = receiver Actor, numArg = depth value, strArg = context bitmask
;   (decimal string)
;
; Updates are throttled to one per receiver per event_rate_ms, but a context
; bitmask CHANGE is sent immediately - transitions are never late.
; The context bit table is documented at GetContext below.
;
; PERFORMANCE: the underlying plugin API fires for every actor in the scene
; every game frame - that firehose stays in the DLL, which keeps a cached
; per-receiver snapshot. The getters below read that cache cheaply, but they
; are still external Papyrus calls: poll them at your own low cadence
; (contact edges, expression cycles), NEVER in a tight per-frame loop - the
; plugin's author warns Papyrus lock contention here can overload the VM.
;
; Handler example:
;   RegisterForModEvent("AudioUtilPPA_Update", "OnPPAUpdate")
;   Event OnPPAUpdate(string eventName, string strArg, float numArg, Form sender)
;       Actor receiver = sender as Actor
;       int ctx = strArg as int
;       float depth = numArg
;   EndEvent
; =============================================================================

; True when the PPA plugin is loaded and its API handshake succeeded.
; Cheap - safe to call per decision rather than caching.
bool Function IsConnected() global native

; Throttle for AudioUtilPPA_Update events per receiver (default from toml
; event_rate_ms). Lower = more responsive motion sync, more Papyrus load.
Function SetEventRate(int milliseconds) global native

; Current context bitmask for a receiver - an int where each bit is an
; independent flag and several are typically set at once. Per the plugin's
; author this is scene CLASSIFICATION "just like what you'd see in SexLab,
; but abstracted from SexLab/OStim and more narrow" - i.e. what kind of
; scene/act this is, NOT live collision state. For "physically inserted
; right now", combine an act bit with GetDepth() > 0.
;
;   value  bit  flag
;       1    1  Vaginal
;       2    2  Anal
;       4    3  Oral
;       8    4  Aggressive     (tone)
;      16    5  FemDom         (tone)
;      32    6  Loving         (tone)
;      64    7  Dirty          (tone)
;     128    8  Boobjob
;     256    9  Handjob
;     512   10  Footjob
;    1024   11  Masturbation
;
; A returned value is the SUM of its active bits - combinations add up:
;
;   value  =              reads as
;       3  = 1+2          Vaginal + Anal (DP)
;       9  = 1+8          Vaginal, aggressive
;      36  = 4+32         Oral, loving
;      41  = 1+8+32       Vaginal, aggressive + loving
;     130  = 2+128        Anal + Boobjob (two partners)
;    1025  = 1+1024       Vaginal + Masturbation
;
; Any sum is possible; never enumerate expected totals - decompose by
; testing the individual bits you care about.
;
; 0 means "nothing tracked" and is deliberately ambiguous: plugin not
; connected, actor unknown, or no interaction happening right now. Treat it
; as "no measurement available", not as a definite no.
;
; Read individual bits with Math.LogicalAnd - never compare the whole value,
; that breaks as soon as a second bit joins:
;   int ctx = AudioUtilPPA.GetContext(actorref)
;   if ctx > 0                                 ; actively tracked
;       bool actA = Math.LogicalAnd(ctx, 1) == 1
;       bool actB = Math.LogicalAnd(ctx, 2) == 2
;   endif
;
; Polls the per-receiver snapshot kept on every plugin update, so the value
; is current regardless of the event throttle. The same mask is delivered as
; strArg (decimal string) in AudioUtilPPA_Update events, and a CHANGE in it
; is what triggers the immediate, throttle-bypassing event send.
int Function GetContext(Actor akReceiver) global native

; Current depth value for a receiver: the deepest active interaction across
; all partners; 0.0 when idle. Distance-like units - the plugin's own
; depth-scaled effects treat roughly 2.0 (shallow) to 10.0 (deep) as the
; working range.
float Function GetDepth(Actor akReceiver) global native

; Current opening values reported by the plugin. Per its author these are
; "magic unsigned numbers" with exactly one contract: 0.0 = closed, larger =
; more open, NO defined scale or units. The two orifices use different
; internal scales, so thresholds must be per-orifice and calibrated
; empirically (log real values in a scene; never hardcode assumptions about
; the range). Use only for a specific purpose - the author discourages
; casual consumption of these.
float Function GetVaginalOpening(Actor akReceiver) global native
float Function GetAnalOpening(Actor akReceiver) global native
