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

#include "PresentMonTraceConsumer.hpp"

#include "D3d9EventStructs.hpp"
#include "DwmEventStructs.hpp"
#include "DxgiEventStructs.hpp"
#include "DxgkrnlEventStructs.hpp"
#include "Win32kEventStructs.hpp"

#include <assert.h>

#if DEBUG_VERBOSE

namespace {

bool gDebugDone = false;
bool gDebugTrace = false;
uint64_t* gFirstTimestamp= 0;
uint64_t gTimestampFrequency = 0;

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

    if (hdr.ProviderId == Microsoft_Windows_D3D9::GUID) {
        switch (id) {
        case Microsoft_Windows_D3D9::Present_Start::Id: PrintEventHeader(hdr); printf("D3D9PresentStart\n"); break;
        case Microsoft_Windows_D3D9::Present_Stop::Id:  PrintEventHeader(hdr); printf("D3D9PresentStop\n"); break;
        }
        return;
    }

    if (hdr.ProviderId == Microsoft_Windows_DXGI::GUID) {
        switch (id) {
        case Microsoft_Windows_DXGI::Present_Start::Id:                  PrintEventHeader(hdr); printf("DXGIPresent_Start\n"); break;
        case Microsoft_Windows_DXGI::Present_Stop::Id:                   PrintEventHeader(hdr); printf("DXGIPresent_Stop\n"); break;
        case Microsoft_Windows_DXGI::PresentMultiplaneOverlay_Start::Id: PrintEventHeader(hdr); printf("DXGIPresentMPO_Start\n"); break;
        case Microsoft_Windows_DXGI::PresentMultiplaneOverlay_Stop::Id:  PrintEventHeader(hdr); printf("DXGIPresentMPO_Stop\n"); break;
        }
        return;
    }

    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::BLT_GUID)            { PrintEventHeader(hdr); printf("Win7::BLT\n"); return; }
    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::FLIP_GUID)           { PrintEventHeader(hdr); printf("Win7::FLIP\n"); return; }
    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::PRESENTHISTORY_GUID) { PrintEventHeader(hdr); printf("Win7::PRESENTHISTORY\n"); return; }
    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::QUEUEPACKET_GUID)    { PrintEventHeader(hdr); printf("Win7::QUEUEPACKET\n"); return; }
    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::VSYNCDPC_GUID)       { PrintEventHeader(hdr); printf("Win7::VSYNCDPC\n"); return; }
    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::MMIOFLIP_GUID)       { PrintEventHeader(hdr); printf("Win7::MMIOFLIP\n"); return; }

    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::GUID) {
        switch (id) {
        case Microsoft_Windows_DxgKrnl::Flip_Info::Id:                      PrintEventHeader(hdr); printf("DxgKrnl_Flip\n"); break;
        case Microsoft_Windows_DxgKrnl::FlipMultiPlaneOverlay_Info::Id:     PrintEventHeader(hdr); printf("DxgKrnl_FlipMPO\n"); break;
        case Microsoft_Windows_DxgKrnl::QueuePacket_Start::Id:              PrintEventHeader(hdr); printf("DxgKrnl_QueueSubmit\n"); break;
        case Microsoft_Windows_DxgKrnl::QueuePacket_Stop::Id:               PrintEventHeader(hdr); printf("DxgKrnl_QueueComplete\n"); break;
        case Microsoft_Windows_DxgKrnl::MMIOFlip_Info::Id:                  PrintEventHeader(hdr); printf("DxgKrnl_MMIOFlip\n"); break;
        case Microsoft_Windows_DxgKrnl::MMIOFlipMultiPlaneOverlay_Info::Id: PrintEventHeader(hdr); printf("DxgKrnl_MMIOFlipMPO\n"); break;
        case Microsoft_Windows_DxgKrnl::HSyncDPCMultiPlane_Info::Id:        PrintEventHeader(hdr); printf("DxgKrnl_HSyncDPC\n"); break;
        case Microsoft_Windows_DxgKrnl::VSyncDPC_Info::Id:                  PrintEventHeader(hdr); printf("DxgKrnl_VSyncDPC\n"); break;
        case Microsoft_Windows_DxgKrnl::Present_Info::Id:                   PrintEventHeader(hdr); printf("DxgKrnl_Present\n"); break;
        case Microsoft_Windows_DxgKrnl::Blit_Info::Id:                      PrintEventHeader(hdr); printf("DxgKrnl_Blit\n"); break;

        // The first part of the event data is the same for all these (detailed
        // has other members after).
        case Microsoft_Windows_DxgKrnl::PresentHistory_Start::Id:
        case Microsoft_Windows_DxgKrnl::PresentHistory_Info::Id:
        case Microsoft_Windows_DxgKrnl::PresentHistoryDetailed_Start::Id:
            PrintEventHeader(hdr);
            switch (id) {
            case Microsoft_Windows_DxgKrnl::PresentHistory_Start::Id:         printf("DxgKrnl_PresentHistoryStart"); break;
            case Microsoft_Windows_DxgKrnl::PresentHistory_Info::Id:          printf("DxgKrnl_PresentHistoryInfo"); break;
            case Microsoft_Windows_DxgKrnl::PresentHistoryDetailed_Start::Id: printf("DxgKrnl_PresentHistoryDetailed"); break;
            }

            // TODO: use 32-bit pointer if 32-bit
            {
                auto e = (Microsoft_Windows_DxgKrnl::PresentHistory_Info_Struct<uint64_t>*) eventRecord->UserData;
                printf(" token=%llx, model=", e->Token);
                switch (e->Model) {
                case D3DKMT_PM_UNINITIALIZED:          printf("uninitialized"); break;
                case D3DKMT_PM_REDIRECTED_GDI:         printf("redirected_gdi"); break;
                case D3DKMT_PM_REDIRECTED_FLIP:        printf("redirected_flip"); break;
                case D3DKMT_PM_REDIRECTED_BLT:         printf("redirected_blt"); break;
                case D3DKMT_PM_REDIRECTED_VISTABLT:    printf("redirected_vistablt"); break;
                case D3DKMT_PM_SCREENCAPTUREFENCE:     printf("screencapturefence"); break;
                case D3DKMT_PM_REDIRECTED_GDI_SYSMEM:  printf("redirected_gdi_sysmem"); break;
                case D3DKMT_PM_REDIRECTED_COMPOSITION: printf("redirected_composition"); break;
                default:                               printf("unknown"); break;
                }
                printf("\n");
            }
            break;
        }
        return;
    }

    if (hdr.ProviderId == Microsoft_Windows_Dwm_Core::GUID ||
        hdr.ProviderId == Microsoft_Windows_Dwm_Core::Win7::GUID) {
        switch (id) {
        case Microsoft_Windows_Dwm_Core::MILEVENT_MEDIA_UCE_PROCESSPRESENTHISTORY_GetPresentHistory_Info::Id:
                                                                          PrintEventHeader(hdr); printf("DWM_GetPresentHistory\n"); break;
        case Microsoft_Windows_Dwm_Core::SCHEDULE_PRESENT_Start::Id:      PrintEventHeader(hdr); printf("DWM_Schedule_Present_Start\n"); break;
        case Microsoft_Windows_Dwm_Core::FlipChain_Pending::Id:           PrintEventHeader(hdr); printf("DWM_FlipChain_Pending\n"); break;
        case Microsoft_Windows_Dwm_Core::FlipChain_Complete::Id:          PrintEventHeader(hdr); printf("DWM_FlipChain_Complete\n"); break;
        case Microsoft_Windows_Dwm_Core::FlipChain_Dirty::Id:             PrintEventHeader(hdr); printf("DWM_FlipChain_Dirty\n"); break;
        case Microsoft_Windows_Dwm_Core::SCHEDULE_SURFACEUPDATE_Info::Id: PrintEventHeader(hdr); printf("DWM_Schedule_SurfaceUpdate\n"); break;
        }
        return;
    }

    if (hdr.ProviderId == Microsoft_Windows_Win32k::GUID) {
        switch (id) {
        case Microsoft_Windows_Win32k::TokenCompositionSurfaceObject_Info::Id:
            PrintEventHeader(hdr);
            printf("Win32K_TokenCompositionSurfaceObject\n");
            break;
        case Microsoft_Windows_Win32k::TokenStateChanged_Info::Id:
            PrintEventHeader(hdr);
            printf("Win32K_TokenStateChanged ");

            {
                auto newState = (eventRecord->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER)
                    ? ((Microsoft_Windows_Win32k::TokenStateChanged_Info_Struct<uint32_t>*) eventRecord->UserData)->NewState
                    : ((Microsoft_Windows_Win32k::TokenStateChanged_Info_Struct<uint64_t>*) eventRecord->UserData)->NewState;
                switch (newState) {
                case Microsoft_Windows_Win32k::TokenState::InFrame:   printf("inframe\n"); break;
                case Microsoft_Windows_Win32k::TokenState::Confirmed: printf("confirmed\n"); break;
                case Microsoft_Windows_Win32k::TokenState::Retired:   printf("retired\n"); break;
                case Microsoft_Windows_Win32k::TokenState::Discarded: printf("discarded\n"); break;
                default:                                              printf("unknown\n"); break;
                }
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
