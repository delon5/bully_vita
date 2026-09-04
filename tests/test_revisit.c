/* test_revisit.c -- leave an area and come back to it.
 *
 * The cache frees textures the game has stopped drawing with, but the game has
 * no idea and will never upload them again, so every one it drops has to come
 * back when the player returns. Before the backing store existed this test
 * failed with 128 of 200 textures rendering white.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "harness.h"

#define AREA 200
#define TEX_BYTES (256 * 1024)

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  harness_start(512 * MB);

  GLuint a[AREA], b[AREA], c[AREA];
  GLuint *areas[3] = { a, b, c };
  const uint32_t tags[3] = { 0xA0000000u, 0xB0000000u, 0xC0000000u };

  printf("load area A\n");
  for (int i = 0; i < AREA; i++) {
    a[i] = tex_upload(tags[0] + i, 512, 512, TEX_BYTES);
    drain();
    texture_cache_tick();
  }
  wander(a, AREA, 60);
  for (int i = 0; i < AREA; i++)
    assert(fake_sampled(a[i]) == fake_fingerprint_of(tags[0] + i) && "wrong right after loading");

  printf("wander through B and C until A goes cold\n");
  for (int area = 1; area < 3; area++) {
    for (int i = 0; i < AREA; i++) {
      areas[area][i] = tex_upload(tags[area] + i, 512, 512, TEX_BYTES);
      drain();
      texture_cache_tick();
    }
    wander(areas[area], AREA, 400);
  }

  int evicted = 0;
  for (int i = 0; i < AREA; i++)
    if (textures[a[i]].evicted)
      evicted++;
  printf("  %d/%d of area A evicted, %zu MB in use\n", evicted, AREA, tracked_bytes / MB);
  assert(evicted > AREA / 2 && "most of a cold area should have been evicted");
  assert(tracked_bytes <= (size_t)TEXTURE_BUDGET_MB * MB && "should be back under budget");

  printf("walk back into area A\n");
  wander(a, AREA, 5);
  int wrong = 0;
  for (int i = 0; i < AREA; i++)
    if (fake_sampled(a[i]) != fake_fingerprint_of(tags[0] + i))
      wrong++;
  printf("  %d/%d came back wrong\n", wrong, AREA);
  assert(wrong == 0 && "revisiting an area must restore its textures");

  printf("loop between the three areas\n");
  for (int round = 0; round < 12; round++)
    for (int area = 0; area < 3; area++) {
      wander(areas[area], AREA, 220); // walking in is what restores them
      for (int i = 0; i < AREA; i++)
        assert(fake_sampled(areas[area][i]) == fake_fingerprint_of(tags[area] + i) &&
               "an area came back wrong on revisit");
    }
  printf("  still correct after 12 round trips, %zu MB in use\n", tracked_bytes / MB);

  printf("PASS\n");
  return 0;
}
