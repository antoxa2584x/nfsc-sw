/*
 * nv2a_gl.h -- OpenGL renderer for the NV2A pushbuffer executor.
 *
 * Call nv2a_gl_install() before the title starts rendering, with the
 * executor enabled (RECOMP_PB_EXEC). The window and GL context are created
 * on the executor's thread on first use.
 */
#ifndef NV2A_GL_H
#define NV2A_GL_H

void nv2a_gl_install(void);

#endif
