# AudioUtilPPA (penetration bridge)

`Scriptname AudioUtilPPA Hidden` — an **optional** bridge to the third-party *Accurate Penetration* (PPA) plugin. Everything here requires that plugin to be installed **and** `[ppa] enable = true` in `AudioUtil.toml`. Without it, `IsConnected()` returns `false` and every getter returns `0`.

The core player API in [AudioUtil](audioutil.md) works independently of this script.

## How it works

While connected, AudioUtil listens to PPA's interaction events, keeps a **per-receiver snapshot** (depth, context bitmask, penetration sites, opening values) in the DLL, and fires throttled mod events:

| Event | When |
|-------|------|
| `AudioUtilPPA_Update` | state for a receiver changed / periodic tick |
| `AudioUtilPPA_End` | all interactions on a receiver stopped |

Event payload: `sender` = the **receiver `Actor`**, `numArg` = depth value, `strArg` = context bitmask (decimal string).

Updates are throttled to one per receiver per `event_rate_ms`, **but a context bitmask change is sent immediately** — transitions are never late.

!!! danger "Do not poll in a tight loop"
    The underlying PPA API fires **for every actor in the scene, every game frame**. That firehose stays in the DLL, which caches a per-receiver snapshot. The getters below read that cache cheaply, but they are still external Papyrus calls — poll them at your own low cadence (contact edges, expression cycles), **never** in a per-frame loop. PPA's author warns that Papyrus lock contention here can overload the VM.

### Handler example

```papyrus
RegisterForModEvent("AudioUtilPPA_Update", "OnPPAUpdate")

Event OnPPAUpdate(string eventName, string strArg, float numArg, Form sender)
    Actor receiver = sender as Actor
    int ctx = strArg as int
    float depth = numArg
EndEvent
```

## Functions

### `IsConnected`

```papyrus
bool Function IsConnected() global native
```

`True` when the PPA plugin is loaded and its API handshake succeeded. Cheap — safe to call per decision rather than caching.

### `SetEventRate`

```papyrus
Function SetEventRate(int milliseconds) global native
```

Throttle for `AudioUtilPPA_Update` events per receiver (default from the TOML `event_rate_ms`). Lower = more responsive motion sync, more Papyrus load. **Floored at 1000 ms** — smaller values (and `≤ 0`) are clamped up to keep the per-frame firehose off the Papyrus VM.

### `GetContext`

```papyrus
int Function GetContext(Actor akReceiver) global native
```

Current **context bitmask** for a receiver — an `int` where each bit is an independent flag; several are typically set at once. Per PPA's author this is scene **classification** ("just like what you'd see in SexLab, but abstracted… and more narrow") — *what kind of scene/act this is, **not** live collision state*. For "physically inserted right now", combine an act bit with `GetDepth() > 0`.

| value | bit | flag |
|------:|----:|------|
| 1 | 1 | Vaginal |
| 2 | 2 | Anal |
| 4 | 3 | Oral |
| 8 | 4 | Aggressive *(tone)* |
| 16 | 5 | FemDom *(tone)* |
| 32 | 6 | Loving *(tone)* |
| 64 | 7 | Dirty *(tone)* |
| 128 | 8 | Boobjob |
| 256 | 9 | Handjob |
| 512 | 10 | Footjob |
| 1024 | 11 | Masturbation |

A returned value is the **sum** of its active bits — combinations add up (e.g. `3` = `1+2` = Vaginal + Anal; `41` = `1+8+32` = Vaginal, aggressive + loving). Any sum is possible; never enumerate expected totals.

`0` means "nothing tracked" and is deliberately ambiguous: plugin not connected, actor unknown, or nothing happening right now. Treat it as "**no measurement available**", not a definite no.

Read individual bits with `Math.LogicalAnd` — never compare the whole value, which breaks the moment a second bit joins:

```papyrus
int ctx = AudioUtilPPA.GetContext(actorref)
if ctx > 0                                 ; actively tracked
    bool vaginal = Math.LogicalAnd(ctx, 1) == 1
    bool anal    = Math.LogicalAnd(ctx, 2) == 2
endif
```

The same mask is delivered as `strArg` in `AudioUtilPPA_Update` events, and a **change** in it is what triggers the immediate, throttle-bypassing send. The getter polls the snapshot, so it is current regardless of the event throttle.

### `GetDepth`

```papyrus
float Function GetDepth(Actor akReceiver) global native
```

Current depth for a receiver: the deepest active interaction across all partners; `0.0` when idle. Distance-like units — PPA's own depth-scaled effects treat roughly `2.0` (shallow) to `10.0` (deep) as the working range.

### `GetVaginalOpening` / `GetAnalOpening`

```papyrus
float Function GetVaginalOpening(Actor akReceiver) global native
float Function GetAnalOpening(Actor akReceiver) global native
```

Current opening values reported by PPA. Per its author these are **"magic unsigned numbers"** with exactly one contract: `0.0` = closed, larger = more open, **no defined scale or units**. The two orifices use different internal scales, so thresholds must be **per-orifice and calibrated empirically** — log real values in a scene; never hardcode range assumptions. The author discourages casual consumption of these; use only for a specific purpose.

### `GetPenetrationSite` / `GetPenetrationSites` / `GetSelfPenetrationSite`

```papyrus
int Function GetPenetrationSite(Actor akReceiver) global native
int Function GetPenetrationSites(Actor akReceiver) global native
int Function GetSelfPenetrationSite(Actor akReceiver) global native
```

Requires API version **>= 9**; guard with `GetAPIVersion()` on older installs, or
the call cannot bind.

**Where**, as opposed to `GetContext`'s *what kind of scene*. The context bits are
scene classification and stay fixed the way a SexLab tag does; these report the
live `PenetrationSite` PPA tracks per partner, which is what its own in-scene
redirect menu rewrites. If a scene starts vaginal and the player switches hole,
the context bits do not move but these do.

| value | site |
|---|---|
| `0` | None — no measurement (see below) |
| `1` | Mouth |
| `2` | Anus |
| `3` | Vagina |
| `4` | Both |
| `5` | HandL |
| `6` | HandR |
| `7` | Hands |

These are **ordinals, not bit flags** — compare with `==`, never `Math.LogicalAnd`.
`0` carries the same deliberate ambiguity as `GetContext`'s `0`: PPA not connected,
actor unknown, or nothing happening. "No measurement available", not a definite no.

`GetPenetrationSite` is the **deepest partner's** site — where this actor is being
taken. Note it is a narrower pick than `GetDepth`, which also counts
self-interaction and partners reporting no site, so the two can describe different
interactions: never read `GetDepth() > 0 && GetPenetrationSite() == 2` as "anal,
this deep".

`GetPenetrationSites` is **every partner's site at once**, as a bitmask, so a DP
reports both. A site's bit is `1` shifted left by its ordinal, so there is no bit
`0` and a mask of `0` means no site reported:

| site ordinal | value | site |
|---|---|---|
| 1 | `2` | Mouth |
| 2 | `4` | Anus |
| 3 | `8` | Vagina |
| 4 | `16` | Both |
| 5 | `32` | HandL |
| 6 | `64` | HandR |
| 7 | `128` | Hands |

Test the **value** column, never the ordinal — Anus is ordinal `2` but value `4`:

```papyrus
int sites = AudioUtilPPA.GetPenetrationSites(actorref)
bool oral = Math.LogicalAnd(sites, 2) == 2
```

`GetSelfPenetrationSite` is **self-penetration** — the hole the receiver is using on
*themselves* (masturbation). Per PPA's docs that site is on the receiver too, like
the two above; it is a different **act**, not a different body, which is why it is
reported separately rather than merged into the mask.

!!! warning "Poll these — do not wait for an event"
    `AudioUtilPPA_Update` carries only depth and the context bitmask, so the site
    is **not in the payload**. Unlike a context change, a site change does **not**
    bypass the event throttle: the site is live per-frame data and can flicker, so
    bypassing on it would fire an event per frame per receiver — the VM overload
    the throttle exists to prevent. The getters read the cached snapshot, which is
    refreshed on every PPA tick, so they are always current no matter when the last
    event fired.

!!! note "Liveness is assumed, not documented"
    PPA's published docs describe `site` as the "target site of penetration" but do
    not state that it follows a mid-scene redirect, nor do they document callback
    frequency. Treat a site as a hint that improves a choice, not as ground truth to
    gate a whole branch on.

### `GetSnapshot`

```papyrus
float[] Function GetSnapshot(Actor akReceiver) global native
```

Requires API version **>= 10**; guard with `GetAPIVersion()` on older installs, or
the call cannot bind.

The whole per-receiver snapshot in **one** call, for a consumer that asks several
questions about the same moment. A voice scheduler reading depth + context + site
per line pays three VM round-trips through the scalar getters; this is one. The
values come from the same cache those getters read, taken together, so the slots
always describe a single consistent snapshot rather than three reads that could
straddle a PPA tick.

Returns an **empty array** when there is no measurement - plugin not connected,
actor unknown, or nothing tracked. That is the same deliberate ambiguity as the
scalar getters' `0`, so test `length` before reading. Otherwise **7 floats**:

| slot | value | see |
|---|---|---|
| `[0]` | depth | [`GetDepth`](#getdepth) |
| `[1]` | context bitmask | [`GetContext`](#getcontext) |
| `[2]` | penetration site ordinal | [`GetPenetrationSite`](#getpenetrationsite-getpenetrationsites-getselfpenetrationsite) |
| `[3]` | sites bitmask | [`GetPenetrationSites`](#getpenetrationsite-getpenetrationsites-getselfpenetrationsite) |
| `[4]` | self-penetration site | [`GetSelfPenetrationSite`](#getpenetrationsite-getpenetrationsites-getselfpenetrationsite) |
| `[5]` | vaginal opening | [`GetVaginalOpening`](#getvaginalopening-getanalopening) - magic number |
| `[6]` | anal opening | [`GetAnalOpening`](#getvaginalopening-getanalopening) - magic number |

The layout is **append-only**: a later version may add slots, but these never move.

The int-valued slots are exact - their ranges sit far below float's `2^24` integer
ceiling - so cast back with `as int` before any bit test:

```papyrus
float[] snap = AudioUtilPPA.GetSnapshot(actorref)
if snap.length > 0
    int ctx   = snap[1] as int
    int site  = snap[2] as int
    bool anal = Math.LogicalAnd(ctx, 2) == 2 && snap[0] > 0.0
endif
```

!!! tip "One call or several?"
    Reading a single field stays cheaper through its scalar getter. Reach for
    `GetSnapshot` from the second field onward - and always when the fields must
    agree with each other, since the scalar getters are separate reads of a cache
    PPA refreshes every tick.
