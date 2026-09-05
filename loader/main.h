#ifndef __MAIN_H__
#define __MAIN_H__

#include <psp2/touch.h>
#include "so_util.h"

extern so_module bully_mod;

int debugPrintf(char *text, ...);
int traceLog(char *text, ...);

// Presented frames, counted in jni_patch.c's swapBuffers.
extern int frames_swapped;

int ret0();

int sceKernelChangeThreadCpuAffinityMask(SceUID thid, int cpuAffinityMask);

SceUID _vshKernelSearchModuleByName(const char *, int *);

extern SceTouchPanelInfo panelInfoFront;

#endif
