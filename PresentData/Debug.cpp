/*
Copyright 2019 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdint.h>
#include <windows.h>
#include <tdh.h> // must be after windows.h

#include "PresentMonTraceConsumer.hpp"

#if DEBUG_VERBOSE

namespace {

bool gDebugDone = false;
bool gDebugTrace = false;
uint64_t* gFirstTimestamp= 0;
uint64_t gTimestampFrequency = 0;

    enum {
        DXGIPresent_Start = 42,
        DXGIPresent_Stop,
        DXGIPresentMPO_Start = 55,
        DXGIPresentMPO_Stop = 56,
    };

    enum {
        DxgKrnl_Flip = 168,
        DxgKrnl_FlipMPO = 252,
        DxgKrnl_QueueSubmit = 178,
        DxgKrnl_QueueComplete = 180,
        DxgKrnl_MMIOFlip = 116,
        DxgKrnl_MMIOFlipMPO = 259,
        DxgKrnl_HSyncDPC = 382,
        DxgKrnl_VSyncDPC = 17,
        DxgKrnl_Present = 184,
        DxgKrnl_PresentHistoryDetailed = 215,
        DxgKrnl_SubmitPresentHistory = 171,
        DxgKrnl_PresentHistory = 172,
        DxgKrnl_Blit = 166,
    };

    enum {
        Win32K_TokenCompositionSurfaceObject = 201,
        Win32K_TokenStateChanged = 301,
    };

    enum {
        DWM_GetPresentHistory = 64,
        DWM_Schedule_Present_Start = 15,
        DWM_FlipChain_Pending = 69,
        DWM_FlipChain_Complete = 70,
        DWM_FlipChain_Dirty = 101,
        DWM_Schedule_SurfaceUpdate = 196,
    };

    enum {
        D3D9PresentStart = 1,
        D3D9PresentStop,
    };

char const* const gPresentModelString[] = {
    "uninitialized",
    "redirected_gdi",
    "redirected_flip",
    "redirected_blt",
    "redirected_vistablt",
    "screencapturefence",
    "redirected_gdi_sysmem",
    "redirected_composition",
};

uint64_t ConvertTimestampDeltaToNs(uint64_t timestampDelta)
{
    return 1000000000ull * timestampDelta / gTimestampFrequency;
}

uint64_t ConvertTimestampToNs(uint64_t timestamp)
{
    return ConvertTimestampDeltaToNs(timestamp - *gFirstTimestamp);
}

char* AddCommas(uint64_t t)
{
    static char buf[128];
    auto r = sprintf_s(buf, "%llu", t);

    auto commaCount = r == 0 ? 0 : ((r - 1) / 3);
    for (int i = 0; i < commaCount; ++i) {
        auto p = r + commaCount - 4 * i;
        auto q = r - 3 * i;
        buf[p - 1] = buf[q - 1];
        buf[p - 2] = buf[q - 2];
        buf[p - 3] = buf[q - 3];
        buf[p - 4] = ',';
    }

    r += commaCount;
    buf[r] = '\0';
    return buf;
}

void PrintPresentMode(PresentMode mode)
{
    switch (mode) {
    case PresentMode::Unknown:                              printf("Unknown"); break;
    case PresentMode::Hardware_Legacy_Flip:                 printf("Hardware_Legacy_Flip"); break;
    case PresentMode::Hardware_Legacy_Copy_To_Front_Buffer: printf("Hardware_Legacy_Copy_To_Front_Buffer"); break;
    case PresentMode::Hardware_Direct_Flip:                 printf("Hardware_Direct_Flip"); break;
    case PresentMode::Hardware_Independent_Flip:            printf("Hardware_Independent_Flip"); break;
    case PresentMode::Composed_Flip:                        printf("Composed_Flip"); break;
    case PresentMode::Composed_Copy_GPU_GDI:                printf("Composed_Copy_GPU_GDI"); break;
    case PresentMode::Composed_Copy_CPU_GDI:                printf("Composed_Copy_CPU_GDI"); break;
    case PresentMode::Composed_Composition_Atlas:           printf("Composed_Composition_Atlas"); break;
    case PresentMode::Hardware_Composed_Independent_Flip:   printf("Hardware_Composed_Independent_Flip"); break;
    default:                                                printf("ERROR"); break;
    }
}

void PrintEventHeader(EVENT_HEADER const& hdr)
{
    printf("%16s %5u %5u ", AddCommas(ConvertTimestampToNs(hdr.TimeStamp.QuadPart)), hdr.ProcessId, hdr.ThreadId);
}

void PrintUpdateHeader(uint64_t id, int indent=0)
{
    printf("%*sp%llu", 17 + 6 + 6 + indent*4, "", id);
}

}

void DebugInitialize(uint64_t* firstTimestamp, uint64_t timestampFrequency)
{
    gDebugDone = false;
    gDebugTrace = DEBUG_START_TIME_NS == 0;
    gFirstTimestamp = firstTimestamp;
    gTimestampFrequency = timestampFrequency;

    printf("       Time (ns)   PID   TID EVENT\n");
}

bool DebugDone()
{
    return gDebugDone;
}

void DebugEvent(EVENT_RECORD* eventRecord)
{
    auto const& hdr = eventRecord->EventHeader;
    auto id = hdr.EventDescriptor.Id;

#if DEBUG_START_TIME_NS
    if (DEBUG_START_TIME_NS <= t) {
        gDebugTrace = true;
    }
#endif

#if DEBUG_STOP_TIME_NS
    if (DEBUG_STOP_TIME_NS <= t) {
        gDebugTrace = false;
        gDebugDone = true;
    }
#endif

    if (!gDebugTrace) {
        return;
    }

    if (hdr.ProviderId == D3D9_PROVIDER_GUID) {
        switch (id) {
        case D3D9PresentStart: PrintEventHeader(hdr); printf("D3D9PresentStart\n"); break;
        case D3D9PresentStop:  PrintEventHeader(hdr); printf("D3D9PresentStop\n"); break;
        }
        return;
    }

    if (hdr.ProviderId == DXGI_PROVIDER_GUID) {
        switch (id) {
        case DXGIPresent_Start:     PrintEventHeader(hdr); printf("DXGIPresent_Start\n"); break;
        case DXGIPresent_Stop:      PrintEventHeader(hdr); printf("DXGIPresent_Stop\n"); break;
        case DXGIPresentMPO_Start:  PrintEventHeader(hdr); printf("DXGIPresentMPO_Start\n"); break;
        case DXGIPresentMPO_Stop:   PrintEventHeader(hdr); printf("DXGIPresentMPO_Stop\n"); break;
        }
        return;
    }

    if (hdr.ProviderId == Win7::DXGKBLT_GUID)            { PrintEventHeader(hdr); printf("Win7::BLT\n"); return; }
    if (hdr.ProviderId == Win7::DXGKFLIP_GUID)           { PrintEventHeader(hdr); printf("Win7::FLIP\n"); return; }
    if (hdr.ProviderId == Win7::DXGKPRESENTHISTORY_GUID) { PrintEventHeader(hdr); printf("Win7::PRESENTHISTORY\n"); return; }
    if (hdr.ProviderId == Win7::DXGKQUEUEPACKET_GUID)    { PrintEventHeader(hdr); printf("Win7::QUEUEPACKET\n"); return; }
    if (hdr.ProviderId == Win7::DXGKVSYNCDPC_GUID)       { PrintEventHeader(hdr); printf("Win7::VSYNCDPC\n"); return; }
    if (hdr.ProviderId == Win7::DXGKMMIOFLIP_GUID)       { PrintEventHeader(hdr); printf("Win7::MMIOFLIP\n"); return; }

    if (hdr.ProviderId == DXGKRNL_PROVIDER_GUID) {
        switch (id) {
        case DxgKrnl_Flip:          PrintEventHeader(hdr); printf("DxgKrnl_Flip\n"); break;
        case DxgKrnl_FlipMPO:       PrintEventHeader(hdr); printf("DxgKrnl_FlipMPO\n"); break;
        case DxgKrnl_QueueSubmit:   PrintEventHeader(hdr); printf("DxgKrnl_QueueSubmit\n"); break;
        case DxgKrnl_QueueComplete: PrintEventHeader(hdr); printf("DxgKrnl_QueueComplete\n"); break;
        case DxgKrnl_MMIOFlip:      PrintEventHeader(hdr); printf("DxgKrnl_MMIOFlip\n"); break;
        case DxgKrnl_MMIOFlipMPO:   PrintEventHeader(hdr); printf("DxgKrnl_MMIOFlipMPO\n"); break;
        case DxgKrnl_HSyncDPC:      PrintEventHeader(hdr); printf("DxgKrnl_HSyncDPC\n"); break;
        case DxgKrnl_VSyncDPC:      PrintEventHeader(hdr); printf("DxgKrnl_VSyncDPC\n"); break;
        case DxgKrnl_Present:       PrintEventHeader(hdr); printf("DxgKrnl_Present\n"); break;
        case DxgKrnl_Blit:          PrintEventHeader(hdr); printf("DxgKrnl_Blit\n"); break;
        /* PresentHistory event:
        uint64_t hAdapter;
        uint64_t Token;
        uint32_t Model;
        ...
        */
        case DxgKrnl_PresentHistory:
            PrintEventHeader(hdr);
            printf("DxgKrnl_PresentHistory token=%llx\n",
                ((uint64_t*) eventRecord->UserData)[1]);
            break;
        case DxgKrnl_SubmitPresentHistory:
            PrintEventHeader(hdr);
            printf("DxgKrnl_SubmitPresentHistory token=%llx, model=%s\n",
                ((uint64_t*) eventRecord->UserData)[1],
                gPresentModelString[((uint32_t*) eventRecord->UserData)[4]]);
            break;
        case DxgKrnl_PresentHistoryDetailed:
            PrintEventHeader(hdr);
            printf("DxgKrnl_PresentHistoryDetailed token=%llx, model=%s\n",
                ((uint64_t*) eventRecord->UserData)[1],
                gPresentModelString[((uint32_t*) eventRecord->UserData)[4]]);
            break;
        }
        return;
    }

    if (hdr.ProviderId == DWM_PROVIDER_GUID ||
        hdr.ProviderId == Win7::DWM_PROVIDER_GUID) {
        switch (id) {
        case DWM_GetPresentHistory:         PrintEventHeader(hdr); printf("DWM_GetPresentHistory\n"); break;
        case DWM_Schedule_Present_Start:    PrintEventHeader(hdr); printf("DWM_Schedule_Present_Start\n"); break;
        case DWM_FlipChain_Pending:         PrintEventHeader(hdr); printf("DWM_FlipChain_Pending\n"); break;
        case DWM_FlipChain_Complete:        PrintEventHeader(hdr); printf("DWM_FlipChain_Complete\n"); break;
        case DWM_FlipChain_Dirty:           PrintEventHeader(hdr); printf("DWM_FlipChain_Dirty\n"); break;
        case DWM_Schedule_SurfaceUpdate:    PrintEventHeader(hdr); printf("DWM_Schedule_SurfaceUpdate\n"); break;
        }
        return;
    }

    if (hdr.ProviderId == WIN32K_PROVIDER_GUID) {
        switch (id) {
        case Win32K_TokenCompositionSurfaceObject:  PrintEventHeader(hdr); printf("Win32K_TokenCompositionSurfaceObject\n"); break;
        case Win32K_TokenStateChanged:
            PrintEventHeader(hdr);
            printf("Win32K_TokenStateChanged ");

            /*
            PointerT    pCompositionSurfaceObject;
            uint32_t    SwapChainIndex;
            uint32_t    PresentCount;
            uint64_t    FenceValue;
            uint32_t    NewState;
            ...
            */
            switch (((uint32_t*) eventRecord->UserData)[(eventRecord->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 5 : 6]) {
            case 3: printf("inframe\n"); break;
            case 4: printf("confirmed\n"); break;
            case 5: printf("retired\n"); break;
            case 6: printf("discarded\n"); break;
            default: printf("unknown\n"); break;
            }
            break;
        }
        return;
    }

    assert(false);
}

void DebugCreatePresent(PresentEvent const& p)
{
    if (gDebugTrace) {
        PrintUpdateHeader(p.Id);
        printf(" Create PresentMode=");
        PrintPresentMode(p.PresentMode);
        printf(" SwapChainAddress=%llx\n", p.SwapChainAddress);
    }
}

void DebugCompletePresent(PresentEvent const& p, int indent)
{
    if (gDebugTrace) {
        PrintUpdateHeader(p.Id, indent);
        printf(" CompletePresent TimeTaken=%s ",            AddCommas(ConvertTimestampDeltaToNs(p.TimeTaken)));
        printf("ReadyTime=%s ",   p.ReadyTime  == 0 ? "0" : AddCommas(ConvertTimestampDeltaToNs(p.ReadyTime  - p.QpcTime)));
        printf("ScreenTime=%s\n", p.ScreenTime == 0 ? "0" : AddCommas(ConvertTimestampDeltaToNs(p.ScreenTime - p.QpcTime)));
    }
}

void DebugPrintPresentMode(PresentEvent const& p)
{
    if (gDebugTrace) {
        PrintUpdateHeader(p.Id);
        printf(" PresentMode=");
        PrintPresentMode(p.PresentMode);
        printf("\n");
    }
}

void DebugPrintDwmNotified(PresentEvent const& p)
{
    if (gDebugTrace) {
        PrintUpdateHeader(p.Id);
        printf(" DwmNotified=%u\n", p.DwmNotified);
    }
}

void DebugPrintTokenPtr(PresentEvent const& p)
{
    if (gDebugTrace) {
        PrintUpdateHeader(p.Id);
        printf(" token=%llx\n", p.TokenPtr);
    }
}

#endif // if DEBUG_VERBOSE
