/*
 * d3d8_null.c -- the HLE D3D8 layer on hosts that have neither D3D11 nor a
 * GL loader (the Switch).
 *
 * A title that links its own XDK D3D draws through the NV2A pushbuffer
 * executor and never reaches this layer; the video player and the legacy
 * PGRAPH->D3D path only ask whether a device exists, and hear that none
 * does.
 */
#include "d3d8_xbox.h"

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    return NULL;
}

IDirect3DDevice8 *xbox_GetD3DDevice(void) { return NULL; }
IDirect3DDevice8 *d3d8_GetDevice(void) { return NULL; }
