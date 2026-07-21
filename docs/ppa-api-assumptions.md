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

## Performance guidance (unprompted, important)
**Author: "Papyrus can cause crazy lock contention issues, and it's very easy
to overload the VM. PPA API is being called for every actor in the scene,
every game frame."**
→ Validates the bridge design: the per-frame firehose stays in C++ (listener +
snapshot cache); Papyrus only sees throttled mod events (2s default) and
explicit low-frequency polls (expression cycle ~2s, contact edges). Never
poll AudioUtilPPA getters in a tight Papyrus loop.
