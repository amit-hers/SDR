# Testing rules

Two rules, both earned. Each is written with the failure that produced it,
because a rule without its cause gets relaxed by the next person who finds it
inconvenient.

---

## 1. Every safety or health feature needs a NEGATIVE-path acceptance test

A feature that only proves "the good case works" has not proven it detects the
failure it exists to detect. That is not a hypothetical concern here: **three
features in this project passed their positive tests and were broken in exactly
the case they were written for.**

| feature | positive test | what the negative test found |
|---|---|---|
| peer handshake | both units reported COMPATIBLE | breaking the frequency crossing left UNIT-A reporting **COMPATIBLE for ever** -- with the crossing broken it cannot hear the peer at all, so no contradicting HELLO can arrive. The health signal asserted health precisely when the link was dead. |
| single-instance supervision | one bridge started and stayed | a second bridge **survived 25 s alongside it**, doubling every frame onto the radio, because the duplicate check ran only at the top of a loop that sits in a 60 s poll |
| demodulator recovery | recovered a stall after traffic | could not recover a **cold** stall at all: the monitor armed only after a first frame decoded, so a demodulator that never decoded one stayed dead for ever |

Required negative paths, by feature class:

- **peer/link health** -- a peer that disappears, and one that is incompatible
- **single-instance supervision** -- a duplicate start, and a hung (not crashed) process
- **configuration** -- malformed, truncated, empty, missing required fields, and
  values that are individually valid but together carry nothing
- **loop suppression** -- a real frame returning through the redundant path
- **stimulus monitoring** -- a source deliberately exhausted mid-measurement
- **recovery** -- the fault it recovers from, induced; and a HEALTHY system that
  must NOT be recovered (a false positive here reset a working demodulator
  three times)

---

## 2. Every test must prove its PRECONDITIONS before evaluating its result

A green result from a fixture that was never exercised is worse than a red one:
it closes the question. `tests/precondition.hpp` aborts with exit 2 rather than
reporting, and the Phase 8 harness classifies such a run **INVALID** -- an
outcome distinct from FAIL, because it says nothing about the appliance.

Four suites here reported green while exercising nothing:

- peer fixtures left both MACs zeroed, so every "pair" compared a unit with
  itself -- **13 assertions passed for the wrong reason**
- a config harness never regenerated its file, so each negative case
  re-validated the previous case's already-invalid file -- **15 false passes**
- RF captures ran past the end of the transmit feed, so the tail recorded
  silence; "the demodulator saturates at 11 MS/s" was a fixture artifact and had
  to be retracted
- a status parser matched "rx" inside "rx gap" and reported 0 where the line
  said 863

Minimum preconditions for a multi-unit test: `MAC_A != MAC_B`,
`NODE_ID_A != NODE_ID_B`, `endpoint_A != endpoint_B`, frequencies crossed as
intended, and the stimulus count greater than zero. For a measurement: the
source was still alive when the measurement ended.

---

## Why the format of a number matters

Twice, something re-parsed the bridge's human-readable statistics line and got a
plausible wrong answer -- a pattern for `rx` also matched `rx gap`, and a
pattern containing a slash collided with sed's delimiter. Both were fixed by
tightening the pattern, and both would have recurred, because **the format was
the bug source, not the patterns**. The bridge now publishes JSON directly and
nothing parses prose.
