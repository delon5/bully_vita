/* texture_cache.h -- reloadable LRU texture cache
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __TEXTURE_CACHE_H__
#define __TEXTURE_CACHE_H__

#include <vitaGL.h>

// Opens the backing store and starts the writer thread. Call once, after fios
// is up and before the game gets a chance to upload anything.
void texture_cache_init(void);

// GL entry points the game resolves through the dynamic library table. They
// forward to vitaGL, keep track of how much texture memory is in use, and put
// evicted textures back when the game asks for them again.
void glActiveTextureHook(GLenum texture);
void glBindTextureHook(GLenum target, GLuint texture);
void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
void glDeleteTexturesHook(GLsizei n, const GLuint *ids);
void glFramebufferTexture2DHook(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void glGenTexturesHook(GLsizei n, GLuint *res);
void glTexImage2DHook(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data);
void glTexParameterfHook(GLenum target, GLenum pname, GLfloat param);
void glTexParameteriHook(GLenum target, GLenum pname, GLint param);
void glTexSubImage2DHook(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);

// Removes the backing store. Call on the way out.
void texture_cache_shutdown(void);

// Drops the least recently used textures if we are over budget. Must be called
// once per frame from the thread that renders, right before swapping buffers.
void texture_cache_tick(void);
// What the cache is holding and what it has had to do to hold it. parked_mb and
// spilled apart tell a cheap eviction from an expensive one, and starved says
// the cache wanted to reclaim and could not -- which is what precedes running
// out of memory.
typedef struct {
  int tracked_mb;
  int parked_mb; // evicted textures sitting in the newlib heap
  int evicted, restored, failed;
  int spilled; // evictions that had to reach the memory card
  int starved; // frames it failed to get back under its limits
  int deferred; // evictions it wanted to make and could not, for want of a copy
  int blocked; // consecutive frames it has wanted to free memory and freed none
} TextureCacheStats;

void texture_cache_stats(TextureCacheStats *out);

#endif
