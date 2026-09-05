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

  // A texture is written on the thread that uploaded it, so by the time the
  // upload call returns its file is on the card and it is reloadable. There is
  // no window any more where it is tracked but not yet written.
  GLuint pending = tex_upload(0x1234u, 512, 512, TEX_BYTES);
  assert(is_restorable(&textures[pending]) && "must be reloadable as soon as it is uploaded");
  printf("write timing : reloadable as soon as the upload returns  OK\n");

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
  frames(TEXTURE_LOSSY_AFTER_FRAMES + TEXTURE_IDLE_FRAMES + 200);
  printf("  %zu MB in use, %d evicted\n", tracked_bytes / MB, count_evicted());
  assert(count_evicted() == 0 && "must not lose textures it cannot get back while merely over budget");

  // Keep pushing well past the budget. Memory has to be bounded whatever the
  // card is doing, so now it has to start letting go even though that loses
  // them. Upload a fixed number rather than aiming at a figure: once the ceiling
  // engages it evicts as fast as we upload, so a target would never be reached.
  for (int i = 0; i < 300; i++) {
    tex_upload(0x3000u + uploaded, 512, 512, TEX_BYTES);
    texture_cache_tick();
    uploaded++;
  }
  frames(TEXTURE_LOSSY_AFTER_FRAMES + TEXTURE_IDLE_FRAMES + 400);
  printf("  after pushing well past budget: %zu MB in use, %d evicted\n",
         tracked_bytes / MB, count_evicted());
  assert(count_evicted() > 0 && "memory must stay bounded even with nothing to fall back on");
  assert(tracked_bytes <= budget + budget / 2 && "must come back under the ceiling");

  printf("PASS\n");
  return 0;
}
