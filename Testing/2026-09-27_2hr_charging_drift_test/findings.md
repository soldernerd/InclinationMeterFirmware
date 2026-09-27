# Findings

**Clean run:** ~18Hz, 129151 samples, **zero sequence gaps** over the full 2 hours.

## Noise while charging — clean, actionable finding

| | S1 | S2 |
|---|---|---|
| step-to-step std, baseline | 0.00293mm | 0.00526mm |
| step-to-step std, charging | 0.00387mm (+32%) | 0.00729mm (+39%) |
| jump rate >0.02mm, baseline | 0.028% | 0.813% |
| jump rate >0.02mm, charging | 0.269% (~9.6x) | 2.370% (~2.9x) |

**Recommendation: don't take precision measurements while charging.**

## Drift does NOT reduce to a simple temperature coefficient

This revises the earlier (shorter, 10-minute-session) "~14-15µm/°C" finding —
that number does not hold up over a longer window with a phase change:

- **Temperature correlation flips sign between phases**: baseline r=-0.63(S1)/
  -0.55(S2), charging r=+0.43(S1)/+0.42(S2). A real linear thermal coefficient
  would not flip sign like that.
- **Net drift**: baseline +6.8µm(S1)/+5.1µm(S2) over 60 min; charging
  -22.6µm(S1)/-25.1µm(S2) over 60 min — charging shows ~3-4x more drift AND in
  the opposite direction from baseline.
- Live-observed non-monotonic wobble in the first 30 minutes of charging
  (down ~6µm, up ~6µm, up ~3µm, down ~5-7µm) while temperature was still
  rising then plateauing — not a clean single-time-constant settling curve.

## S1/S2 stay tightly correlated in both phases

r=0.985 (baseline), r=0.999 (charging) — if anything tighter than the earlier
10-minute session. Two independently-aging 45-year-old sensors with their own
internal electronics wouldn't be expected to correlate this tightly by
coincidence. This argues AWAY from "each old sensor's own electronics
drifting independently" as the primary mechanism, and toward something truly
shared: either the asymmetric S-channel-gain-path idea from `setup.md`, or a
shared MECHANICAL effect (a common mounting bracket/plate flexing with
temperature, physically tilting both sensor zero points together) — which
would produce a real x/delta change indistinguishable from tilt by
construction of the demod math, not an electrical artifact that should have
cancelled.

**Still unresolved, not executed this session:** watch the raw A/B exciter
phasor amplitude (Topic 0x5/0x02) during a similar thermal transient. A shared
mechanical tilt would show up as a real x/delta change; an asymmetric
S-channel gain drift would show up as S's own phasor magnitude changing
without a matching real physical change. This would distinguish the two
remaining hypotheses.
