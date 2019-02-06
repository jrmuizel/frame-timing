/*
Copyright 2017-2019 Intel Corporation

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

#define NOMINMAX
#include <algorithm>
#include <d3d9.h>
#include <dxgi.h>

#include "PresentMonTraceConsumer.hpp"

#include "D3d9EventStructs.hpp"
#include "DwmEventStructs.hpp"
#include "DxgiEventStructs.hpp"
#include "DxgkrnlEventStructs.hpp"
#include "EventMetadataEventStructs.hpp"
#include "Win32kEventStructs.hpp"

// First, structures that can be produced either by processing legacy events
// or modern manifest-based events.

struct DxgkEventBase
{
    EVENT_HEADER const* pEventHeader;
};

struct DxgkBltEventArgs : DxgkEventBase
{
    uint64_t Hwnd;
    bool Present;
};

struct DxgkFlipEventArgs : DxgkEventBase
{
    int32_t FlipInterval;
    bool MMIO;
};

// A QueueSubmit can be many types, but these are interesting for present.
enum class DxgKrnl_QueueSubmit_Type {
    MMIOFlip = 3,
    Software = 7,
};
struct DxgkQueueSubmitEventArgs : DxgkEventBase
{
    DxgKrnl_QueueSubmit_Type PacketType;
    uint32_t SubmitSequence;
    uint64_t Context;
    bool Present;
    bool SupportsDxgkPresentEvent;
};

struct DxgkQueueCompleteEventArgs : DxgkEventBase
{
    uint32_t SubmitSequence;
};

enum DxgKrnl_MMIOFlip_Flags {
    FlipImmediate = 0x2,
    FlipOnNextVSync = 0x4
};
struct DxgkMMIOFlipEventArgs : DxgkEventBase
{
    uint32_t FlipSubmitSequence;
    DxgKrnl_MMIOFlip_Flags Flags;
};

struct DxgkSyncDPCEventArgs : DxgkEventBase
{
    uint32_t FlipSubmitSequence;
};

struct DxgkSubmitPresentHistoryEventArgs : DxgkEventBase
{
    uint64_t Token;
    uint64_t TokenData;
    PresentMode KnownPresentMode;
};

struct DxgkPropagatePresentHistoryEventArgs : DxgkEventBase
{
    uint64_t Token;
};

struct DxgkRetirePresentHistoryEventArgs : DxgkEventBase
{
    uint64_t Token;
    PresentMode KnownPresentMode;
};

// These are the structures used to process the legacy events.

//
// Ensure that all ETW structure won't be aligned. This is necessary to
// guarantee that a user mode application trying to get to this data
// can get the same structure layout using a different compiler.
//
#pragma pack(push)
#pragma pack(1)

//
// Dxgkrnl Blt Event
//

typedef struct _DXGKETW_PRESENTFLAGS
{
    struct
    {
        UINT    Blt : 1;    // 0x00000001
        UINT    ColorFill : 1;    // 0x00000002
        UINT    Flip : 1;    // 0x00000004
        UINT    FlipWithNoWait : 1;    // 0x00000008
        UINT    SrcColorKey : 1;    // 0x00000010
        UINT    DstColorKey : 1;    // 0x00000020
        UINT    LinearToSrgb : 1;    // 0x00000040
        UINT    Rotate : 1;    // 0x00000080
        UINT    Reserved : 24;    // 0xFFFFFF00
    } Bits;
} DXGKETW_PRESENTFLAGS;

typedef struct _DXGKETW_BLTEVENT {
    ULONGLONG                  hwnd;
    ULONGLONG                  pDmaBuffer;
    ULONGLONG                  PresentHistoryToken;
    ULONGLONG                  hSourceAllocation;
    ULONGLONG                  hDestAllocation;
    BOOL                       bSubmit;
    BOOL                       bRedirectedPresent;
    DXGKETW_PRESENTFLAGS       Flags;
    RECT                       SourceRect;
    RECT                       DestRect;
    UINT                       SubRectCount; // followed by variable number of ETWGUID_DXGKBLTRECT events
} DXGKETW_BLTEVENT;

//
// Dxgkrnl Flip Event
//

typedef struct _DXGKETW_FLIPEVENT {
    ULONGLONG                  pDmaBuffer;
    ULONG                      VidPnSourceId;
    ULONGLONG                  FlipToAllocation;
    UINT                       FlipInterval; // D3DDDI_FLIPINTERVAL_TYPE
    BOOLEAN                    FlipWithNoWait;
    BOOLEAN                    MMIOFlip;
} DXGKETW_FLIPEVENT;

//
// Dxgkrnl Present History Event.
//

typedef enum _D3DKMT_PRESENT_MODEL
{
    D3DKMT_PM_UNINITIALIZED = 0,
    D3DKMT_PM_REDIRECTED_GDI = 1,
    D3DKMT_PM_REDIRECTED_FLIP = 2,
    D3DKMT_PM_REDIRECTED_BLT = 3,
    D3DKMT_PM_REDIRECTED_VISTABLT = 4,
    D3DKMT_PM_SCREENCAPTUREFENCE = 5,
    D3DKMT_PM_REDIRECTED_GDI_SYSMEM = 6,
    D3DKMT_PM_REDIRECTED_COMPOSITION = 7,
} D3DKMT_PRESENT_MODEL;

typedef struct _DXGKETW_PRESENTHISTORYEVENT {
    ULONGLONG             hAdapter;
    ULONGLONG             Token;
    D3DKMT_PRESENT_MODEL  Model;     // available only for _STOP event type.
    UINT                  TokenSize; // available only for _STOP event type.
} DXGKETW_PRESENTHISTORYEVENT;

//
// Dxgkrnl Scheduler Submit QueuePacket Event
//

typedef enum _DXGKETW_QUEUE_PACKET_TYPE {
    DXGKETW_RENDER_COMMAND_BUFFER = 0,
    DXGKETW_DEFERRED_COMMAND_BUFFER = 1,
    DXGKETW_SYSTEM_COMMAND_BUFFER = 2,
    DXGKETW_MMIOFLIP_COMMAND_BUFFER = 3,
    DXGKETW_WAIT_COMMAND_BUFFER = 4,
    DXGKETW_SIGNAL_COMMAND_BUFFER = 5,
    DXGKETW_DEVICE_COMMAND_BUFFER = 6,
    DXGKETW_SOFTWARE_COMMAND_BUFFER = 7,
    DXGKETW_PAGING_COMMAND_BUFFER = 8,
} DXGKETW_QUEUE_PACKET_TYPE;

typedef struct _DXGKETW_QUEUESUBMITEVENT {
    ULONGLONG                  hContext;
    DXGKETW_QUEUE_PACKET_TYPE  PacketType;
    ULONG                      SubmitSequence;
    ULONGLONG                  DmaBufferSize;
    UINT                       AllocationListSize;
    UINT                       PatchLocationListSize;
    BOOL                       bPresent;
    ULONGLONG                  hDmaBuffer;
} DXGKETW_QUEUESUBMITEVENT;

//
// Dxgkrnl Scheduler Complete QueuePacket Event
//

typedef struct _DXGKETW_QUEUECOMPLETEEVENT {
    ULONGLONG                  hContext;
    ULONG                      PacketType;
    ULONG                      SubmitSequence;
    union {
        BOOL                   bPreempted;
        BOOL                   bTimeouted; // PacketType is WaitCommandBuffer.
    };
} DXGKETW_QUEUECOMPLETEEVENT;

//
// Dxgkrnl VSync Dpc event
//

typedef enum _DXGKETW_FLIPMODE_TYPE
{
    DXGKETW_FLIPMODE_NO_DEVICE = 0,
    DXGKETW_FLIPMODE_IMMEDIATE = 1,
    DXGKETW_FLIPMODE_VSYNC_HW_FLIP_QUEUE = 2,
    DXGKETW_FLIPMODE_VSYNC_SW_FLIP_QUEUE = 3,
    DXGKETW_FLIPMODE_VSYNC_BUILT_IN_WAIT = 4,
    DXGKETW_FLIPMODE_IMMEDIATE_SW_FLIP_QUEUE = 5,
} DXGKETW_FLIPMODE_TYPE;

typedef LARGE_INTEGER PHYSICAL_ADDRESS;

typedef struct _DXGKETW_SCHEDULER_VSYNC_DPC {
    ULONGLONG                 pDxgAdapter;
    UINT                      VidPnTargetId;
    PHYSICAL_ADDRESS          ScannedPhysicalAddress;
    UINT                      VidPnSourceId;
    UINT                      FrameNumber;
    LONGLONG                  FrameQPCTime;
    ULONGLONG                 hFlipDevice;
    DXGKETW_FLIPMODE_TYPE     FlipType;
    union
    {
        ULARGE_INTEGER        FlipFenceId;
        PHYSICAL_ADDRESS      FlipToAddress;
    };
} DXGKETW_SCHEDULER_VSYNC_DPC;

//
// Dxgkrnl Scheduler mmio flip event
//

// there is 2 version of structure due to pointer size difference
// between x86 (32bits) and x64/ia64 (64bits).
//

typedef struct _DXGKETW_SCHEDULER_MMIO_FLIP_32 {
    ULONGLONG        pDxgAdapter;
    UINT             VidPnSourceId;
    ULONG            FlipSubmitSequence; // ContextUserSubmissionId
    UINT             FlipToDriverAllocation;
    PHYSICAL_ADDRESS FlipToPhysicalAddress;
    UINT             FlipToSegmentId;
    UINT             FlipPresentId;
    UINT             FlipPhysicalAdapterMask;
    ULONG            Flags;
} DXGKETW_SCHEDULER_MMIO_FLIP_32;

typedef struct _DXGKETW_SCHEDULER_MMIO_FLIP_64 {
    ULONGLONG        pDxgAdapter;
    UINT             VidPnSourceId;
    ULONG            FlipSubmitSequence; // ContextUserSubmissionId
    ULONGLONG        FlipToDriverAllocation;
    PHYSICAL_ADDRESS FlipToPhysicalAddress;
    UINT             FlipToSegmentId;
    UINT             FlipPresentId;
    UINT             FlipPhysicalAdapterMask;
    ULONG            Flags;
} DXGKETW_SCHEDULER_MMIO_FLIP_64;

#pragma pack(pop)


PresentEvent::PresentEvent(EVENT_HEADER const& hdr, ::Runtime runtime)
    : QpcTime(*(uint64_t*) &hdr.TimeStamp)
    , ProcessId(hdr.ProcessId)
    , ThreadId(hdr.ThreadId)
    , TimeTaken(0)
    , ReadyTime(0)
    , ScreenTime(0)
    , SwapChainAddress(0)
    , SyncInterval(-1)
    , PresentFlags(0)
    , Hwnd(0)
    , TokenPtr(0)
    , QueueSubmitSequence(0)
    , Runtime(runtime)
    , PresentMode(PresentMode::Unknown)
    , FinalState(PresentResult::Unknown)
    , SupportsTearing(false)
    , MMIO(false)
    , SeenDxgkPresent(false)
    , SeenWin32KEvents(false)
    , WasBatched(false)
    , DwmNotified(false)
    , Completed(false)
{
#if DEBUG_VERBOSE
    static uint64_t presentCount = 0;
    presentCount += 1;
    Id = presentCount;
#endif
}

#ifndef NDEBUG
static bool gPresentMonTraceConsumer_Exiting = false;
#endif

PresentEvent::~PresentEvent()
{
    assert(Completed || gPresentMonTraceConsumer_Exiting);
}

void PresentEvent::SetPresentMode(::PresentMode mode)
{
    PresentMode = mode;
    DebugPrintPresentMode(*this);
}

void PresentEvent::SetDwmNotified(bool notified)
{
    DwmNotified = notified;
    DebugPrintDwmNotified(*this);
}

void PresentEvent::SetTokenPtr(uint64_t tokenPtr)
{
    TokenPtr = tokenPtr;
    DebugPrintTokenPtr(*this);
}

PMTraceConsumer::~PMTraceConsumer()
{
#ifndef NDEBUG
    gPresentMonTraceConsumer_Exiting = true;
#endif
}

void HandleDXGIEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    auto const& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) {
    case Microsoft_Windows_DXGI::Present_Start::Id:
    case Microsoft_Windows_DXGI::PresentMultiplaneOverlay_Start::Id:
    {
        PresentEvent event(hdr, Runtime::DXGI);
        pmConsumer->mMetadata.GetEventData(pEventRecord, L"pIDXGISwapChain", &event.SwapChainAddress);
        pmConsumer->mMetadata.GetEventData(pEventRecord, L"Flags",           &event.PresentFlags);
        pmConsumer->mMetadata.GetEventData(pEventRecord, L"SyncInterval",    &event.SyncInterval);

        pmConsumer->CreatePresent(event);
        break;
    }
    case Microsoft_Windows_DXGI::Present_Stop::Id:
    case Microsoft_Windows_DXGI::PresentMultiplaneOverlay_Stop::Id:
    {
        auto result = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"Result");

        bool AllowBatching =
            SUCCEEDED(result) &&
            result != DXGI_STATUS_OCCLUDED &&
            result != DXGI_STATUS_MODE_CHANGE_IN_PROGRESS &&
            result != DXGI_STATUS_NO_DESKTOP_ACCESS;

        pmConsumer->RuntimePresentStop(hdr, AllowBatching);
        break;
    }
    }
}

void PMTraceConsumer::HandleDxgkBlt(DxgkBltEventArgs& args)
{
    auto eventIter = FindOrCreatePresent(*args.pEventHeader);

    // Check if we might have retrieved a 'stuck' present from a previous
    // frame.  If the present mode isn't unknown at this point, we've already
    // seen this present progress further
    //
    // TODO: do we really want to just throw it away?  Should we complete with
    // unknown completion status or something?  Does this happen?
    if (eventIter->second->PresentMode != PresentMode::Unknown) {
        mPresentByThreadId.erase(eventIter);
        eventIter = FindOrCreatePresent(*args.pEventHeader);
    }

    // This could be one of several types of presents. Further events will clarify.
    // For now, assume that this is a blt straight into a surface which is already on-screen.
    eventIter->second->SetPresentMode(args.Present ? PresentMode::Composed_Copy_CPU_GDI : PresentMode::Hardware_Legacy_Copy_To_Front_Buffer);
    eventIter->second->SupportsTearing = !args.Present;
    eventIter->second->Hwnd = args.Hwnd;
}

void PMTraceConsumer::HandleDxgkFlip(DxgkFlipEventArgs& args)
{
    // A flip event is emitted during fullscreen present submission.
    // Afterwards, expect an MMIOFlip packet on the same thread, used
    // to trace the flip to screen.
    auto eventIter = FindOrCreatePresent(*args.pEventHeader);

    // Check if we might have retrieved a 'stuck' present from a previous frame.
    // The only events that we can expect before a Flip/FlipMPO are a runtime present start, or a previous FlipMPO.
    if (eventIter->second->QueueSubmitSequence != 0 || eventIter->second->SeenDxgkPresent) {
        // It's already progressed further but didn't complete, ignore it and create a new one.
        mPresentByThreadId.erase(eventIter);
        eventIter = FindOrCreatePresent(*args.pEventHeader);
    }

    if (eventIter->second->PresentMode != PresentMode::Unknown) {
        // For MPO, N events may be issued, but we only care about the first
        return;
    }

    eventIter->second->MMIO = args.MMIO;
    eventIter->second->SetPresentMode(PresentMode::Hardware_Legacy_Flip);

    if (eventIter->second->SyncInterval == -1) {
        eventIter->second->SyncInterval = args.FlipInterval;
    }
    if (!args.MMIO) {
        eventIter->second->SupportsTearing = args.FlipInterval == 0;
    }

    // If this is the DWM thread, piggyback these pending presents on our fullscreen present
    if (args.pEventHeader->ThreadId == DwmPresentThreadId) {
        std::swap(eventIter->second->DependentPresents, mPresentsWaitingForDWM);
        DwmPresentThreadId = 0;
    }
}

void PMTraceConsumer::HandleDxgkQueueSubmit(DxgkQueueSubmitEventArgs& args)
{
    // If we know we're never going to get a DxgkPresent event for a given blt, then let's try to determine if it's a redirected blt or not.
    // If it's redirected, then the SubmitPresentHistory event should've been emitted before submitting anything else to the same context,
    // and therefore we'll know it's a redirected present by this point. If it's still non-redirected, then treat this as if it was a DxgkPresent
    // event - the present will be considered completed once its work is done, or if the work is already done, complete it now.
    if (!args.SupportsDxgkPresentEvent) {
        auto eventIter = mBltsByDxgContext.find(args.Context);
        if (eventIter != mBltsByDxgContext.end()) {
            if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer) {
                eventIter->second->SeenDxgkPresent = true;
                if (eventIter->second->ScreenTime != 0) {
                    CompletePresent(eventIter->second);
                }
            }
            mBltsByDxgContext.erase(eventIter);
        }
    }

    // This event is emitted after a flip/blt/PHT event, and may be the only way
    // to trace completion of the present.
    if (args.PacketType == DxgKrnl_QueueSubmit_Type::MMIOFlip ||
        args.PacketType == DxgKrnl_QueueSubmit_Type::Software ||
        args.Present) {
        auto eventIter = mPresentByThreadId.find(args.pEventHeader->ThreadId);
        if (eventIter == mPresentByThreadId.end() || eventIter->second->QueueSubmitSequence != 0) {
            return;
        }

        eventIter->second->QueueSubmitSequence = args.SubmitSequence;
        mPresentsBySubmitSequence.emplace(args.SubmitSequence, eventIter->second);

        if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer && !args.SupportsDxgkPresentEvent) {
            mBltsByDxgContext[args.Context] = eventIter->second;
        }
    }
}

void PMTraceConsumer::HandleDxgkQueueComplete(DxgkQueueCompleteEventArgs& args)
{
    auto eventIter = mPresentsBySubmitSequence.find(args.SubmitSequence);
    if (eventIter == mPresentsBySubmitSequence.end()) {
        return;
    }

    auto pEvent = eventIter->second;

    if (pEvent->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer ||
        (pEvent->PresentMode == PresentMode::Hardware_Legacy_Flip && !pEvent->MMIO)) {
        pEvent->ReadyTime = args.pEventHeader->TimeStamp.QuadPart;
        pEvent->ScreenTime = args.pEventHeader->TimeStamp.QuadPart;
        pEvent->FinalState = PresentResult::Presented;

        // Sometimes, the queue packets associated with a present will complete before the DxgKrnl present event is fired
        // In this case, for blit presents, we have no way to differentiate between fullscreen and windowed blits
        // So, defer the completion of this present until we know all events have been fired
        if (pEvent->SeenDxgkPresent || pEvent->PresentMode != PresentMode::Hardware_Legacy_Copy_To_Front_Buffer) {
            CompletePresent(pEvent);
        }
    }
}

void PMTraceConsumer::HandleDxgkMMIOFlip(DxgkMMIOFlipEventArgs& args)
{
    // An MMIOFlip event is emitted when an MMIOFlip packet is dequeued.
    // This corresponds to all GPU work prior to the flip being completed
    // (i.e. present "ready")
    // It also is emitted when an independent flip PHT is dequed,
    // and will tell us whether the present is immediate or vsync.
    auto eventIter = mPresentsBySubmitSequence.find(args.FlipSubmitSequence);
    if (eventIter == mPresentsBySubmitSequence.end()) {
        return;
    }

    eventIter->second->ReadyTime = args.pEventHeader->TimeStamp.QuadPart;

    if (eventIter->second->PresentMode == PresentMode::Composed_Flip) {
        eventIter->second->SetPresentMode(PresentMode::Hardware_Independent_Flip);
    }

    if (args.Flags & DxgKrnl_MMIOFlip_Flags::FlipImmediate) {
        eventIter->second->FinalState = PresentResult::Presented;
        eventIter->second->ScreenTime = args.pEventHeader->TimeStamp.QuadPart;
        eventIter->second->SupportsTearing = true;
        if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Flip) {
            CompletePresent(eventIter->second);
        }
    }
}

void PMTraceConsumer::HandleDxgkSyncDPC(DxgkSyncDPCEventArgs& args)
{
    // The VSyncDPC/HSyncDPC contains a field telling us what flipped to screen.
    // This is the way to track completion of a fullscreen present.
    auto eventIter = mPresentsBySubmitSequence.find(args.FlipSubmitSequence);
    if (eventIter == mPresentsBySubmitSequence.end()) {
        return;
    }

    eventIter->second->ScreenTime = args.pEventHeader->TimeStamp.QuadPart;
    eventIter->second->FinalState = PresentResult::Presented;
    if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Flip) {
        CompletePresent(eventIter->second);
    }
}

void PMTraceConsumer::HandleDxgkSubmitPresentHistoryEventArgs(DxgkSubmitPresentHistoryEventArgs& args)
{
    // These events are emitted during submission of all types of windowed presents while DWM is on.
    // It gives us up to two different types of keys to correlate further.
    auto eventIter = FindOrCreatePresent(*args.pEventHeader);

    // Check if we might have retrieved a 'stuck' present from a previous frame.
    if (eventIter->second->TokenPtr != 0) {
        // It's already progressed further but didn't complete, ignore it and create a new one.
        mPresentByThreadId.erase(eventIter);
        eventIter = FindOrCreatePresent(*args.pEventHeader);
    }

    eventIter->second->ReadyTime = 0;
    eventIter->second->ScreenTime = 0;
    eventIter->second->SupportsTearing = false;
    eventIter->second->FinalState = PresentResult::Unknown;
    eventIter->second->SetTokenPtr(args.Token);

    if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer) {
        eventIter->second->SetPresentMode(PresentMode::Composed_Copy_GPU_GDI);
        assert(args.KnownPresentMode == PresentMode::Unknown ||
               args.KnownPresentMode == PresentMode::Composed_Copy_GPU_GDI);

    } else if (eventIter->second->PresentMode == PresentMode::Unknown) {
        if (args.KnownPresentMode == PresentMode::Composed_Composition_Atlas) {
            eventIter->second->SetPresentMode(PresentMode::Composed_Composition_Atlas);
        } else {
            // When there's no Win32K events, we'll assume PHTs that aren't after a blt, and aren't composition tokens
            // are flip tokens and that they're displayed. There are no Win32K events on Win7, and they might not be
            // present in some traces - don't let presents get stuck/dropped just because we can't track them perfectly.
            assert(!eventIter->second->SeenWin32KEvents);
            eventIter->second->SetPresentMode(PresentMode::Composed_Flip);
        }
    } else if (eventIter->second->PresentMode == PresentMode::Composed_Copy_CPU_GDI) {
        if (args.TokenData == 0) {
            // This is the best we can do, we won't be able to tell how many frames are actually displayed.
            mPresentsWaitingForDWM.emplace_back(eventIter->second);
        } else {
            mPresentsByLegacyBlitToken[args.TokenData] = eventIter->second;
        }
    }
    mDxgKrnlPresentHistoryTokens[args.Token] = eventIter->second;
}

void PMTraceConsumer::HandleDxgkPropagatePresentHistoryEventArgs(DxgkPropagatePresentHistoryEventArgs& args)
{
    // This event is emitted when a token is being handed off to DWM, and is a good way to indicate a ready state
    auto eventIter = mDxgKrnlPresentHistoryTokens.find(args.Token);
    if (eventIter == mDxgKrnlPresentHistoryTokens.end()) {
        return;
    }

    eventIter->second->ReadyTime = eventIter->second->ReadyTime == 0
        ? args.pEventHeader->TimeStamp.QuadPart
        : std::min(eventIter->second->ReadyTime, (uint64_t) args.pEventHeader->TimeStamp.QuadPart);

    if (eventIter->second->PresentMode == PresentMode::Composed_Composition_Atlas ||
        (eventIter->second->PresentMode == PresentMode::Composed_Flip && !eventIter->second->SeenWin32KEvents)) {
        mPresentsWaitingForDWM.emplace_back(eventIter->second);
    }

    if (eventIter->second->PresentMode == PresentMode::Composed_Copy_GPU_GDI) {
        // Manipulate the map here
        // When DWM is ready to present, we'll query for the most recent blt targeting this window and take it out of the map
        mLastWindowPresent[eventIter->second->Hwnd] = eventIter->second;
    }

    mDxgKrnlPresentHistoryTokens.erase(eventIter);
}

void HandleDXGKEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    auto const& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) {
    case Microsoft_Windows_DxgKrnl::Flip_Info::Id:
    case Microsoft_Windows_DxgKrnl::FlipMultiPlaneOverlay_Info::Id:
    {
        DxgkFlipEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.FlipInterval = -1;
        if (hdr.EventDescriptor.Id == (USHORT) Microsoft_Windows_DxgKrnl::Flip_Info::Id) {
            Args.FlipInterval = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"FlipInterval");
            Args.MMIO = pmConsumer->mMetadata.GetEventData<BOOL>(pEventRecord, L"MMIOFlip") != 0;
        } else {
            Args.MMIO = true; // All MPO flips are MMIO
        }
        pmConsumer->HandleDxgkFlip(Args);
        break;
    }
    case Microsoft_Windows_DxgKrnl::QueuePacket_Start::Id:
    {
        DxgkQueueSubmitEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.PacketType = pmConsumer->mMetadata.GetEventData<DxgKrnl_QueueSubmit_Type>(pEventRecord, L"PacketType");
        Args.SubmitSequence = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"SubmitSequence");
        Args.Present = pmConsumer->mMetadata.GetEventData<BOOL>(pEventRecord, L"bPresent") != 0;
        Args.Context = pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"hContext");
        Args.SupportsDxgkPresentEvent = true;
        pmConsumer->HandleDxgkQueueSubmit(Args);
        break;
    }
    case Microsoft_Windows_DxgKrnl::QueuePacket_Stop::Id:
    {
        DxgkQueueCompleteEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.SubmitSequence = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"SubmitSequence");
        pmConsumer->HandleDxgkQueueComplete(Args);
        break;
    }
    case Microsoft_Windows_DxgKrnl::MMIOFlip_Info::Id:
    {
        DxgkMMIOFlipEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.FlipSubmitSequence = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"FlipSubmitSequence");
        Args.Flags = pmConsumer->mMetadata.GetEventData<DxgKrnl_MMIOFlip_Flags>(pEventRecord, L"Flags");
        pmConsumer->HandleDxgkMMIOFlip(Args);
        break;
    }
    case Microsoft_Windows_DxgKrnl::MMIOFlipMultiPlaneOverlay_Info::Id:
    {
        // See above for more info about this packet.
        // Note: Event does not exist on Win7
        auto FlipFenceId = pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"FlipSubmitSequence");
        uint32_t FlipSubmitSequence = (uint32_t)(FlipFenceId >> 32u);

        auto eventIter = pmConsumer->mPresentsBySubmitSequence.find(FlipSubmitSequence);
        if (eventIter == pmConsumer->mPresentsBySubmitSequence.end()) {
            return;
        }

        // Avoid double-marking a single present packet coming from the MPO API
        if (eventIter->second->ReadyTime == 0) {
            eventIter->second->ReadyTime = hdr.TimeStamp.QuadPart;
        }

        if (eventIter->second->PresentMode == PresentMode::Hardware_Independent_Flip ||
            eventIter->second->PresentMode == PresentMode::Composed_Flip) {
            eventIter->second->SetPresentMode(PresentMode::Hardware_Composed_Independent_Flip);
        }

        if (hdr.EventDescriptor.Version >= 2) {
            enum {
                FlipWaitVSync = 5,
                FlipWaitComplete = 11,
                FlipWaitHSync = 15,
                // There are others, but they're more complicated to deal with.
            };

            auto FlipEntryStatusAfterFlip = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"FlipEntryStatusAfterFlip");
            if (FlipEntryStatusAfterFlip != FlipWaitVSync &&
                FlipEntryStatusAfterFlip != FlipWaitHSync) {
                eventIter->second->FinalState = PresentResult::Presented;
                eventIter->second->SupportsTearing = true;
                if (FlipEntryStatusAfterFlip == FlipWaitComplete) {
                    eventIter->second->ScreenTime = hdr.TimeStamp.QuadPart;
                }
                if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Flip) {
                    pmConsumer->CompletePresent(eventIter->second);
                }
            }
        }
        break;
    }
    case Microsoft_Windows_DxgKrnl::HSyncDPCMultiPlane_Info::Id:
    {
        // Used for Hardware Independent Flip, and Hardware Composed Flip to signal flipping to the screen 
        // on Windows 10 build numbers 17134 and up where the associated display is connected to 
        // integrated graphics
        // MMIOFlipMPO [EntryStatus:FlipWaitHSync] ->HSync DPC

        auto FlipCount = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"FlipEntryCount");
        for (uint32_t i = 0; i < FlipCount; i++) {
            auto FlipId = pmConsumer->mMetadata.GetEventDataFromArray<uint64_t>(pEventRecord, L"FlipSubmitSequence", i);
            DxgkSyncDPCEventArgs Args = {};
            Args.pEventHeader = &hdr;
            Args.FlipSubmitSequence = (uint32_t)(FlipId >> 32u);
            pmConsumer->HandleDxgkSyncDPC(Args);
        }
        break;
    }
    case Microsoft_Windows_DxgKrnl::VSyncDPC_Info::Id:
    {
        auto FlipFenceId = pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"FlipFenceId");

        DxgkSyncDPCEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.FlipSubmitSequence = (uint32_t)(FlipFenceId >> 32u);
        pmConsumer->HandleDxgkSyncDPC(Args);
        break;
    }
    case Microsoft_Windows_DxgKrnl::Present_Info::Id:
    {
        // This event is emitted at the end of the kernel present, before returning.
        // The presence of this event is used with blt presents to indicate that no
        // PHT is to be expected.
        auto eventIter = pmConsumer->mPresentByThreadId.find(hdr.ThreadId);
        if (eventIter == pmConsumer->mPresentByThreadId.end()) {
            return;
        }

        eventIter->second->SeenDxgkPresent = true;
        if (eventIter->second->Hwnd == 0) {
            eventIter->second->Hwnd = pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"hWindow");
        }

        if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer &&
            eventIter->second->ScreenTime != 0) {
            // This is a fullscreen or DWM-off blit where all work associated was already done, so it's on-screen
            // It was deferred to here because there was no way to be sure it was really fullscreen until now
            pmConsumer->CompletePresent(eventIter->second);
        }

        if (eventIter->second->ThreadId != hdr.ThreadId) {
            if (eventIter->second->TimeTaken == 0) {
                eventIter->second->TimeTaken = hdr.TimeStamp.QuadPart - eventIter->second->QpcTime;
            }
            eventIter->second->WasBatched = true;
            pmConsumer->mPresentByThreadId.erase(eventIter);
        }
        break;
    }
    case Microsoft_Windows_DxgKrnl::PresentHistoryDetailed_Start::Id:
    case Microsoft_Windows_DxgKrnl::PresentHistory_Start::Id:
    {
        DxgkSubmitPresentHistoryEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.Token = pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"Token");
        Args.TokenData = pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"TokenData");
        auto KMTPresentModel = pmConsumer->mMetadata.GetEventData<D3DKMT_PRESENT_MODEL>(pEventRecord, L"Model");
        switch (KMTPresentModel) {
        case D3DKMT_PM_REDIRECTED_BLT:          Args.KnownPresentMode = PresentMode::Composed_Copy_GPU_GDI; break;
        case D3DKMT_PM_REDIRECTED_VISTABLT:     Args.KnownPresentMode = PresentMode::Composed_Copy_CPU_GDI; break;
        case D3DKMT_PM_REDIRECTED_FLIP:         Args.KnownPresentMode = PresentMode::Composed_Flip; break;
        case D3DKMT_PM_REDIRECTED_COMPOSITION:  Args.KnownPresentMode = PresentMode::Composed_Composition_Atlas; break;
        default:                                Args.KnownPresentMode = PresentMode::Unknown; break;
        }
        if (KMTPresentModel != D3DKMT_PM_REDIRECTED_GDI) {
            pmConsumer->HandleDxgkSubmitPresentHistoryEventArgs(Args);
        }
        break;
    }
    case Microsoft_Windows_DxgKrnl::PresentHistory_Info::Id:
    {
        DxgkPropagatePresentHistoryEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.Token = pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"Token");
        pmConsumer->HandleDxgkPropagatePresentHistoryEventArgs(Args);
        break;
    }
    case Microsoft_Windows_DxgKrnl::Blit_Info::Id:
    {
        DxgkBltEventArgs Args = {};
        Args.pEventHeader = &hdr;
        Args.Hwnd = pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"hwnd");
        Args.Present = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"bRedirectedPresent") != 0;
        pmConsumer->HandleDxgkBlt(Args);
        break;
    }
    }
}

namespace Win7 {

void HandleDxgkBlt(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    DxgkBltEventArgs Args = {};
    Args.pEventHeader = &pEventRecord->EventHeader;
    auto pBltEvent = reinterpret_cast<DXGKETW_BLTEVENT*>(pEventRecord->UserData);
    Args.Hwnd = pBltEvent->hwnd;
    Args.Present = pBltEvent->bRedirectedPresent != 0;
    pmConsumer->HandleDxgkBlt(Args);
}

void HandleDxgkFlip(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    DxgkFlipEventArgs Args = {};
    Args.pEventHeader = &pEventRecord->EventHeader;
    auto pFlipEvent = reinterpret_cast<DXGKETW_FLIPEVENT*>(pEventRecord->UserData);
    Args.FlipInterval = pFlipEvent->FlipInterval;
    Args.MMIO = pFlipEvent->MMIOFlip != 0;
    pmConsumer->HandleDxgkFlip(Args);
}

void HandleDxgkPresentHistory(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    auto pPresentHistoryEvent = reinterpret_cast<DXGKETW_PRESENTHISTORYEVENT*>(pEventRecord->UserData);
    if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_START) {
        DxgkSubmitPresentHistoryEventArgs Args = {};
        Args.pEventHeader = &pEventRecord->EventHeader;
        Args.KnownPresentMode = PresentMode::Unknown;
        Args.Token = pPresentHistoryEvent->Token;
        pmConsumer->HandleDxgkSubmitPresentHistoryEventArgs(Args);
    } else if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_INFO) {
        DxgkPropagatePresentHistoryEventArgs Args = {};
        Args.pEventHeader = &pEventRecord->EventHeader;
        Args.Token = pPresentHistoryEvent->Token;
        pmConsumer->HandleDxgkPropagatePresentHistoryEventArgs(Args);
    }
}

void HandleDxgkQueuePacket(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_START) {
        DxgkQueueSubmitEventArgs Args = {};
        Args.pEventHeader = &pEventRecord->EventHeader;
        auto pSubmitEvent = reinterpret_cast<DXGKETW_QUEUESUBMITEVENT*>(pEventRecord->UserData);
        switch (pSubmitEvent->PacketType) {
        case DXGKETW_MMIOFLIP_COMMAND_BUFFER:
            Args.PacketType = DxgKrnl_QueueSubmit_Type::MMIOFlip;
            break;
        case DXGKETW_SOFTWARE_COMMAND_BUFFER:
            Args.PacketType = DxgKrnl_QueueSubmit_Type::Software;
            break;
        default:
            Args.Present = pSubmitEvent->bPresent != 0;
            break;
        }
        Args.SubmitSequence = pSubmitEvent->SubmitSequence;
        Args.Context = pSubmitEvent->hContext;
        pmConsumer->HandleDxgkQueueSubmit(Args);
    } else if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_STOP) {
        DxgkQueueCompleteEventArgs Args = {};
        Args.pEventHeader = &pEventRecord->EventHeader;
        auto pCompleteEvent = reinterpret_cast<DXGKETW_QUEUECOMPLETEEVENT*>(pEventRecord->UserData);
        Args.SubmitSequence = pCompleteEvent->SubmitSequence;
        pmConsumer->HandleDxgkQueueComplete(Args);
    }
}

void HandleDxgkVSyncDPC(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    DxgkSyncDPCEventArgs Args = {};
    Args.pEventHeader = &pEventRecord->EventHeader;
    auto pVSyncDPCEvent = reinterpret_cast<DXGKETW_SCHEDULER_VSYNC_DPC*>(pEventRecord->UserData);
    Args.FlipSubmitSequence = (uint32_t)(pVSyncDPCEvent->FlipFenceId.QuadPart >> 32u);
    pmConsumer->HandleDxgkSyncDPC(Args);
}

void HandleDxgkMMIOFlip(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    DxgkMMIOFlipEventArgs Args = {};
    Args.pEventHeader = &pEventRecord->EventHeader;
    if (pEventRecord->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER)
    {
        auto pMMIOFlipEvent = reinterpret_cast<DXGKETW_SCHEDULER_MMIO_FLIP_32*>(pEventRecord->UserData);
        Args.Flags = static_cast<DxgKrnl_MMIOFlip_Flags>(pMMIOFlipEvent->Flags);
        Args.FlipSubmitSequence = pMMIOFlipEvent->FlipSubmitSequence;
    }
    else
    {
        auto pMMIOFlipEvent = reinterpret_cast<DXGKETW_SCHEDULER_MMIO_FLIP_64*>(pEventRecord->UserData);
        Args.Flags = static_cast<DxgKrnl_MMIOFlip_Flags>(pMMIOFlipEvent->Flags);
        Args.FlipSubmitSequence = pMMIOFlipEvent->FlipSubmitSequence;
    }
    pmConsumer->HandleDxgkMMIOFlip(Args);
}

} // namespace Win7

void HandleWin32kEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    auto const& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) {
    case Microsoft_Windows_Win32k::TokenCompositionSurfaceObject_Info::Id:
    {
        auto eventIter = pmConsumer->FindOrCreatePresent(hdr);

        // Check if we might have retrieved a 'stuck' present from a previous frame.
        if (eventIter->second->SeenWin32KEvents) {
            pmConsumer->mPresentByThreadId.erase(eventIter);
            eventIter = pmConsumer->FindOrCreatePresent(hdr);
        }

        eventIter->second->SetPresentMode(PresentMode::Composed_Flip);
        eventIter->second->SeenWin32KEvents = true;

        PMTraceConsumer::Win32KPresentHistoryTokenKey key(pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"CompositionSurfaceLuid"),
            pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"PresentCount"),
            pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"BindId"));
        pmConsumer->mWin32KPresentHistoryTokens[key] = eventIter->second;
        break;
    }
    case Microsoft_Windows_Win32k::TokenStateChanged_Info::Id:
    {
        PMTraceConsumer::Win32KPresentHistoryTokenKey key(pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"CompositionSurfaceLuid"),
            pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"PresentCount"),
            pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"BindId"));
        auto eventIter = pmConsumer->mWin32KPresentHistoryTokens.find(key);
        if (eventIter == pmConsumer->mWin32KPresentHistoryTokens.end()) {
            return;
        }

        auto &event = *eventIter->second;

        auto state = (Microsoft_Windows_Win32k::TokenState) pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"NewState");
        switch (state) {
        case Microsoft_Windows_Win32k::TokenState::InFrame: // Composition is starting
        {
            if (event.Hwnd) {
                auto hWndIter = pmConsumer->mLastWindowPresent.find(event.Hwnd);
                if (hWndIter == pmConsumer->mLastWindowPresent.end()) {
                    pmConsumer->mLastWindowPresent.emplace(event.Hwnd, eventIter->second);
                } else if (hWndIter->second != eventIter->second) {
                    hWndIter->second->FinalState = PresentResult::Discarded;
                    hWndIter->second = eventIter->second;
                }
            }

            bool iFlip = pmConsumer->mMetadata.GetEventData<BOOL>(pEventRecord, L"IndependentFlip") != 0;
            if (iFlip && event.PresentMode == PresentMode::Composed_Flip) {
                event.SetPresentMode(PresentMode::Hardware_Independent_Flip);
            }
            break;
        }

        case Microsoft_Windows_Win32k::TokenState::Confirmed: // Present has been submitted
            // If we haven't already decided we're going to discard a token,
            // now's a good time to indicate it'll make it to screen
            if (event.FinalState == PresentResult::Unknown) {
                if (event.PresentFlags & DXGI_PRESENT_DO_NOT_SEQUENCE) {
                    // DO_NOT_SEQUENCE presents may get marked as confirmed,
                    // if a frame was composed when this token was completed
                    event.FinalState = PresentResult::Discarded;
                } else {
                    event.FinalState = PresentResult::Presented;
                }
            }
            if (event.Hwnd) {
                pmConsumer->mLastWindowPresent.erase(event.Hwnd);
            }
            break;

        case Microsoft_Windows_Win32k::TokenState::Retired: // Present has been completed, token's buffer is now displayed
            event.ScreenTime = hdr.TimeStamp.QuadPart;
            break;

        case Microsoft_Windows_Win32k::TokenState::Discarded: // Present has been discarded
        {
            auto sharedPtr = eventIter->second;
            pmConsumer->mWin32KPresentHistoryTokens.erase(eventIter);

            if (event.FinalState == PresentResult::Unknown || event.ScreenTime == 0) {
                event.FinalState = PresentResult::Discarded;
            }

            pmConsumer->CompletePresent(sharedPtr);
            break;
        }
        }
        break;
    }
    }
}

void HandleDWMEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    auto const& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) {
    case Microsoft_Windows_Dwm_Core::MILEVENT_MEDIA_UCE_PROCESSPRESENTHISTORY_GetPresentHistory_Info::Id:
        for (auto& hWndPair : pmConsumer->mLastWindowPresent) {
            auto& present = hWndPair.second;
            // Pickup the most recent present from a given window
            if (present->PresentMode != PresentMode::Composed_Copy_GPU_GDI &&
                present->PresentMode != PresentMode::Composed_Copy_CPU_GDI) {
                continue;
            }
            present->SetDwmNotified(true);
            pmConsumer->mPresentsWaitingForDWM.emplace_back(present);
        }
        pmConsumer->mLastWindowPresent.clear();
        break;

    case Microsoft_Windows_Dwm_Core::SCHEDULE_PRESENT_Start::Id:
        pmConsumer->DwmPresentThreadId = hdr.ThreadId;
        break;

    case Microsoft_Windows_Dwm_Core::FlipChain_Pending::Id:
    case Microsoft_Windows_Dwm_Core::FlipChain_Complete::Id:
    case Microsoft_Windows_Dwm_Core::FlipChain_Dirty::Id:
    {
        if (InlineIsEqualGUID(hdr.ProviderId, Microsoft_Windows_Dwm_Core::Win7::GUID)) {
            return;
        }

        auto flipChainId  = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"ulFlipChain");
        auto serialNumber = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"ulSerialNumber");

        // The 64-bit token data from the PHT submission is actually two 32-bit
        // data chunks, corresponding to a "flip chain" id and present id
        auto token = ((uint64_t) flipChainId << 32ull) | serialNumber;
        auto flipIter = pmConsumer->mDxgKrnlPresentHistoryTokens.find(token);
        if (flipIter == pmConsumer->mDxgKrnlPresentHistoryTokens.end()) {
            return;
        }

        // Watch for multiple legacy blits completing against the same window		
        auto hwnd = pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"hwnd");
        pmConsumer->mLastWindowPresent[hwnd] = flipIter->second;
        flipIter->second->SetDwmNotified(true);
        pmConsumer->mPresentsByLegacyBlitToken.erase(flipIter);
        break;
    }
    case Microsoft_Windows_Dwm_Core::SCHEDULE_SURFACEUPDATE_Info::Id:
    {
        PMTraceConsumer::Win32KPresentHistoryTokenKey key(pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"luidSurface"),
                                                          pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"PresentCount"),
                                                          pmConsumer->mMetadata.GetEventData<uint64_t>(pEventRecord, L"bindId"));
        auto eventIter = pmConsumer->mWin32KPresentHistoryTokens.find(key);
        if (eventIter != pmConsumer->mWin32KPresentHistoryTokens.end()) {
            eventIter->second->SetDwmNotified(true);
        }
        break;
    }
    }
}

void HandleD3D9Event(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    DebugEvent(pEventRecord);

    auto const& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) {
    case Microsoft_Windows_D3D9::Present_Start::Id:
    {
        PresentEvent event(hdr, Runtime::D3D9);
        pmConsumer->mMetadata.GetEventData(pEventRecord, L"pSwapchain", &event.SwapChainAddress);
        uint32_t D3D9Flags = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"Flags");
        event.PresentFlags =
            ((D3D9Flags & D3DPRESENT_DONOTFLIP) ? DXGI_PRESENT_DO_NOT_SEQUENCE : 0) |
            ((D3D9Flags & D3DPRESENT_DONOTWAIT) ? DXGI_PRESENT_DO_NOT_WAIT : 0) |
            ((D3D9Flags & D3DPRESENT_FLIPRESTART) ? DXGI_PRESENT_RESTART : 0);
        if ((D3D9Flags & D3DPRESENT_FORCEIMMEDIATE) != 0) {
            event.SyncInterval = 0;
        }

        pmConsumer->CreatePresent(event);
        break;
    }
    case Microsoft_Windows_D3D9::Present_Stop::Id:
    {
        auto result = pmConsumer->mMetadata.GetEventData<uint32_t>(pEventRecord, L"Result");

        bool AllowBatching =
            SUCCEEDED(result) &&
            result != S_PRESENT_OCCLUDED;

        pmConsumer->RuntimePresentStop(hdr, AllowBatching);
        break;
    }
    }
}

void PMTraceConsumer::CompletePresent(std::shared_ptr<PresentEvent> p, uint32_t recurseDepth)
{
    DebugCompletePresent(*p, recurseDepth);

    if (p->Completed)
    {
        p->FinalState = PresentResult::Error;
        return;
    }

    // Complete all other presents that were riding along with this one (i.e. this one came from DWM)
    for (auto& p2 : p->DependentPresents) {
        p2->ScreenTime = p->ScreenTime;
        p2->FinalState = PresentResult::Presented;
        CompletePresent(p2, recurseDepth + 1);
    }
    p->DependentPresents.clear();

    // Remove it from any tracking maps that it may have been inserted into
    if (p->QueueSubmitSequence != 0) {
        mPresentsBySubmitSequence.erase(p->QueueSubmitSequence);
    }
    if (p->Hwnd != 0) {
        auto hWndIter = mLastWindowPresent.find(p->Hwnd);
        if (hWndIter != mLastWindowPresent.end() && hWndIter->second == p) {
            mLastWindowPresent.erase(hWndIter);
        }
    }
    if (p->TokenPtr != 0) {
        auto iter = mDxgKrnlPresentHistoryTokens.find(p->TokenPtr);
        if (iter != mDxgKrnlPresentHistoryTokens.end() && iter->second == p) {
            mDxgKrnlPresentHistoryTokens.erase(iter);
        }
    }
    auto& processMap = mPresentsByProcess[p->ProcessId];
    processMap.erase(p->QpcTime);

    auto& presentDeque = mPresentsByProcessAndSwapChain[std::make_tuple(p->ProcessId, p->SwapChainAddress)];
    auto presentIter = presentDeque.begin();
    assert(!presentIter->get()->Completed); // It wouldn't be here anymore if it was

    if (p->FinalState == PresentResult::Presented) {
        while (*presentIter != p) {
            CompletePresent(*presentIter, recurseDepth + 1);
            presentIter = presentDeque.begin();
        }
    }

    p->Completed = true;
    if (*presentIter == p) {
        auto lock = scoped_lock(mMutex);
        while (presentIter != presentDeque.end() && presentIter->get()->Completed) {
            mCompletedPresents.push_back(*presentIter);
            presentDeque.pop_front();
            presentIter = presentDeque.begin();
        }
    }
}

decltype(PMTraceConsumer::mPresentByThreadId.begin()) PMTraceConsumer::FindOrCreatePresent(EVENT_HEADER const& hdr)
{
    // Easy: we're on a thread that had some step in the present process
    auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
    if (eventIter != mPresentByThreadId.end()) {
        return eventIter;
    }

    // No such luck, check for batched presents
    auto& processMap = mPresentsByProcess[hdr.ProcessId];
    auto processIter = std::find_if(processMap.begin(), processMap.end(),
        [](auto processIter) {return processIter.second->PresentMode == PresentMode::Unknown; });
    if (processIter != processMap.end()) {
        // Assume batched presents are popped off the front of the driver queue by process in order, do the same here
        eventIter = mPresentByThreadId.emplace(hdr.ThreadId, processIter->second).first;
        processMap.erase(processIter);
        return eventIter;
    }

    // This likely didn't originate from a runtime whose events we're tracking (DXGI/D3D9)
    // Could be composition buffers, or maybe another runtime (e.g. GL)
    auto newEvent = std::make_shared<PresentEvent>(hdr, Runtime::Other);
    processMap.emplace(newEvent->QpcTime, newEvent);

    auto& processSwapChainDeque = mPresentsByProcessAndSwapChain[std::make_tuple(hdr.ProcessId, 0ull)];
    processSwapChainDeque.emplace_back(newEvent);

    eventIter = mPresentByThreadId.emplace(hdr.ThreadId, newEvent).first;
    return eventIter;
}

void PMTraceConsumer::CreatePresent(PresentEvent &event)
{
    // Ignore PRESENT_TEST: it's just to check if you're still fullscreen, doesn't actually do anything
    if ((event.PresentFlags & DXGI_PRESENT_TEST) != 0) {
        event.Completed = true;
        return;
    }

    DebugCreatePresent(event);

    auto pEvent = std::make_shared<PresentEvent>(event);
    mPresentByThreadId[event.ThreadId] = pEvent;

    auto& processMap = mPresentsByProcess[event.ProcessId];
    processMap.emplace(event.QpcTime, pEvent);

    auto& processSwapChainDeque = mPresentsByProcessAndSwapChain[std::make_tuple(event.ProcessId, event.SwapChainAddress)];
    processSwapChainDeque.emplace_back(pEvent);

    // Set the caller's local event instance to completed so the assert
    // in ~PresentEvent() doesn't fire when it is destructed.
    event.Completed = true;
}

void PMTraceConsumer::RuntimePresentStop(EVENT_HEADER const& hdr, bool AllowPresentBatching)
{
    auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
    if (eventIter == mPresentByThreadId.end()) {
        return;
    }
    auto &event = *eventIter->second;

    assert(event.QpcTime <= *(uint64_t*) &hdr.TimeStamp);
    event.TimeTaken = *(uint64_t*) &hdr.TimeStamp - event.QpcTime;

    if (!AllowPresentBatching || mSimpleMode) {
        event.FinalState = AllowPresentBatching ? PresentResult::Presented : PresentResult::Discarded;
        CompletePresent(eventIter->second);
    }

    mPresentByThreadId.erase(eventIter);
}

void HandleNTProcessEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    NTProcessEvent event;
    event.QpcTime = pEventRecord->EventHeader.TimeStamp.QuadPart;

    switch (pEventRecord->EventHeader.EventDescriptor.Opcode) {
    case EVENT_TRACE_TYPE_START:
    case EVENT_TRACE_TYPE_DC_START:
        pmConsumer->mMetadata.GetEventData(pEventRecord, L"ProcessId",     &event.ProcessId);
        pmConsumer->mMetadata.GetEventData(pEventRecord, L"ImageFileName", &event.ImageFileName);
        break;

    case EVENT_TRACE_TYPE_END:
    case EVENT_TRACE_TYPE_DC_END:
        pmConsumer->mMetadata.GetEventData(pEventRecord, L"ProcessId", &event.ProcessId);
        break;
    }

    {
        auto lock = scoped_lock(pmConsumer->mNTProcessEventMutex);
        pmConsumer->mNTProcessEvents.emplace_back(event);
    }
}

void HandleMetadataEvent(EVENT_RECORD* pEventRecord, PMTraceConsumer* pmConsumer)
{
    if (pEventRecord->EventHeader.EventDescriptor.Opcode == Microsoft_Windows_EventMetadata::EventInfo::Opcode) {
        auto pTraceEventInfo = reinterpret_cast<TRACE_EVENT_INFO const*>(pEventRecord->UserData);
        if (pTraceEventInfo->DecodingSource == DecodingSourceTlg ||
            pTraceEventInfo->EventDescriptor.Channel == 0xB) {
            // Unlikely to have tracelogging metadata, but don't store it.
            return;
        }

        pmConsumer->mMetadata.InsertMetadata(
            pTraceEventInfo->ProviderGuid,
            pTraceEventInfo->EventDescriptor,
            pTraceEventInfo,
            pEventRecord->UserDataLength);
    }
}

