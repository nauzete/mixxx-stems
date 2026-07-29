# Background separation queue

`StemSeparationManager` owns a persistent priority queue and a dedicated
`QThreadPool` configured for one low-priority thread. No queue or processor
method is called by the audio thread.

Priority order is:

1. loaded but not playing;
2. next selected;
3. manual;
4. batch.

Jobs with equal priority are FIFO. Duplicate non-terminal cache IDs reuse the
existing job. Retryable errors receive at most two retries by default; a
permanent error or an exhausted retry budget enters `failed`.

## State and progress

The state values are stable for ControlObjects:

| Value | State |
| ---: | --- |
| 0 | idle |
| 1 | queued |
| 2 | preparing |
| 3 | separating |
| 4 | encoding |
| 5 | validating |
| 6 | ready |
| 7 | cancelled |
| 8 | failed |
| 9 | unavailable |
| 10 | paused |

Processor progress is clamped to 0–100 and regressions within an attempt are
ignored. A successful job always finishes at 100.

## Cancellation, pause, and recovery

Active jobs receive lock-free cooperative cancellation and pause flags.
Queued cancellation is immediate. Pausing removes waiting jobs from dispatch
and asks the active processor to stop at its next safe boundary; resuming
requeues paused jobs.

Unfinished requests, priority, FIFO sequence, retry count, and current state
are atomically persisted as JSON. After a process crash, any in-flight state
is restored as queued. Terminal jobs are deliberately not persisted.
