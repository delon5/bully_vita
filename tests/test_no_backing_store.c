/* test_no_backing_store.c -- what happens when nothing can be copied.
 *
 * If the card is full, or the writer cannot keep up, textures have no copy and
 * dropping one loses it for good. The cache must then prefer holding on to
 * them, and only start losing them when memory genuinely has to be bounded.
 * Getting this wrong in either direction is bad: too eager and the game turns
 * white, too reluctant and it runs out of memory and dies.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "harness.h"

#define TEX_BYTES (512 * 1024)

static int count_evicted(void) {
  int n = 0;
  for (GLuint id = 1; id < MAX_TEXTURES; id++)
    if (textures[id].evicted)
      n++;
  return n;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  harness_start(768 * MB);

  const size_t budget = (size_t)TEXTURE_BUDGET_MB * MB;

  // Nothing is copied at upload time any more -- the bytes are read back out of
  // the GPU at the moment a texture is actually dropped. What an upload has to
  // leave behind is the description of the upload itself: the level sizes and
  // formats needed to rebuild the texture later. Once that is recorded the
  // texture is evictable, without a single byte having touched the card.
  GLuint pending = tex_upload(0x1234u, 512, 512, TEX_BYTES);
  assert(is_restorable(&textures[pending]) && "an upload must leave enough behind to rebuild it");
  assert(fake_store_files() == 0 && "an upload must not write anything");
  printf("write timing : evictable after upload, nothing written yet  OK\n");

  // A card with no room to spare must not get a backing store at all -- filling
  // it would take the game's own saves and .obb indexes down with it.
  {
    backup_ready = 0;
    fake_card_free = 200ull * 1024 * 1024;   // less than the reserve
    texture_cache_init();
    assert(!backup_ready && "a nearly full card must not get a backing store");
    printf("full card    : no store taken when the card is nearly full  OK\n");
  }
  printf("backing store disabled\n");

  // Go over budget, but not far over. Nothing here can be reloaded, so the
  // cache should hold on to all of it rather than lose textures.
  int uploaded = 0;
  while (tracked_bytes < budget + budget / 4) {
    tex_upload(0x2000u + uploaded, 512, 512, TEX_BYTES);
    texture_cache_tick();
    uploaded++;
  }
  frames(TEXTURE_IDLE_FRAMES + 260);
  printf("  %zu MB in use, %d evicted\n", tracked_bytes / MB, count_evicted());
  assert(count_evicted() == 0 && "must not lose textures it cannot get back while merely over budget");

  // Keep pushing well past the budget. This used to be the point where a last
  // resort engaged and started evicting textures with no copy on the card, on
  // the reasoning that bounded memory beats running out of it. It does not: on
  // hardware it took the ground and the buildings out of the world, and nothing
  // was ever going to upload them again, because the game has no idea the cache
  // exists. Whatever the pressure, a texture that cannot be restored is one this
  // cache must not drop.
  for (int i = 0; i < 300; i++) {
    tex_upload(0x3000u + uploaded, 512, 512, TEX_BYTES);
    texture_cache_tick();
    uploaded++;
  }
  frames(TEXTURE_IDLE_FRAMES + 460);
  printf("  after pushing well past budget: %zu MB in use, %d evicted\n",
         tracked_bytes / MB, count_evicted());
  assert(count_evicted() == 0 && "must never drop a texture it cannot restore");

  printf("PASS\n");
  return 0;
}
