#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <xinput.h>
#include <stdint.h>
#include <string>

// ----------------------------------------------------------------------------
// VR-DLSS5 Shared Memory Control Block (v3)
// Synchronized with OptiScaler Pre-SR Engine's Config.h
// ----------------------------------------------------------------------------
#define VRDLSS5_CTL_MAGIC 0x354C4456u // 'VDL5'
#define VRDLSS5_CTL_VERSION 3u
#define VRDLSS5_CTL_MAPPING_NAME "Local\\VRDLSS5_Control_1"

#pragma pack(push, 4)
struct VrDlss5ControlBlock
{
    unsigned int magic;
    unsigned int version;
    volatile LONG seq;
    volatile LONG ackSeq;
    volatile LONG optiReady;
    volatile LONG enabled;
    float workingScale;
    volatile LONG runBeforeSR;
    volatile LONG residualAcrossRR;
    volatile LONG preset;
    float intensity;
    volatile LONG style;
    volatile LONG featureRunning;
    float gpuFrameMs;
    float localStructure;      // DlssNr.LocalStructure (0.00 - 2.00)
    float localTone;           // DlssNr.LocalTone (0.00 - 2.00)
    volatile LONG autoMask;    // DlssNr.AutoMask (0 / 1)
    float skinStructure;       // DlssNr.SkinStructure (-1.00 - 2.00)
    float transferStrength;    // DlssNr.TransferStrength (0.00 - 2.00)
    float colourStrength;      // DlssNr.ColourStrength (0.00 - 1.00)
    volatile LONG passes;      // DlssNr.Passes (1 - 3)
    volatile LONG reserved[8];
};
#pragma pack(pop)
