#ifndef __MAIN_H__
#define __MAIN_H__

#include <psp2/touch.h>
#include "so_util.h"

extern so_module bully_mod;

int debugPrintf(char *text, ...);

// Notable, rare events from the memory work. Appends to ux0:data/bully_log.txt.
int traceLog(char *text, ...);

// Frames the game has run, counted in ProcessEvents.
extern int frames_swapped;

int ret0();

int sceKernelChangeThreadCpuAffinityMask(SceUID thid, int cpuAffinityMask);

SceUID _vshKernelSearchModuleByName(const char *, int *);

extern SceTouchPanelInfo panelInfoFront;

#endif
