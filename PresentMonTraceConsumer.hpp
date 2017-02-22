#pragma once

#include "CommonIncludes.hpp"
#include "TraceConsumer.hpp"
#include <dxgi.h>

struct __declspec(uuid("{CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}")) DXGI_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{802ec45a-1e99-4b83-9920-87c98277ba9d}")) DXGKRNL_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{8c416c79-d49b-4f01-a467-e56d3aa8234c}")) WIN32K_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{9e9bba3c-2e38-40cb-99f4-9e8281425164}")) DWM_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{783ACA0A-790E-4d7f-8451-AA850511C6B9}")) D3D9_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{3d6fa8d0-fe05-11d0-9dda-00c04fd7ba7c}")) NT_PROCESS_EVENT_GUID_HOLDER;
static const auto DXGI_PROVIDER_GUID = __uuidof(DXGI_PROVIDER_GUID_HOLDER);
static const auto DXGKRNL_PROVIDER_GUID = __uuidof(DXGKRNL_PROVIDER_GUID_HOLDER);
static const auto WIN32K_PROVIDER_GUID = __uuidof(WIN32K_PROVIDER_GUID_HOLDER);
static const auto DWM_PROVIDER_GUID = __uuidof(DWM_PROVIDER_GUID_HOLDER);
static const auto D3D9_PROVIDER_GUID = __uuidof(D3D9_PROVIDER_GUID_HOLDER);
static const auto NT_PROCESS_EVENT_GUID = __uuidof(NT_PROCESS_EVENT_GUID_HOLDER);

extern bool g_Quit;

template <typename mutex_t> std::unique_lock<mutex_t> scoped_lock(mutex_t &m)
{
    return std::unique_lock<mutex_t>(m);
}

enum class PresentMode
{
    Unknown,
    Hardware_Legacy_Flip,
    Hardware_Legacy_Copy_To_Front_Buffer,
    Hardware_Direct_Flip,
    Hardware_Independent_Flip,
    Composed_Flip,
    Composed_Copy_GPU_GDI,
    Composed_Copy_CPU_GDI,
    Composed_Composition_Atlas,
    Hardware_Composed_Independent_Flip,
};

enum class PresentResult
{
    Unknown, Presented, Discarded
};

enum class Runtime
{
    DXGI, D3D9, Other
};

struct PresentEvent {
    // Available from DXGI Present
    uint64_t QpcTime = 0;
    uint64_t SwapChainAddress = 0;
    int32_t SyncInterval = -1;
    uint32_t PresentFlags = 0;
    uint32_t ProcessId = 0;

    PresentMode PresentMode = PresentMode::Unknown;
    bool SupportsTearing = false;
    bool MMIO = false;
    bool SeenDxgkPresent = false;

    Runtime Runtime = Runtime::Other;

    // Time spent in DXGI Present call
    uint64_t TimeTaken = 0;

    // Timestamp of "ready" state (GPU work completed)
    uint64_t ReadyTime = 0;

    // Timestamp of "complete" state (data on screen or discarded)
    uint64_t ScreenTime = 0;
    PresentResult FinalState = PresentResult::Unknown;
    uint32_t PlaneIndex = 0;

    // Additional transient state
    uint32_t QueueSubmitSequence = 0;
    uint32_t RuntimeThread = 0;
    uint64_t Hwnd = 0;
    std::deque<std::shared_ptr<PresentEvent>> DependentPresents;
    bool Completed = false;
#if _DEBUG
    ~PresentEvent() { assert(Completed || g_Quit); }
#endif
};

struct SwapChainData {
    Runtime mRuntime = Runtime::Other;
    uint64_t mLastUpdateTicks = 0;
    uint32_t mLastSyncInterval = -1;
    uint32_t mLastFlags = -1;
    std::deque<PresentEvent> mPresentHistory;
    std::deque<PresentEvent> mDisplayedPresentHistory;
    PresentMode mLastPresentMode = PresentMode::Unknown;
    uint32_t mLastPlane = 0;
};

struct ProcessInfo {
    uint64_t mLastRefreshTicks = 0; // GetTickCount64
    std::string mModuleName;
    std::map<uint64_t, SwapChainData> mChainMap;
    bool mTerminationProcess;
    bool mProcessExists = false;
};

struct PMTraceConsumer : ITraceConsumer
{
    PMTraceConsumer(bool simple) : mSimpleMode(simple) { }
    bool mSimpleMode;

    std::mutex mMutex;
    // A set of presents that are "completed":
    // They progressed as far as they can through the pipeline before being either discarded or hitting the screen.
    // These will be handed off to the consumer thread.
    std::vector<std::shared_ptr<PresentEvent>> mCompletedPresents;

    // A high-level description of the sequence of events for each present type, ignoring runtime end:
    // Hardware Legacy Flip:
    //   Runtime PresentStart -> Flip (by thread/process, for classification) -> QueueSubmit (by thread, for submit sequence) ->
    //    MMIOFlip (by submit sequence, for ready time and immediate flags) [-> VSyncDPC (by submit sequence, for screen time)]
    // Composed Flip (FLIP_SEQUENTIAL, FLIP_DISCARD, FlipEx),
    //   Runtime PresentStart -> TokenCompositionSurfaceObject (by thread/process, for classification and token key) ->
    //    PresentHistoryDetailed (by thread, for token ptr) -> QueueSubmit (by thread, for submit sequence) ->
    //    PropagatePresentHistory (by token ptr, for ready time) and TokenStateChanged (by token key, for discard status and screen time)
    // Hardware Direct Flip,
    //   N/A, not currently uniquely detectable (follows the same path as composed_flip)
    // Hardware Independent Flip,
    //   Follows composed flip, TokenStateChanged indicates IndependentFlip -> MMIOFlip (by submit sequence, for immediate flags) [->
    //   VSyncDPC (by submit sequence, for screen time)]
    // Hardware Composed Independent Flip,
    //   Identical to IndependentFlip, but MMIOFlipMPO is received instead
    // Composed Copy with GPU GDI (a.k.a. Win7 Blit),
    //   Runtime PresentStart -> Blt (by thread/process, for classification) -> PresentHistoryDetailed (by thread, for token ptr and classification) ->
    //    DxgKrnl Present (by thread, for hWnd) -> PropagatePresentHistory (by token ptr, for ready time) ->
    //    DWM UpdateWindow (by hWnd, marks hWnd active for composition) -> DWM Present (consumes most recent present per hWnd, marks DWM thread ID) ->
    //    A fullscreen present is issued by DWM, and when it completes, this present is on screen
    // Hardware Copy to front buffer,
    //   Runtime PresentStart -> Blt (by thread/process, for classification) -> QueueSubmit (by thread, for submit sequence) ->
    //    QueueComplete (by submit sequence, indicates ready and screen time)
    //    Distinction between FS and windowed blt is done by LACK of other events
    // Composed Copy with CPU GDI (a.k.a. Vista Blit),
    //   Runtime PresentStart -> Blt (by thread/process, for classification) -> SubmitPresentHistory (by thread, for token ptr, legacy blit token, and classification) ->
    //    PropagatePresentHsitory (by token ptr, for ready time) -> DWM FlipChain (by legacy blit token, for hWnd and marks hWnd active for composition) ->
    //    Follows the Windowed_Blit path for tracking to screen
    // Composed Composition Atlas (DirectComposition),
    //   SubmitPresentHistory (use model field for classification, get token ptr) -> PropagatePresentHistory (by token ptr) ->
    //    Assume DWM will compose this buffer on next present (missing InFrame event), follow windowed blit paths to screen time

    // For each process, stores each present started. Used for present batching
    std::map<uint32_t, std::map<uint64_t, std::shared_ptr<PresentEvent>>> mPresentsByProcess;

    // For each (process, swapchain) pair, stores each present started. Used to ensure consumer sees presents targeting the same swapchain in the order they were submitted.
    typedef std::tuple<uint32_t, uint64_t> ProcessAndSwapChainKey;
    std::map<ProcessAndSwapChainKey, std::deque<std::shared_ptr<PresentEvent>>> mPresentsByProcessAndSwapChain;

    // Presents in the process of being submitted
    // The first map contains a single present that is currently in-between a set of expected events on the same thread:
    //   (e.g. DXGI_Present_Start/DXGI_Present_Stop, or Flip/QueueSubmit)
    // Used for mapping from runtime events to future events, and thread map used extensively for correlating kernel events
    std::map<uint32_t, std::shared_ptr<PresentEvent>> mPresentByThreadId;

    // Maps from queue packet submit sequence
    // Used for Flip -> MMIOFlip -> VSyncDPC for FS, for PresentHistoryToken -> MMIOFlip -> VSyncDPC for iFlip,
    // and for Blit Submission -> Blit completion for FS Blit
    std::map<uint32_t, std::shared_ptr<PresentEvent>> mPresentsBySubmitSequence;

    // Win32K present history tokens are uniquely identified by (composition surface pointer, present count, bind id)
    // Using a tuple instead of named struct simply to have auto-generated comparison operators
    // These tokens are used for "flip model" presents (windowed flip, dFlip, iFlip) only
    typedef std::tuple<uint64_t, uint64_t, uint32_t> Win32KPresentHistoryTokenKey;
    std::map<Win32KPresentHistoryTokenKey, std::shared_ptr<PresentEvent>> mWin32KPresentHistoryTokens;

    // DxgKrnl present history tokens are uniquely identified by a single pointer
    // These are used for all types of windowed presents to track a "ready" time
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mDxgKrnlPresentHistoryTokens;

    // Present by window, used for determining superceding presents
    // For windowed blit presents, when DWM issues a present event, we choose the most recent event as the one that will make it to screen
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mPresentByWindow;

    // Presents that will be completed by DWM's next present
    std::deque<std::shared_ptr<PresentEvent>> mPresentsWaitingForDWM;
    // Used to understand that a flip event is coming from the DWM
    uint32_t DwmPresentThreadId = 0;

    // Windows that will be composed the next time DWM presents
    // Generated by DWM events indicating it's received tokens targeting a given hWnd
    std::set<uint32_t> mWindowsBeingComposed;

    // Yet another unique way of tracking present history tokens, this time from DxgKrnl -> DWM, only for legacy blit
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mPresentsByLegacyBlitToken;

    std::mutex mProcessMutex;
    std::map<uint32_t, ProcessInfo> mNewProcessesFromETW;
    std::vector<uint32_t> mDeadProcessIds;

    bool DequeuePresents(std::vector<std::shared_ptr<PresentEvent>>& outPresents)
    {
        if (mCompletedPresents.size())
        {
            auto lock = scoped_lock(mMutex);
            outPresents.swap(mCompletedPresents);
            return !outPresents.empty();
        }
        return false;
    }

    void GetProcessEvents(decltype(mNewProcessesFromETW)& outNewProcesses, decltype(mDeadProcessIds)& outDeadProcesses)
    {
        auto lock = scoped_lock(mProcessMutex);
        outNewProcesses.swap(mNewProcessesFromETW);
        outDeadProcesses.swap(mDeadProcessIds);
    }

    virtual void OnEventRecord(_In_ PEVENT_RECORD pEventRecord);
    virtual bool ContinueProcessing() { return !g_Quit; }

private:
    void CompletePresent(std::shared_ptr<PresentEvent> p)
    {
        assert(!p->Completed);

        // Complete all other presents that were riding along with this one (i.e. this one came from DWM)
        for (auto& p2 : p->DependentPresents) {
            p2->ScreenTime = p->ScreenTime;
            p2->FinalState = PresentResult::Presented;
            CompletePresent(p2);
        }
        p->DependentPresents.clear();

        // Remove it from any tracking maps that it may have been inserted into
        if (p->QueueSubmitSequence != 0) {
            mPresentsBySubmitSequence.erase(p->QueueSubmitSequence);
        }
        if (p->Hwnd != 0) {
            auto hWndIter = mPresentByWindow.find(p->Hwnd);
            if (hWndIter != mPresentByWindow.end() && hWndIter->second == p) {
                mPresentByWindow.erase(hWndIter);
            }
        }
        auto& processMap = mPresentsByProcess[p->ProcessId];
        processMap.erase(p->QpcTime);

        auto& presentDeque = mPresentsByProcessAndSwapChain[std::make_tuple(p->ProcessId, p->SwapChainAddress)];
        auto presentIter = presentDeque.begin();
        assert(!presentIter->get()->Completed); // It wouldn't be here anymore if it was

        if (p->FinalState == PresentResult::Presented) {
            while (*presentIter != p) {
                CompletePresent(*presentIter);
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

    decltype(mPresentByThreadId.begin()) FindOrCreatePresent(_In_ PEVENT_RECORD pEventRecord)
    {
        // Easy: we're on a thread that had some step in the present process
        auto eventIter = mPresentByThreadId.find(pEventRecord->EventHeader.ThreadId);
        if (eventIter != mPresentByThreadId.end()) {
            return eventIter;
        }

        // No such luck, check for batched presents
        auto& processMap = mPresentsByProcess[pEventRecord->EventHeader.ProcessId];
        auto processIter = std::find_if(processMap.begin(), processMap.end(),
            [](auto processIter) {return processIter.second->PresentMode == PresentMode::Unknown; });
        uint64_t EventTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
        if (processIter == processMap.end()) {
            // This likely didn't originate from a runtime whose events we're tracking (DXGI/D3D9)
            // Could be composition buffers, or maybe another runtime (e.g. GL)
            auto newEvent = std::make_shared<PresentEvent>();
            newEvent->QpcTime = EventTime;
            newEvent->ProcessId = pEventRecord->EventHeader.ProcessId;
            processMap.emplace(EventTime, newEvent);

            auto& processSwapChainDeque = mPresentsByProcessAndSwapChain[std::make_tuple(pEventRecord->EventHeader.ProcessId, 0ull)];
            processSwapChainDeque.emplace_back(newEvent);

            eventIter = mPresentByThreadId.emplace(pEventRecord->EventHeader.ThreadId, newEvent).first;
        }
        else {
            // Assume batched presents are popped off the front of the driver queue by process in order, do the same here
            eventIter = mPresentByThreadId.emplace(pEventRecord->EventHeader.ThreadId, processIter->second).first;
            processMap.erase(processIter);
        }

        return eventIter;
    }

    void RuntimePresentStart(_In_ PEVENT_RECORD pEventRecord, PresentEvent &event)
    {
        auto& hdr = pEventRecord->EventHeader;
        uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;
        event.ProcessId = hdr.ProcessId;
        event.QpcTime = EventTime;
        event.RuntimeThread = hdr.ThreadId;

        // Ignore PRESENT_TEST: it's just to check if you're still fullscreen, doesn't actually do anything
        if ((event.PresentFlags & DXGI_PRESENT_TEST) == 0) {
            auto pEvent = std::make_shared<PresentEvent>(event);
            mPresentByThreadId[hdr.ThreadId] = pEvent;

            auto& processMap = mPresentsByProcess[hdr.ProcessId];
            processMap.emplace(EventTime, pEvent);

            auto& processSwapChainDeque = mPresentsByProcessAndSwapChain[std::make_tuple(hdr.ProcessId, event.SwapChainAddress)];
            processSwapChainDeque.emplace_back(pEvent);
        }
    }

    void RuntimePresentStop(_In_ PEVENT_RECORD pEventRecord, bool AllowPresentBatching = true)
    {
        auto& hdr = pEventRecord->EventHeader;
        uint64_t EndTime = *(uint64_t*)&hdr.TimeStamp;
        auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
        if (eventIter == mPresentByThreadId.end()) {
            return;
        }
        auto &event = *eventIter->second;

        assert(event.QpcTime < EndTime);
        event.TimeTaken = EndTime - event.QpcTime;

        if (!AllowPresentBatching || mSimpleMode) {
            event.FinalState = AllowPresentBatching ? PresentResult::Presented : PresentResult::Discarded;
            CompletePresent(eventIter->second);
        }
        mPresentByThreadId.erase(eventIter);
    }

    void OnDXGIEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnDXGKrnlEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnWin32kEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnDWMEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnD3D9Event(_In_ PEVENT_RECORD pEventRecord);
    void OnNTProcessEvent(_In_ PEVENT_RECORD pEventRecord);
};