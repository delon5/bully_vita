/* texture_cache.h -- LRU eviction for the game's textures
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __TEXTURE_CACHE_H__
#define __TEXTURE_CACHE_H__

#include <vitaGL.h>

// GL entry points the game resolves through the dynamic library table. They
// forward to vitaGL and keep track of how much texture memory is in use.
void glActiveTextureHook(GLenum texture);
void glBindTextureHook(GLenum target, GLuint texture);
void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
void glDeleteTexturesHook(GLsizei n, const GLuint *ids);
void glFramebufferTexture2DHook(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void glGenTexturesHook(GLsizei n, GLuint *res);
void glTexImage2DHook(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data);
void glTexSubImage2DHook(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);

// Drops the least recently used textures if we are over budget. Must be called
// once per frame from the thread that renders, right before swapping buffers.
void texture_cache_tick(void);

#endif
