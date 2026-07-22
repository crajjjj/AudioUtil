# AudioUtilPPA (penetration bridge)

`Scriptname AudioUtilPPA Hidden` — an **optional** bridge to the third-party *Accurate Penetration* (PPA) plugin. Everything here requires that plugin to be installed **and** `[ppa] enable = true` in `AudioUtil.toml`. Without it, `IsConnected()` returns `false` and every getter returns `0`.

The core player API in [AudioUtil](audioutil.md) works independently of this script.

## How it works

While connected, AudioUtil listens to PPA's interaction events, keeps a **per-receiver snapshot** (depth, context bitmask, opening values) in the DLL, and fires throttled mod events:

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

Throttle for `AudioUtilPPA_Update` events per receiver (default from the TOML `event_rate_ms`). Lower = more responsive motion sync, more Papyrus load.

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
