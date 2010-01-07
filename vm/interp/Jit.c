/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifdef WITH_JIT

/*
 * Target independent portion of Android's Jit
 */

#include "Dalvik.h"
#include "Jit.h"


#include "dexdump/OpCodeNames.h"
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>
#include "compiler/Compiler.h"
#include "compiler/CompilerUtility.h"
#include "compiler/CompilerIR.h"
#include <errno.h>

#if defined(WITH_SELF_VERIFICATION)
/* Allocate space for per-thread ShadowSpace data structures */
void* dvmSelfVerificationShadowSpaceAlloc(Thread* self)
{
    self->shadowSpace = (ShadowSpace*) calloc(1, sizeof(ShadowSpace));
    if (self->shadowSpace == NULL)
        return NULL;

    self->shadowSpace->registerSpaceSize = REG_SPACE;
    self->shadowSpace->registerSpace =
        (int*) calloc(self->shadowSpace->registerSpaceSize, sizeof(int));

    return self->shadowSpace->registerSpace;
}

/* Free per-thread ShadowSpace data structures */
void dvmSelfVerificationShadowSpaceFree(Thread* self)
{
    free(self->shadowSpace->registerSpace);
    free(self->shadowSpace);
}

/*
 * Save out PC, FP, InterpState, and registers to shadow space.
 * Return a pointer to the shadow space for JIT to use.
 */
void* dvmSelfVerificationSaveState(const u2* pc, const void* fp,
                                   InterpState* interpState, int targetTrace)
{
    Thread *self = dvmThreadSelf();
    ShadowSpace *shadowSpace = self->shadowSpace;
    int preBytes = interpState->method->outsSize*4 + sizeof(StackSaveArea);
    int postBytes = interpState->method->registersSize*4;

    //LOGD("### selfVerificationSaveState(%d) pc: 0x%x fp: 0x%x",
    //    self->threadId, (int)pc, (int)fp);

    if (shadowSpace->selfVerificationState != kSVSIdle) {
        LOGD("~~~ Save: INCORRECT PREVIOUS STATE(%d): %d",
            self->threadId, shadowSpace->selfVerificationState);
        LOGD("********** SHADOW STATE DUMP **********");
        LOGD("PC: 0x%x FP: 0x%x", (int)pc, (int)fp);
    }
    shadowSpace->selfVerificationState = kSVSStart;

    // Dynamically grow shadow register space if necessary
    while (preBytes + postBytes > shadowSpace->registerSpaceSize) {
        shadowSpace->registerSpaceSize *= 2;
        free(shadowSpace->registerSpace);
        shadowSpace->registerSpace =
            (int*) calloc(shadowSpace->registerSpaceSize, sizeof(int));
    }

    // Remember original state
    shadowSpace->startPC = pc;
    shadowSpace->fp = fp;
    shadowSpace->glue = interpState;
    /*
     * Store the original method here in case the trace ends with a
     * return/invoke, the last method.
     */
    shadowSpace->method = interpState->method;
    shadowSpace->shadowFP = shadowSpace->registerSpace +
                            shadowSpace->registerSpaceSize - postBytes/4;

    // Create a copy of the InterpState
    //shadowSpace->interpState = *interpState;
    memcpy(&(shadowSpace->interpState), interpState, sizeof(InterpState));
    shadowSpace->interpState.fp = shadowSpace->shadowFP;
    shadowSpace->interpState.interpStackEnd = (u1*)shadowSpace->registerSpace;

    // Create a copy of the stack
    memcpy(((char*)shadowSpace->shadowFP)-preBytes, ((char*)fp)-preBytes,
        preBytes+postBytes);

    // Setup the shadowed heap space
    shadowSpace->heapSpaceTail = shadowSpace->heapSpace;

    // Reset trace length
    shadowSpace->traceLength = 0;

    return shadowSpace;
}

/*
 * Save ending PC, FP and compiled code exit point to shadow space.
 * Return a pointer to the shadow space for JIT to restore state.
 */
void* dvmSelfVerificationRestoreState(const u2* pc, const void* fp,
                                      SelfVerificationState exitPoint)
{
    Thread *self = dvmThreadSelf();
    ShadowSpace *shadowSpace = self->shadowSpace;
    shadowSpace->endPC = pc;
    shadowSpace->endShadowFP = fp;

    //LOGD("### selfVerificationRestoreState(%d) pc: 0x%x fp: 0x%x endPC: 0x%x",
    //    self->threadId, (int)shadowSpace->startPC, (int)shadowSpace->fp,
    //    (int)pc);

    if (shadowSpace->selfVerificationState != kSVSStart) {
        LOGD("~~~ Restore: INCORRECT PREVIOUS STATE(%d): %d",
            self->threadId, shadowSpace->selfVerificationState);
        LOGD("********** SHADOW STATE DUMP **********");
        LOGD("Dalvik PC: 0x%x endPC: 0x%x", (int)shadowSpace->startPC,
            (int)shadowSpace->endPC);
        LOGD("Interp FP: 0x%x", (int)shadowSpace->fp);
        LOGD("Shadow FP: 0x%x endFP: 0x%x", (int)shadowSpace->shadowFP,
            (int)shadowSpace->endShadowFP);
    }

    // Special case when punting after a single instruction
    if (exitPoint == kSVSPunt && pc == shadowSpace->startPC) {
        shadowSpace->selfVerificationState = kSVSIdle;
    } else {
        shadowSpace->selfVerificationState = exitPoint;
    }

    return shadowSpace;
}

/* Print contents of virtual registers */
static void selfVerificationPrintRegisters(int* addr, int* addrRef,
                                           int numWords)
{
    int i;
    for (i = 0; i < numWords; i++) {
        LOGD("(v%d) 0x%8x%s", i, addr[i], addr[i] != addrRef[i] ? " X" : "");
    }
}

/* Print values maintained in shadowSpace */
static void selfVerificationDumpState(const u2* pc, Thread* self)
{
    ShadowSpace* shadowSpace = self->shadowSpace;
    StackSaveArea* stackSave = SAVEAREA_FROM_FP(self->curFrame);
    int frameBytes = (int) shadowSpace->registerSpace +
                     shadowSpace->registerSpaceSize*4 -
                     (int) shadowSpace->shadowFP;
    int localRegs = 0;
    int frameBytes2 = 0;
    if (self->curFrame < shadowSpace->fp) {
        localRegs = (stackSave->method->registersSize -
                     stackSave->method->insSize)*4;
        frameBytes2 = (int) shadowSpace->fp - (int) self->curFrame - localRegs;
    }
    LOGD("********** SHADOW STATE DUMP **********");
    LOGD("CurrentPC: 0x%x, Offset: 0x%04x", (int)pc,
        (int)(pc - stackSave->method->insns));
    LOGD("Class: %s", shadowSpace->method->clazz->descriptor);
    LOGD("Method: %s", shadowSpace->method->name);
    LOGD("Dalvik PC: 0x%x endPC: 0x%x", (int)shadowSpace->startPC,
        (int)shadowSpace->endPC);
    LOGD("Interp FP: 0x%x endFP: 0x%x", (int)shadowSpace->fp,
        (int)self->curFrame);
    LOGD("Shadow FP: 0x%x endFP: 0x%x", (int)shadowSpace->shadowFP,
        (int)shadowSpace->endShadowFP);
    LOGD("Frame1 Bytes: %d Frame2 Local: %d Bytes: %d", frameBytes,
        localRegs, frameBytes2);
    LOGD("Trace length: %d State: %d", shadowSpace->traceLength,
        shadowSpace->selfVerificationState);
}

/* Print decoded instructions in the current trace */
static void selfVerificationDumpTrace(const u2* pc, Thread* self)
{
    ShadowSpace* shadowSpace = self->shadowSpace;
    StackSaveArea* stackSave = SAVEAREA_FROM_FP(self->curFrame);
    int i, addr, offset;
    DecodedInstruction *decInsn;

    LOGD("********** SHADOW TRACE DUMP **********");
    for (i = 0; i < shadowSpace->traceLength; i++) {
        addr = shadowSpace->trace[i].addr;
        offset =  (int)((u2*)addr - stackSave->method->insns);
        decInsn = &(shadowSpace->trace[i].decInsn);
        /* Not properly decoding instruction, some registers may be garbage */
        LOGD("0x%x: (0x%04x) %s", addr, offset, getOpcodeName(decInsn->opCode));
    }
}

/* Code is forced into this spin loop when a divergence is detected */
static void selfVerificationSpinLoop(ShadowSpace *shadowSpace)
{
    const u2 *startPC = shadowSpace->startPC;
    JitTraceDescription* desc = dvmCopyTraceDescriptor(startPC);
    if (desc) {
        dvmCompilerWorkEnqueue(startPC, kWorkOrderTraceDebug, desc);
    }
    gDvmJit.selfVerificationSpin = true;
    while(gDvmJit.selfVerificationSpin) sleep(10);
}

/* Manage self verification while in the debug interpreter */
static bool selfVerificationDebugInterp(const u2* pc, Thread* self)
{
    ShadowSpace *shadowSpace = self->shadowSpace;
    SelfVerificationState state = shadowSpace->selfVerificationState;

    DecodedInstruction decInsn;
    dexDecodeInstruction(gDvm.instrFormat, pc, &decInsn);

    //LOGD("### DbgIntp(%d): PC: 0x%x endPC: 0x%x state: %d len: %d %s",
    //    self->threadId, (int)pc, (int)shadowSpace->endPC, state,
    //    shadowSpace->traceLength, getOpcodeName(decInsn.opCode));

    if (state == kSVSIdle || state == kSVSStart) {
        LOGD("~~~ DbgIntrp: INCORRECT PREVIOUS STATE(%d): %d",
            self->threadId, state);
        selfVerificationDumpState(pc, self);
        selfVerificationDumpTrace(pc, self);
    }

    /* Skip endPC once when trace has a backward branch */
    if ((state == kSVSBackwardBranch && pc == shadowSpace->endPC) ||
        state != kSVSBackwardBranch) {
        shadowSpace->selfVerificationState = kSVSDebugInterp;
    }

    /* Check that the current pc is the end of the trace */
    if ((state == kSVSSingleStep || state == kSVSDebugInterp) &&
        pc == shadowSpace->endPC) {

        shadowSpace->selfVerificationState = kSVSIdle;

        /* Check register space */
        int frameBytes = (int) shadowSpace->registerSpace +
                         shadowSpace->registerSpaceSize*4 -
                         (int) shadowSpace->shadowFP;
        if (memcmp(shadowSpace->fp, shadowSpace->shadowFP, frameBytes)) {
            LOGD("~~~ DbgIntp(%d): REGISTERS DIVERGENCE!", self->threadId);
            selfVerificationDumpState(pc, self);
            selfVerificationDumpTrace(pc, self);
            LOGD("*** Interp Registers: addr: 0x%x bytes: %d",
                (int)shadowSpace->fp, frameBytes);
            selfVerificationPrintRegisters((int*)shadowSpace->fp,
                                           (int*)shadowSpace->shadowFP,
                                           frameBytes/4);
            LOGD("*** Shadow Registers: addr: 0x%x bytes: %d",
                (int)shadowSpace->shadowFP, frameBytes);
            selfVerificationPrintRegisters((int*)shadowSpace->shadowFP,
                                           (int*)shadowSpace->fp,
                                           frameBytes/4);
            selfVerificationSpinLoop(shadowSpace);
        }
        /* Check new frame if it exists (invokes only) */
        if (self->curFrame < shadowSpace->fp) {
            StackSaveArea* stackSave = SAVEAREA_FROM_FP(self->curFrame);
            int localRegs = (stackSave->method->registersSize -
                             stackSave->method->insSize)*4;
            int frameBytes2 = (int) shadowSpace->fp -
                              (int) self->curFrame - localRegs;
            if (memcmp(((char*)self->curFrame)+localRegs,
                ((char*)shadowSpace->endShadowFP)+localRegs, frameBytes2)) {
                LOGD("~~~ DbgIntp(%d): REGISTERS (FRAME2) DIVERGENCE!",
                    self->threadId);
                selfVerificationDumpState(pc, self);
                selfVerificationDumpTrace(pc, self);
                LOGD("*** Interp Registers: addr: 0x%x l: %d bytes: %d",
                    (int)self->curFrame, localRegs, frameBytes2);
                selfVerificationPrintRegisters((int*)self->curFrame,
                                               (int*)shadowSpace->endShadowFP,
                                               (frameBytes2+localRegs)/4);
                LOGD("*** Shadow Registers: addr: 0x%x l: %d bytes: %d",
                    (int)shadowSpace->endShadowFP, localRegs, frameBytes2);
                selfVerificationPrintRegisters((int*)shadowSpace->endShadowFP,
                                               (int*)self->curFrame,
                                               (frameBytes2+localRegs)/4);
                selfVerificationSpinLoop(shadowSpace);
            }
        }

        /* Check memory space */
        bool memDiff = false;
        ShadowHeap* heapSpacePtr;
        for (heapSpacePtr = shadowSpace->heapSpace;
             heapSpacePtr != shadowSpace->heapSpaceTail; heapSpacePtr++) {
            int memData = *((unsigned int*) heapSpacePtr->addr);
            if (heapSpacePtr->data != memData) {
                LOGD("~~~ DbgIntp(%d): MEMORY DIVERGENCE!", self->threadId);
                LOGD("Addr: 0x%x Intrp Data: 0x%x Jit Data: 0x%x",
                    heapSpacePtr->addr, memData, heapSpacePtr->data);
                selfVerificationDumpState(pc, self);
                selfVerificationDumpTrace(pc, self);
                memDiff = true;
            }
        }
        if (memDiff) selfVerificationSpinLoop(shadowSpace);
        return true;

    /* If end not been reached, make sure max length not exceeded */
    } else if (shadowSpace->traceLength >= JIT_MAX_TRACE_LEN) {
        LOGD("~~~ DbgIntp(%d): CONTROL DIVERGENCE!", self->threadId);
        LOGD("startPC: 0x%x endPC: 0x%x currPC: 0x%x",
            (int)shadowSpace->startPC, (int)shadowSpace->endPC, (int)pc);
        selfVerificationDumpState(pc, self);
        selfVerificationDumpTrace(pc, self);
        selfVerificationSpinLoop(shadowSpace);

        return true;
    }
    /* Log the instruction address and decoded instruction for debug */
    shadowSpace->trace[shadowSpace->traceLength].addr = (int)pc;
    shadowSpace->trace[shadowSpace->traceLength].decInsn = decInsn;
    shadowSpace->traceLength++;

    return false;
}
#endif

int dvmJitStartup(void)
{
    unsigned int i;
    bool res = true;  /* Assume success */

#if defined(WITH_SELF_VERIFICATION)
    // Force JIT into blocking, translate everything mode
    gDvmJit.threshold = 1;
    gDvmJit.blockingMode = true;
#endif

    // Create the compiler thread and setup miscellaneous chores */
    res &= dvmCompilerStartup();

    dvmInitMutex(&gDvmJit.tableLock);
    if (res && gDvm.executionMode == kExecutionModeJit) {
        JitEntry *pJitTable = NULL;
        unsigned char *pJitProfTable = NULL;
        // Power of 2?
        assert(gDvmJit.jitTableSize &&
               !(gDvmJit.jitTableSize & (gDvmJit.jitTableSize - 1)));
        dvmLockMutex(&gDvmJit.tableLock);
        pJitTable = (JitEntry*)
                    calloc(gDvmJit.jitTableSize, sizeof(*pJitTable));
        if (!pJitTable) {
            LOGE("jit table allocation failed\n");
            res = false;
            goto done;
        }
        /*
         * NOTE: the profile table must only be allocated once, globally.
         * Profiling is turned on and off by nulling out gDvm.pJitProfTable
         * and then restoring its original value.  However, this action
         * is not syncronized for speed so threads may continue to hold
         * and update the profile table after profiling has been turned
         * off by null'ng the global pointer.  Be aware.
         */
        pJitProfTable = (unsigned char *)malloc(JIT_PROF_SIZE);
        if (!pJitProfTable) {
            LOGE("jit prof table allocation failed\n");
            res = false;
            goto done;
        }
        memset(pJitProfTable, gDvmJit.threshold, JIT_PROF_SIZE);
        for (i=0; i < gDvmJit.jitTableSize; i++) {
           pJitTable[i].u.info.chain = gDvmJit.jitTableSize;
        }
        /* Is chain field wide enough for termination pattern? */
        assert(pJitTable[0].u.info.chain == gDvmJit.jitTableSize);

done:
        gDvmJit.pJitEntryTable = pJitTable;
        gDvmJit.jitTableMask = gDvmJit.jitTableSize - 1;
        gDvmJit.jitTableEntriesUsed = 0;
        gDvmJit.pProfTable = pJitProfTable;
        dvmUnlockMutex(&gDvmJit.tableLock);
    }
    return res;
}

/*
 * If one of our fixed tables or the translation buffer fills up,
 * call this routine to avoid wasting cycles on future translation requests.
 */
void dvmJitStopTranslationRequests()
{
    /*
     * Note 1: This won't necessarily stop all translation requests, and
     * operates on a delayed mechanism.  Running threads look to the copy
     * of this value in their private InterpState structures and won't see
     * this change until it is refreshed (which happens on interpreter
     * entry).
     * Note 2: This is a one-shot memory leak on this table. Because this is a
     * permanent off switch for Jit profiling, it is a one-time leak of 1K
     * bytes, and no further attempt will be made to re-allocate it.  Can't
     * free it because some thread may be holding a reference.
     */
    gDvmJit.pProfTable = NULL;
}

#if defined(EXIT_STATS)
/* Convenience function to increment counter from assembly code */
void dvmBumpNoChain(int from)
{
    gDvmJit.noChainExit[from]++;
}

/* Convenience function to increment counter from assembly code */
void dvmBumpNormal()
{
    gDvmJit.normalExit++;
}

/* Convenience function to increment counter from assembly code */
void dvmBumpPunt(int from)
{
    gDvmJit.puntExit++;
}
#endif

/* Dumps debugging & tuning stats to the log */
void dvmJitStats()
{
    int i;
    int hit;
    int not_hit;
    int chains;
    int stubs;
    if (gDvmJit.pJitEntryTable) {
        for (i=0, stubs=chains=hit=not_hit=0;
             i < (int) gDvmJit.jitTableSize;
             i++) {
            if (gDvmJit.pJitEntryTable[i].dPC != 0) {
                hit++;
                if (gDvmJit.pJitEntryTable[i].codeAddress ==
                      gDvmJit.interpretTemplate)
                    stubs++;
            } else
                not_hit++;
            if (gDvmJit.pJitEntryTable[i].u.info.chain != gDvmJit.jitTableSize)
                chains++;
        }
        LOGD(
         "JIT: %d traces, %d slots, %d chains, %d maxQ, %d thresh, %s",
         hit, not_hit + hit, chains, gDvmJit.compilerMaxQueued,
         gDvmJit.threshold, gDvmJit.blockingMode ? "Blocking" : "Non-blocking");
#if defined(EXIT_STATS)
        LOGD(
         "JIT: Lookups: %d hits, %d misses; %d normal, %d punt",
         gDvmJit.addrLookupsFound, gDvmJit.addrLookupsNotFound,
         gDvmJit.normalExit, gDvmJit.puntExit);
        LOGD(
         "JIT: noChainExit: %d IC miss, %d interp callsite, %d switch overflow",
         gDvmJit.noChainExit[kInlineCacheMiss],
         gDvmJit.noChainExit[kCallsiteInterpreted],
         gDvmJit.noChainExit[kSwitchOverflow]);
#endif
        LOGD("JIT: %d Translation chains, %d interp stubs",
             gDvmJit.translationChains, stubs);
#if defined(INVOKE_STATS)
        LOGD("JIT: Invoke: %d chainable, %d pred. chain, %d native, "
             "%d return",
             gDvmJit.invokeChain, gDvmJit.invokePredictedChain,
             gDvmJit.invokeNative, gDvmJit.returnOp);
#endif
        if (gDvmJit.profile) {
            dvmCompilerSortAndPrintTraceProfiles();
        }
    }
}


/*
 * Final JIT shutdown.  Only do this once, and do not attempt to restart
 * the JIT later.
 */
void dvmJitShutdown(void)
{
    /* Shutdown the compiler thread */

    dvmCompilerShutdown();

    dvmCompilerDumpStats();

    dvmDestroyMutex(&gDvmJit.tableLock);

    if (gDvmJit.pJitEntryTable) {
        free(gDvmJit.pJitEntryTable);
        gDvmJit.pJitEntryTable = NULL;
    }

    if (gDvmJit.pProfTable) {
        free(gDvmJit.pProfTable);
        gDvmJit.pProfTable = NULL;
    }
}

void setTraceConstruction(JitEntry *slot, bool value)
{

    JitEntryInfoUnion oldValue;
    JitEntryInfoUnion newValue;
    do {
        oldValue = slot->u;
        newValue = oldValue;
        newValue.info.traceConstruction = value;
    } while (!ATOMIC_CMP_SWAP( &slot->u.infoWord,
             oldValue.infoWord, newValue.infoWord));
}

void resetTracehead(InterpState* interpState, JitEntry *slot)
{
    slot->codeAddress = gDvmJit.interpretTemplate;
    setTraceConstruction(slot, false);
}

/* Clean up any pending trace builds */
void dvmJitAbortTraceSelect(InterpState* interpState)
{
    if (interpState->jitState == kJitTSelect)
        interpState->jitState = kJitTSelectAbort;
}

#if defined(WITH_SELF_VERIFICATION)
static bool selfVerificationPuntOps(DecodedInstruction *decInsn)
{
    OpCode op = decInsn->opCode;
    int flags =  dexGetInstrFlags(gDvm.instrFlags, op);
    return (op == OP_MONITOR_ENTER || op == OP_MONITOR_EXIT ||
            op == OP_NEW_INSTANCE || op == OP_NEW_ARRAY ||
            op == OP_CHECK_CAST || op == OP_MOVE_EXCEPTION ||
            (flags & kInstrInvoke));
}
#endif

/*
 * Adds to the current trace request one instruction at a time, just
 * before that instruction is interpreted.  This is the primary trace
 * selection function.  NOTE: return instruction are handled a little
 * differently.  In general, instructions are "proposed" to be added
 * to the current trace prior to interpretation.  If the interpreter
 * then successfully completes the instruction, is will be considered
 * part of the request.  This allows us to examine machine state prior
 * to interpretation, and also abort the trace request if the instruction
 * throws or does something unexpected.  However, return instructions
 * will cause an immediate end to the translation request - which will
 * be passed to the compiler before the return completes.  This is done
 * in response to special handling of returns by the interpreter (and
 * because returns cannot throw in a way that causes problems for the
 * translated code.
 */
int dvmCheckJit(const u2* pc, Thread* self, InterpState* interpState)
{
    int flags,i,len;
    int switchInterp = false;
    int debugOrProfile = (gDvm.debuggerActive || self->suspendCount
#if defined(WITH_PROFILER)
                          || gDvm.activeProfilers
#endif
            );

    /* Prepare to handle last PC and stage the current PC */
    const u2 *lastPC = interpState->lastPC;
    interpState->lastPC = pc;

#if defined(WITH_SELF_VERIFICATION)
    /*
     * We can't allow some instructions to be executed twice, and so they
     * must not appear in any translations.  End the trace before they
     * are inlcluded.
     */
    if (lastPC && interpState->jitState == kJitTSelect) {
        DecodedInstruction decInsn;
        dexDecodeInstruction(gDvm.instrFormat, lastPC, &decInsn);
        if (selfVerificationPuntOps(&decInsn)) {
            interpState->jitState = kJitTSelectEnd;
        }
    }
#endif

    switch (interpState->jitState) {
        char* nopStr;
        int target;
        int offset;
        DecodedInstruction decInsn;
        case kJitTSelect:
            /* First instruction - just remember the PC and exit */
            if (lastPC == NULL) break;
            /* Grow the trace around the last PC if jitState is kJitTSelect */
            dexDecodeInstruction(gDvm.instrFormat, lastPC, &decInsn);

            /*
             * Treat {PACKED,SPARSE}_SWITCH as trace-ending instructions due
             * to the amount of space it takes to generate the chaining
             * cells.
             */
            if (interpState->totalTraceLen != 0 &&
                (decInsn.opCode == OP_PACKED_SWITCH ||
                 decInsn.opCode == OP_SPARSE_SWITCH)) {
                interpState->jitState = kJitTSelectEnd;
                break;
            }


#if defined(SHOW_TRACE)
            LOGD("TraceGen: adding %s",getOpcodeName(decInsn.opCode));
#endif
            flags = dexGetInstrFlags(gDvm.instrFlags, decInsn.opCode);
            len = dexGetInstrOrTableWidthAbs(gDvm.instrWidth, lastPC);
            offset = lastPC - interpState->method->insns;
            assert((unsigned) offset <
                   dvmGetMethodInsnsSize(interpState->method));
            if (lastPC != interpState->currRunHead + interpState->currRunLen) {
                int currTraceRun;
                /* We need to start a new trace run */
                currTraceRun = ++interpState->currTraceRun;
                interpState->currRunLen = 0;
                interpState->currRunHead = (u2*)lastPC;
                interpState->trace[currTraceRun].frag.startOffset = offset;
                interpState->trace[currTraceRun].frag.numInsts = 0;
                interpState->trace[currTraceRun].frag.runEnd = false;
                interpState->trace[currTraceRun].frag.hint = kJitHintNone;
            }
            interpState->trace[interpState->currTraceRun].frag.numInsts++;
            interpState->totalTraceLen++;
            interpState->currRunLen += len;

            /* Will probably never hit this with the current trace buildier */
            if (interpState->currTraceRun == (MAX_JIT_RUN_LEN - 1)) {
                interpState->jitState = kJitTSelectEnd;
            }

            if (  ((flags & kInstrUnconditional) == 0) &&
                  /* don't end trace on INVOKE_DIRECT_EMPTY  */
                  (decInsn.opCode != OP_INVOKE_DIRECT_EMPTY) &&
                  ((flags & (kInstrCanBranch |
                             kInstrCanSwitch |
                             kInstrCanReturn |
                             kInstrInvoke)) != 0)) {
                    interpState->jitState = kJitTSelectEnd;
#if defined(SHOW_TRACE)
            LOGD("TraceGen: ending on %s, basic block end",
                 getOpcodeName(decInsn.opCode));
#endif
            }
            /* Break on throw or self-loop */
            if ((decInsn.opCode == OP_THROW) || (lastPC == pc)){
                interpState->jitState = kJitTSelectEnd;
            }
            if (interpState->totalTraceLen >= JIT_MAX_TRACE_LEN) {
                interpState->jitState = kJitTSelectEnd;
            }
            if (debugOrProfile) {
                interpState->jitState = kJitTSelectAbort;
                switchInterp = !debugOrProfile;
                break;
            }
            if ((flags & kInstrCanReturn) != kInstrCanReturn) {
                break;
            }
            /* NOTE: intentional fallthrough for returns */
        case kJitTSelectEnd:
            {
                if (interpState->totalTraceLen == 0) {
                    /* Bad trace - mark as untranslatable */
                    dvmJitAbortTraceSelect(interpState);
                    switchInterp = !debugOrProfile;
                    break;
                }
                JitTraceDescription* desc =
                   (JitTraceDescription*)malloc(sizeof(JitTraceDescription) +
                     sizeof(JitTraceRun) * (interpState->currTraceRun+1));
                if (desc == NULL) {
                    LOGE("Out of memory in trace selection");
                    dvmJitStopTranslationRequests();
                    interpState->jitState = kJitTSelectAbort;
                    dvmJitAbortTraceSelect(interpState);
                    switchInterp = !debugOrProfile;
                    break;
                }
                interpState->trace[interpState->currTraceRun].frag.runEnd =
                     true;
                interpState->jitState = kJitNormal;
                desc->method = interpState->method;
                memcpy((char*)&(desc->trace[0]),
                    (char*)&(interpState->trace[0]),
                    sizeof(JitTraceRun) * (interpState->currTraceRun+1));
#if defined(SHOW_TRACE)
                LOGD("TraceGen:  trace done, adding to queue");
#endif
                dvmCompilerWorkEnqueue(
                       interpState->currTraceHead,kWorkOrderTrace,desc);
                setTraceConstruction(
                     dvmJitLookupAndAdd(interpState->currTraceHead), false);
                if (gDvmJit.blockingMode) {
                    dvmCompilerDrainQueue();
                }
                switchInterp = !debugOrProfile;
            }
            break;
        case kJitSingleStep:
            interpState->jitState = kJitSingleStepEnd;
            break;
        case kJitSingleStepEnd:
            interpState->entryPoint = kInterpEntryResume;
            switchInterp = !debugOrProfile;
            break;
        case kJitTSelectAbort:
#if defined(SHOW_TRACE)
            LOGD("TraceGen:  trace abort");
#endif
            dvmJitAbortTraceSelect(interpState);
            interpState->jitState = kJitNormal;
            switchInterp = !debugOrProfile;
            break;
        case kJitNormal:
            switchInterp = !debugOrProfile;
            break;
#if defined(WITH_SELF_VERIFICATION)
        case kJitSelfVerification:
            if (selfVerificationDebugInterp(pc, self)) {
                interpState->jitState = kJitNormal;
                switchInterp = !debugOrProfile;
            }
            break;
#endif
        /* If JIT is off stay out of interpreter selections */
        case kJitOff:
            break;
        default:
            if (!debugOrProfile) {
                LOGE("Unexpected JIT state: %d", interpState->jitState);
                dvmAbort();
            }
            break;
    }
    return switchInterp;
}

JitEntry *dvmFindJitEntry(const u2* pc)
{
    int idx = dvmJitHash(pc);

    /* Expect a high hit rate on 1st shot */
    if (gDvmJit.pJitEntryTable[idx].dPC == pc)
        return &gDvmJit.pJitEntryTable[idx];
    else {
        int chainEndMarker = gDvmJit.jitTableSize;
        while (gDvmJit.pJitEntryTable[idx].u.info.chain != chainEndMarker) {
            idx = gDvmJit.pJitEntryTable[idx].u.info.chain;
            if (gDvmJit.pJitEntryTable[idx].dPC == pc)
                return &gDvmJit.pJitEntryTable[idx];
        }
    }
    return NULL;
}

/*
 * If a translated code address exists for the davik byte code
 * pointer return it.  This routine needs to be fast.
 */
void* dvmJitGetCodeAddr(const u2* dPC)
{
    int idx = dvmJitHash(dPC);

    /* If anything is suspended, don't re-enter the code cache */
    if (gDvm.sumThreadSuspendCount > 0) {
        return NULL;
    }

    /* Expect a high hit rate on 1st shot */
    if (gDvmJit.pJitEntryTable[idx].dPC == dPC) {
#if defined(EXIT_STATS)
        gDvmJit.addrLookupsFound++;
#endif
        return gDvmJit.pJitEntryTable[idx].codeAddress;
    } else {
        int chainEndMarker = gDvmJit.jitTableSize;
        while (gDvmJit.pJitEntryTable[idx].u.info.chain != chainEndMarker) {
            idx = gDvmJit.pJitEntryTable[idx].u.info.chain;
            if (gDvmJit.pJitEntryTable[idx].dPC == dPC) {
#if defined(EXIT_STATS)
                gDvmJit.addrLookupsFound++;
#endif
                return gDvmJit.pJitEntryTable[idx].codeAddress;
            }
        }
    }
#if defined(EXIT_STATS)
    gDvmJit.addrLookupsNotFound++;
#endif
    return NULL;
}

/*
 * Find an entry in the JitTable, creating if necessary.
 * Returns null if table is full.
 */
JitEntry *dvmJitLookupAndAdd(const u2* dPC)
{
    u4 chainEndMarker = gDvmJit.jitTableSize;
    u4 idx = dvmJitHash(dPC);

    /* Walk the bucket chain to find an exact match for our PC */
    while ((gDvmJit.pJitEntryTable[idx].u.info.chain != chainEndMarker) &&
           (gDvmJit.pJitEntryTable[idx].dPC != dPC)) {
        idx = gDvmJit.pJitEntryTable[idx].u.info.chain;
    }

    if (gDvmJit.pJitEntryTable[idx].dPC != dPC) {
        /*
         * No match.  Aquire jitTableLock and find the last
         * slot in the chain. Possibly continue the chain walk in case
         * some other thread allocated the slot we were looking
         * at previuosly (perhaps even the dPC we're trying to enter).
         */
        dvmLockMutex(&gDvmJit.tableLock);
        /*
         * At this point, if .dPC is NULL, then the slot we're
         * looking at is the target slot from the primary hash
         * (the simple, and common case).  Otherwise we're going
         * to have to find a free slot and chain it.
         */
        MEM_BARRIER(); /* Make sure we reload [].dPC after lock */
        if (gDvmJit.pJitEntryTable[idx].dPC != NULL) {
            u4 prev;
            while (gDvmJit.pJitEntryTable[idx].u.info.chain != chainEndMarker) {
                if (gDvmJit.pJitEntryTable[idx].dPC == dPC) {
                    /* Another thread got there first for this dPC */
                    dvmUnlockMutex(&gDvmJit.tableLock);
                    return &gDvmJit.pJitEntryTable[idx];
                }
                idx = gDvmJit.pJitEntryTable[idx].u.info.chain;
            }
            /* Here, idx should be pointing to the last cell of an
             * active chain whose last member contains a valid dPC */
            assert(gDvmJit.pJitEntryTable[idx].dPC != NULL);
            /* Linear walk to find a free cell and add it to the end */
            prev = idx;
            while (true) {
                idx++;
                if (idx == chainEndMarker)
                    idx = 0;  /* Wraparound */
                if ((gDvmJit.pJitEntryTable[idx].dPC == NULL) ||
                    (idx == prev))
                    break;
            }
            if (idx != prev) {
                JitEntryInfoUnion oldValue;
                JitEntryInfoUnion newValue;
                /*
                 * Although we hold the lock so that noone else will
                 * be trying to update a chain field, the other fields
                 * packed into the word may be in use by other threads.
                 */
                do {
                    oldValue = gDvmJit.pJitEntryTable[prev].u;
                    newValue = oldValue;
                    newValue.info.chain = idx;
                } while (!ATOMIC_CMP_SWAP(
                         &gDvmJit.pJitEntryTable[prev].u.infoWord,
                         oldValue.infoWord, newValue.infoWord));
            }
        }
        if (gDvmJit.pJitEntryTable[idx].dPC == NULL) {
            /*
             * Initialize codeAddress and allocate the slot.  Must
             * happen in this order (since dPC is set, the entry is live.
             */
            gDvmJit.pJitEntryTable[idx].dPC = dPC;
            gDvmJit.jitTableEntriesUsed++;
        } else {
            /* Table is full */
            idx = chainEndMarker;
        }
        dvmUnlockMutex(&gDvmJit.tableLock);
    }
    return (idx == chainEndMarker) ? NULL : &gDvmJit.pJitEntryTable[idx];
}
/*
 * Register the translated code pointer into the JitTable.
 * NOTE: Once a codeAddress field transitions from initial state to
 * JIT'd code, it must not be altered without first halting all
 * threads.  This routine should only be called by the compiler
 * thread.
 */
void dvmJitSetCodeAddr(const u2* dPC, void *nPC, JitInstructionSetType set) {
    JitEntryInfoUnion oldValue;
    JitEntryInfoUnion newValue;
    JitEntry *jitEntry = dvmJitLookupAndAdd(dPC);
    assert(jitEntry);
    /* Note: order of update is important */
    do {
        oldValue = jitEntry->u;
        newValue = oldValue;
        newValue.info.instructionSet = set;
    } while (!ATOMIC_CMP_SWAP(
             &jitEntry->u.infoWord,
             oldValue.infoWord, newValue.infoWord));
    jitEntry->codeAddress = nPC;
}

/*
 * Determine if valid trace-bulding request is active.  Return true
 * if we need to abort and switch back to the fast interpreter, false
 * otherwise.  NOTE: may be called even when trace selection is not being
 * requested
 */
bool dvmJitCheckTraceRequest(Thread* self, InterpState* interpState)
{
    bool res = false;         /* Assume success */
    int i;
    /*
     * If previous trace-building attempt failed, force it's head to be
     * interpret-only.
     */
    if (gDvmJit.pJitEntryTable != NULL) {
        /* Two-level filtering scheme */
        for (i=0; i< JIT_TRACE_THRESH_FILTER_SIZE; i++) {
            if (interpState->pc == interpState->threshFilter[i]) {
                break;
            }
        }
        if (i == JIT_TRACE_THRESH_FILTER_SIZE) {
            /*
             * Use random replacement policy - otherwise we could miss a large
             * loop that contains more traces than the size of our filter array.
             */
            i = rand() % JIT_TRACE_THRESH_FILTER_SIZE;
            interpState->threshFilter[i] = interpState->pc;
            res = true;
        }

        /* If stress mode (threshold <= 6), always translate */
        res &= (gDvmJit.threshold > 6);

        /*
         * If the compiler is backlogged, or if a debugger or profiler is
         * active, cancel any JIT actions
         */
        if (res || (gDvmJit.compilerQueueLength >= gDvmJit.compilerHighWater)
            || gDvm.debuggerActive || self->suspendCount
#if defined(WITH_PROFILER)
                 || gDvm.activeProfilers
#endif
                                             ) {
            if (interpState->jitState != kJitOff) {
                interpState->jitState = kJitNormal;
            }
        } else if (interpState->jitState == kJitTSelectRequest) {
            JitEntry *slot = dvmJitLookupAndAdd(interpState->pc);
            if (slot == NULL) {
                /*
                 * Table is full.  This should have been
                 * detected by the compiler thread and the table
                 * resized before we run into it here.  Assume bad things
                 * are afoot and disable profiling.
                 */
                interpState->jitState = kJitTSelectAbort;
                LOGD("JIT: JitTable full, disabling profiling");
                dvmJitStopTranslationRequests();
            } else if (slot->u.info.traceConstruction) {
                /*
                 * Trace request already in progress, but most likely it
                 * aborted without cleaning up.  Assume the worst and
                 * mark trace head as untranslatable.  If we're wrong,
                 * the compiler thread will correct the entry when the
                 * translation is completed.  The downside here is that
                 * some existing translation may chain to the interpret-only
                 * template instead of the real translation during this
                 * window.  Performance, but not correctness, issue.
                 */
                interpState->jitState = kJitTSelectAbort;
                resetTracehead(interpState, slot);
            } else if (slot->codeAddress) {
                 /* Nothing to do here - just return */
                interpState->jitState = kJitTSelectAbort;
            } else {
                /*
                 * Mark request.  Note, we are not guaranteed exclusivity
                 * here.  A window exists for another thread to be
                 * attempting to build this same trace.  Rather than
                 * bear the cost of locking, we'll just allow that to
                 * happen.  The compiler thread, if it chooses, can
                 * discard redundant requests.
                 */
                setTraceConstruction(slot, true);
            }
        }
        switch (interpState->jitState) {
            case kJitTSelectRequest:
                 interpState->jitState = kJitTSelect;
                 interpState->currTraceHead = interpState->pc;
                 interpState->currTraceRun = 0;
                 interpState->totalTraceLen = 0;
                 interpState->currRunHead = interpState->pc;
                 interpState->currRunLen = 0;
                 interpState->trace[0].frag.startOffset =
                       interpState->pc - interpState->method->insns;
                 interpState->trace[0].frag.numInsts = 0;
                 interpState->trace[0].frag.runEnd = false;
                 interpState->trace[0].frag.hint = kJitHintNone;
                 interpState->lastPC = 0;
                 break;
            case kJitTSelect:
            case kJitTSelectAbort:
                 res = true;
            case kJitSingleStep:
            case kJitSingleStepEnd:
            case kJitOff:
            case kJitNormal:
#if defined(WITH_SELF_VERIFICATION)
            case kJitSelfVerification:
#endif
                break;
            default:
                LOGE("Unexpected JIT state: %d", interpState->jitState);
                dvmAbort();
        }
    }
    return res;
}

/*
 * Resizes the JitTable.  Must be a power of 2, and returns true on failure.
 * Stops all threads, and thus is a heavyweight operation.
 */
bool dvmJitResizeJitTable( unsigned int size )
{
    JitEntry *pNewTable;
    JitEntry *pOldTable;
    u4 newMask;
    unsigned int oldSize;
    unsigned int i;

    assert(gDvmJit.pJitEntryTable != NULL);
    assert(size && !(size & (size - 1)));   /* Is power of 2? */

    LOGD("Jit: resizing JitTable from %d to %d", gDvmJit.jitTableSize, size);

    newMask = size - 1;

    if (size <= gDvmJit.jitTableSize) {
        return true;
    }

    pNewTable = (JitEntry*)calloc(size, sizeof(*pNewTable));
    if (pNewTable == NULL) {
        return true;
    }
    for (i=0; i< size; i++) {
        pNewTable[i].u.info.chain = size;  /* Initialize chain termination */
    }

    /* Stop all other interpreting/jit'ng threads */
    dvmSuspendAllThreads(SUSPEND_FOR_TBL_RESIZE);

    pOldTable = gDvmJit.pJitEntryTable;
    oldSize = gDvmJit.jitTableSize;

    dvmLockMutex(&gDvmJit.tableLock);
    gDvmJit.pJitEntryTable = pNewTable;
    gDvmJit.jitTableSize = size;
    gDvmJit.jitTableMask = size - 1;
    gDvmJit.jitTableEntriesUsed = 0;
    dvmUnlockMutex(&gDvmJit.tableLock);

    for (i=0; i < oldSize; i++) {
        if (pOldTable[i].dPC) {
            JitEntry *p;
            u2 chain;
            p = dvmJitLookupAndAdd(pOldTable[i].dPC);
            p->dPC = pOldTable[i].dPC;
            /*
             * Compiler thread may have just updated the new entry's
             * code address field, so don't blindly copy null.
             */
            if (pOldTable[i].codeAddress != NULL) {
                p->codeAddress = pOldTable[i].codeAddress;
            }
            /* We need to preserve the new chain field, but copy the rest */
            dvmLockMutex(&gDvmJit.tableLock);
            chain = p->u.info.chain;
            p->u = pOldTable[i].u;
            p->u.info.chain = chain;
            dvmUnlockMutex(&gDvmJit.tableLock);
        }
    }

    free(pOldTable);

    /* Restart the world */
    dvmResumeAllThreads(SUSPEND_FOR_TBL_RESIZE);

    return false;
}

/*
 * Reset the JitTable to the initial clean state.
 */
void dvmJitResetTable(void)
{
    JitEntry *jitEntry = gDvmJit.pJitEntryTable;
    unsigned int size = gDvmJit.jitTableSize;
    unsigned int i;

    dvmLockMutex(&gDvmJit.tableLock);
    memset((void *) jitEntry, 0, sizeof(JitEntry) * size);
    for (i=0; i< size; i++) {
        jitEntry[i].u.info.chain = size;  /* Initialize chain termination */
    }
    gDvmJit.jitTableEntriesUsed = 0;
    dvmUnlockMutex(&gDvmJit.tableLock);
}

/*
 * Float/double conversion requires clamping to min and max of integer form.  If
 * target doesn't support this normally, use these.
 */
s8 dvmJitd2l(double d)
{
    static const double kMaxLong = (double)(s8)0x7fffffffffffffffULL;
    static const double kMinLong = (double)(s8)0x8000000000000000ULL;
    if (d >= kMaxLong)
        return (s8)0x7fffffffffffffffULL;
    else if (d <= kMinLong)
        return (s8)0x8000000000000000ULL;
    else if (d != d) // NaN case
        return 0;
    else
        return (s8)d;
}

s8 dvmJitf2l(float f)
{
    static const float kMaxLong = (float)(s8)0x7fffffffffffffffULL;
    static const float kMinLong = (float)(s8)0x8000000000000000ULL;
    if (f >= kMaxLong)
        return (s8)0x7fffffffffffffffULL;
    else if (f <= kMinLong)
        return (s8)0x8000000000000000ULL;
    else if (f != f) // NaN case
        return 0;
    else
        return (s8)f;
}


#endif /* WITH_JIT */
