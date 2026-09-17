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

; -----------------------------------------------------------------------------
; WHERE, as opposed to GetContext's WHAT KIND OF SCENE.
;
; GetContext reports the plugin's scene CLASSIFICATION, which is fixed for the
; scene the way a SexLab tag is. These report the live PenetrationSite the
; plugin tracks per partner - which is what its own in-scene redirect menu
; rewrites. If a scene starts vaginal and the player switches hole, the context
; bits do NOT move but these do.
;
;   value  site
;       0  None      (no measurement - see the ambiguity note below)
;       1  Mouth
;       2  Anus
;       3  Vagina
;       4  Both
;       5  HandL
;       6  HandR
;       7  Hands
;
; These are ORDINALS, not bit flags - compare with ==, never Math.LogicalAnd.
; The bitmask form is GetPenetrationSites() below.
;
; 0 carries the same deliberate ambiguity as GetContext's 0: plugin not
; connected, actor unknown, or nothing happening. "No measurement available",
; not a definite no.
;
; NOTE these are read off the third-party plugin's per-frame data, whose site
; semantics its author has not documented for us the way the context bits were
; (see docs/ppa-api-assumptions.md). Treat a site as a hint that improves a
; line choice, not as ground truth to gate a whole branch on.
;
; Deepest partner's site - where THIS actor is being taken.
;
; NOT the same pick as GetDepth(): depth also counts self-interaction and
; partners reporting no site, so the two can describe different interactions.
; Do NOT read "GetDepth() > 0 && GetPenetrationSite() == 2" as "anal, this
; deep" - the depth may belong to something else entirely.
int Function GetPenetrationSite(Actor akReceiver) global native

; Every partner's site at once, as a bitmask - DP reports both. A site's bit is
; 1 shifted left by its ORDINAL from the table above, so site 0 (None) has no
; bit and a mask of 0 means "no site reported":
;
;   site ordinal  value  site
;              1      2  Mouth
;              2      4  Anus
;              3      8  Vagina
;              4     16  Both
;              5     32  HandL
;              6     64  HandR
;              7    128  Hands
;
; Test the VALUE column, never the ordinal: Anus is ordinal 2 but value 4.
;
; Like GetContext, a returned value is the SUM of its active bits - decompose
; it with Math.LogicalAnd, never compare the whole value:
;
;   int sites = AudioUtilPPA.GetPenetrationSites(actorref)
;   bool oral = Math.LogicalAnd(sites, 2) == 2
;
; POLL this - do not wait for an event. AudioUtilPPA_Update carries only depth
; and the context bitmask, so the site is not in the payload, and unlike a
; context change a site change does NOT bypass the throttle (it is live
; per-frame data and would fire an event per frame). The getters above read the
; cached snapshot, which is updated on every plugin tick and is therefore always
; current no matter when the last event fired - so a mid-scene redirect is
; visible immediately to a caller that asks.
int Function GetPenetrationSites(Actor akReceiver) global native

; Self-penetration: the hole the receiver is using on THEMSELVES (masturbation).
; Still a site on this actor, like the two above, but a different act - being
; taken by a partner and doing it yourself are never merged into one answer.
; 0 for most actors. Same value table.
int Function GetSelfPenetrationSite(Actor akReceiver) global native

; The whole per-receiver snapshot in ONE call (AudioUtil API v10) - for a
; consumer that asks several questions about the same moment. A voice scheduler
; reading depth + context + site per line pays three VM round-trips through the
; scalar getters above; this is one. The values are the same cache the scalar
; getters read, taken together, so the slots always describe one snapshot.
;
; Returns an EMPTY array when there is no measurement - plugin not connected,
; actor unknown, or nothing tracked - the same deliberate ambiguity as the
; scalar getters' 0. Test arr.length before reading. Otherwise 7 floats
; (append-only layout - later versions may add slots, never move these):
;
;   [0] depth                     (see GetDepth)
;   [1] context bitmask           (see GetContext - decompose with LogicalAnd)
;   [2] penetration site ordinal  (see GetPenetrationSite)
;   [3] sites bitmask             (see GetPenetrationSites)
;   [4] self-penetration site     (see GetSelfPenetrationSite)
;   [5] vaginal opening           (see GetVaginalOpening - magic number)
;   [6] anal opening              (see GetAnalOpening - magic number)
;
; The int-valued slots are exact (their ranges sit far below float's 2^24
; integer ceiling); cast back with `as int` before bit tests:
;
;   float[] snap = AudioUtilPPA.GetSnapshot(actorref)
;   if snap.length > 0
;       int ctx = snap[1] as int
;       int site = snap[2] as int
;   endif
float[] Function GetSnapshot(Actor akReceiver) global native
