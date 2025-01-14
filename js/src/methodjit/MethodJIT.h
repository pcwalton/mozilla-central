/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla SpiderMonkey JavaScript 1.9 code, released
 * May 28, 2008.
 *
 * The Initial Developer of the Original Code is
 *   Brendan Eich <brendan@mozilla.org>
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL. 
 *
 * ***** END LICENSE BLOCK ***** */

#if !defined jsjaeger_h__ && defined JS_METHODJIT
#define jsjaeger_h__

#include "jscntxt.h"
#include "jscompartment.h"

#include "assembler/assembler/MacroAssemblerCodeRef.h"
#include "assembler/assembler/CodeLocation.h"

#if !defined JS_CPU_X64 && \
    !defined JS_CPU_X86 && \
    !defined JS_CPU_SPARC && \
    !defined JS_CPU_ARM
# error "Oh no, you should define a platform so this compiles."
#endif

#if !defined(JS_NUNBOX32) && !defined(JS_PUNBOX64)
# error "No boxing format selected."
#endif

namespace js {

namespace mjit { struct JITScript; }

struct VMFrame
{
#if defined(JS_CPU_SPARC)
    void *savedL0;
    void *savedL1;
    void *savedL2;
    void *savedL3;
    void *savedL4;
    void *savedL5;
    void *savedL6;
    void *savedL7;
    void *savedI0;
    void *savedI1;
    void *savedI2;
    void *savedI3;
    void *savedI4;
    void *savedI5;
    void *savedI6;
    void *savedI7;

    void *str_p;

    void *outgoing_p0;
    void *outgoing_p1;
    void *outgoing_p2;
    void *outgoing_p3;
    void *outgoing_p4;
    void *outgoing_p5;

    void *outgoing_p6;

    void *reserve_0;
    void *reserve_1;
#endif

    union Arguments {
        struct {
            void *ptr;
            void *ptr2;
        } x;
        struct {
            uint32 lazyArgsObj;
            uint32 dynamicArgc;
        } call;
    } u;

    VMFrame      *previous;
    void         *scratch;
    FrameRegs    regs;
    JSContext    *cx;
    Value        *stackLimit;
    StackFrame   *entryfp;
    FrameRegs    *oldregs;
    JSRejoinState stubRejoin;  /* How to rejoin if inside a call from an IC stub. */

#if defined(JS_CPU_X86)
    void         *unused0, *unused1;  /* For 16 byte alignment */
#endif

#if defined(JS_CPU_X86)
    void *savedEBX;
    void *savedEDI;
    void *savedESI;
    void *savedEBP;
    void *savedEIP;

# ifdef JS_NO_FASTCALL
    inline void** returnAddressLocation() {
        return reinterpret_cast<void**>(this) - 5;
    }
# else
    inline void** returnAddressLocation() {
        return reinterpret_cast<void**>(this) - 1;
    }
# endif

    /* The gap between ebp and esp in JaegerTrampoline frames on X86 platforms. */
    static const uint32 STACK_BASE_DIFFERENCE = 0x38;

#elif defined(JS_CPU_X64)
    void *savedRBX;
# ifdef _WIN64
    void *savedRSI;
    void *savedRDI;
# endif
    void *savedR15;
    void *savedR14;
    void *savedR13;
    void *savedR12;
    void *savedRBP;
    void *savedRIP;

# ifdef _WIN64
    inline void** returnAddressLocation() {
        return reinterpret_cast<void**>(this) - 5;
    }
# else
    inline void** returnAddressLocation() {
        return reinterpret_cast<void**>(this) - 1;
    }
# endif

#elif defined(JS_CPU_ARM)
    void *savedR4;
    void *savedR5;
    void *savedR6;
    void *savedR7;
    void *savedR8;
    void *savedR9;
    void *savedR10;
    void *savedR11;
    void *savedLR;

    inline void** returnAddressLocation() {
        return reinterpret_cast<void**>(this) - 1;
    }
#elif defined(JS_CPU_SPARC)
    JSStackFrame *topRetrunAddr;
    void* veneerReturn;
    void* _align;
    inline void** returnAddressLocation() {
        return reinterpret_cast<void**>(&this->veneerReturn);
    }
#else
# error "The VMFrame layout isn't defined for your processor architecture!"
#endif

    JSRuntime *runtime() { return cx->runtime; }

    /*
     * Get the current frame and JIT. Note that these are NOT stable in case
     * of recompilations; all code which expects these to be stable should
     * check that cx->recompilations() has not changed across a call that could
     * trigger recompilation (pretty much any time the VM is called into).
     */
    StackFrame *fp() { return regs.fp(); }
    mjit::JITScript *jit() { return fp()->jit(); }

    /* Get the inner script/PC in case of inlining. */
    inline JSScript *script();
    inline jsbytecode *pc();

#if defined(JS_CPU_SPARC)
    static const size_t offsetOfFp = 30 * sizeof(void *) + FrameRegs::offsetOfFp;
    static const size_t offsetOfInlined = 30 * sizeof(void *) + FrameRegs::offsetOfInlined;
#else
    static const size_t offsetOfFp = 4 * sizeof(void *) + FrameRegs::offsetOfFp;
    static const size_t offsetOfInlined = 4 * sizeof(void *) + FrameRegs::offsetOfInlined;
#endif

    static void staticAssert() {
        JS_STATIC_ASSERT(offsetOfFp == offsetof(VMFrame, regs) + FrameRegs::offsetOfFp);
        JS_STATIC_ASSERT(offsetOfInlined == offsetof(VMFrame, regs) + FrameRegs::offsetOfInlined);
    }
};

#if defined(JS_CPU_ARM) || defined(JS_CPU_SPARC)
// WARNING: Do not call this function directly from C(++) code because it is not ABI-compliant.
extern "C" void JaegerStubVeneer(void);
#endif

namespace mjit {

/*
 * For a C++ or scripted call made from JIT code, indicates properties of the
 * register and stack state after the call finishes, which RejoinInterpreter
 * must use to construct a coherent state for rejoining into the interpreter.
 */
enum RejoinState {
    /*
     * Return value of call at this bytecode is held in ReturnReg_{Data,Type}
     * and needs to be restored before starting the next bytecode. f.regs.pc
     * is *not* intact when rejoining from a scripted call (unlike all other
     * rejoin states). The pc's offset into the script is stored in the upper
     * 31 bits of the rejoin state, and the remaining values for RejoinState
     * are shifted left by one in stack frames to leave the lower bit set only
     * for scripted calls.
     */
    REJOIN_SCRIPTED = 1,

    /* Recompilations and frame expansion are impossible for this call. */
    REJOIN_NONE,

    /* State is coherent for the start of the current bytecode. */
    REJOIN_RESUME,

    /*
     * State is coherent for the start of the current bytecode, which is a TRAP
     * that has already been invoked and should not be invoked again.
     */
    REJOIN_TRAP,

    /* State is coherent for the start of the next (fallthrough) bytecode. */
    REJOIN_FALLTHROUGH,

    /*
     * As for REJOIN_FALLTHROUGH, but holds a reference on the compartment's
     * orphaned native pools which needs to be reclaimed by InternalInterpret.
     * The return value needs to be adjusted if REJOIN_NATIVE_LOWERED, and
     * REJOIN_NATIVE_GETTER is for ABI calls made for property accesses.
     */
    REJOIN_NATIVE,
    REJOIN_NATIVE_LOWERED,
    REJOIN_NATIVE_GETTER,

    /*
     * Dummy rejoin stored in VMFrames to indicate they return into a native
     * stub (and their FASTCALL return address should not be observed) but
     * that they have already been patched and can be ignored.
     */
    REJOIN_NATIVE_PATCHED,

    /* Call returns a payload, which should be pushed before starting next bytecode. */
    REJOIN_PUSH_BOOLEAN,
    REJOIN_PUSH_OBJECT,

    /* Call returns an object, which should be assigned to a local per the current bytecode. */
    REJOIN_DEFLOCALFUN,

    /*
     * During the prologue of constructing scripts, after the function's
     * .prototype property has been fetched.
     */
    REJOIN_THIS_PROTOTYPE,

    /*
     * Type check on arguments failed during prologue, need stack check and
     * the rest of the JIT prologue before the script can execute.
     */
    REJOIN_CHECK_ARGUMENTS,

    /*
     * The script's jitcode was discarded after marking an outer function as
     * reentrant or due to a GC while creating a call object.
     */
    REJOIN_FUNCTION_PROLOGUE,

    /*
     * State after calling a stub which returns a JIT code pointer for a call
     * or NULL for an already-completed call.
     */
    REJOIN_CALL_PROLOGUE,
    REJOIN_CALL_PROLOGUE_LOWERED_CALL,
    REJOIN_CALL_PROLOGUE_LOWERED_APPLY,

    /* Triggered a recompilation while placing the arguments to an apply on the stack. */
    REJOIN_CALL_SPLAT,

    /* FALLTHROUGH ops which can be implemented as part of an IncOp. */
    REJOIN_GETTER,
    REJOIN_POS,
    REJOIN_BINARY,

    /*
     * For an opcode fused with IFEQ/IFNE, call returns a boolean indicating
     * the result of the comparison and whether to take or not take the branch.
     */
    REJOIN_BRANCH
};

/* Helper to watch for recompilation and frame expansion activity on a compartment. */
struct RecompilationMonitor
{
    JSContext *cx;

    /*
     * If either inline frame expansion or recompilation occurs, then ICs and
     * stubs should not depend on the frame or JITs being intact. The two are
     * separated for logging.
     */
    unsigned recompilations;
    unsigned frameExpansions;

    /* If a GC occurs it may discard jit code on the stack. */
    unsigned gcNumber;

    RecompilationMonitor(JSContext *cx)
        : cx(cx),
          recompilations(cx->compartment->types.recompilations),
          frameExpansions(cx->compartment->types.frameExpansions),
          gcNumber(cx->runtime->gcNumber)
    {}

    bool recompiled() {
        return cx->compartment->types.recompilations != recompilations
            || cx->compartment->types.frameExpansions != frameExpansions
            || cx->runtime->gcNumber != gcNumber;
    }
};

/*
 * Trampolines to force returns from jit code.
 * See also TrampolineCompiler::generateForceReturn(Fast).
 */
struct Trampolines {
    typedef void (*TrampolinePtr)();

    TrampolinePtr       forceReturn;
    JSC::ExecutablePool *forceReturnPool;

#if (defined(JS_NO_FASTCALL) && defined(JS_CPU_X86)) || defined(_WIN64)
    TrampolinePtr       forceReturnFast;
    JSC::ExecutablePool *forceReturnFastPool;
#endif
};

/* Result status of executing mjit code on a frame. */
enum JaegerStatus
{
    /* Entry frame finished, and is throwing an exception. */
    Jaeger_Throwing = 0,

    /* Entry frame finished, and is returning. */
    Jaeger_Returned = 1,

    /*
     * Entry frame did not finish. cx->regs reflects where to resume execution.
     * This result is only possible if 'partial' is passed as true below.
     */
    Jaeger_Unfinished = 2,

    /*
     * As for Unfinished, but stopped after a TRAP triggered recompilation.
     * The trap has been reinstalled, but should not execute again when
     * resuming execution.
     */
    Jaeger_UnfinishedAtTrap = 3
};

/*
 * Method JIT compartment data. Currently, there is exactly one per
 * JS compartment. It would be safe for multiple JS compartments to
 * share a JaegerCompartment as long as only one thread can enter
 * the JaegerCompartment at a time.
 */
class JaegerCompartment {
    JSC::ExecutableAllocator *execAlloc_;    // allocator for jit code
    Trampolines              trampolines;    // force-return trampolines
    VMFrame                  *activeFrame_;  // current active VMFrame
    JaegerStatus             lastUnfinished_;// result status of last VM frame,
                                             // if unfinished

    void Finish();

  public:
    bool Initialize();

    JaegerCompartment();
    ~JaegerCompartment() { Finish(); }

    JSC::ExecutableAllocator *execAlloc() {
        return execAlloc_;
    }

    VMFrame *activeFrame() {
        return activeFrame_;
    }

    void pushActiveFrame(VMFrame *f) {
        JS_ASSERT(!lastUnfinished_);
        f->previous = activeFrame_;
        f->scratch = NULL;
        activeFrame_ = f;
    }

    void popActiveFrame() {
        JS_ASSERT(activeFrame_);
        activeFrame_ = activeFrame_->previous;
    }

    void setLastUnfinished(JaegerStatus status) {
        JS_ASSERT(!lastUnfinished_);
        lastUnfinished_ = status;
    }

    JaegerStatus lastUnfinished() {
        JaegerStatus result = lastUnfinished_;
        lastUnfinished_ = (JaegerStatus) 0;
        return result;
    }

    void *forceReturnFromExternC() const {
        return JS_FUNC_TO_DATA_PTR(void *, trampolines.forceReturn);
    }

    void *forceReturnFromFastCall() const {
#if (defined(JS_NO_FASTCALL) && defined(JS_CPU_X86)) || defined(_WIN64)
        return JS_FUNC_TO_DATA_PTR(void *, trampolines.forceReturnFast);
#else
        return JS_FUNC_TO_DATA_PTR(void *, trampolines.forceReturn);
#endif
    }

    /*
     * References held on pools created for native ICs, where the IC was
     * destroyed and we are waiting for the pool to finish use and jump
     * into the interpoline.
     */
    Vector<StackFrame *, 8, SystemAllocPolicy> orphanedNativeFrames;
    Vector<JSC::ExecutablePool *, 8, SystemAllocPolicy> orphanedNativePools;
};

/*
 * Allocation policy for compiler jstl objects. The goal is to free the
 * compiler from having to check and propagate OOM after every time we
 * append to a vector. We do this by reporting OOM to the engine and
 * setting a flag on the compiler when OOM occurs. The compiler is required
 * to check for OOM only before trying to use the contents of the list.
 */
class CompilerAllocPolicy : public TempAllocPolicy
{
    bool *oomFlag;

    void *checkAlloc(void *p) {
        if (!p)
            *oomFlag = true;
        return p;
    }

  public:
    CompilerAllocPolicy(JSContext *cx, bool *oomFlag)
    : TempAllocPolicy(cx), oomFlag(oomFlag) {}
    CompilerAllocPolicy(JSContext *cx, Compiler &compiler);

    void *malloc_(size_t bytes) { return checkAlloc(TempAllocPolicy::malloc_(bytes)); }
    void *realloc_(void *p, size_t oldBytes, size_t bytes) {
        return checkAlloc(TempAllocPolicy::realloc_(p, oldBytes, bytes));
    }
};

namespace ic {
# if defined JS_POLYIC
    struct PICInfo;
    struct GetElementIC;
    struct SetElementIC;
# endif
# if defined JS_MONOIC
    struct GetGlobalNameIC;
    struct SetGlobalNameIC;
    struct EqualityICInfo;
    struct TraceICInfo;
    struct CallICInfo;
# endif
}
}

typedef void (JS_FASTCALL *VoidStub)(VMFrame &);
typedef void (JS_FASTCALL *VoidVpStub)(VMFrame &, Value *);
typedef void (JS_FASTCALL *VoidStubUInt32)(VMFrame &, uint32);
typedef void (JS_FASTCALL *VoidStubInt32)(VMFrame &, int32);
typedef JSBool (JS_FASTCALL *BoolStub)(VMFrame &);
typedef void * (JS_FASTCALL *VoidPtrStub)(VMFrame &);
typedef void * (JS_FASTCALL *VoidPtrStubPC)(VMFrame &, jsbytecode *);
typedef void * (JS_FASTCALL *VoidPtrStubUInt32)(VMFrame &, uint32);
typedef JSObject * (JS_FASTCALL *JSObjStub)(VMFrame &);
typedef JSObject * (JS_FASTCALL *JSObjStubUInt32)(VMFrame &, uint32);
typedef JSObject * (JS_FASTCALL *JSObjStubFun)(VMFrame &, JSFunction *);
typedef void (JS_FASTCALL *VoidStubFun)(VMFrame &, JSFunction *);
typedef JSObject * (JS_FASTCALL *JSObjStubJSObj)(VMFrame &, JSObject *);
typedef void (JS_FASTCALL *VoidStubAtom)(VMFrame &, JSAtom *);
typedef JSString * (JS_FASTCALL *JSStrStub)(VMFrame &);
typedef JSString * (JS_FASTCALL *JSStrStubUInt32)(VMFrame &, uint32);
typedef void (JS_FASTCALL *VoidStubJSObj)(VMFrame &, JSObject *);
typedef void (JS_FASTCALL *VoidStubPC)(VMFrame &, jsbytecode *);
typedef JSBool (JS_FASTCALL *BoolStubUInt32)(VMFrame &f, uint32);
#ifdef JS_MONOIC
typedef void (JS_FASTCALL *VoidStubCallIC)(VMFrame &, js::mjit::ic::CallICInfo *);
typedef void * (JS_FASTCALL *VoidPtrStubCallIC)(VMFrame &, js::mjit::ic::CallICInfo *);
typedef void (JS_FASTCALL *VoidStubGetGlobal)(VMFrame &, js::mjit::ic::GetGlobalNameIC *);
typedef void (JS_FASTCALL *VoidStubSetGlobal)(VMFrame &, js::mjit::ic::SetGlobalNameIC *);
typedef JSBool (JS_FASTCALL *BoolStubEqualityIC)(VMFrame &, js::mjit::ic::EqualityICInfo *);
typedef void * (JS_FASTCALL *VoidPtrStubTraceIC)(VMFrame &, js::mjit::ic::TraceICInfo *);
#endif
#ifdef JS_POLYIC
typedef void (JS_FASTCALL *VoidStubPIC)(VMFrame &, js::mjit::ic::PICInfo *);
typedef void (JS_FASTCALL *VoidStubGetElemIC)(VMFrame &, js::mjit::ic::GetElementIC *);
typedef void (JS_FASTCALL *VoidStubSetElemIC)(VMFrame &f, js::mjit::ic::SetElementIC *);
#endif

namespace mjit {

struct InlineFrame;
struct CallSite;

struct NativeMapEntry {
    size_t          bcOff;  /* bytecode offset in script */
    void            *ncode; /* pointer to native code */
};

/* Per-op counts of performance metrics. */
struct PCLengthEntry {
    double          codeLength; /* amount of inline code generated */
    double          picsLength; /* amount of PIC stub code generated */
};

/*
 * Pools and patch locations for managing stubs for non-FASTCALL C++ calls made
 * from native call and PropertyOp stubs. Ownership of these may be transferred
 * into the orphanedNativePools for the compartment.
 */
struct NativeCallStub {
    /* pc/inlined location of the stub. */
    jsbytecode *pc;
    CallSite *inlined;

    /* Pool for the stub, NULL if it has been removed from the script. */
    JSC::ExecutablePool *pool;

    /*
     * Fallthrough jump returning to jitcode which may be patched during
     * recompilation. On x64 this is an indirect jump to avoid issues with far
     * jumps on relative branches.
     */
#ifdef JS_CPU_X64
    JSC::CodeLocationDataLabelPtr jump;
#else
    JSC::CodeLocationJump jump;
#endif
};

struct JITScript {
    typedef JSC::MacroAssemblerCodeRef CodeRef;
    CodeRef         code;       /* pool & code addresses */

    JSScript        *script;

    void            *invokeEntry;       /* invoke address */
    void            *fastEntry;         /* cached entry, fastest */
    void            *arityCheckEntry;   /* arity check address */
    void            *argsCheckEntry;    /* arguments check address */

    PCLengthEntry   *pcLengths;         /* lengths for outer and inline frames */

    /*
     * This struct has several variable-length sections that are allocated on
     * the end:  nmaps, MICs, callICs, etc.  To save space -- worthwhile
     * because JITScripts are common -- we only record their lengths.  We can
     * find any of the sections from the lengths because we know their order.
     * Therefore, do not change the section ordering in finishThisUp() without
     * changing nMICs() et al as well.
     */
    uint32          nNmapPairs:31;      /* The NativeMapEntrys are sorted by .bcOff.
                                           .ncode values may not be NULL. */
    bool            singleStepMode:1;   /* compiled in "single step mode" */
    uint32          nInlineFrames;
    uint32          nCallSites;
    uint32          nRootedObjects;
#ifdef JS_MONOIC
    uint32          nGetGlobalNames;
    uint32          nSetGlobalNames;
    uint32          nCallICs;
    uint32          nEqualityICs;
    uint32          nTraceICs;
#endif
#ifdef JS_POLYIC
    uint32          nGetElems;
    uint32          nSetElems;
    uint32          nPICs;
#endif

#ifdef JS_MONOIC
    /* Inline cache at function entry for checking this/argument types. */
    JSC::CodeLocationLabel argsCheckStub;
    JSC::CodeLocationLabel argsCheckFallthrough;
    JSC::CodeLocationJump  argsCheckJump;
    JSC::ExecutablePool *argsCheckPool;
    void resetArgsCheck();
#endif

    /* List of inline caches jumping to the fastEntry. */
    JSCList          callers;

#ifdef JS_MONOIC
    // Additional ExecutablePools that IC stubs were generated into.
    typedef Vector<JSC::ExecutablePool *, 0, SystemAllocPolicy> ExecPoolVector;
    ExecPoolVector execPools;
#endif

    // Additional ExecutablePools for native call and getter stubs.
    Vector<NativeCallStub, 0, SystemAllocPolicy> nativeCallStubs;

    NativeMapEntry *nmap() const;
    js::mjit::InlineFrame *inlineFrames() const;
    js::mjit::CallSite *callSites() const;
    JSObject **rootedObjects() const;
#ifdef JS_MONOIC
    ic::GetGlobalNameIC *getGlobalNames() const;
    ic::SetGlobalNameIC *setGlobalNames() const;
    ic::CallICInfo *callICs() const;
    ic::EqualityICInfo *equalityICs() const;
    ic::TraceICInfo *traceICs() const;
#endif
#ifdef JS_POLYIC
    ic::GetElementIC *getElems() const;
    ic::SetElementIC *setElems() const;
    ic::PICInfo     *pics() const;
#endif

    ~JITScript();

    bool isValidCode(void *ptr) {
        char *jitcode = (char *)code.m_code.executableAddress();
        char *jcheck = (char *)ptr;
        return jcheck >= jitcode && jcheck < jitcode + code.m_size;
    }

    void purgeGetterPICs();

    void sweepCallICs(JSContext *cx);
    void purgeMICs();
    void purgePICs();

    void trace(JSTracer *trc);

    /* |usf| can be NULL here, in which case the fallback size computation will be used. */
    size_t scriptDataSize(JSUsableSizeFun usf);

    jsbytecode *nativeToPC(void *returnAddress, CallSite **pinline) const;

  private:
    /* Helpers used to navigate the variable-length sections. */
    char *commonSectionLimit() const;
    char *monoICSectionsLimit() const;
    char *polyICSectionsLimit() const;
};

void PurgeICs(JSContext *cx, JSScript *script);

/*
 * Execute the given mjit code. This is a low-level call and callers must
 * provide the same guarantees as JaegerShot/CheckStackAndEnterMethodJIT.
 */
JaegerStatus EnterMethodJIT(JSContext *cx, StackFrame *fp, void *code, Value *stackLimit,
                            bool partial);

/* Execute a method that has been JIT compiled. */
JaegerStatus JaegerShot(JSContext *cx, bool partial);

/* Drop into the middle of a method at an arbitrary point, and execute. */
JaegerStatus JaegerShotAtSafePoint(JSContext *cx, void *safePoint, bool partial);

enum CompileStatus
{
    Compile_Okay,
    Compile_Abort,        // abort compilation
    Compile_InlineAbort,  // inlining attempt failed, continue compilation
    Compile_Retry,        // static overflow or failed inline, try to recompile
    Compile_Error,        // OOM
    Compile_Skipped
};

void JS_FASTCALL
ProfileStubCall(VMFrame &f);

CompileStatus JS_NEVER_INLINE
TryCompile(JSContext *cx, JSScript *script, bool construct);

void
ReleaseScriptCode(JSContext *cx, JSScript *script, bool construct);

inline void
ReleaseScriptCode(JSContext *cx, JSScript *script)
{
    if (script->jitCtor)
        mjit::ReleaseScriptCode(cx, script, true);
    if (script->jitNormal)
        mjit::ReleaseScriptCode(cx, script, false);
}

// Expand all stack frames inlined by the JIT within a compartment.
void
ExpandInlineFrames(JSCompartment *compartment);

// Return all VMFrames in a compartment to the interpreter. This must be
// followed by destroying all JIT code in the compartment.
void
ClearAllFrames(JSCompartment *compartment);

// Information about a frame inlined during compilation.
struct InlineFrame
{
    InlineFrame *parent;
    jsbytecode *parentpc;
    JSFunction *fun;

    // Total distance between the start of the outer JSStackFrame and the start
    // of this frame, in multiples of sizeof(Value).
    uint32 depth;
};

struct CallSite
{
    uint32 codeOffset;
    uint32 inlineIndex;
    uint32 pcOffset;
    RejoinState rejoin;

    void initialize(uint32 codeOffset, uint32 inlineIndex, uint32 pcOffset, RejoinState rejoin) {
        this->codeOffset = codeOffset;
        this->inlineIndex = inlineIndex;
        this->pcOffset = pcOffset;
        this->rejoin = rejoin;
    }

    bool isTrap() const {
        return rejoin == REJOIN_TRAP;
    }
};

/*
 * Re-enables a tracepoint in the method JIT. When full is true, we
 * also reset the iteration counter.
 */
void
ResetTraceHint(JSScript *script, jsbytecode *pc, uint16_t index, bool full);

uintN
GetCallTargetCount(JSScript *script, jsbytecode *pc);

void
DumpAllProfiles(JSContext *cx);

inline void * bsearch_nmap(NativeMapEntry *nmap, size_t nPairs, size_t bcOff)
{
    size_t lo = 1, hi = nPairs;
    while (1) {
        /* current unsearched space is from lo-1 to hi-1, inclusive. */
        if (lo > hi)
            return NULL; /* not found */
        size_t mid       = (lo + hi) / 2;
        size_t bcOff_mid = nmap[mid-1].bcOff;
        if (bcOff < bcOff_mid) {
            hi = mid-1;
            continue;
        } 
        if (bcOff > bcOff_mid) {
            lo = mid+1;
            continue;
        }
        return nmap[mid-1].ncode;
    }
}

} /* namespace mjit */

inline JSScript *
VMFrame::script()
{
    if (regs.inlined())
        return jit()->inlineFrames()[regs.inlined()->inlineIndex].fun->script();
    return fp()->script();
}

inline jsbytecode *
VMFrame::pc()
{
    if (regs.inlined())
        return script()->code + regs.inlined()->pcOffset;
    return regs.pc;
}

} /* namespace js */

inline void *
JSScript::maybeNativeCodeForPC(bool constructing, jsbytecode *pc)
{
    js::mjit::JITScript *jit = getJIT(constructing);
    if (!jit)
        return NULL;
    JS_ASSERT(pc >= code && pc < code + length);
    return bsearch_nmap(jit->nmap(), jit->nNmapPairs, (size_t)(pc - code));
}

inline void *
JSScript::nativeCodeForPC(bool constructing, jsbytecode *pc)
{
    js::mjit::JITScript *jit = getJIT(constructing);
    JS_ASSERT(pc >= code && pc < code + length);
    void* native = bsearch_nmap(jit->nmap(), jit->nNmapPairs, (size_t)(pc - code));
    JS_ASSERT(native);
    return native;
}

extern "C" void JaegerTrampolineReturn();
extern "C" void JaegerInterpoline();
extern "C" void JaegerInterpolineScripted();

#if defined(_MSC_VER) || defined(_WIN64)
extern "C" void *JaegerThrowpoline(js::VMFrame *vmFrame);
#else
extern "C" void JaegerThrowpoline();
#endif

#if (defined(JS_NO_FASTCALL) && defined(JS_CPU_X86)) || defined(_WIN64)
extern "C" void JaegerInterpolinePatched();
#endif

#endif /* jsjaeger_h__ */

