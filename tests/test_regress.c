/* test_regress.c -- the specific mistakes this cache has already made once.
 *
 * Each of these was a real bug found by reading vitaGL's source, and each one
 * silently corrupted something rather than failing loudly.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "harness.h"

#define TEX_BYTES (256 * 1024)

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  harness_start(512 * MB);

  // vglMemFree cannot be asked for the total. VGL_MEM_ALL is the enum
  // terminator and the wrapper returns 0 for it, so a cache that asked would
  // believe it was permanently out of memory and evict everything it could.
  assert(vglMemFree(VGL_MEM_ALL) == 0 && "the trap this fake exists to preserve");
  size_t pools[VGL_POOLS];
  vitagl_free_per_pool(pools);
  assert(pools[0] + pools[1] + pools[2] > 0 && "asking per pool must see the real figure");
  printf("free memory  : read per pool, not via VGL_MEM_ALL     OK\n");

  // Cube map faces come through the same entry point but are six buffers behind
  // one name. Tracking one as a flat texture meant freeing all six and
  // restoring a single face as a 2D texture.
  GLuint cube;
  glGenTexturesHook(1, &cube);
  glBindTextureHook(GL_TEXTURE_2D, cube);
  fill_source(0xC0DE0001u, TEX_BYTES);
  glCompressedTexImage2DHook(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0,
                             GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, 128, 128, 0, 8192, source_bytes);
  assert(!textures[cube].tracked && "a cube map face must not be tracked as a 2D texture");
  assert(tracked_bytes == 0 && "a cube map face must not be accounted");
  // And it must still be a cube map after enough pressure to evict anything the
  // cache thinks it owns.
  for (int i = 0; i < 96; i++) { tex_upload(0x7000u + i, 512, 512, TEX_BYTES); drain(); }
  frames(TEXTURE_IDLE_FRAMES * 4);
  assert(fake_sampled(cube) == 0xCBCBCBCBu && "the cube map must survive eviction untouched");
  printf("cube maps    : not tracked, and survive eviction      OK\n");

  // vitaGL reports a rejected upload by returning having allocated nothing.
  // Counting those left phantom bytes in the budget and let the cache "evict" a
  // texture that was never there, which actually allocated a placeholder.
  size_t before = tracked_bytes;
  GLuint bad;
  glGenTexturesHook(1, &bad);
  glBindTextureHook(GL_TEXTURE_2D, bad);
  fake_reject_next_upload = 1;
  glCompressedTexImage2DHook(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, 130, 130, 0,
                             8192, source_bytes);
  assert(tracked_bytes == before && "a rejected upload must not be accounted");
  assert(!textures[bad].tracked && "a rejected upload must not be tracked");
  printf("bad uploads  : rejected, not accounted                OK\n");

  // The store used to be one file carved into extents, and re-uploading a
  // texture appended rather than reusing its space, so a long session -- the
  // exact case this feature exists for -- walked the file to its cap and
  // everything after that degraded to white. Uploading writes nothing at all
  // now, so no amount of re-streaming can grow the store.
  GLuint churn[64];
  for (int i = 0; i < 64; i++) {
    churn[i] = tex_upload(0xF00D0000u + i, 512, 512, TEX_BYTES);
    drain();
  }
  uint32_t after_one_round = fake_store_files();
  for (int round = 0; round < 40; round++)
    for (int i = 0; i < 64; i++) {
      tex_reupload(churn[i], 0xF00D0000u + i, 512, 512, TEX_BYTES);
      drain();
    }
  printf("store growth : %u files after 1 round, %u after 41     OK\n", after_one_round,
         fake_store_files());
  assert(fake_store_files() == after_one_round &&
         "re-uploading the same contents must not add files");

  // Uploading a texture is the hot path -- an area transition is thousands of
  // them back to back -- and it must not touch the card. Writing at upload time
  // is what made loading an area take minutes.
  uint32_t writes_before = fake_store_writes();
  for (int i = 0; i < 64; i++) {
    tex_reupload(churn[i], 0xF00D0000u + i, 512, 512, TEX_BYTES);
    drain();
  }
  assert(fake_store_writes() == writes_before && "uploading must never write to the card");
  printf("upload cost  : no card writes on the upload path       OK\n");

  // Evicting must not write either, while the heap still has room for the
  // copies. A card write costs far more than a frame, and doing every eviction
  // through the card turned reclaiming into a visible stall.
  {
    harness_start(512 * MB);
    const int count = (int)(((size_t)TEXTURE_RAM_CACHE_MB * MB / 2) / TEX_BYTES);
    GLuint cold[256];
    assert(count > 0 && count <= 256);
    for (int i = 0; i < count; i++) {
      cold[i] = tex_upload(0xEE000000u + i, 512, 512, TEX_BYTES);
      drain();
    }
    // Enough pressure to force them out, from textures that stay drawn.
    GLuint hot[64];
    for (int i = 0; i < 64; i++) {
      hot[i] = tex_upload(0xEF000000u + i, 512, 512, TEX_BYTES);
      drain();
    }
    while (tracked_bytes < (size_t)TEXTURE_BUDGET_MB * MB + 32 * MB) {
      tex_upload(0xF1000000u + (unsigned)tracked_bytes, 512, 512, TEX_BYTES);
      drain();
    }
    wander(hot, 64, TEXTURE_IDLE_FRAMES * 3);

    int evicted = 0;
    for (int i = 0; i < count; i++)
      if (textures[cold[i]].evicted)
        evicted++;
    printf("heap tier    : %d evicted, %u to heap, %u to card, %zu MB parked  OK\n", evicted,
           ram_evicted_count, card_evicted_count, ram_cache_bytes / MB);
    assert(evicted > count / 2 && "the cold set should have been evicted");
    assert(ram_evicted_count > 0 && "eviction must use the heap");
    // The card is the overflow, not the destination: nothing may be written
    // while the tier still had room for it.
    assert((card_evicted_count == 0 ||
            ram_cache_bytes + TEX_BYTES > (size_t)TEXTURE_RAM_CACHE_MB * MB) &&
           "the card must not be touched until the heap tier is full");

    // And they must come back from the heap just as they would off the card.
    wander(cold, count, 4);
    for (int i = 0; i < count; i++)
      assert(fake_sampled(cold[i]) == fake_fingerprint_of(0xEE000000u + i) &&
             "a texture parked in the heap must restore correctly");
    printf("heap restore : parked textures come back byte for byte OK\n");
  }

  // The store outlives the run, and a texture it already holds costs nothing at
  // all to evict. This is what the persistent store is for: evicting used to
  // mean writing a few hundred KB inside a frame, which is why writes are
  // rationed to emergencies, which is why a session with the card shut evicted
  // 337 textures and then watched the pools drain to nothing.
  {
    // A driver small enough that the RAM pool genuinely runs out -- smaller
    // than the byte budget, so the budget cannot cap the pressure first. The
    // card is only for that case now: past the heap tier, with memory to spare,
    // a texture stays resident rather than costing a write inside a frame.
    const size_t over = ((size_t)TEXTURE_BUDGET_MB + TEXTURE_RAM_CACHE_MB + 96) * MB;
    const int count = (int)(over / TEX_BYTES);
    GLuint ids[1536];
    assert(count <= (int)(sizeof(ids) / sizeof(ids[0])));

    harness_start_empty(192 * MB);
    assert(store_files == 0 && "this block starts from an empty store");

    for (int i = 0; i < count; i++) {
      tex_upload(0xDE000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      texture_cache_tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    unsigned spilled = fake_store_files();
    assert(spilled > 0 && "a pool actually running out has to be able to reach the card");

    // A restart, and then the same assets again. Different texture names, same
    // textures: the store is keyed by what they are, not where they landed.
    fake_reset(192 * MB);
    texture_cache_init();
    assert(fake_store_files() == spilled && "the store must survive a restart");
    assert(store_files == spilled && "and be read back into the index");

    for (int i = 0; i < count; i++) {
      ids[i] = tex_upload(0xDE000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      texture_cache_tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    printf("store reuse  : %u spilled, %u evictions free on the next run, %u written  OK\n",
           spilled, store_reused, card_evicted_count);
    assert(store_reused >= spilled && "every stored texture must evict for free");
    // Not zero, and it should not be. Freeing an eviction from its ration means
    // more of them succeed, so textures that only ever reached the heap tier on
    // the first run get as far as the card on the second and are written once,
    // for the first time. The store converges; what must not happen is paying
    // again for what it already holds.
    assert(card_evicted_count < spilled / 8 && "but next to nothing may be written again");

    // And a file this run never wrote still restores byte for byte. A sample of
    // them, with a tick between, because restoring six hundred textures into a
    // driver this size would run it out of memory on its own.
    int checked = 0;
    for (int i = 0; i < count && checked < 16; i++) {
      if (!textures[ids[i]].evicted)
        continue;
      glBindTextureHook(GL_TEXTURE_2D, ids[i]);
      assert(fake_sampled(ids[i]) == fake_fingerprint_of(0xDE000000u + (unsigned)i) &&
             "a texture restored from a previous run's file must be the right one");
      checked++;
      texture_cache_tick();
    }
    assert(checked > 0 && "something had to have been evicted to check this");
    printf("             : %d restored from files written before the restart  OK\n", checked);
  }

  // ...and the key has to actually tell textures apart, because the failure it
  // guards against is silent. A texture whose contents differ must not be
  // handed a stored file just because it is the same size and shape.
  {
    const size_t over = ((size_t)TEXTURE_BUDGET_MB + TEXTURE_RAM_CACHE_MB + 96) * MB;
    const int count = (int)(over / TEX_BYTES);

    harness_start_empty(192 * MB);
    for (int i = 0; i < count; i++) {
      tex_upload(0xAB000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      texture_cache_tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    unsigned first = fake_store_files();
    assert(first > 0);

    // Same dimensions, same format, same sizes -- different pixels.
    fake_reset(192 * MB);
    texture_cache_init();
    GLuint ids[1536];
    for (int i = 0; i < count; i++) {
      ids[i] = tex_upload(0xCD000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      texture_cache_tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    printf("key discrim  : %u stored, %u reused, %u written for different pixels  OK\n", first,
           store_reused, card_evicted_count);
    assert(store_reused == 0 && "different pixels must never match a stored file");
    assert(card_evicted_count > 0 && "they have to be written for themselves");

    int checked = 0;
    for (int i = 0; i < count && checked < 16; i++) {
      if (!textures[ids[i]].evicted)
        continue;
      glBindTextureHook(GL_TEXTURE_2D, ids[i]);
      assert(fake_sampled(ids[i]) == fake_fingerprint_of(0xCD000000u + (unsigned)i) &&
             "and each must restore as itself, not as the texture it displaced");
      checked++;
      texture_cache_tick();
    }
    assert(checked > 0 && "something had to have been evicted to check this");
  }

  // Reclaiming is bursty, not a trickle. A pool sitting a little under its ideal
  // is left alone, because chasing it evicts textures the game asks straight
  // back for -- and every one of those is a read off the memory card on the
  // drawing thread. A session ran with the RAM pool at 24-25 MB free against a
  // 24 MB threshold and paid 2434 evictions and 1022 restores for it.
  {
    harness_start_empty(0);
    fake_set_pools(81 * MB, 100 * MB, 26 * MB);
    texture_cache_tick(); // takes the starting figures
    assert(pool_start[1] == 100 * MB);

    // Enough resident to have something to evict, and idle enough to qualify.
    GLuint cold[128];
    for (int i = 0; i < 128; i++) {
      cold[i] = tex_upload(0xBB000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
    }
    frames(TEXTURE_IDLE_FRAMES * 2);

    // Between the marks: under the quarter it would like, well above the point
    // at which it should start doing anything about it.
    fake_set_pools(81 * MB, 20 * MB, 26 * MB);
    int before = evicted_count;
    frames(600);
    printf("headroom     : %d MB free against a %d MB target, %d evicted over 600 frames  OK\n",
           20, (int)(pool_start[1] / 100 * TEXTURE_FREE_HEADROOM_PERCENT / MB),
           evicted_count - before);
    assert(evicted_count == before && "a pool between the marks must be left alone");

    // Below the low mark, and it has to act -- and act past the low mark rather
    // than just back over it, or it lands straight back here next frame.
    fake_set_pools(81 * MB, 10 * MB, 26 * MB);
    frames(4);
    assert(evicted_count > before && "a pool below the low mark must be reclaimed");
    printf("             : %d evicted once it dropped below the low mark  OK\n",
           evicted_count - before);

    // Hand the pools back healthy, or every block after this one inherits a
    // driver at its floor and tests a policy it did not mean to.
    fake_set_pools(81 * MB, 100 * MB, 26 * MB);
    frames(2);
  }

  // A texture still bound to a unit can be drawn without the game ever binding
  // it again, so there would be no moment at which to restore it.
  GLuint stuck = tex_upload(0x5AFE0001u, 512, 512, TEX_BYTES);
  drain();
  glBindTextureHook(GL_TEXTURE_2D, stuck);
  for (int i = 0; i < 64; i++) {
    tex_upload(0x99000000u + i, 512, 512, TEX_BYTES);
    drain();
  }
  frames(TEXTURE_IDLE_FRAMES * 4);
  assert(!textures[stuck].evicted && "a still-bound texture must not be evicted");
  printf("bound guard  : a bound texture is never evicted       OK\n");

  // A restore can fail -- the card may be gone, or vitaGL may be out of memory.
  // It frees the old data before it tries, so failing without putting a real
  // texture back leaves the game drawing from memory already handed away.
  // From a clean start, so that evicting it by hand is not competing with a
  // tick that has already spent this frame's ration of card writes.
  harness_start(512 * MB);
  GLuint fragile = tex_upload(0x5EED0001u, 512, 512, TEX_BYTES);
  drain();
  frames(1);
  glBindTextureHook(GL_TEXTURE_2D, 0); // so it is not the bound texture
  evict_texture(fragile);
  assert(textures[fragile].evicted);
  fake_reject_next_upload = 1; // the restore's upload will be refused
  glBindTextureHook(GL_TEXTURE_2D, fragile);
  assert(fake_slot_bytes[fragile] > 0 && "a failed restore must leave a real texture behind");
  assert(fake_sampled(fragile) == 0xFFFFFFFFu && "and it must be the placeholder");
  printf("failed restore: leaves a placeholder, not freed memory OK\n");

  // The failure this cache was written to prevent, reproduced exactly as it
  // happened on hardware: CDRAM runs dry while the RAM pool still has plenty,
  // so the total free memory reads healthy and a cache watching the total
  // evicts nothing. vitaGL does not fail the allocation -- it falls back to
  // RAM and then to the newlib heap -- so the game dies later, somewhere else,
  // out of heap. A session that crashed had CDRAM at 0 for 50,000 iterations
  // with 101 MB "free" and ev 0.
  {
    harness_start(0);
    fake_set_pools(81 * MB, 122 * MB, 26 * MB); // the figures off the console
    GLuint hot[64];
    for (int i = 0; i < 64; i++)
      hot[i] = tex_upload(0xC0000000u + i, 512, 512, TEX_BYTES);
    // A fixed number of uploads, chosen to come to less than the byte budget in
    // total, so the byte budget can never be what saves us -- but to more than
    // CDRAM holds, so that pool runs dry. Only the pool rule can catch this.
    const int count = (int)(((size_t)TEXTURE_BUDGET_MB * MB - 12 * MB) / TEX_BYTES);
    assert((size_t)count * TEX_BYTES > 81 * MB && "must be more than CDRAM holds");
    for (int i = 0; i < count; i++) {
      tex_upload(0xC1000000u + i, 512, 512, TEX_BYTES);
      texture_cache_tick();
    }
    wander(hot, 64, TEXTURE_IDLE_FRAMES * 3);
    int gone = 0;
    for (GLuint t = 1; t < MAX_TEXTURES; t++)
      if (textures[t].evicted)
        gone++;
    printf("pool drain   : cdram %zu MB free of 81 (was 0), %d evicted (was 0)   OK\n",
           fake_pool_free[0] / MB, gone);
    assert(tracked_bytes < (size_t)TEXTURE_BUDGET_MB * MB &&
           "the byte budget must not be what triggers this");
    assert(gone > 0 && "a pool running dry must be reclaimed against, however healthy the total looks");
    // Not the full 25% reserve: textures the game keeps drawing are reallocated
    // as they are restored, and with CDRAM preferred they land back in it. What
    // matters is that the pool is held well off the floor instead of sitting at
    // zero while the cache does nothing, which is what happened on hardware.
    assert(fake_pool_free[0] > fake_pool_start[0] / 8 &&
           "the drained pool must be held well clear of empty");
  }

  // Reclaiming that is refused must not be refused forever. On hardware the
  // RAM pool settled at 13% of its start -- above the 12% that opens the memory
  // card, below the 25% the cache aims for -- with the heap tier full because
  // the game had the heap. The cache wanted to evict and was told no 3.6
  // million times while the pools drained under it, and the session ended in
  // malloc. Cheap first is right; cheap forever is not.
  {
    // A driver small enough that the byte budget cannot cap the pressure before
    // the RAM pool gets low, since it is the pool that has to end up wedged.
    harness_start(160 * MB);
    fake_heap_used = (size_t)MEMORY_NEWLIB_MB * MB; // the game has the heap: no parking
    GLuint hot[32];
    for (int i = 0; i < 32; i++)
      hot[i] = tex_upload(0xB0000000u + i, 512, 512, TEX_BYTES);
    // Sit the RAM pool between the two thresholds -- under the 25% the cache
    // aims for, over the 12% that opens the card -- which is where nothing
    // would ever happen if the fixed threshold were the only way out.
    for (int i = 0; i < 4000 && fake_pool_free[1] > fake_pool_start[1] / 100 * 15; i++) {
      tex_upload(0xB1000000u + i, 512, 512, TEX_BYTES);
      texture_cache_tick();
    }
    assert(fake_pool_free[1] <= fake_pool_start[1] / 100 * 15 && "failed to wedge the pool");
    uint32_t spilled_before = card_evicted_count;
    wander(hot, 32, TEXTURE_BLOCKED_FRAMES * 4);
    printf("wedge        : ram %zu%% of start, %u written once cheap ran out  OK\n",
           fake_pool_free[1] * 100 / fake_pool_start[1], card_evicted_count - spilled_before);
    assert(fake_pool_free[1] > fake_pool_start[1] / 100 * TEXTURE_POOL_EMERGENCY_PERCENT &&
           "the fixed threshold must not be what saves this");
    assert(card_evicted_count > spilled_before &&
           "reclaiming blocked for long enough has to escalate, not wait forever");
  }

  // A file the console never finished writing must not be indexed as a good
  // copy. If it is, the next eviction of that texture is told the store already
  // holds it, frees the pixels without writing, and the restore then has
  // nothing to read: the texture is lost for the session, and the store goes on
  // lying about it in every run after this one.
  {
    const size_t over = ((size_t)TEXTURE_BUDGET_MB + TEXTURE_RAM_CACHE_MB + 96) * MB;
    const int count = (int)(over / TEX_BYTES);

    harness_start_empty(192 * MB);
    for (int i = 0; i < count; i++) {
      tex_upload(0x7C000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      texture_cache_tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    unsigned written = fake_store_files();
    assert(written > 1 && "this block needs a store to damage");

    assert(fake_tear_one_store_file((long)sizeof(BackupRecord)) &&
           "the harness had a file to tear");
    fake_reset(192 * MB);
    texture_cache_init();
    assert(store_files == written - 1 &&
           "a file with a header and no data was counted as a usable copy");
    printf("torn file    : %u of %u indexed, the truncated one skipped  OK\n",
           store_files, written);
  }

  // The loader's own size estimate must never be what a copy is measured by.
  //
  // resident_size guesses from the GL enums and rounds rows by a rule of thumb.
  // It says so where it is defined -- close enough for the budget, not meant to
  // match vitaGL to the byte -- and for GL_RGBA4 with GL_UNSIGNED_BYTE it comes
  // out at four bytes a pixel where the driver allocated two. Evicting one of
  // those read twice the buffer out of vitaGL's pool, and restoring it wrote
  // twice the buffer back in, over whatever was next.
  {
    harness_start_empty(192 * MB);
    const int count = 260;
    GLuint ids[260];
    for (int i = 0; i < count; i++) {
      ids[i] = tex_upload_narrow(0x5E000000u + (unsigned)i, 512, 512);
      drain();
      texture_cache_tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);

    unsigned evicted = 0;
    for (int i = 0; i < count; i++)
      if (textures[ids[i]].evicted)
        evicted++;
    assert(evicted > 0 && "nothing was evicted, so nothing was copied");
    assert(!fake_first_overrun() && "eviction read or wrote past a texture buffer");

    int checked = 0;
    for (int i = 0; i < count && checked < 16; i++) {
      if (!textures[ids[i]].evicted)
        continue;
      glBindTextureHook(GL_TEXTURE_2D, ids[i]);
      assert(fake_sampled(ids[i]) == fake_fingerprint_of(0x5E000000u + (unsigned)i) &&
             "a narrow-format texture must come back as itself");
      checked++;
      texture_cache_tick();
    }
    assert(checked > 0);
    assert(!fake_first_overrun() && "restoring wrote past a texture buffer");
    printf("narrow fmt   : %u evicted, %d restored, no buffer overrun  OK\n",
           evicted, checked);
  }

  // A file that belongs to a different texture must never be drawn as this one.
  //
  // The store is keyed by a hash of a sample of the pixels, so two textures can
  // in principle land on the same name -- and not only by chance: two that
  // differ solely in the gaps between sampled slices collide every time. The
  // record used to guard against that by carrying the key back and comparing
  // it, which is circular, since the file was found by that key. The verify
  // hash is the real guard: written only inside the record, over bytes the key
  // never reads. Scrambling it here is exactly what a colliding texture's file
  // looks like -- right name, right length, its own valid checksum, wrong
  // texture.
  {
    harness_start_empty(192 * MB);
    const size_t over = ((size_t)TEXTURE_BUDGET_MB + TEXTURE_RAM_CACHE_MB + 96) * MB;
    const int count = (int)(over / TEX_BYTES);
    GLuint ids[1536];
    assert(count <= (int)(sizeof(ids) / sizeof(ids[0])));

    for (int i = 0; i < count; i++) {
      ids[i] = tex_upload(0x33000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      texture_cache_tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    assert(fake_store_files() > 0 && "this block needs a file on the card");

    // Word 3 is verify_lo: magic, key_lo, key_hi, then verify_lo.
    assert(fake_scramble_store_word(3) && "the harness had a file to scramble");

    // Start again so the store is read fresh, then find the texture that file
    // belongs to and ask for it back.
    fake_reset(192 * MB);
    texture_cache_init();
    for (int i = 0; i < count; i++) {
      ids[i] = tex_upload(0x33000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      texture_cache_tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);

    uint32_t refused_before = restore_failed_count;
    int wrong = 0, looked = 0;
    for (int i = 0; i < count; i++) {
      if (!textures[ids[i]].evicted)
        continue;
      glBindTextureHook(GL_TEXTURE_2D, ids[i]);
      uint32_t got = fake_sampled(ids[i]);
      looked++;
      // Either the right texture, or a refusal. Never another texture's pixels.
      for (int j = 0; j < count; j++)
        if (j != i && got == fake_fingerprint_of(0x33000000u + (unsigned)j))
          wrong++;
      texture_cache_tick();
    }
    assert(looked > 0 && "nothing was evicted, so nothing was checked");
    assert(wrong == 0 && "a texture was restored as a different texture");

    // The scrambled one is only refused if the verify hash is actually
    // compared. Nothing else in the record would notice: the name, the length
    // and the checksum over the data are all still correct, which is precisely
    // the position a colliding texture's file leaves us in. Every other file
    // here is sound, so a refusal in this pass is that one and nothing else.
    assert(restore_failed_count > refused_before &&
           "the mismatched file was accepted rather than refused");
    // And refusing it has to take it off the card. Left there, every future
    // eviction of that texture is told the store already holds it, frees the
    // pixels without writing, and finds nothing to read -- for the rest of this
    // session and every session after it.
    assert(access(fake_last_scrambled_path(), F_OK) != 0 &&
           "the file that would not read back is still on the card");
    // Off the card and out of the index, which are two separate things. An
    // index still claiming a file that is gone is the same trap by another
    // route: store_has says yes, the eviction frees the pixels without writing,
    // and the restore opens nothing.
    assert(store_files == fake_store_files() &&
           "the index claims files the card does not have");
    printf("wrong file   : %d restores checked, %d drawn as another texture, "
           "%u refused  OK\n", looked, wrong, restore_failed_count - refused_before);
  }

  // What the key and the verify hash have to be, before any of the machinery
  // above can mean anything.
  {
    harness_start_empty(192 * MB);
    const GLsizei size = 64 * 1024;

    // Two different textures must not share either hash.
    GLuint a = tex_upload(0x11111111u, 128, 128, size);
    GLuint b = tex_upload(0x22222222u, 128, 128, size);
    assert(textures[a].key != textures[b].key && "different pixels, different key");
    assert(textures[a].verify != textures[b].verify &&
           "the verify hash does not vary, so it can refuse nothing");

    // And the verify hash has to look where the key does not. The key reads the
    // first and last 8 KB in full and a 128-byte slice every 8 KB in between;
    // two textures differing only in the gaps between those slices collide
    // every time, not once in a billion. Build exactly that pair: identical
    // everywhere the key looks, different at a byte only the verify hash's
    // midpoint slices reach.
    memset(source_bytes, 0x5A, size);
    GLuint c;
    glGenTexturesHook(1, &c);
    glBindTextureHook(GL_TEXTURE_2D, c);
    glCompressedTexImage2DHook(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG,
                               128, 128, 0, size, source_bytes);

    // 12288 is 8 KB + half a stride: past the leading block, past the key's
    // first slice, and precisely where the verify hash starts sampling.
    source_bytes[12288] = 0x5B;
    GLuint d;
    glGenTexturesHook(1, &d);
    glBindTextureHook(GL_TEXTURE_2D, d);
    glCompressedTexImage2DHook(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG,
                               128, 128, 0, size, source_bytes);

    assert(textures[c].key == textures[d].key &&
           "the pair was meant to collide on the key; the sampling must have moved");
    assert(textures[c].verify != textures[d].verify &&
           "the verify hash reads the same bytes as the key, so it adds nothing");
    printf("hashes       : distinct textures differ; a key collision is caught "
           "by verify  OK\n");
  }

  printf("PASS\n");
  return 0;
}
