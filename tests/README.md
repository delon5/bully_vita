# Texture cache tests

`loader/texture_cache.c` is plain C driven entirely through GL and a handful of
Vita OS calls, so it can be exercised on a build machine with stand-ins for
both. These run in a few seconds and need no hardware.

```bash
make -C tests VITASDK=/path/to/vitasdk
```

They compile against the real VitaSDK headers, so the enum values and function
signatures are the ones the cache will meet on the device. That matters: one of
the bugs these tests exist to prevent was calling `vglMemFree(VGL_MEM_ALL)`,
which quietly returns 0 because `VGL_MEM_ALL` is the enum terminator.

`fake_vitagl.c` stands in for the driver. It models the behaviour the cache
actually depends on rather than being a generic mock: texture names are
recycled, re-specifying a texture frees what it held, an upload the driver
rejects allocates nothing *after* freeing what was there, and a cube map is six
faces behind one name. Running it out of memory aborts, because that is the
crash this whole feature exists to prevent.

`fake_vita_os.c` provides the file and thread calls on top of POSIX, so the
writer thread, the semaphore handshake and the on-disk record format are
genuinely exercised rather than mocked away.

| suite | what it pins down |
| --- | --- |
| `test_budget` | memory stays bounded across 40,000 frames of streaming through a driver smaller than the game's texture set |
| `test_revisit` | leaving an area and coming back restores its textures; before the backing store existed, 128 of 200 came back white |
| `test_regress` | the specific mistakes already made once: `VGL_MEM_ALL`, cube maps tracked as flat textures, uploads the driver rejected being accounted, the cache file never reclaiming space, a failed restore leaving freed memory bound |
| `test_no_backing_store` | when nothing can be copied, textures are held rather than lost, until memory genuinely has to be bounded |

The suites are checked by mutation: reintroducing any of the bugs above makes at
least one of them fail. If you change the cache, break it on purpose once and
confirm a test notices — a suite that cannot fail is not protecting anything.
