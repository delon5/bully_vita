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
  assert(vitagl_free_memory() > 0 && "summing the pools must see the real figure");
  printf("free memory  : summed per pool, not via VGL_MEM_ALL   OK\n");

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
    writes_before = fake_store_writes();
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
    printf("heap tier    : %d evicted, %u card writes, %zu MB parked  OK\n", evicted,
           fake_store_writes() - writes_before, ram_cache_bytes / MB);
    assert(evicted > count / 2 && "the cold set should have been evicted");
    assert(fake_store_writes() == writes_before &&
           "evicting into a heap that has room must not touch the card");

    // And they must come back from the heap just as they would off the card.
    wander(cold, count, 4);
    for (int i = 0; i < count; i++)
      assert(fake_sampled(cold[i]) == fake_fingerprint_of(0xEE000000u + i) &&
             "a texture parked in the heap must restore correctly");
    printf("heap restore : parked textures come back byte for byte OK\n");
  }

  // Nothing in the store survives a run: a file is named for a texture name and
  // a generation of it, and the next run hands both out to different textures.
  // Left alone the folder would grow every session.
  {
    harness_start(1024 * MB);
    // Enough to fill the budget and the heap tier and still have textures left
    // over, so eviction has no choice but to reach the card. Counted rather
    // than measured against tracked_bytes, which stops growing once the cache
    // starts keeping up and would leave the loop running forever.
    const size_t over = ((size_t)TEXTURE_BUDGET_MB + TEXTURE_RAM_CACHE_MB + 64) * MB;
    for (size_t i = 0; i < over / TEX_BYTES; i++) {
      tex_upload(0xDE000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      texture_cache_tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    unsigned spilled = fake_store_files();
    assert(spilled > 0 && "past the heap tier, eviction has to reach the card");
    texture_cache_init();
    printf("store purge  : %u files spilled, %u after a restart     OK\n", spilled,
           fake_store_files());
    assert(fake_store_files() == 0 && "starting up must clear the store");
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

  printf("PASS\n");
  return 0;
}
