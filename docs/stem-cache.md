# Stem cache

The separated-track cache is owned by the low-priority background worker. It
does not perform work on the audio thread.

## Identity

Each filename is the hexadecimal SHA-256 of a length-delimited identity that
contains:

- SHA-256, size, and modification time of the source;
- model name, version, and SHA-256;
- codec and per-stream bitrate;
- output-container version.

Changing any input produces a new entry ID. Old model or encoding results are
therefore never returned as current results and remain eligible for normal LRU
cleanup.

## Limits and protection

Defaults are a 4 GiB quota and 5 GiB free-space reserve on Windows, and a
1 GiB quota and 2 GiB reserve on other targets, including Ubuntu ARM64. Limits
and the 256 MiB per-track guard are configurable.

Capacity is reclaimed from the least recently accessed entry. The queue must
provide the complete protected-ID snapshot on every reservation or removal.
That snapshot includes loaded, playing, processing, pinned, immediate-queue,
and currently open entries. Protected entries are never selected for eviction.

## Persistence and recovery

The JSON index is committed through `QSaveFile`. At startup the cache:

1. discards index records whose files are missing or changed;
2. removes cache-owned `<id>.stem.mp4.partial` files left by interrupted jobs;
3. recovers completed `<id>.stem.mp4` files missing from the index;
4. applies quota and free-space limits;
5. atomically rewrites the reconciled index.

Files that do not match an exact cache-owned name are ignored.
