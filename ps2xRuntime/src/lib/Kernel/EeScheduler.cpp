#include "runtime/ee_scheduler.h"
#include "runtime/runtime_profile.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    bool xmenDiagnosticsEnabled()
    {
        static const bool enabled = std::getenv("PS2X_XMEN_DIAGNOSTICS") != nullptr;
        return enabled;
    }

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    uint32_t g_xmenMovieWorkerExitCount = 0u;
    bool g_xmenTraceFinalMovieHandoff = false;

    uint32_t readXmenWord(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0u;
        std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
        return value;
    }

    uint64_t readXmenDoubleword(const uint8_t *rdram, uint32_t address)
    {
        uint64_t value = 0u;
        std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
        return value;
    }

    bool eeInterruptsEnabled(const R5900Context &context)
    {
        constexpr uint32_t kStatusIe = 1u << 0u;
        constexpr uint32_t kStatusExl = 1u << 1u;
        constexpr uint32_t kStatusErl = 1u << 2u;
        constexpr uint32_t kStatusEie = 1u << 16u;
        const uint32_t status = context.cop0_status;
        return (status & (kStatusIe | kStatusEie)) == (kStatusIe | kStatusEie) &&
               (status & (kStatusExl | kStatusErl)) == 0u;
    }

    int xmenWaitObjectId(const GuestThread *thread)
    {
        if (!thread)
        {
            return 0;
        }
        if (const auto *semaphore = std::get_if<EeSemaphoreWait>(&thread->wait.payload))
        {
            return semaphore->id;
        }
        if (const auto *eventFlag = std::get_if<EeEventFlagWait>(&thread->wait.payload))
        {
            return eventFlag->id;
        }
        return 0;
    }

    void traceXmenMovieHandoff(const uint8_t *rdram,
                               const char *reason,
                               uint64_t index,
                               uint64_t tick,
                               uint64_t cycle,
                               const GuestThread *mainThread)
    {
        const R5900Context *context = mainThread ? &mainThread->activeContext() : nullptr;
        std::fprintf(stdout,
                     "[xmen-movie-handoff] reason=%s index=%llu tick=%llu cycle=%llu mainStatus=%u wait=%u waitId=%d invocations=%zu pc=0x%x ra=0x%x basePc=0x%x baseRa=0x%x stream0=0x%x streamC=0x%x stream20=0x%x stream24=0x%x stream28=0x%x stream34=0x%x stream58=0x%x managerRef=0x%x initialized=0x%x manager10=0x%x manager48=0x%x manager4C=0x%x manager54=0x%x manager60=0x%x manager64=0x%x manager68=0x%x callbackBusy=0x%x callback=0x%x wrapper0=0x%x wrapper14=0x%x wrapper20=0x%x\n",
                     reason,
                     static_cast<unsigned long long>(index),
                     static_cast<unsigned long long>(tick),
                     static_cast<unsigned long long>(cycle),
                     mainThread ? static_cast<unsigned>(mainThread->status) : 0u,
                     mainThread ? static_cast<unsigned>(mainThread->wait.reason) : 0u,
                     xmenWaitObjectId(mainThread),
                     mainThread ? mainThread->invocations.size() : 0u,
                     context ? context->pc : 0u,
                     context ? getRegU32(context, 31) : 0u,
                     mainThread ? mainThread->context.pc : 0u,
                     mainThread ? getRegU32(&mainThread->context, 31) : 0u,
                     readXmenWord(rdram, 0x006787F8u),
                     readXmenWord(rdram, 0x00678804u),
                     readXmenWord(rdram, 0x00678818u),
                     readXmenWord(rdram, 0x0067881Cu),
                     readXmenWord(rdram, 0x00678820u),
                     readXmenWord(rdram, 0x0067882Cu),
                     readXmenWord(rdram, 0x00678850u),
                     readXmenWord(rdram, 0x0066DCF4u),
                     readXmenWord(rdram, 0x00666D7Cu),
                     readXmenWord(rdram, 0x00666D90u),
                     readXmenWord(rdram, 0x00666DC8u),
                     readXmenWord(rdram, 0x00666DCCu),
                     readXmenWord(rdram, 0x00666DD4u),
                     readXmenWord(rdram, 0x00666DE0u),
                     readXmenWord(rdram, 0x00666DE4u),
                     readXmenWord(rdram, 0x00666DE8u),
                     readXmenWord(rdram, 0x0066DE0Cu),
                     readXmenWord(rdram, 0x0066DE10u),
                     readXmenWord(rdram, 0x0066A168u),
                     readXmenWord(rdram, 0x0066A17Cu),
                     readXmenWord(rdram, 0x0066A188u));
        std::fflush(stdout);
    }

    uint32_t xmenMovieStreamState(const uint8_t *rdram)
    {
        uint32_t value = 0u;
        std::memcpy(&value, rdram + (0x006787F8u & PS2_RAM_MASK), sizeof(value));
        return value;
    }

    bool xmenMoviePlaybackStarted(const uint8_t *rdram)
    {
        return ((xmenMovieStreamState(rdram) >> 8u) & 0xFFu) >= 2u;
    }

    bool shouldTraceXmenMovieThread()
    {
        static uint64_t count = 0u;
        const uint64_t index = count++;
        return index < 128u || (index != 0u && (index & (index - 1u)) == 0u);
    }

    bool isXmenMovieThreadEntry(uint32_t entry)
    {
        return entry == 0x00579108u || entry == 0x005791F8u ||
               entry == 0x005792D8u || entry == 0x005793B8u;
    }

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    for (const auto &[key, top] : m_invocationStackTops)
    {
        (void)key;
        m_freeInvocationStackTops.push_back(top);
    }
    m_invocationStackTops.clear();
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    g_xmenMovieWorkerExitCount = 0u;
    g_xmenTraceFinalMovieHandoff = false;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);
    uint32_t lastGuestDispatchPc = 0u;
    uint32_t lastGuestDispatchRa = 0u;
    uint32_t lastGuestDispatchSp = 0u;

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        RuntimeProfile::Scope schedulerProfile(RuntimeProfile::Phase::Scheduler);
        if (RuntimeProfile::enabled())
            RuntimeProfile::state.tick = m_vsyncTick;
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                if (xmenDiagnosticsEnabled() &&
                    invocation.kind == GuestInvocationKind::Interrupt &&
                    getRegU32(&invocation.context, 4) == 1u)
                {
                    std::fprintf(stderr,
                                 "[xmen-vif1-irq-promote-idle] sequence=%llu handler=0x%x owner=%d pending=%zu cycle=%llu\n",
                                 static_cast<unsigned long long>(invocation.sequence),
                                 invocation.context.pc,
                                 owner->id,
                                 m_pendingInvocations.size(),
                                 static_cast<unsigned long long>(m_eeCycle));
                }
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (xmenDiagnosticsEnabled() &&
            (context.pc == 0x002DE3D0u ||
             context.pc == 0x002DE524u ||
             context.pc == 0x002DE52Cu))
        {
            const uint32_t queue = readXmenWord(m_rdram, 0x007507D4u);
            const uint64_t sequence = running->invocations.empty()
                                          ? 0u
                                          : running->invocations.back().sequence;
            std::fprintf(stderr,
                         "[xmen-vif1-handler-state] pc=0x%x sequence=%llu thread=%d depth=%zu "
                         "status=0x%x queue=0x%x head0=0x%llx head8=0x%llx next=0x%x "
                         "flags=0x%x completed=0x%x submitted=0x%x cycle=%llu\n",
                         context.pc,
                         static_cast<unsigned long long>(sequence),
                         running->id,
                         running->invocations.size(),
                         context.cop0_status,
                         queue,
                         static_cast<unsigned long long>(queue ? readXmenDoubleword(m_rdram, queue) : 0u),
                         static_cast<unsigned long long>(queue ? readXmenDoubleword(m_rdram, queue + 8u) : 0u),
                         queue ? readXmenWord(m_rdram, queue + 0x34u) : 0u,
                         queue ? readXmenWord(m_rdram, queue + 0x40u) : 0u,
                         readXmenWord(m_rdram, 0x007537E0u),
                         readXmenWord(m_rdram, 0x007537E8u),
                         static_cast<unsigned long long>(m_eeCycle));
        }
        if (xmenDiagnosticsEnabled() && context.pc == 0x002DE550u)
        {
            static uint32_t gifHandlerEntryTraceCount = 0u;
            const uint32_t queueHead = readXmenWord(m_rdram, 0x007507D8u);
            const uint64_t issued = readXmenDoubleword(m_rdram, 0x00750810u);
            const uint64_t completed = readXmenDoubleword(m_rdram, 0x00750860u);
            const uint64_t sequence = running->invocations.empty()
                                          ? 0u
                                          : running->invocations.back().sequence;
            if (gifHandlerEntryTraceCount < 256u || issued >= 0x4Cu || completed >= 0x4Cu)
            {
                std::fprintf(stderr,
                             "[xmen-gif-irq-entry] index=%u sequence=%llu thread=%d depth=%zu "
                             "pending=%zu head=0x%x head0=0x%llx head8=0x%llx next=0x%x "
                             "flags=0x%x issued=0x%llx completed=0x%llx cycle=%llu\n",
                             gifHandlerEntryTraceCount,
                             static_cast<unsigned long long>(sequence),
                             running->id,
                             running->invocations.size(),
                             m_pendingInvocations.size(),
                             queueHead,
                             static_cast<unsigned long long>(queueHead ? readXmenDoubleword(m_rdram, queueHead) : 0u),
                             static_cast<unsigned long long>(queueHead ? readXmenDoubleword(m_rdram, queueHead + 8u) : 0u),
                             queueHead ? readXmenWord(m_rdram, queueHead + 0x34u) : 0u,
                             queueHead ? readXmenWord(m_rdram, queueHead + 0x40u) : 0u,
                             static_cast<unsigned long long>(issued),
                             static_cast<unsigned long long>(completed),
                             static_cast<unsigned long long>(m_eeCycle));
            }
            ++gifHandlerEntryTraceCount;
        }
        if (xmenDiagnosticsEnabled())
        {
            const uint32_t movieStreamState = xmenMovieStreamState(m_rdram);
            if (((movieStreamState >> 8u) & 0xFFu) >= 2u)
            {
                static uint32_t xmenMovieDispatchTraceCount = 0u;
                if (xmenMovieDispatchTraceCount++ < 128u)
                {
                    std::fprintf(stderr,
                                 "[xmen-movie-dispatch] index=%u thread=%d invocations=%zu pc=0x%x ra=0x%x sp=0x%x basePc=0x%x baseRa=0x%x stream0=0x%x cycle=%llu\n",
                                 xmenMovieDispatchTraceCount - 1u,
                                 running->id,
                                 running->invocations.size(),
                                 context.pc,
                                 getRegU32(&context, 31),
                                 getRegU32(&context, 29),
                                 running->context.pc,
                                 getRegU32(&running->context, 31),
                                 movieStreamState,
                                 static_cast<unsigned long long>(m_eeCycle));
                }
            }
        }
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
        m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
        m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
        m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                const size_t completedDepth = running->invocations.size() - 1u;
                GuestInvocation completed = std::move(running->invocations.back());
                releaseInvocationStack(running->id, completedDepth);
                running->invocations.pop_back();
                static uint32_t invocationCompleteTraceCount = 0u;
                if (xmenDiagnosticsEnabled() && invocationCompleteTraceCount++ < 128u)
                {
                    std::fprintf(stderr,
                                 "[ee-invocation:complete] thread=%d kind=%u sequence=%llu remaining=%zu resumePc=0x%x resumeRa=0x%x\n",
                                 running->id,
                                 static_cast<unsigned>(completed.kind),
                                 static_cast<unsigned long long>(completed.sequence),
                                 running->invocations.size(),
                                 running->activeContext().pc,
                                 getRegU32(&running->activeContext(), 31));
                }
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            if (running->id >= 0)
            {
                std::fprintf(stderr,
                             "[ee-thread:main-zero] id=%d entry=0x%x lastPc=0x%x lastRa=0x%x lastSp=0x%x ra=0x%x sp=0x%x cycle=%llu\n",
                             running->id,
                             running->entry,
                             lastGuestDispatchPc,
                             lastGuestDispatchRa,
                             lastGuestDispatchSp,
                             getRegU32(&running->context, 31),
                             getRegU32(&running->context, 29),
                             static_cast<unsigned long long>(m_eeCycle));
            }
            static uint32_t dormantTraceCount = 0u;
            if (xmenDiagnosticsEnabled() && dormantTraceCount++ < 128u)
            {
                std::fprintf(stderr,
                             "[ee-thread:dormant] id=%d entry=0x%x ra=0x%x sp=0x%x cycle=%llu\n",
                             running->id,
                             running->entry,
                             getRegU32(&running->context, 31),
                             getRegU32(&running->context, 29),
                             static_cast<unsigned long long>(m_eeCycle));
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            continue;
        }

        if (!m_pendingInvocations.empty())
        {
            const GuestInvocation &pending = m_pendingInvocations.front();
            if (pending.kind == GuestInvocationKind::Interrupt && !eeInterruptsEnabled(context))
            {
                if (xmenDiagnosticsEnabled() && getRegU32(&pending.context, 4) == 1u)
                {
                    static uint32_t xmenVif1BlockedTraceCount = 0u;
                    if (xmenVif1BlockedTraceCount++ < 128u)
                    {
                        const GuestInvocation *active = running->invocations.empty()
                                                            ? nullptr
                                                            : &running->invocations.back();
                        std::fprintf(stderr,
                                     "[xmen-vif1-irq-blocked] sequence=%llu handler=0x%x current=%d "
                                     "pc=0x%x status=0x%x depth=%zu activeKind=%u activeSequence=%llu "
                                     "pending=%zu cycle=%llu\n",
                                     static_cast<unsigned long long>(pending.sequence),
                                     pending.context.pc,
                                     running->id,
                                     context.pc,
                                     context.cop0_status,
                                     running->invocations.size(),
                                     active ? static_cast<unsigned>(active->kind) : 0u,
                                     active ? static_cast<unsigned long long>(active->sequence) : 0ull,
                                     m_pendingInvocations.size(),
                                     static_cast<unsigned long long>(m_eeCycle));
                    }
                }
                if (xmenDiagnosticsEnabled() && pending.context.pc == 0x002E63E0u &&
                    (m_runtime.memory().gs().csr.load(std::memory_order_relaxed) & 0x2u) != 0u)
                {
                    static uint32_t xmenFinishBlockedTraceCount = 0u;
                    if (xmenFinishBlockedTraceCount++ < 64u)
                    {
                        std::fprintf(stderr,
                                     "[xmen-gs-finish-blocked] current=%d pc=0x%x status=0x%x depth=%zu pending=%zu cycle=%llu\n",
                                     running->id,
                                     context.pc,
                                     context.cop0_status,
                                     running->invocations.size(),
                                     m_pendingInvocations.size(),
                                     static_cast<unsigned long long>(m_eeCycle));
                    }
                }
                // Hardware IRQs remain pending while DI, EXL, or ERL masks them.
                // The active guest context must run through EI/ERET before they
                // can preempt it.
            }
            else
            {
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                if (xmenDiagnosticsEnabled() &&
                    invocation.kind == GuestInvocationKind::Interrupt &&
                    getRegU32(&invocation.context, 4) == 1u)
                {
                    std::fprintf(stderr,
                                 "[xmen-vif1-irq-preempt] sequence=%llu handler=0x%x current=%d "
                                 "pc=0x%x status=0x%x depth=%zu pending=%zu cycle=%llu\n",
                                 static_cast<unsigned long long>(invocation.sequence),
                                 invocation.context.pc,
                                 running->id,
                                 context.pc,
                                 context.cop0_status,
                                 running->invocations.size(),
                                 m_pendingInvocations.size(),
                                 static_cast<unsigned long long>(m_eeCycle));
                }
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                running->invocations.push_back(std::move(invocation));
                continue;
            }
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                if (running->id >= 0)
                {
                    std::fprintf(stderr,
                                 "[ee-thread:missing-pc] id=%d pc=0x%x lastPc=0x%x lastRa=0x%x lastSp=0x%x ra=0x%x sp=0x%x a0=0x%x v0=0x%x cycle=%llu\n",
                                 running->id,
                                 context.pc,
                                 lastGuestDispatchPc,
                                 lastGuestDispatchRa,
                                 lastGuestDispatchSp,
                                 getRegU32(&context, 31),
                                 getRegU32(&context, 29),
                                 getRegU32(&context, 4),
                                 getRegU32(&context, 2),
                                 static_cast<unsigned long long>(m_eeCycle));
                }
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        if (context.pc == 0x003207D0u || context.pc == 0x003208E0u ||
            context.pc == 0x00320A44u || context.pc == 0x00320BF8u)
        {
            const uint32_t sp = getRegU32(&context, 29);
            const auto read32 = [&](uint32_t address)
            {
                uint32_t value = 0u;
                std::memcpy(&value, m_rdram + (address & PS2_RAM_MASK), sizeof(value));
                return value;
            };
            const uint32_t result = getRegU32(&context, 2);
            const uint32_t out = read32(sp + 0x1ACu);
            std::fprintf(stderr,
                         "[xmen-legal-factory:resume] pc=0x%x result=0x%x resultVtable=0x%x result8=0x%x out=0x%x outVtable=0x%x s0=0x%x s2=0x%x s4=0x%x s5=0x%x\n",
                         context.pc,
                         result,
                         read32(result),
                         read32(result + 8u),
                         out,
                         read32(out),
                         getRegU32(&context, 16),
                         getRegU32(&context, 18),
                         getRegU32(&context, 20),
                         getRegU32(&context, 21));
        }
        if (xmenDiagnosticsEnabled() &&
            (context.pc == 0x00325AE0u || context.pc == 0x00325840u ||
             context.pc == 0x00325890u || context.pc == 0x003258F8u ||
             context.pc == 0x00158300u))
        {
            static std::atomic<uint32_t> s_xmenLayerResumeTraceCount{0u};
            const uint32_t count = s_xmenLayerResumeTraceCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 256u)
            {
                std::fprintf(stderr,
                             "[xmen-layer-resume] index=%u pc=0x%x ra=0x%x sp=0x%x a0=0x%x a1=0x%x a2=0x%x s0=0x%x s4=0x%x cycle=%llu\n",
                             count,
                             context.pc,
                             getRegU32(&context, 31),
                             getRegU32(&context, 29),
                             getRegU32(&context, 4),
                             getRegU32(&context, 5),
                             getRegU32(&context, 6),
                             getRegU32(&context, 16),
                             getRegU32(&context, 20),
                             static_cast<unsigned long long>(m_eeCycle));
            }
        }
        const uint32_t xmenFastReturnDispatchPc = context.pc;
        static const bool traceXmenFastReturnEnabled =
            std::getenv("PS2X_TRACE_XMEN_FAST_RETURN") != nullptr;
        const bool traceXmenFastReturnDispatch =
            traceXmenFastReturnEnabled &&
            (xmenFastReturnDispatchPc == 0x00420360u ||
             xmenFastReturnDispatchPc == 0x003DF434u);
        if (traceXmenFastReturnDispatch)
        {
            std::fprintf(stderr,
                         "[xmen-fast-return] phase=scheduler-before thread=%d depth=%zu "
                         "pc=0x%x ra=0x%x cycle=%llu checkpoint=%d reschedule=%d\n",
                         running->id,
                         running->invocations.size(),
                         context.pc,
                         getRegU32(&context, 31),
                         static_cast<unsigned long long>(m_eeCycle),
                         m_checkpointPending.load(std::memory_order_acquire) ? 1 : 0,
                         m_rescheduleRequested ? 1 : 0);
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        const bool xmenPollingWorker = running->entry == 0x00579108u;
        static uint32_t xmenPollingDispatchTraceCount = 0u;
        const bool traceXmenPollingDispatch = xmenPollingWorker && xmenPollingDispatchTraceCount < 64u;
        if (traceXmenPollingDispatch)
        {
            std::fprintf(stderr,
                         "[xmen-poll-worker:before] index=%u id=%d pc=0x%x ra=0x%x priority=%d cycle=%llu checkpoint=%d reschedule=%d\n",
                         xmenPollingDispatchTraceCount,
                         running->id,
                         context.pc,
                         getRegU32(&context, 31),
                         running->currentPriority,
                         static_cast<unsigned long long>(m_eeCycle),
                         m_checkpointPending.load(std::memory_order_acquire) ? 1 : 0,
                         m_rescheduleRequested ? 1 : 0);
        }

        if (checkpointDue(kGuestDispatchCycles))
        {
            if (traceXmenFastReturnDispatch)
            {
                std::fprintf(stderr,
                             "[xmen-fast-return] phase=scheduler-checkpoint thread=%d "
                             "pc=0x%x cycle=%llu checkpoint=%d reschedule=%d\n",
                             running->id,
                             context.pc,
                             static_cast<unsigned long long>(m_eeCycle),
                             m_checkpointPending.load(std::memory_order_acquire) ? 1 : 0,
                             m_rescheduleRequested ? 1 : 0);
            }
            if (traceXmenPollingDispatch)
            {
                std::fprintf(stderr,
                             "[xmen-poll-worker:checkpoint] index=%u id=%d pc=0x%x cycle=%llu checkpoint=%d reschedule=%d\n",
                             xmenPollingDispatchTraceCount++,
                             running->id,
                             context.pc,
                             static_cast<unsigned long long>(m_eeCycle),
                             m_checkpointPending.load(std::memory_order_acquire) ? 1 : 0,
                             m_rescheduleRequested ? 1 : 0);
            }
            continue;
        }

        try
        {
            const uint32_t xmenChainBeforePc = context.pc;
            const bool traceXmenChainDispatch =
                xmenChainBeforePc >= 0x002E6C60u && xmenChainBeforePc < 0x002E6E20u;
            uint32_t xmenChainTraceIndex = 0u;
            if (traceXmenChainDispatch)
            {
                static std::atomic<uint32_t> s_xmenChainSchedulerTraceCount{0u};
                xmenChainTraceIndex =
                    s_xmenChainSchedulerTraceCount.fetch_add(1u, std::memory_order_relaxed);
                if (xmenChainTraceIndex < 8192u)
                {
                    const uint32_t sp = getRegU32(&context, 29);
                    std::fprintf(stderr,
                                 "[xmen-chain-scheduler:before] index=%u thread=%d depth=%zu pc=0x%x ra=0x%x sp=0x%x slot=0x%x slotValue=0x%llx cycle=%llu checkpoint=%d reschedule=%d\n",
                                 xmenChainTraceIndex,
                                 running->id,
                                 running->invocations.size(),
                                 context.pc,
                                 getRegU32(&context, 31),
                                 sp,
                                 sp + 0x50u,
                                 static_cast<unsigned long long>(readXmenDoubleword(m_rdram, sp + 0x50u)),
                                 static_cast<unsigned long long>(m_eeCycle),
                                 m_checkpointPending.load(std::memory_order_acquire) ? 1 : 0,
                                 m_rescheduleRequested ? 1 : 0);
                }
            }
            if (running->id >= 0)
            {
                lastGuestDispatchPc = context.pc;
                lastGuestDispatchRa = getRegU32(&context, 31);
                lastGuestDispatchSp = getRegU32(&context, 29);
            }
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            {
                RuntimeProfile::Scope guestProfile(RuntimeProfile::Phase::Guest);
                function(m_rdram, &context, &m_runtime);
            }
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
            if (traceXmenFastReturnDispatch)
            {
                std::fprintf(stderr,
                             "[xmen-fast-return] phase=scheduler-after thread=%d depth=%zu "
                             "beforePc=0x%x pc=0x%x ra=0x%x cycle=%llu checkpoint=%d "
                             "reschedule=%d\n",
                             running->id,
                             running->invocations.size(),
                             xmenFastReturnDispatchPc,
                             context.pc,
                             getRegU32(&context, 31),
                             static_cast<unsigned long long>(m_eeCycle),
                             m_checkpointPending.load(std::memory_order_acquire) ? 1 : 0,
                             m_rescheduleRequested ? 1 : 0);
            }
            if (traceXmenChainDispatch && xmenChainTraceIndex < 8192u)
            {
                std::fprintf(stderr,
                             "[xmen-chain-scheduler:after] index=%u thread=%d depth=%zu beforePc=0x%x pc=0x%x ra=0x%x sp=0x%x cycle=%llu checkpoint=%d reschedule=%d\n",
                             xmenChainTraceIndex,
                             running->id,
                             running->invocations.size(),
                             xmenChainBeforePc,
                             context.pc,
                             getRegU32(&context, 31),
                             getRegU32(&context, 29),
                             static_cast<unsigned long long>(m_eeCycle),
                             m_checkpointPending.load(std::memory_order_acquire) ? 1 : 0,
                             m_rescheduleRequested ? 1 : 0);
            }
            if (traceXmenPollingDispatch)
            {
                std::fprintf(stderr,
                             "[xmen-poll-worker:after] index=%u id=%d pc=0x%x ra=0x%x cycle=%llu checkpoint=%d reschedule=%d\n",
                             xmenPollingDispatchTraceCount++,
                             running->id,
                             context.pc,
                             getRegU32(&context, 31),
                             static_cast<unsigned long long>(m_eeCycle),
                             m_checkpointPending.load(std::memory_order_acquire) ? 1 : 0,
                             m_rescheduleRequested ? 1 : 0);
            }
        }
        catch (const EeDispatcherTransfer &)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        return true;
    }

    const GuestThread *running = currentThread();
    if (m_rescheduleRequested && running != nullptr && eeInterruptsEnabled(running->activeContext()))
    {
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        if (!eeInterruptsEnabled(running->activeContext()))
        {
            return false;
        }
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < 0 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    if (isXmenMovieThreadEntry(params.entry))
    {
        std::fprintf(stdout,
                     "[xmen-movie-lifecycle] action=create id=%d entry=0x%x priority=%d caller=%d cycle=%llu\n",
                     id,
                     params.entry,
                     params.priority,
                     m_currentThreadId,
                     static_cast<unsigned long long>(m_eeCycle));
        std::fflush(stdout);
    }
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    if (isXmenMovieThreadEntry(it->second.entry))
    {
        const GuestThread *caller = currentThread();
        std::fprintf(stdout,
                     "[xmen-movie-lifecycle] action=delete id=%d entry=0x%x caller=%d callerPc=0x%x callerRa=0x%x cycle=%llu\n",
                     id,
                     it->second.entry,
                     caller ? caller->id : 0,
                     caller ? caller->activeContext().pc : 0u,
                     caller ? getRegU32(&caller->activeContext(), 31) : 0u,
                     static_cast<unsigned long long>(m_eeCycle));
        std::fflush(stdout);
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    if (isXmenMovieThreadEntry(target->entry))
    {
        std::fprintf(stdout,
                     "[xmen-movie-lifecycle] action=start id=%d entry=0x%x arg=0x%x caller=%d callerPc=0x%x callerRa=0x%x cycle=%llu\n",
                     id,
                     target->entry,
                     arg,
                     m_currentThreadId,
                     caller.pc,
                     getRegU32(&caller, 31),
                     static_cast<unsigned long long>(m_eeCycle));
        std::fflush(stdout);
    }
    enqueueReady(*target);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    std::fprintf(stderr,
                 "[ee-thread:exit-current] id=%d delete=%d pc=0x%x ra=0x%x sp=0x%x invocations=%zu cycle=%llu\n",
                 exiting->id,
                 deleteThreadRecord ? 1 : 0,
                 exiting->activeContext().pc,
                 getRegU32(&exiting->activeContext(), 31),
                 getRegU32(&exiting->activeContext(), 29),
                 exiting->invocations.size(),
                 static_cast<unsigned long long>(m_eeCycle));
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    if (isXmenMovieThreadEntry(exiting->entry))
    {
        ++g_xmenMovieWorkerExitCount;
        std::fprintf(stdout,
                     "[xmen-movie-lifecycle] action=exit id=%d entry=0x%x delete=%d pc=0x%x ra=0x%x cycle=%llu\n",
                     id,
                     exiting->entry,
                     deleteThreadRecord ? 1 : 0,
                     exiting->activeContext().pc,
                     getRegU32(&exiting->activeContext(), 31),
                     static_cast<unsigned long long>(m_eeCycle));
        std::fflush(stdout);
        if ((g_xmenMovieWorkerExitCount % 4u) == 0u)
        {
            traceXmenMovieHandoff(m_rdram,
                                  "worker-group-exit",
                                  g_xmenMovieWorkerExitCount / 4u,
                                  m_vsyncTick,
                                  m_eeCycle,
                                  thread(kMainThreadId));
            g_xmenTraceFinalMovieHandoff = g_xmenMovieWorkerExitCount >= 24u;
        }
    }
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    GuestThread *caller = currentThread();
    std::fprintf(stderr,
                 "[ee-thread:terminate] caller=%d target=%d callerPc=0x%x callerRa=0x%x targetPc=0x%x targetRa=0x%x cycle=%llu\n",
                 caller ? caller->id : 0,
                 id,
                 caller ? caller->activeContext().pc : 0u,
                 caller ? getRegU32(&caller->activeContext(), 31) : 0u,
                 target->activeContext().pc,
                 getRegU32(&target->activeContext(), 31),
                 static_cast<unsigned long long>(m_eeCycle));
    if (isXmenMovieThreadEntry(target->entry))
    {
        std::fprintf(stdout,
                     "[xmen-movie-lifecycle] action=terminate id=%d entry=0x%x caller=%d callerPc=0x%x callerRa=0x%x targetPc=0x%x targetRa=0x%x cycle=%llu\n",
                     id,
                     target->entry,
                     caller ? caller->id : 0,
                     caller ? caller->activeContext().pc : 0u,
                     caller ? getRegU32(&caller->activeContext(), 31) : 0u,
                     target->activeContext().pc,
                     getRegU32(&target->activeContext(), 31),
                     static_cast<unsigned long long>(m_eeCycle));
        std::fflush(stdout);
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    const bool traceXmenMovie = xmenMoviePlaybackStarted(m_rdram) && shouldTraceXmenMovieThread();
    GuestThread *caller = currentThread();
    if (id == 0)
    {
        if (traceXmenMovie)
        {
            std::fprintf(stderr,
                         "[xmen-movie-thread:resume] caller=%d target=%d result=%d reason=illegal-id cycle=%llu\n",
                         caller ? caller->id : 0,
                         id,
                         KE_ILLEGAL_THID,
                         static_cast<unsigned long long>(m_eeCycle));
        }
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        if (traceXmenMovie)
        {
            std::fprintf(stderr,
                         "[xmen-movie-thread:resume] caller=%d target=%d result=%d reason=unknown-id cycle=%llu\n",
                         caller ? caller->id : 0,
                         id,
                         KE_UNKNOWN_THID,
                         static_cast<unsigned long long>(m_eeCycle));
        }
        return KE_UNKNOWN_THID;
    }
    if (traceXmenMovie)
    {
        std::fprintf(stderr,
                     "[xmen-movie-thread:resume] caller=%d target=%d status=%u suspend=%d pc=0x%x ra=0x%x priority=%d callerPriority=%d cycle=%llu\n",
                     caller ? caller->id : 0,
                     id,
                     static_cast<unsigned>(target->status),
                     target->suspendCount,
                     target->activeContext().pc,
                     getRegU32(&target->activeContext(), 31),
                     target->currentPriority,
                     caller ? caller->currentPriority : -1,
                     static_cast<unsigned long long>(m_eeCycle));
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->id == kMainThreadId && m_eeCycle >= 5000000000ull)
    {
        static uint32_t xmenLateMainSleepCount = 0u;
        if (xmenLateMainSleepCount++ < 64u)
        {
            std::fprintf(stdout,
                         "[xmen-main-sleep] pc=0x%x ra=0x%x sp=0x%x state=0x%x wakeups=%u cycle=%llu\n",
                         self->activeContext().pc,
                         getRegU32(&self->activeContext(), 31),
                         getRegU32(&self->activeContext(), 29),
                         xmenMovieStreamState(m_rdram),
                         self->wakeupCount,
                         static_cast<unsigned long long>(m_eeCycle));
            std::fflush(stdout);
        }
    }
    if (xmenMoviePlaybackStarted(m_rdram) && shouldTraceXmenMovieThread())
    {
        std::fprintf(stderr,
                     "[xmen-movie-thread:sleep] id=%d status=%u suspend=%d wakeups=%u pc=0x%x ra=0x%x sp=0x%x cycle=%llu\n",
                     self->id,
                     static_cast<unsigned>(self->status),
                     self->suspendCount,
                     self->wakeupCount,
                     self->activeContext().pc,
                     getRegU32(&self->activeContext(), 31),
                     getRegU32(&self->activeContext(), 29),
                     static_cast<unsigned long long>(m_eeCycle));
    }
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    const bool traceXmenMovie = xmenMoviePlaybackStarted(m_rdram) && shouldTraceXmenMovieThread();
    GuestThread *caller = currentThread();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (traceXmenMovie)
    {
        std::fprintf(stderr,
                     "[xmen-movie-thread:wakeup] caller=%d target=%d status=%u wait=%u suspend=%d wakeups=%u pc=0x%x cycle=%llu\n",
                     caller ? caller->id : 0,
                     id,
                     static_cast<unsigned>(target->status),
                     static_cast<unsigned>(target->wait.reason),
                     target->suspendCount,
                     target->wakeupCount,
                     target->activeContext().pc,
                     static_cast<unsigned long long>(m_eeCycle));
    }
    const bool traceXmenWakeup = std::getenv("PS2X_XMEN_WAKEUP_TRACE") != nullptr &&
                                 target->entry == 0x005793B8u;
    const uint32_t wakeupsBefore = target->wakeupCount;
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    if (traceXmenWakeup)
    {
        static uint64_t traceCount = 0u;
        const uint64_t index = traceCount++;
        if (index < 16u || (index != 0u && (index & (index - 1u)) == 0u))
        {
            const R5900Context *callerContext = caller ? &caller->activeContext() : nullptr;
            std::fprintf(stderr,
                         "[xmen-wakeup-source] index=%llu source=guest caller=%d pc=0x%x ra=0x%x "
                         "target=%d status=%u wait=%u before=%u after=%u interrupt=%u insideInterrupt=%u cycle=%llu\n",
                         static_cast<unsigned long long>(index),
                         caller ? caller->id : 0,
                         callerContext ? callerContext->pc : 0u,
                         callerContext ? getRegU32(callerContext, 31) : 0u,
                         id,
                         static_cast<unsigned>(target->status),
                         static_cast<unsigned>(target->wait.reason),
                         wakeupsBefore,
                         target->wakeupCount,
                         interruptSafe ? 1u : 0u,
                         m_insideInterrupt ? 1u : 0u,
                         static_cast<unsigned long long>(m_eeCycle));
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::queueThreadWakeup(int id, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target || target->status == EeThreadStatus::Dormant)
    {
        return KE_UNKNOWN_THID;
    }
    const bool traceXmenWakeup = std::getenv("PS2X_XMEN_WAKEUP_TRACE") != nullptr &&
                                 target->entry == 0x005793B8u;
    const uint32_t wakeupsBefore = target->wakeupCount;
    if (target->status == EeThreadStatus::Waiting && target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    if (traceXmenWakeup)
    {
        static uint64_t traceCount = 0u;
        const uint64_t index = traceCount++;
        if (index < 16u || (index != 0u && (index & (index - 1u)) == 0u))
        {
            const GuestThread *caller = currentThread();
            const R5900Context *callerContext = caller ? &caller->activeContext() : nullptr;
            std::fprintf(stderr,
                         "[xmen-wakeup-source] index=%llu source=host caller=%d pc=0x%x ra=0x%x "
                         "target=%d status=%u wait=%u before=%u after=%u interrupt=%u insideInterrupt=%u cycle=%llu\n",
                         static_cast<unsigned long long>(index),
                         caller ? caller->id : 0,
                         callerContext ? callerContext->pc : 0u,
                         callerContext ? getRegU32(callerContext, 31) : 0u,
                         id,
                         static_cast<unsigned>(target->status),
                         static_cast<unsigned>(target->wait.reason),
                         wakeupsBefore,
                         target->wakeupCount,
                         interruptSafe ? 1u : 0u,
                         m_insideInterrupt ? 1u : 0u,
                         static_cast<unsigned long long>(m_eeCycle));
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return oldPriority;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (id == 29)
    {
        static uint64_t xmenSema29SignalCount = 0u;
        const uint64_t traceIndex = xmenSema29SignalCount++;
        if (traceIndex < 32u || (traceIndex % 120u) == 0u || object->waiters.size() > 1u)
        {
            const GuestThread *caller = currentThread();
            const R5900Context *callerContext = caller ? &caller->activeContext() : nullptr;
            std::fprintf(stdout,
                         "[xmen-sema29:signal] index=%llu tick=%llu thread=%d pc=0x%x ra=0x%x count=%d max=%d waiters=%zu interrupt=%u\n",
                         static_cast<unsigned long long>(traceIndex),
                         static_cast<unsigned long long>(m_vsyncTick),
                         caller ? caller->id : 0,
                         callerContext ? callerContext->pc : 0u,
                         callerContext ? getRegU32(callerContext, 31) : 0u,
                         object->count,
                         object->maxCount,
                         object->waiters.size(),
                         interruptSafe ? 1u : 0u);
            std::fflush(stdout);
        }
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    if (xmenDiagnosticsEnabled() &&
        invocation.kind == GuestInvocationKind::Interrupt &&
        getRegU32(&invocation.context, 4) == 1u)
    {
        const GuestThread *owner = currentThread();
        const R5900Context *active = owner ? &owner->activeContext() : nullptr;
        std::fprintf(stderr,
                     "[xmen-vif1-irq-queued] sequence=%llu handler=0x%x current=%d "
                     "pc=0x%x status=0x%x depth=%zu pendingBefore=%zu cycle=%llu\n",
                     static_cast<unsigned long long>(invocation.sequence),
                     invocation.context.pc,
                     owner ? owner->id : 0,
                     active ? active->pc : 0u,
                     active ? active->cop0_status : 0u,
                     owner ? owner->invocations.size() : 0u,
                     m_pendingInvocations.size(),
                     static_cast<unsigned long long>(m_eeCycle));
    }
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    uint32_t top = 0u;
    if (!m_freeInvocationStackTops.empty())
    {
        top = m_freeInvocationStackTops.back();
        m_freeInvocationStackTops.pop_back();
    }
    else
    {
        top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    }
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

void EeScheduler::releaseInvocationStack(int threadId, size_t depth) noexcept
{
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(threadId)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing == m_invocationStackTops.end())
    {
        return;
    }
    m_freeInvocationStackTops.push_back(existing->second);
    m_invocationStackTops.erase(existing);
}

void EeScheduler::releaseInvocationStacks(const GuestThread &thread) noexcept
{
    for (size_t depth = 0u; depth < thread.invocations.size(); ++depth)
    {
        releaseInvocationStack(thread.id, depth);
    }
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    std::fprintf(stderr,
                 "[ee-irq:add] kind=%s id=%d cause=%u handler=0x%x arg=0x%x gp=0x%x sp=0x%x append=%u\n",
                 dmac ? "dmac" : "intc",
                 id,
                 cause,
                 handler,
                 argument,
                 gp,
                 sp,
                 append ? 1u : 0u);
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    if (xmenDiagnosticsEnabled() && !dmac && cause == 0u &&
        (m_runtime.memory().gs().csr.load(std::memory_order_relaxed) & 0x2u) != 0u)
    {
        const GuestThread *interrupted = currentThread();
        const R5900Context *active = interrupted ? &interrupted->activeContext() : nullptr;
        std::fprintf(stderr,
                     "[xmen-gs-finish-dispatch] registered=%zu matching=%zu current=%d pc=0x%x status=0x%x depth=%zu pending=%zu csr=0x%llx imr=0x%llx cycle=%llu\n",
                     handlers.size(),
                     matching.size(),
                     interrupted ? interrupted->id : 0,
                     active ? active->pc : 0u,
                     active ? active->cop0_status : 0u,
                     interrupted ? interrupted->invocations.size() : 0u,
                     m_pendingInvocations.size(),
                     static_cast<unsigned long long>(m_runtime.memory().gs().csr.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(m_runtime.memory().gs().imr),
                     static_cast<unsigned long long>(m_eeCycle));
    }
    if (xmenDiagnosticsEnabled() && dmac && cause == 1u)
    {
        static uint32_t vif1IrqTraceCount = 0u;
        const GuestThread *interrupted = currentThread();
        std::fprintf(stderr,
                     "[xmen-vif1-irq] index=%u registered=%zu matching=%zu enabledMask=0x%x "
                     "current=%d pc=0x%x invocations=%zu cycle=%llu\n",
                     vif1IrqTraceCount++,
                     handlers.size(),
                     matching.size(),
                     mask,
                     interrupted ? interrupted->id : 0,
                     interrupted ? interrupted->activeContext().pc : 0u,
                     interrupted ? interrupted->invocations.size() : 0u,
                     static_cast<unsigned long long>(m_eeCycle));
        for (const EeIrqHandler &handler : matching)
        {
            std::fprintf(stderr,
                         "[xmen-vif1-irq-handler] id=%d handler=0x%x argument=0x%x gp=0x%x order=%d\n",
                         handler.id,
                         handler.handler,
                         handler.argument,
                         handler.gp,
                         handler.order);
        }
    }
    if (xmenDiagnosticsEnabled() && dmac && cause == 2u)
    {
        static uint32_t gifIrqDispatchTraceCount = 0u;
        const GuestThread *interrupted = currentThread();
        const uint32_t queueHead = readXmenWord(m_rdram, 0x007507D8u);
        const uint64_t issued = readXmenDoubleword(m_rdram, 0x00750810u);
        const uint64_t completed = readXmenDoubleword(m_rdram, 0x00750860u);
        if (gifIrqDispatchTraceCount < 256u || issued >= 0x4Cu || completed >= 0x4Cu)
        {
            std::fprintf(stderr,
                         "[xmen-gif-irq-dispatch] index=%u current=%d pc=0x%x depth=%zu pending=%zu "
                         "head=0x%x head0=0x%llx head8=0x%llx next=0x%x flags=0x%x "
                         "issued=0x%llx completed=0x%llx cycle=%llu\n",
                         gifIrqDispatchTraceCount,
                         interrupted ? interrupted->id : 0,
                         interrupted ? interrupted->activeContext().pc : 0u,
                         interrupted ? interrupted->invocations.size() : 0u,
                         m_pendingInvocations.size(),
                         queueHead,
                         static_cast<unsigned long long>(queueHead ? readXmenDoubleword(m_rdram, queueHead) : 0u),
                         static_cast<unsigned long long>(queueHead ? readXmenDoubleword(m_rdram, queueHead + 8u) : 0u),
                         queueHead ? readXmenWord(m_rdram, queueHead + 0x34u) : 0u,
                         queueHead ? readXmenWord(m_rdram, queueHead + 0x40u) : 0u,
                         static_cast<unsigned long long>(issued),
                         static_cast<unsigned long long>(completed),
                         static_cast<unsigned long long>(m_eeCycle));
        }
        ++gifIrqDispatchTraceCount;
    }
    static uint32_t irqTraceCount = 0u;
    if (xmenDiagnosticsEnabled() && irqTraceCount++ < 128u)
    {
        std::fprintf(stderr,
                     "[ee-irq:dispatch] kind=%s cause=%u registered=%zu matching=%zu cycle=%llu\n",
                     dmac ? "dmac" : "intc",
                     cause,
                     handlers.size(),
                     matching.size(),
                     static_cast<unsigned long long>(m_eeCycle));
    }
    for (const EeIrqHandler &handler : matching)
    {
        const GuestThread *interrupted = currentThread();
        static uint32_t irqQueueTraceCount = 0u;
        if (xmenDiagnosticsEnabled() && irqQueueTraceCount++ < 128u)
        {
            std::fprintf(stderr,
                         "[ee-irq:queue] kind=%s cause=%u handler=0x%x current=%d activePc=0x%x basePc=0x%x depth=%zu\n",
                         dmac ? "dmac" : "intc",
                         cause,
                         handler.handler,
                         interrupted ? interrupted->id : 0,
                         interrupted ? interrupted->activeContext().pc : 0u,
                         interrupted ? interrupted->context.pc : 0u,
                         interrupted ? interrupted->invocations.size() : 0u);
        }
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        if (const GuestThread *interrupted = currentThread())
        {
            invocation.context.cop0_status = interrupted->activeContext().cop0_status;
        }
        invocation.context.cop0_status &= ~(1u << 16u);
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        // EE interrupt handlers execute on a kernel-owned stack. Reusing the
        // registration-time caller SP corrupts paused guest call frames.
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::scheduleDmacIrq(uint32_t cause, uint64_t delayCycles)
{
    assertExecutor();
    const uint64_t delay = std::max<uint64_t>(delayCycles, 1u);
    const uint64_t deadlineCycle = m_eeCycle + delay;
    const auto hostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(delay);
    {
        std::lock_guard lock(m_eventMutex);
        // Each DMAC cause is a latched status bit, not a queue of completions.
        const auto pending = std::find_if(m_deadlines.begin(), m_deadlines.end(),
                                          [cause](const ScheduledEvent &item)
                                          {
                                              return item.event.type == EeEventType::Dmac &&
                                                     item.event.id == cause;
                                          });
        if (pending != m_deadlines.end())
        {
            return;
        }
        m_deadlines.push_back(ScheduledEvent{
            deadlineCycle,
            hostDeadline,
            EeEvent{EeEventType::Dmac, cause, 0u},
            ++m_eventSequence,
        });
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

uint64_t EeScheduler::currentEeCycle() const noexcept
{
    return m_eeCycle;
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        snapshot.pc = item.activeContext().pc;
        snapshot.ra = getRegU32(&item.activeContext(), 31);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    if (m_rdram && xmenMoviePlaybackStarted(m_rdram))
    {
        static uint32_t xmenEmptyReadyTraceCount = 0u;
        if (xmenEmptyReadyTraceCount++ < 16u)
        {
            std::fprintf(stderr,
                         "[xmen-ready:empty] cycle=%llu current=%d\n",
                         static_cast<unsigned long long>(m_eeCycle),
                         m_currentThreadId);
            for (const auto &[id, item] : m_threads)
            {
                if (item.entry < 0x00579108u || item.entry > 0x005793B8u)
                {
                    continue;
                }
                std::fprintf(stderr,
                             "[xmen-ready:worker] id=%d entry=0x%x status=%u priority=%d suspend=%d wait=%u pc=0x%x\n",
                             id,
                             item.entry,
                             static_cast<unsigned>(item.status),
                             item.currentPriority,
                             item.suspendCount,
                             static_cast<unsigned>(item.wait.reason),
                             item.activeContext().pc);
            }
        }
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    releaseInvocationStacks(item);
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!eeInterruptsEnabled(self->activeContext()))
    {
        return;
    }
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    RuntimeProfile::Scope eventProfile(RuntimeProfile::Phase::Events);
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline)
            {
                RuntimeProfile::Scope waitProfile(RuntimeProfile::Phase::Wait);
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              scheduled.hostDeadline + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              scheduled.hostDeadline + kVBlankPeriod,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        if (g_xmenTraceFinalMovieHandoff)
        {
            static uint64_t xmenFinalHandoffTraceCount = 0u;
            const uint64_t traceIndex = xmenFinalHandoffTraceCount++;
            if (traceIndex < 32u || (traceIndex % 30u) == 0u)
            {
                traceXmenMovieHandoff(m_rdram,
                                      "post-final-vblank",
                                      traceIndex,
                                      m_vsyncTick,
                                      m_eeCycle,
                                      thread(kMainThreadId));
            }
        }
        {
            GSRegisters &gs = m_runtime.memory().gs();
            gs.vsyncTick.store(m_vsyncTick, std::memory_order_release);
            gs.csr.fetch_or(0x8ull, std::memory_order_acq_rel);
            if ((gs.imr & (1ull << 11u)) == 0u)
            {
                dispatchIrq(false, 0u);
            }
        }
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            static uint64_t xmenGsCallbackQueueCount = 0u;
            const uint64_t traceIndex = xmenGsCallbackQueueCount++;
            const GuestThread *owner = currentThread();
            const size_t activeDepth = owner ? owner->invocations.size() : 0u;
            const size_t pendingDepth = m_pendingInvocations.size();
            if (traceIndex < 32u || (traceIndex % 120u) == 0u || activeDepth > 1u || pendingDepth > 1u)
            {
                const R5900Context *ownerContext = owner ? &owner->activeContext() : nullptr;
                std::fprintf(stdout,
                             "[xmen-gs-vsync:queue] index=%llu tick=%llu callback=0x%x owner=%d pc=0x%x ra=0x%x active=%zu pending=%zu\n",
                             static_cast<unsigned long long>(traceIndex),
                             static_cast<unsigned long long>(m_vsyncTick),
                             m_gsVSyncCallback,
                             owner ? owner->id : 0,
                             ownerContext ? ownerContext->pc : 0u,
                             ownerContext ? getRegU32(ownerContext, 31) : 0u,
                             activeDepth,
                             pendingDepth);
                for (const auto &[threadId, thread] : m_threads)
                {
                    if (!isXmenMovieThreadEntry(thread.entry))
                    {
                        continue;
                    }
                    const R5900Context &context = thread.activeContext();
                    std::fprintf(stdout,
                                 "[xmen-movie-worker-state] tick=%llu id=%d entry=0x%x status=%u wait=%u waitId=%d suspend=%u wakeups=%u invocations=%zu pc=0x%x ra=0x%x sp=0x%x basePc=0x%x baseRa=0x%x baseSp=0x%x\n",
                                 static_cast<unsigned long long>(m_vsyncTick),
                                 threadId,
                                 thread.entry,
                                 static_cast<unsigned>(thread.status),
                                 static_cast<unsigned>(thread.wait.reason),
                                 xmenWaitObjectId(&thread),
                                 thread.suspendCount,
                                 thread.wakeupCount,
                                 thread.invocations.size(),
                                 context.pc,
                                 getRegU32(&context, 31),
                                 getRegU32(&context, 29),
                                 thread.context.pc,
                                 getRegU32(&thread.context, 31),
                                 getRegU32(&thread.context, 29));
                }
                std::fflush(stdout);
            }
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Intc:
        if (xmenDiagnosticsEnabled() && event.id == 0u &&
            (m_runtime.memory().gs().csr.load(std::memory_order_relaxed) & 0x2u) != 0u)
        {
            std::fprintf(stderr,
                         "[xmen-gs-finish-event] csr=0x%llx imr=0x%llx pending=%zu cycle=%llu\n",
                         static_cast<unsigned long long>(m_runtime.memory().gs().csr.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(m_runtime.memory().gs().imr),
                         m_pendingInvocations.size(),
                         static_cast<unsigned long long>(m_eeCycle));
        }
        dispatchIrq(false, event.id);
        break;
    case EeEventType::Dmac:
        dispatchIrq(true, event.id);
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, alarm.sp);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    RuntimeProfile::Scope waitProfile(RuntimeProfile::Phase::Wait);
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    static uint32_t timerWaitTraceCount = 0u;
    if (timerWaitTraceCount++ < 64u)
    {
        std::fprintf(stderr,
                     "[ee-scheduler:wait] cycle=%llu timerDeadline=%s timerCycles=%llu deadlines=%zu\n",
                     static_cast<unsigned long long>(m_eeCycle),
                     hasTimerDeadline ? "yes" : "no",
                     hasTimerDeadline ? static_cast<unsigned long long>(timerCycles) : 0ull,
                     m_deadlines.size());
        if (timerWaitTraceCount <= 8u)
        {
            for (const auto &[id, item] : m_threads)
            {
                if (id < 0)
                {
                    continue;
                }
                const R5900Context &context = item.activeContext();
                std::fprintf(stderr,
                             "[ee-scheduler:thread] id=%d status=%u wait=%u waitId=%d pc=0x%x ra=0x%x entry=0x%x invocations=%zu wakeups=%u\n",
                             id,
                             static_cast<unsigned>(item.status),
                             static_cast<unsigned>(item.wait.reason),
                             waitObjectId(item.wait),
                             context.pc,
                             getRegU32(&context, 31),
                             item.entry,
                             item.invocations.size(),
                             item.wakeupCount);
            }
        }
    }
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}
