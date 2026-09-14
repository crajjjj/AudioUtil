# PPA API — author's answers (was: assumptions to confirm)

Asked 2026-07-21, answered by the author. Original assumptions kept for
context, with the correction and what we changed because of it.

## 1. Opening units — WRONG assumption
We assumed openings are distances on the RaceConfig `Scale`/`ScaleMax` axis.
**Author: "Don't try to infer direct data from opening/closing. Treat them as
magic unsigned numbers. 0.0 = closed. I wouldn't use them unless you have a
very specific purpose."**
→ The only contract: `0.0` = closed, larger = more open, no defined scale.
Our gape thresholds stay per-orifice but are now documented as EMPIRICAL
starting points to calibrate via printdebug, not values derived from config.

## 2. Openings while idle — CONFIRMED (implicitly)
`0.0 = closed` confirms an idle orifice reads 0.0; our "0 = no measurement"
fallback stands.

## 3. Decay after pull-out — UNANSWERED
Still unknown whether values stay elevated briefly after withdrawal. Our
read-at-pull-out-edge gape check works in practice; keep printdebug evidence.

## 4. Context act bits — WRONG assumption
We treated Vaginal/Anal bits as live collision state.
**Author: "Context is just like what you'd see in SexLab. But abstracted from
SexLab/OStim and obviously more narrow."**
→ Bits are scene/animation CLASSIFICATION, not physical contact. They're a
framework-agnostic version of animation tags — same tier as our labels, not a
measured upgrade. For "physically inserted right now", `penetrationDepth > 0`
is the physical signal; expression checks now require ctx bit AND depth > 0.

## 5. `ending` semantics — CONFIRMED
**Author: "Ending is always sent."** State-erase on `ending` is safe.

## 6. `InteractionPartner::site` — PARTLY ANSWERED (exposed in 0.9.21)
`PenetrationSite` (None/Mouth/Anus/Vagina/Both/HandL/HandR/Hands, ordinals in
that order) sits on the per-partner struct next to `penetrationDepth`. We read
it and expose it as `GetPenetrationSite`/`GetPenetrationSites`/
`GetSelfPenetrationSite`.

**Checked against the published docs** (asdasdduck.github.io/ppa-docs/skse-api):
our recreated `include/API/AccuratePenetrationAPI.h` matches the reference
header exactly — same field order, same types, same enumerator order — so the
ordinals we publish to Papyrus are right.

**ANSWERED — whose anatomy.** Docs: a partner's `site` is "where they are
penetrating the receiver", and selfInteraction's "refers to where the receiver
is self-penetrating". So BOTH name a hole on the receiver; selfInteraction is
masturbation, not a hole on someone else. (Our first reading had this backwards
and said so in the psc — corrected in 0.9.21 before release.)

**STILL UNANSWERED — is it live.** The docs describe `site` as "target site of
penetration" and do not say whether it tracks a mid-scene redirect through PPA's
own menu, nor do they document callback frequency ("callbacks run during
Accurate Penetration's main-thread update and cleanup"). The whole value of the
field to us rests on it being live, so this is the one to confirm in a scene:
switch hole in PPA's menu and watch whether the reported site follows.

If it does not, the feature is inert and should be documented as such rather
than left as a trap; the psc already tells consumers to treat a site as a hint
that improves a line choice, not as ground truth to gate a branch on.

**Threading, while we were in there.** Docs: callbacks arrive "on Skyrim's main
thread" during PPA's update, and listeners must be registered/unregistered on
that thread. Our bridge is fine either way (the snapshot mutex is still needed,
since Papyrus reads the cache from the VM thread), but our own notes used to say
the callback ran on "PPA's thread" — corrected.

## Performance guidance (unprompted, important)
**Author: "Papyrus can cause crazy lock contention issues, and it's very easy
to overload the VM. PPA API is being called for every actor in the scene,
every game frame."**
→ Validates the bridge design: the per-frame firehose stays in C++ (listener +
snapshot cache); Papyrus only sees throttled mod events (2s default) and
explicit low-frequency polls (expression cycle ~2s, contact edges). Never
poll AudioUtilPPA getters in a tight Papyrus loop.
