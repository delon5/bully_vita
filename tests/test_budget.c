/* test_budget.c -- texture memory has to stay bounded, however long you play.
 *
 * This is the original crash: the game never frees a texture, so on a Vita
 * vitaGL's pools run dry and it dies. The driver here aborts if that happens,
 * so the test fails the same way the game would.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "harness.h"

#define STREAM_FRAMES 40000
#define LIVE_TEXTURES 4000
#define TEX_BYTES (64 * 1024)

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  // A deliberately tight driver: less memory than the game's texture set.
  harness_start(160 * MB);

  const size_t budget = (size_t)TEXTURE_BUDGET_MB * MB;

  printf("stream %d textures through a 160MB driver\n", STREAM_FRAMES);
  GLuint live[LIVE_TEXTURES];
  int count = 0, at = 0;
  for (int frame = 0; frame < STREAM_FRAMES; frame++) {
    // The game streams: it uploads new textures and eventually deletes them.
    if (count == LIVE_TEXTURES)
      glDeleteTexturesHook(1, &live[at]);
    else
      count++;
    live[at] = tex_upload(0x10000000u + frame, 256, 256, TEX_BYTES);
    at = (at + 1) % LIVE_TEXTURES;
    drain();
    texture_cache_tick();

    if (frame % 8000 == 0)
      printf("  frame %5d: %zu MB tracked, %zu MB free\n", frame, tracked_bytes / MB,
             fake_free_memory / MB);
  }

  printf("  worst free was %zu MB of 160 MB\n", fake_low_water / MB);
  assert(fake_low_water > 8 * MB && "never came close to running the driver dry");
  assert(tracked_bytes <= budget + budget / 2 && "texture memory must stay bounded");

  // The accounting must agree with what we told the driver, or the budget is
  // being computed against a fiction.
  size_t sum = 0;
  for (GLuint id = 1; id < MAX_TEXTURES; id++)
    if (textures[id].tracked)
      sum += textures[id].size;
  assert(sum == tracked_bytes && "per-texture sizes must add up to the total");

  // A texture the game keeps drawing with must never be taken away.
  GLuint hot = tex_upload(0x40000001u, 256, 256, TEX_BYTES);
  for (int i = 0; i < 2000; i++) {
    glBindTextureHook(GL_TEXTURE_2D, hot);
    texture_cache_tick();
  }
  assert(!textures[hot].evicted && "a texture in constant use must not be evicted");

  printf("PASS\n");
  return 0;
}
