#include "Common.h"
#include "Thread.h"
#include "runtime/ee_scheduler.h"

#include <cstring>
#include <cstdio>

namespace ps2_syscalls
{
    namespace
    {
        EeScheduler &scheduler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            EeScheduler &result = runtime->eeScheduler();
            result.bindMainContextForSyscall(*ctx, rdram);
            return result;
        }

        int rawThreadStatus(EeThreadStatus status)
        {
            switch (status)
            {
            case EeThreadStatus::Running:
                return THS_RUN;
            case EeThreadStatus::Ready:
                return THS_READY;
            case EeThreadStatus::Waiting:
                return THS_WAIT;
            case EeThreadStatus::WaitingSuspended:
                return THS_WAITSUSPEND;
            case EeThreadStatus::Suspended:
                return THS_SUSPEND;
            case EeThreadStatus::Dormant:
                return THS_DORMANT;
            }
            return THS_DORMANT;
        }

        int rawWaitType(EeWaitReason reason)
        {
            switch (reason)
            {
            case EeWaitReason::Sleep:
                return TSW_SLEEP;
            case EeWaitReason::Semaphore:
                return TSW_SEMA;
            case EeWaitReason::EventFlag:
                return TSW_EVENT;
            case EeWaitReason::VSync:
                return 4;
            case EeWaitReason::External:
            case EeWaitReason::Mpeg:
                return 5;
            case EeWaitReason::None:
                return TSW_NONE;
            }
            return TSW_NONE;
        }

        int waitId(const GuestThread &thread)
        {
            if (thread.wait.reason == EeWaitReason::Semaphore)
            {
                return std::get<EeSemaphoreWait>(thread.wait.payload).id;
            }
            if (thread.wait.reason == EeWaitReason::EventFlag)
            {
                return std::get<EeEventFlagWait>(thread.wait.payload).id;
            }
            return 0;
        }

        bool xmenMoviePlaybackStarted(const uint8_t *rdram)
        {
            constexpr uint32_t kMovieStreamAddress = 0x006787F8u;
            uint32_t state = 0u;
            std::memcpy(&state, rdram + kMovieStreamAddress, sizeof(state));
            return state == 0x00000201u;
        }

        bool shouldTraceXmenMovieThread()
        {
            static uint64_t count = 0u;
            const uint64_t index = count++;
            return index < 128u || (index != 0u && (index & (index - 1u)) == 0u);
        }

        [[noreturn]] void exitThreadWithHandlers(int tid,
                                                 R5900Context *ctx,
                                                 PS2Runtime *runtime,
                                                 bool deleteThread)
        {
            EeScheduler &ee = runtime->eeScheduler();
            const auto handlers = runtime->takeEeExitHandlers(tid);
            std::vector<GuestInvocation> invocations;
            invocations.reserve(handlers.size());
            for (const PS2Runtime::EeExitHandlerRegistration &handler : handlers)
            {
                if (handler.function == 0u || !runtime->hasFunction(handler.function))
                {
                    continue;
                }
                GuestInvocation invocation{};
                invocation.kind = GuestInvocationKind::ExitHandler;
                invocation.context = *ctx;
                invocation.context.pc = handler.function;
                SET_GPR_U32(&invocation.context, 4, handler.argument);
                SET_GPR_U32(&invocation.context, 29, ee.invocationStackTop());
                SET_GPR_U32(&invocation.context, 31, 0u);
                invocations.push_back(std::move(invocation));
            }
            if (invocations.empty())
            {
                ee.exitCurrent(deleteThread);
            }
            invocations.back().onComplete = [runtime, deleteThread](const R5900Context &, R5900Context &)
            {
                runtime->eeScheduler().exitCurrent(deleteThread);
            };
            ee.invokeCurrentSequence(std::move(invocations));
        }

        void changePriorityImpl(uint8_t *rdram,
                                R5900Context *ctx,
                                PS2Runtime *runtime,
                                bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int id = static_cast<int>(getRegU32(ctx, 4));
            const int priority = static_cast<int>(getRegU32(ctx, 5));
            int oldPriority = 0;
            const int result = ee.changePriority(id, priority, interruptSafe, oldPriority);
            const GuestThread *target = ee.thread(id == 0 ? ee.currentThreadId() : id);
            if (target && target->entry >= 0x00578ED0u && target->entry <= 0x005794C8u)
            {
                static uint32_t xmenWorkerPriorityTraceCount = 0u;
                if (xmenWorkerPriorityTraceCount++ < 32u)
                {
                    std::fprintf(stderr,
                                 "[xmen-worker:priority] caller=%d id=%d entry=0x%x requested=%d old=%d current=%d result=%d pc=0x%x ra=0x%x interrupt=%d\n",
                                 ee.currentThreadId(),
                                 id,
                                 target->entry,
                                 priority,
                                 oldPriority,
                                 target->currentPriority,
                                 result,
                                 ctx->pc,
                                 getRegU32(ctx, 31),
                                 interruptSafe ? 1 : 0);
                }
            }
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }

        void rotateReadyQueueImpl(uint8_t *rdram,
                                  R5900Context *ctx,
                                  PS2Runtime *runtime,
                                  bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int result = ee.rotateReadyQueue(static_cast<int>(getRegU32(ctx, 4)), interruptSafe);
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }

        void wakeupThreadImpl(uint8_t *rdram,
                              R5900Context *ctx,
                              PS2Runtime *runtime,
                              bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int result = ee.wakeupThread(static_cast<int>(getRegU32(ctx, 4)), interruptSafe);
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }

        void releaseWaitImpl(uint8_t *rdram,
                             R5900Context *ctx,
                             PS2Runtime *runtime,
                             bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int result = ee.releaseWait(static_cast<int>(getRegU32(ctx, 4)), interruptSafe);
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }
    }

    void FlushCache(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void iFlushCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        FlushCache(rdram, ctx, runtime);
    }

    void EnableCache(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void DisableCache(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void ResetEE(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void SetMemoryMode(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void InitThread(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, EeScheduler::kMainThreadId);
    }

    void CreateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t address = getRegU32(ctx, 4);
        if (address == 0u)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        const auto *param = getEeGuestStruct<ee_thread_t>(rdram, address);
        if (!param)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        if (param->stack_size < 0)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        if (param->stack != 0u)
        {
            uint32_t stackOffset = 0u;
            bool scratch = false;
            if (!resolveEeGuestRange(param->stack,
                                     static_cast<size_t>(param->stack_size),
                                     stackOffset,
                                     scratch))
            {
                setReturnS32(ctx, KE_ERROR);
                return;
            }
        }

        // PS2SDK EE t_ee_thread: status, func, stack, stack_size, gp_reg,
        // initial_priority, current_priority, attr, option.
        const EeThreadCreateParams decoded{
            param->attr,
            param->func,
            param->stack,
            static_cast<uint32_t>(param->stack_size),
            param->gp_reg,
            param->initial_priority,
            param->option,
        };
        setReturnS32(ctx, scheduler(rdram, ctx, runtime).createThread(decoded));
    }

    void DeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int id = static_cast<int>(getRegU32(ctx, 4));
        uint32_t ownedStack = 0;
        const int result = ee.deleteThread(id, ownedStack);
        if (result == KE_OK)
        {
            runtime->removeEeExitHandlers(id);
        }
        if (ownedStack != 0u)
        {
            runtime->guestFree(ownedStack);
        }
        setReturnS32(ctx, result);
    }

    void StartThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int id = static_cast<int>(getRegU32(ctx, 4));
        const uint32_t arg = getRegU32(ctx, 5);
        GuestThread *target = ee.thread(id);
        if (!target)
        {
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }
        if (target->status != EeThreadStatus::Dormant)
        {
            setReturnS32(ctx, KE_NOT_DORMANT);
            return;
        }
        if (!runtime->hasFunction(target->entry))
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        if (target->stack == 0u && target->stackSize != 0u)
        {
            target->stack = runtime->guestMalloc(target->stackSize, 16u);
            if (target->stack == 0u)
            {
                setReturnS32(ctx, KE_ERROR);
                return;
            }
            target->ownsStack = true;
        }
        const int result = ee.startThread(id, arg, *ctx, false);
        if (target->entry >= 0x00578ED0u && target->entry <= 0x005794C8u)
        {
            const GuestThread *caller = ee.currentThread();
            std::fprintf(stderr,
                         "[xmen-worker:start] caller=%d callerPriority=%d id=%d entry=0x%x initial=%d current=%d result=%d pc=0x%x ra=0x%x\n",
                         ee.currentThreadId(),
                         caller ? caller->currentPriority : -1,
                         id,
                         target->entry,
                         target->initialPriority,
                         target->currentPriority,
                         result,
                         ctx->pc,
                         getRegU32(ctx, 31));
        }
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    void ExitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        std::fprintf(stderr,
                     "[ee-thread:exit-syscall] delete=0 id=%d pc=0x%x ra=0x%x sp=0x%x v0=0x%x a0=0x%x\n",
                     ee.currentThreadId(),
                     ctx->pc,
                     getRegU32(ctx, 31),
                     getRegU32(ctx, 29),
                     getRegU32(ctx, 2),
                     getRegU32(ctx, 4));
        exitThreadWithHandlers(ee.currentThreadId(), ctx, runtime, false);
    }

    void ExitDeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        std::fprintf(stderr,
                     "[ee-thread:exit-syscall] delete=1 id=%d pc=0x%x ra=0x%x sp=0x%x v0=0x%x a0=0x%x\n",
                     ee.currentThreadId(),
                     ctx->pc,
                     getRegU32(ctx, 31),
                     getRegU32(ctx, 29),
                     getRegU32(ctx, 2),
                     getRegU32(ctx, 4));
        exitThreadWithHandlers(ee.currentThreadId(), ctx, runtime, true);
    }

    void TerminateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        uint32_t ownedStack = 0;
        const int result = ee.terminateThread(static_cast<int>(getRegU32(ctx, 4)), ownedStack, false);
        if (ownedStack != 0u)
        {
            runtime->guestFree(ownedStack);
        }
        setReturnS32(ctx, result);
    }

    void SuspendThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int result = ee.suspendThread(static_cast<int>(getRegU32(ctx, 4)), false);
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    void ResumeThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int result = ee.resumeThread(static_cast<int>(getRegU32(ctx, 4)), false);
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    void GetThreadId(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int id = scheduler(rdram, ctx, runtime).currentThreadId();
        const uint32_t returnAddress = getRegU32(ctx, 31);
        if (returnAddress == 0x00579CECu || returnAddress == 0x0057A8C4u)
        {
            uint32_t wakeTarget = 0u;
            std::memcpy(&wakeTarget, rdram + 0x0066DD98u, sizeof(wakeTarget));
            const uint32_t wakeTargetBefore = wakeTarget;
            if (returnAddress == 0x00579CECu && wakeTarget == 0u && id > 0)
            {
                wakeTarget = static_cast<uint32_t>(id);
                std::memcpy(rdram + 0x0066DD98u, &wakeTarget, sizeof(wakeTarget));
            }
            std::fprintf(stderr,
                         "[xmen-movie-thread:get-id] id=%d ra=0x%x wakeTargetBefore=%u wakeTargetAfter=%u\n",
                         id,
                         returnAddress,
                         wakeTargetBefore,
                         wakeTarget);
        }
        setReturnS32(ctx, id);
    }

    void ReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int requestedId = static_cast<int>(getRegU32(ctx, 4));
        int id = requestedId;
        if (id == 0)
        {
            id = ee.currentThreadId();
        }
        const GuestThread *thread = ee.thread(id);
        if (!thread)
        {
            if (xmenMoviePlaybackStarted(rdram) && shouldTraceXmenMovieThread())
            {
                std::fprintf(stderr,
                             "[xmen-movie-thread:refer] caller=%d requested=%d resolved=%d result=%d pc=0x%x ra=0x%x\n",
                             ee.currentThreadId(),
                             requestedId,
                             id,
                             KE_UNKNOWN_THID,
                             ctx->pc,
                             getRegU32(ctx, 31));
            }
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }
        if (xmenMoviePlaybackStarted(rdram) && shouldTraceXmenMovieThread())
        {
            std::fprintf(stderr,
                         "[xmen-movie-thread:refer] caller=%d requested=%d resolved=%d status=%d raw=%d suspend=%u wakeups=%d wait=%d entry=0x%x priority=%d pc=0x%x ra=0x%x\n",
                         ee.currentThreadId(),
                         requestedId,
                         id,
                         static_cast<int>(thread->status),
                         rawThreadStatus(thread->status),
                         thread->suspendCount,
                         thread->wakeupCount,
                         static_cast<int>(thread->wait.reason),
                         thread->entry,
                         thread->currentPriority,
                         ctx->pc,
                         getRegU32(ctx, 31));
        }
        auto *status = getEeGuestStruct<ee_thread_status_t>(rdram, getRegU32(ctx, 5));
        if (!status)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        *status = {};
        status->status = rawThreadStatus(thread->status);
        status->func = thread->entry;
        status->stack = thread->stack;
        status->stack_size = static_cast<int>(thread->stackSize);
        status->gp_reg = thread->gp;
        status->initial_priority = thread->initialPriority;
        status->current_priority = thread->currentPriority;
        status->attr = thread->attr;
        status->option = thread->option;
        status->waitType = rawWaitType(thread->wait.reason);
        status->waitId = waitId(*thread);
        status->wakeupCount = thread->wakeupCount;
        setReturnS32(ctx, KE_OK);
    }

    void iReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ReferThreadStatus(rdram, ctx, runtime);
    }

    void SleepThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        ee.sleepCurrent();
        setReturnS32(ctx, KE_OK);
    }

    void WakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        wakeupThreadImpl(rdram, ctx, runtime, false);
    }

    void iWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        wakeupThreadImpl(rdram, ctx, runtime, true);
    }

    void CancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx,
                     scheduler(rdram, ctx, runtime).cancelWakeup(static_cast<int>(getRegU32(ctx, 4))));
    }

    void iCancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 4) == 0u)
        {
            setReturnS32(ctx, KE_ILLEGAL_THID);
            return;
        }
        CancelWakeupThread(rdram, ctx, runtime);
    }

    void ChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        changePriorityImpl(rdram, ctx, runtime, false);
    }

    void iChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        changePriorityImpl(rdram, ctx, runtime, true);
    }

    void RotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        rotateReadyQueueImpl(rdram, ctx, runtime, false);
    }

    void iRotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        rotateReadyQueueImpl(rdram, ctx, runtime, true);
    }

    void ReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        releaseWaitImpl(rdram, ctx, runtime, false);
    }

    void iReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        releaseWaitImpl(rdram, ctx, runtime, true);
    }
}
