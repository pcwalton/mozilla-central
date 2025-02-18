/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
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
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
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

/*
 * JS bytecode generation.
 */
#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#include <new>
#include <string.h>
#include "jstypes.h"
#include "jsstdint.h"
#include "jsarena.h"
#include "jsutil.h"
#include "jsbit.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jsemit.h"
#include "jsfun.h"
#include "jsnum.h"
#include "jsopcode.h"
#include "jsparse.h"
#include "jsregexp.h"
#include "jsscan.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsautooplen.h"        // generated headers last
#include "jsstaticcheck.h"

#include "jsatominlines.h"
#include "jsobjinlines.h"
#include "jsscopeinlines.h"
#include "jsscriptinlines.h"

#include "frontend/ParseMaps-inl.h"

/* Allocation chunk counts, must be powers of two in general. */
#define BYTECODE_CHUNK_LENGTH  1024    /* initial bytecode chunk length */
#define SRCNOTE_CHUNK_LENGTH   1024    /* initial srcnote chunk length */

/* Macros to compute byte sizes from typed element counts. */
#define BYTECODE_SIZE(n)        ((n) * sizeof(jsbytecode))
#define SRCNOTE_SIZE(n)         ((n) * sizeof(jssrcnote))

using namespace js;
using namespace js::gc;

static JSBool
NewTryNote(JSContext *cx, JSCodeGenerator *cg, JSTryNoteKind kind,
           uintN stackDepth, size_t start, size_t end);

static bool
EmitIndexOp(JSContext *cx, JSOp op, uintN index, JSCodeGenerator *cg,
            JSOp *psuffix = NULL);

static JSBool
EmitLeaveBlock(JSContext *cx, JSCodeGenerator *cg, JSOp op, JSObjectBox *box);

void
JSTreeContext::trace(JSTracer *trc)
{
    bindings.trace(trc);
}

JSCodeGenerator::JSCodeGenerator(Parser *parser, uintN lineno)
  : JSTreeContext(parser),
    atomIndices(parser->context),
    stackDepth(0), maxStackDepth(0),
    ntrynotes(0), lastTryNode(NULL),
    spanDeps(NULL), jumpTargets(NULL), jtFreeList(NULL),
    numSpanDeps(0), numJumpTargets(0), spanDepTodo(0),
    arrayCompDepth(0),
    emitLevel(0),
    constMap(parser->context),
    constList(parser->context),
    upvarIndices(parser->context),
    upvarMap(parser->context),
    globalUses(parser->context),
    globalMap(parser->context),
    closedArgs(parser->context),
    closedVars(parser->context),
    traceIndex(0),
    typesetCount(0)
{
    flags = TCF_COMPILING;
    memset(&prolog, 0, sizeof prolog);
    memset(&main, 0, sizeof main);
    current = &main;
    firstLine = prolog.currentLine = main.currentLine = lineno;
}

bool
JSCodeGenerator::init(JSContext *cx, JSTreeContext::InitBehavior ib)
{
    roLexdeps.init();
    return JSTreeContext::init(cx, ib) && constMap.init() && atomIndices.ensureMap(cx);
}

JSCodeGenerator::~JSCodeGenerator()
{
    JSContext *cx = parser->context;

    cx->free_(prolog.base);
    cx->free_(prolog.notes);
    cx->free_(main.base);
    cx->free_(main.notes);

    /* NB: non-null only after OOM. */
    if (spanDeps)
        cx->free_(spanDeps);
}

static ptrdiff_t
EmitCheck(JSContext *cx, JSCodeGenerator *cg, ptrdiff_t delta)
{
    jsbytecode *base = CG_BASE(cg);
    jsbytecode *newbase;
    jsbytecode *next = CG_NEXT(cg);
    jsbytecode *limit = CG_LIMIT(cg);
    ptrdiff_t offset = next - base;
    size_t minlength = offset + delta;

    if (next + delta > limit) {
        size_t newlength;
        if (!base) {
            JS_ASSERT(!next && !limit);
            newlength = BYTECODE_CHUNK_LENGTH;
            if (newlength < minlength)     /* make it bigger if necessary */
                newlength = RoundUpPow2(minlength);
            newbase = (jsbytecode *) cx->malloc_(BYTECODE_SIZE(newlength));
        } else {
            JS_ASSERT(base <= next && next <= limit);
            newlength = (limit - base) * 2;
            if (newlength < minlength)     /* make it bigger if necessary */
                newlength = RoundUpPow2(minlength);
            newbase = (jsbytecode *) cx->realloc_(base, BYTECODE_SIZE(newlength));
        }
        if (!newbase) {
            js_ReportOutOfMemory(cx);
            return -1;
        }
        JS_ASSERT(newlength >= size_t(offset + delta));
        CG_BASE(cg) = newbase;
        CG_LIMIT(cg) = newbase + newlength;
        CG_NEXT(cg) = newbase + offset;
    }
    return offset;
}

static void
UpdateDepth(JSContext *cx, JSCodeGenerator *cg, ptrdiff_t target)
{
    jsbytecode *pc;
    JSOp op;
    const JSCodeSpec *cs;
    uintN extra, nuses;
    intN ndefs;

    pc = CG_CODE(cg, target);
    op = (JSOp) *pc;
    cs = &js_CodeSpec[op];
#ifdef JS_TRACER
    extern uint8 js_opcode2extra[];
    extra = js_opcode2extra[op];
#else
    extra = 0;
#endif
    if ((cs->format & JOF_TMPSLOT_MASK) || extra) {
        uintN depth = (uintN) cg->stackDepth +
                      ((cs->format & JOF_TMPSLOT_MASK) >> JOF_TMPSLOT_SHIFT) +
                      extra;
        if (depth > cg->maxStackDepth)
            cg->maxStackDepth = depth;
    }

    nuses = js_GetStackUses(cs, op, pc);
    cg->stackDepth -= nuses;
    JS_ASSERT(cg->stackDepth >= 0);
    if (cg->stackDepth < 0) {
        char numBuf[12];
        TokenStream *ts;

        JS_snprintf(numBuf, sizeof numBuf, "%d", target);
        ts = &cg->parser->tokenStream;
        JS_ReportErrorFlagsAndNumber(cx, JSREPORT_WARNING,
                                     js_GetErrorMessage, NULL,
                                     JSMSG_STACK_UNDERFLOW,
                                     ts->getFilename() ? ts->getFilename() : "stdin",
                                     numBuf);
    }
    ndefs = cs->ndefs;
    if (ndefs < 0) {
        JSObject *blockObj;

        /* We just executed IndexParsedObject */
        JS_ASSERT(op == JSOP_ENTERBLOCK);
        JS_ASSERT(nuses == 0);
        blockObj = cg->objectList.lastbox->object;
        JS_ASSERT(blockObj->isStaticBlock());
        JS_ASSERT(blockObj->getSlot(JSSLOT_BLOCK_DEPTH).isUndefined());

        OBJ_SET_BLOCK_DEPTH(cx, blockObj, cg->stackDepth);
        ndefs = OBJ_BLOCK_COUNT(cx, blockObj);
    }
    cg->stackDepth += ndefs;
    if ((uintN)cg->stackDepth > cg->maxStackDepth)
        cg->maxStackDepth = cg->stackDepth;
}

static inline void
UpdateDecomposeLength(JSCodeGenerator *cg, uintN start)
{
    uintN end = CG_OFFSET(cg);
    JS_ASSERT(uintN(end - start) < 256);
    CG_CODE(cg, start)[-1] = end - start;
}

ptrdiff_t
js_Emit1(JSContext *cx, JSCodeGenerator *cg, JSOp op)
{
    ptrdiff_t offset = EmitCheck(cx, cg, 1);

    if (offset >= 0) {
        *CG_NEXT(cg)++ = (jsbytecode)op;
        UpdateDepth(cx, cg, offset);
    }
    return offset;
}

ptrdiff_t
js_Emit2(JSContext *cx, JSCodeGenerator *cg, JSOp op, jsbytecode op1)
{
    ptrdiff_t offset = EmitCheck(cx, cg, 2);

    if (offset >= 0) {
        jsbytecode *next = CG_NEXT(cg);
        next[0] = (jsbytecode)op;
        next[1] = op1;
        CG_NEXT(cg) = next + 2;
        UpdateDepth(cx, cg, offset);
    }
    return offset;
}

ptrdiff_t
js_Emit3(JSContext *cx, JSCodeGenerator *cg, JSOp op, jsbytecode op1,
         jsbytecode op2)
{
    ptrdiff_t offset = EmitCheck(cx, cg, 3);

    if (offset >= 0) {
        jsbytecode *next = CG_NEXT(cg);
        next[0] = (jsbytecode)op;
        next[1] = op1;
        next[2] = op2;
        CG_NEXT(cg) = next + 3;
        UpdateDepth(cx, cg, offset);
    }
    return offset;
}

ptrdiff_t
js_Emit5(JSContext *cx, JSCodeGenerator *cg, JSOp op, uint16 op1, uint16 op2)
{
    ptrdiff_t offset = EmitCheck(cx, cg, 5);

    if (offset >= 0) {
        jsbytecode *next = CG_NEXT(cg);
        next[0] = (jsbytecode)op;
        next[1] = UINT16_HI(op1);
        next[2] = UINT16_LO(op1);
        next[3] = UINT16_HI(op2);
        next[4] = UINT16_LO(op2);
        CG_NEXT(cg) = next + 5;
        UpdateDepth(cx, cg, offset);
    }
    return offset;
}

ptrdiff_t
js_EmitN(JSContext *cx, JSCodeGenerator *cg, JSOp op, size_t extra)
{
    ptrdiff_t length = 1 + (ptrdiff_t)extra;
    ptrdiff_t offset = EmitCheck(cx, cg, length);

    if (offset >= 0) {
        jsbytecode *next = CG_NEXT(cg);
        *next = (jsbytecode)op;
        memset(next + 1, 0, BYTECODE_SIZE(extra));
        CG_NEXT(cg) = next + length;

        /*
         * Don't UpdateDepth if op's use-count comes from the immediate
         * operand yet to be stored in the extra bytes after op.
         */
        if (js_CodeSpec[op].nuses >= 0)
            UpdateDepth(cx, cg, offset);
    }
    return offset;
}

/* XXX too many "... statement" L10N gaffes below -- fix via js.msg! */
const char js_with_statement_str[] = "with statement";
const char js_finally_block_str[]  = "finally block";
const char js_script_str[]         = "script";

static const char *statementName[] = {
    "label statement",       /* LABEL */
    "if statement",          /* IF */
    "else statement",        /* ELSE */
    "destructuring body",    /* BODY */
    "switch statement",      /* SWITCH */
    "block",                 /* BLOCK */
    js_with_statement_str,   /* WITH */
    "catch block",           /* CATCH */
    "try block",             /* TRY */
    js_finally_block_str,    /* FINALLY */
    js_finally_block_str,    /* SUBROUTINE */
    "do loop",               /* DO_LOOP */
    "for loop",              /* FOR_LOOP */
    "for/in loop",           /* FOR_IN_LOOP */
    "while loop",            /* WHILE_LOOP */
};

JS_STATIC_ASSERT(JS_ARRAY_LENGTH(statementName) == STMT_LIMIT);

static const char *
StatementName(JSCodeGenerator *cg)
{
    if (!cg->topStmt)
        return js_script_str;
    return statementName[cg->topStmt->type];
}

static void
ReportStatementTooLarge(JSContext *cx, JSCodeGenerator *cg)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NEED_DIET,
                         StatementName(cg));
}

/**
  Span-dependent instructions in JS bytecode consist of the jump (JOF_JUMP)
  and switch (JOF_LOOKUPSWITCH, JOF_TABLESWITCH) format opcodes, subdivided
  into unconditional (gotos and gosubs), and conditional jumps or branches
  (which pop a value, test it, and jump depending on its value).  Most jumps
  have just one immediate operand, a signed offset from the jump opcode's pc
  to the target bytecode.  The lookup and table switch opcodes may contain
  many jump offsets.

  Mozilla bug #80981 (http://bugzilla.mozilla.org/show_bug.cgi?id=80981) was
  fixed by adding extended "X" counterparts to the opcodes/formats (NB: X is
  suffixed to prefer JSOP_ORX thereby avoiding a JSOP_XOR name collision for
  the extended form of the JSOP_OR branch opcode).  The unextended or short
  formats have 16-bit signed immediate offset operands, the extended or long
  formats have 32-bit signed immediates.  The span-dependency problem consists
  of selecting as few long instructions as possible, or about as few -- since
  jumps can span other jumps, extending one jump may cause another to need to
  be extended.

  Most JS scripts are short, so need no extended jumps.  We optimize for this
  case by generating short jumps until we know a long jump is needed.  After
  that point, we keep generating short jumps, but each jump's 16-bit immediate
  offset operand is actually an unsigned index into cg->spanDeps, an array of
  JSSpanDep structs.  Each struct tells the top offset in the script of the
  opcode, the "before" offset of the jump (which will be the same as top for
  simplex jumps, but which will index further into the bytecode array for a
  non-initial jump offset in a lookup or table switch), the after "offset"
  adjusted during span-dependent instruction selection (initially the same
  value as the "before" offset), and the jump target (more below).

  Since we generate cg->spanDeps lazily, from within js_SetJumpOffset, we must
  ensure that all bytecode generated so far can be inspected to discover where
  the jump offset immediate operands lie within CG_CODE(cg).  But the bonus is
  that we generate span-dependency records sorted by their offsets, so we can
  binary-search when trying to find a JSSpanDep for a given bytecode offset,
  or the nearest JSSpanDep at or above a given pc.

  To avoid limiting scripts to 64K jumps, if the cg->spanDeps index overflows
  65534, we store SPANDEP_INDEX_HUGE in the jump's immediate operand.  This
  tells us that we need to binary-search for the cg->spanDeps entry by the
  jump opcode's bytecode offset (sd->before).

  Jump targets need to be maintained in a data structure that lets us look
  up an already-known target by its address (jumps may have a common target),
  and that also lets us update the addresses (script-relative, a.k.a. absolute
  offsets) of targets that come after a jump target (for when a jump below
  that target needs to be extended).  We use an AVL tree, implemented using
  recursion, but with some tricky optimizations to its height-balancing code
  (see http://www.cmcrossroads.com/bradapp/ftp/src/libs/C++/AvlTrees.html).

  A final wrinkle: backpatch chains are linked by jump-to-jump offsets with
  positive sign, even though they link "backward" (i.e., toward lower bytecode
  address).  We don't want to waste space and search time in the AVL tree for
  such temporary backpatch deltas, so we use a single-bit wildcard scheme to
  tag true JSJumpTarget pointers and encode untagged, signed (positive) deltas
  in JSSpanDep.target pointers, depending on whether the JSSpanDep has a known
  target, or is still awaiting backpatching.

  Note that backpatch chains would present a problem for BuildSpanDepTable,
  which inspects bytecode to build cg->spanDeps on demand, when the first
  short jump offset overflows.  To solve this temporary problem, we emit a
  proxy bytecode (JSOP_BACKPATCH; JSOP_BACKPATCH_POP for branch ops) whose
  nuses/ndefs counts help keep the stack balanced, but whose opcode format
  distinguishes its backpatch delta immediate operand from a normal jump
  offset.
 */
static int
BalanceJumpTargets(JSJumpTarget **jtp)
{
    JSJumpTarget *jt, *jt2, *root;
    int dir, otherDir, heightChanged;
    JSBool doubleRotate;

    jt = *jtp;
    JS_ASSERT(jt->balance != 0);

    if (jt->balance < -1) {
        dir = JT_RIGHT;
        doubleRotate = (jt->kids[JT_LEFT]->balance > 0);
    } else if (jt->balance > 1) {
        dir = JT_LEFT;
        doubleRotate = (jt->kids[JT_RIGHT]->balance < 0);
    } else {
        return 0;
    }

    otherDir = JT_OTHER_DIR(dir);
    if (doubleRotate) {
        jt2 = jt->kids[otherDir];
        *jtp = root = jt2->kids[dir];

        jt->kids[otherDir] = root->kids[dir];
        root->kids[dir] = jt;

        jt2->kids[dir] = root->kids[otherDir];
        root->kids[otherDir] = jt2;

        heightChanged = 1;
        root->kids[JT_LEFT]->balance = -JS_MAX(root->balance, 0);
        root->kids[JT_RIGHT]->balance = -JS_MIN(root->balance, 0);
        root->balance = 0;
    } else {
        *jtp = root = jt->kids[otherDir];
        jt->kids[otherDir] = root->kids[dir];
        root->kids[dir] = jt;

        heightChanged = (root->balance != 0);
        jt->balance = -((dir == JT_LEFT) ? --root->balance : ++root->balance);
    }

    return heightChanged;
}

typedef struct AddJumpTargetArgs {
    JSContext           *cx;
    JSCodeGenerator     *cg;
    ptrdiff_t           offset;
    JSJumpTarget        *node;
} AddJumpTargetArgs;

static int
AddJumpTarget(AddJumpTargetArgs *args, JSJumpTarget **jtp)
{
    JSJumpTarget *jt;
    int balanceDelta;

    jt = *jtp;
    if (!jt) {
        JSCodeGenerator *cg = args->cg;

        jt = cg->jtFreeList;
        if (jt) {
            cg->jtFreeList = jt->kids[JT_LEFT];
        } else {
            JS_ARENA_ALLOCATE_CAST(jt, JSJumpTarget *, &args->cx->tempPool,
                                   sizeof *jt);
            if (!jt) {
                js_ReportOutOfMemory(args->cx);
                return 0;
            }
        }
        jt->offset = args->offset;
        jt->balance = 0;
        jt->kids[JT_LEFT] = jt->kids[JT_RIGHT] = NULL;
        cg->numJumpTargets++;
        args->node = jt;
        *jtp = jt;
        return 1;
    }

    if (jt->offset == args->offset) {
        args->node = jt;
        return 0;
    }

    if (args->offset < jt->offset)
        balanceDelta = -AddJumpTarget(args, &jt->kids[JT_LEFT]);
    else
        balanceDelta = AddJumpTarget(args, &jt->kids[JT_RIGHT]);
    if (!args->node)
        return 0;

    jt->balance += balanceDelta;
    return (balanceDelta && jt->balance)
           ? 1 - BalanceJumpTargets(jtp)
           : 0;
}

#ifdef DEBUG_brendan
static int AVLCheck(JSJumpTarget *jt)
{
    int lh, rh;

    if (!jt) return 0;
    JS_ASSERT(-1 <= jt->balance && jt->balance <= 1);
    lh = AVLCheck(jt->kids[JT_LEFT]);
    rh = AVLCheck(jt->kids[JT_RIGHT]);
    JS_ASSERT(jt->balance == rh - lh);
    return 1 + JS_MAX(lh, rh);
}
#endif

static JSBool
SetSpanDepTarget(JSContext *cx, JSCodeGenerator *cg, JSSpanDep *sd,
                 ptrdiff_t off)
{
    AddJumpTargetArgs args;

    if (off < JUMPX_OFFSET_MIN || JUMPX_OFFSET_MAX < off) {
        ReportStatementTooLarge(cx, cg);
        return JS_FALSE;
    }

    args.cx = cx;
    args.cg = cg;
    args.offset = sd->top + off;
    args.node = NULL;
    AddJumpTarget(&args, &cg->jumpTargets);
    if (!args.node)
        return JS_FALSE;

#ifdef DEBUG_brendan
    AVLCheck(cg->jumpTargets);
#endif

    SD_SET_TARGET(sd, args.node);
    return JS_TRUE;
}

#define SPANDEPS_MIN            256
#define SPANDEPS_SIZE(n)        ((n) * sizeof(JSSpanDep))
#define SPANDEPS_SIZE_MIN       SPANDEPS_SIZE(SPANDEPS_MIN)

static JSBool
AddSpanDep(JSContext *cx, JSCodeGenerator *cg, jsbytecode *pc, jsbytecode *pc2,
           ptrdiff_t off)
{
    uintN index;
    JSSpanDep *sdbase, *sd;
    size_t size;

    index = cg->numSpanDeps;
    if (index + 1 == 0) {
        ReportStatementTooLarge(cx, cg);
        return JS_FALSE;
    }

    if ((index & (index - 1)) == 0 &&
        (!(sdbase = cg->spanDeps) || index >= SPANDEPS_MIN)) {
        size = sdbase ? SPANDEPS_SIZE(index) : SPANDEPS_SIZE_MIN / 2;
        sdbase = (JSSpanDep *) cx->realloc_(sdbase, size + size);
        if (!sdbase)
            return JS_FALSE;
        cg->spanDeps = sdbase;
    }

    cg->numSpanDeps = index + 1;
    sd = cg->spanDeps + index;
    sd->top = pc - CG_BASE(cg);
    sd->offset = sd->before = pc2 - CG_BASE(cg);

    if (js_CodeSpec[*pc].format & JOF_BACKPATCH) {
        /* Jump offset will be backpatched if off is a non-zero "bpdelta". */
        if (off != 0) {
            JS_ASSERT(off >= 1 + JUMP_OFFSET_LEN);
            if (off > BPDELTA_MAX) {
                ReportStatementTooLarge(cx, cg);
                return JS_FALSE;
            }
        }
        SD_SET_BPDELTA(sd, off);
    } else if (off == 0) {
        /* Jump offset will be patched directly, without backpatch chaining. */
        SD_SET_TARGET(sd, 0);
    } else {
        /* The jump offset in off is non-zero, therefore it's already known. */
        if (!SetSpanDepTarget(cx, cg, sd, off))
            return JS_FALSE;
    }

    if (index > SPANDEP_INDEX_MAX)
        index = SPANDEP_INDEX_HUGE;
    SET_SPANDEP_INDEX(pc2, index);
    return JS_TRUE;
}

static jsbytecode *
AddSwitchSpanDeps(JSContext *cx, JSCodeGenerator *cg, jsbytecode *pc)
{
    JSOp op;
    jsbytecode *pc2;
    ptrdiff_t off;
    jsint low, high;
    uintN njumps, indexlen;

    op = (JSOp) *pc;
    JS_ASSERT(op == JSOP_TABLESWITCH || op == JSOP_LOOKUPSWITCH);
    pc2 = pc;
    off = GET_JUMP_OFFSET(pc2);
    if (!AddSpanDep(cx, cg, pc, pc2, off))
        return NULL;
    pc2 += JUMP_OFFSET_LEN;
    if (op == JSOP_TABLESWITCH) {
        low = GET_JUMP_OFFSET(pc2);
        pc2 += JUMP_OFFSET_LEN;
        high = GET_JUMP_OFFSET(pc2);
        pc2 += JUMP_OFFSET_LEN;
        njumps = (uintN) (high - low + 1);
        indexlen = 0;
    } else {
        njumps = GET_UINT16(pc2);
        pc2 += UINT16_LEN;
        indexlen = INDEX_LEN;
    }
    while (njumps) {
        --njumps;
        pc2 += indexlen;
        off = GET_JUMP_OFFSET(pc2);
        if (!AddSpanDep(cx, cg, pc, pc2, off))
            return NULL;
        pc2 += JUMP_OFFSET_LEN;
    }
    return 1 + pc2;
}

static JSBool
BuildSpanDepTable(JSContext *cx, JSCodeGenerator *cg)
{
    jsbytecode *pc, *end;
    JSOp op;
    const JSCodeSpec *cs;
    ptrdiff_t off;

    pc = CG_BASE(cg) + cg->spanDepTodo;
    end = CG_NEXT(cg);
    while (pc != end) {
        JS_ASSERT(pc < end);
        op = (JSOp)*pc;
        cs = &js_CodeSpec[op];

        switch (JOF_TYPE(cs->format)) {
          case JOF_TABLESWITCH:
          case JOF_LOOKUPSWITCH:
            pc = AddSwitchSpanDeps(cx, cg, pc);
            if (!pc)
                return JS_FALSE;
            break;

          case JOF_JUMP:
            off = GET_JUMP_OFFSET(pc);
            if (!AddSpanDep(cx, cg, pc, pc, off))
                return JS_FALSE;
            /* FALL THROUGH */
          default:
            pc += cs->length;
            break;
        }
    }

    return JS_TRUE;
}

static JSSpanDep *
GetSpanDep(JSCodeGenerator *cg, jsbytecode *pc)
{
    uintN index;
    ptrdiff_t offset;
    int lo, hi, mid;
    JSSpanDep *sd;

    index = GET_SPANDEP_INDEX(pc);
    if (index != SPANDEP_INDEX_HUGE)
        return cg->spanDeps + index;

    offset = pc - CG_BASE(cg);
    lo = 0;
    hi = cg->numSpanDeps - 1;
    while (lo <= hi) {
        mid = (lo + hi) / 2;
        sd = cg->spanDeps + mid;
        if (sd->before == offset)
            return sd;
        if (sd->before < offset)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    JS_ASSERT(0);
    return NULL;
}

static JSBool
SetBackPatchDelta(JSContext *cx, JSCodeGenerator *cg, jsbytecode *pc,
                  ptrdiff_t delta)
{
    JSSpanDep *sd;

    JS_ASSERT(delta >= 1 + JUMP_OFFSET_LEN);
    if (!cg->spanDeps && delta < JUMP_OFFSET_MAX) {
        SET_JUMP_OFFSET(pc, delta);
        return JS_TRUE;
    }

    if (delta > BPDELTA_MAX) {
        ReportStatementTooLarge(cx, cg);
        return JS_FALSE;
    }

    if (!cg->spanDeps && !BuildSpanDepTable(cx, cg))
        return JS_FALSE;

    sd = GetSpanDep(cg, pc);
    JS_ASSERT(SD_GET_BPDELTA(sd) == 0);
    SD_SET_BPDELTA(sd, delta);
    return JS_TRUE;
}

static void
UpdateJumpTargets(JSJumpTarget *jt, ptrdiff_t pivot, ptrdiff_t delta)
{
    if (jt->offset > pivot) {
        jt->offset += delta;
        if (jt->kids[JT_LEFT])
            UpdateJumpTargets(jt->kids[JT_LEFT], pivot, delta);
    }
    if (jt->kids[JT_RIGHT])
        UpdateJumpTargets(jt->kids[JT_RIGHT], pivot, delta);
}

static JSSpanDep *
FindNearestSpanDep(JSCodeGenerator *cg, ptrdiff_t offset, int lo,
                   JSSpanDep *guard)
{
    int num, hi, mid;
    JSSpanDep *sdbase, *sd;

    num = cg->numSpanDeps;
    JS_ASSERT(num > 0);
    hi = num - 1;
    sdbase = cg->spanDeps;
    while (lo <= hi) {
        mid = (lo + hi) / 2;
        sd = sdbase + mid;
        if (sd->before == offset)
            return sd;
        if (sd->before < offset)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (lo == num)
        return guard;
    sd = sdbase + lo;
    JS_ASSERT(sd->before >= offset && (lo == 0 || sd[-1].before < offset));
    return sd;
}

static void
FreeJumpTargets(JSCodeGenerator *cg, JSJumpTarget *jt)
{
    if (jt->kids[JT_LEFT])
        FreeJumpTargets(cg, jt->kids[JT_LEFT]);
    if (jt->kids[JT_RIGHT])
        FreeJumpTargets(cg, jt->kids[JT_RIGHT]);
    jt->kids[JT_LEFT] = cg->jtFreeList;
    cg->jtFreeList = jt;
}

static JSBool
OptimizeSpanDeps(JSContext *cx, JSCodeGenerator *cg)
{
    jsbytecode *pc, *oldpc, *base, *limit, *next;
    JSSpanDep *sd, *sd2, *sdbase, *sdlimit, *sdtop, guard;
    ptrdiff_t offset, growth, delta, top, pivot, span, length, target;
    JSBool done;
    JSOp op;
    uint32 type;
    jssrcnote *sn, *snlimit;
    JSSrcNoteSpec *spec;
    uintN i, n, noteIndex;
    JSTryNode *tryNode;
    DebugOnly<int> passes = 0;

    base = CG_BASE(cg);
    sdbase = cg->spanDeps;
    sdlimit = sdbase + cg->numSpanDeps;
    offset = CG_OFFSET(cg);
    growth = 0;

    do {
        done = JS_TRUE;
        delta = 0;
        top = pivot = -1;
        sdtop = NULL;
        pc = NULL;
        op = JSOP_NOP;
        type = 0;
#ifdef DEBUG_brendan
        passes++;
#endif

        for (sd = sdbase; sd < sdlimit; sd++) {
            JS_ASSERT(JT_HAS_TAG(sd->target));
            sd->offset += delta;

            if (sd->top != top) {
                sdtop = sd;
                top = sd->top;
                JS_ASSERT(top == sd->before);
                pivot = sd->offset;
                pc = base + top;
                op = (JSOp) *pc;
                type = JOF_OPTYPE(op);
                if (JOF_TYPE_IS_EXTENDED_JUMP(type)) {
                    /*
                     * We already extended all the jump offset operands for
                     * the opcode at sd->top.  Jumps and branches have only
                     * one jump offset operand, but switches have many, all
                     * of which are adjacent in cg->spanDeps.
                     */
                    continue;
                }

                JS_ASSERT(type == JOF_JUMP ||
                          type == JOF_TABLESWITCH ||
                          type == JOF_LOOKUPSWITCH);
            }

            if (!JOF_TYPE_IS_EXTENDED_JUMP(type)) {
                span = SD_SPAN(sd, pivot);
                if (span < JUMP_OFFSET_MIN || JUMP_OFFSET_MAX < span) {
                    ptrdiff_t deltaFromTop = 0;

                    done = JS_FALSE;

                    switch (op) {
                      case JSOP_GOTO:         op = JSOP_GOTOX; break;
                      case JSOP_IFEQ:         op = JSOP_IFEQX; break;
                      case JSOP_IFNE:         op = JSOP_IFNEX; break;
                      case JSOP_OR:           op = JSOP_ORX; break;
                      case JSOP_AND:          op = JSOP_ANDX; break;
                      case JSOP_GOSUB:        op = JSOP_GOSUBX; break;
                      case JSOP_CASE:         op = JSOP_CASEX; break;
                      case JSOP_DEFAULT:      op = JSOP_DEFAULTX; break;
                      case JSOP_TABLESWITCH:  op = JSOP_TABLESWITCHX; break;
                      case JSOP_LOOKUPSWITCH: op = JSOP_LOOKUPSWITCHX; break;
                      default:
                        ReportStatementTooLarge(cx, cg);
                        return JS_FALSE;
                    }
                    *pc = (jsbytecode) op;

                    for (sd2 = sdtop; sd2 < sdlimit && sd2->top == top; sd2++) {
                        if (sd2 <= sd) {
                            /*
                             * sd2->offset already includes delta as it stood
                             * before we entered this loop, but it must also
                             * include the delta relative to top due to all the
                             * extended jump offset immediates for the opcode
                             * starting at top, which we extend in this loop.
                             *
                             * If there is only one extended jump offset, then
                             * sd2->offset won't change and this for loop will
                             * iterate once only.
                             */
                            sd2->offset += deltaFromTop;
                            deltaFromTop += JUMPX_OFFSET_LEN - JUMP_OFFSET_LEN;
                        } else {
                            /*
                             * sd2 comes after sd, and won't be revisited by
                             * the outer for loop, so we have to increase its
                             * offset by delta, not merely by deltaFromTop.
                             */
                            sd2->offset += delta;
                        }

                        delta += JUMPX_OFFSET_LEN - JUMP_OFFSET_LEN;
                        UpdateJumpTargets(cg->jumpTargets, sd2->offset,
                                          JUMPX_OFFSET_LEN - JUMP_OFFSET_LEN);
                    }
                    sd = sd2 - 1;
                }
            }
        }

        growth += delta;
    } while (!done);

    if (growth) {
#ifdef DEBUG_brendan
        TokenStream *ts = &cg->parser->tokenStream;

        printf("%s:%u: %u/%u jumps extended in %d passes (%d=%d+%d)\n",
               ts->filename ? ts->filename : "stdin", cg->firstLine,
               growth / (JUMPX_OFFSET_LEN - JUMP_OFFSET_LEN), cg->numSpanDeps,
               passes, offset + growth, offset, growth);
#endif

        /*
         * Ensure that we have room for the extended jumps, but don't round up
         * to a power of two -- we're done generating code, so we cut to fit.
         */
        limit = CG_LIMIT(cg);
        length = offset + growth;
        next = base + length;
        if (next > limit) {
            base = (jsbytecode *) cx->realloc_(base, BYTECODE_SIZE(length));
            if (!base) {
                js_ReportOutOfMemory(cx);
                return JS_FALSE;
            }
            CG_BASE(cg) = base;
            CG_LIMIT(cg) = next = base + length;
        }
        CG_NEXT(cg) = next;

        /*
         * Set up a fake span dependency record to guard the end of the code
         * being generated.  This guard record is returned as a fencepost by
         * FindNearestSpanDep if there is no real spandep at or above a given
         * unextended code offset.
         */
        guard.top = -1;
        guard.offset = offset + growth;
        guard.before = offset;
        guard.target = NULL;
    }

    /*
     * Now work backwards through the span dependencies, copying chunks of
     * bytecode between each extended jump toward the end of the grown code
     * space, and restoring immediate offset operands for all jump bytecodes.
     * The first chunk of bytecodes, starting at base and ending at the first
     * extended jump offset (NB: this chunk includes the operation bytecode
     * just before that immediate jump offset), doesn't need to be copied.
     */
    JS_ASSERT(sd == sdlimit);
    top = -1;
    while (--sd >= sdbase) {
        if (sd->top != top) {
            top = sd->top;
            op = (JSOp) base[top];
            type = JOF_OPTYPE(op);

            for (sd2 = sd - 1; sd2 >= sdbase && sd2->top == top; sd2--)
                continue;
            sd2++;
            pivot = sd2->offset;
            JS_ASSERT(top == sd2->before);
        }

        oldpc = base + sd->before;
        span = SD_SPAN(sd, pivot);

        /*
         * If this jump didn't need to be extended, restore its span immediate
         * offset operand now, overwriting the index of sd within cg->spanDeps
         * that was stored temporarily after *pc when BuildSpanDepTable ran.
         *
         * Note that span might fit in 16 bits even for an extended jump op,
         * if the op has multiple span operands, not all of which overflowed
         * (e.g. JSOP_LOOKUPSWITCH or JSOP_TABLESWITCH where some cases are in
         * range for a short jump, but others are not).
         */
        if (!JOF_TYPE_IS_EXTENDED_JUMP(type)) {
            JS_ASSERT(JUMP_OFFSET_MIN <= span && span <= JUMP_OFFSET_MAX);
            SET_JUMP_OFFSET(oldpc, span);
            continue;
        }

        /*
         * Set up parameters needed to copy the next run of bytecode starting
         * at offset (which is a cursor into the unextended, original bytecode
         * vector), down to sd->before (a cursor of the same scale as offset,
         * it's the index of the original jump pc).  Reuse delta to count the
         * nominal number of bytes to copy.
         */
        pc = base + sd->offset;
        delta = offset - sd->before;
        JS_ASSERT(delta >= 1 + JUMP_OFFSET_LEN);

        /*
         * Don't bother copying the jump offset we're about to reset, but do
         * copy the bytecode at oldpc (which comes just before its immediate
         * jump offset operand), on the next iteration through the loop, by
         * including it in offset's new value.
         */
        offset = sd->before + 1;
        size_t size = BYTECODE_SIZE(delta - (1 + JUMP_OFFSET_LEN));
        if (size) {
            memmove(pc + 1 + JUMPX_OFFSET_LEN,
                    oldpc + 1 + JUMP_OFFSET_LEN,
                    size);
        }

        SET_JUMPX_OFFSET(pc, span);
    }

    if (growth) {
        /*
         * Fix source note deltas.  Don't hardwire the delta fixup adjustment,
         * even though currently it must be JUMPX_OFFSET_LEN - JUMP_OFFSET_LEN
         * at each sd that moved.  The future may bring different offset sizes
         * for span-dependent instruction operands.  However, we fix only main
         * notes here, not prolog notes -- we know that prolog opcodes are not
         * span-dependent, and aren't likely ever to be.
         */
        offset = growth = 0;
        sd = sdbase;
        for (sn = cg->main.notes, snlimit = sn + cg->main.noteCount;
             sn < snlimit;
             sn = SN_NEXT(sn)) {
            /*
             * Recall that the offset of a given note includes its delta, and
             * tells the offset of the annotated bytecode from the main entry
             * point of the script.
             */
            offset += SN_DELTA(sn);
            while (sd < sdlimit && sd->before < offset) {
                /*
                 * To compute the delta to add to sn, we need to look at the
                 * spandep after sd, whose offset - (before + growth) tells by
                 * how many bytes sd's instruction grew.
                 */
                sd2 = sd + 1;
                if (sd2 == sdlimit)
                    sd2 = &guard;
                delta = sd2->offset - (sd2->before + growth);
                if (delta > 0) {
                    JS_ASSERT(delta == JUMPX_OFFSET_LEN - JUMP_OFFSET_LEN);
                    sn = js_AddToSrcNoteDelta(cx, cg, sn, delta);
                    if (!sn)
                        return JS_FALSE;
                    snlimit = cg->main.notes + cg->main.noteCount;
                    growth += delta;
                }
                sd++;
            }

            /*
             * If sn has span-dependent offset operands, check whether each
             * covers further span-dependencies, and increase those operands
             * accordingly.  Some source notes measure offset not from the
             * annotated pc, but from that pc plus some small bias.  NB: we
             * assume that spec->offsetBias can't itself span span-dependent
             * instructions!
             */
            spec = &js_SrcNoteSpec[SN_TYPE(sn)];
            if (spec->isSpanDep) {
                pivot = offset + spec->offsetBias;
                n = spec->arity;
                for (i = 0; i < n; i++) {
                    span = js_GetSrcNoteOffset(sn, i);
                    if (span == 0)
                        continue;
                    target = pivot + span * spec->isSpanDep;
                    sd2 = FindNearestSpanDep(cg, target,
                                             (target >= pivot)
                                             ? sd - sdbase
                                             : 0,
                                             &guard);

                    /*
                     * Increase target by sd2's before-vs-after offset delta,
                     * which is absolute (i.e., relative to start of script,
                     * as is target).  Recompute the span by subtracting its
                     * adjusted pivot from target.
                     */
                    target += sd2->offset - sd2->before;
                    span = target - (pivot + growth);
                    span *= spec->isSpanDep;
                    noteIndex = sn - cg->main.notes;
                    if (!js_SetSrcNoteOffset(cx, cg, noteIndex, i, span))
                        return JS_FALSE;
                    sn = cg->main.notes + noteIndex;
                    snlimit = cg->main.notes + cg->main.noteCount;
                }
            }
        }
        cg->main.lastNoteOffset += growth;

        /*
         * Fix try/catch notes (O(numTryNotes * log2(numSpanDeps)), but it's
         * not clear how we can beat that).
         */
        for (tryNode = cg->lastTryNode; tryNode; tryNode = tryNode->prev) {
            /*
             * First, look for the nearest span dependency at/above tn->start.
             * There may not be any such spandep, in which case the guard will
             * be returned.
             */
            offset = tryNode->note.start;
            sd = FindNearestSpanDep(cg, offset, 0, &guard);
            delta = sd->offset - sd->before;
            tryNode->note.start = offset + delta;

            /*
             * Next, find the nearest spandep at/above tn->start + tn->length.
             * Use its delta minus tn->start's delta to increase tn->length.
             */
            length = tryNode->note.length;
            sd2 = FindNearestSpanDep(cg, offset + length, sd - sdbase, &guard);
            if (sd2 != sd) {
                tryNode->note.length =
                    length + sd2->offset - sd2->before - delta;
            }
        }
    }

#ifdef DEBUG_brendan
  {
    uintN bigspans = 0;
    top = -1;
    for (sd = sdbase; sd < sdlimit; sd++) {
        offset = sd->offset;

        /* NB: sd->top cursors into the original, unextended bytecode vector. */
        if (sd->top != top) {
            JS_ASSERT(top == -1 ||
                      !JOF_TYPE_IS_EXTENDED_JUMP(type) ||
                      bigspans != 0);
            bigspans = 0;
            top = sd->top;
            JS_ASSERT(top == sd->before);
            op = (JSOp) base[offset];
            type = JOF_OPTYPE(op);
            JS_ASSERT(type == JOF_JUMP ||
                      type == JOF_JUMPX ||
                      type == JOF_TABLESWITCH ||
                      type == JOF_TABLESWITCHX ||
                      type == JOF_LOOKUPSWITCH ||
                      type == JOF_LOOKUPSWITCHX);
            pivot = offset;
        }

        pc = base + offset;
        if (JOF_TYPE_IS_EXTENDED_JUMP(type)) {
            span = GET_JUMPX_OFFSET(pc);
            if (span < JUMP_OFFSET_MIN || JUMP_OFFSET_MAX < span) {
                bigspans++;
            } else {
                JS_ASSERT(type == JOF_TABLESWITCHX ||
                          type == JOF_LOOKUPSWITCHX);
            }
        } else {
            span = GET_JUMP_OFFSET(pc);
        }
        JS_ASSERT(SD_SPAN(sd, pivot) == span);
    }
    JS_ASSERT(!JOF_TYPE_IS_EXTENDED_JUMP(type) || bigspans != 0);
  }
#endif

    /*
     * Reset so we optimize at most once -- cg may be used for further code
     * generation of successive, independent, top-level statements.  No jump
     * can span top-level statements, because JS lacks goto.
     */
    cx->free_(cg->spanDeps);
    cg->spanDeps = NULL;
    FreeJumpTargets(cg, cg->jumpTargets);
    cg->jumpTargets = NULL;
    cg->numSpanDeps = cg->numJumpTargets = 0;
    cg->spanDepTodo = CG_OFFSET(cg);
    return JS_TRUE;
}

static ptrdiff_t
EmitJump(JSContext *cx, JSCodeGenerator *cg, JSOp op, ptrdiff_t off)
{
    JSBool extend;
    ptrdiff_t jmp;
    jsbytecode *pc;

    extend = off < JUMP_OFFSET_MIN || JUMP_OFFSET_MAX < off;
    if (extend && !cg->spanDeps && !BuildSpanDepTable(cx, cg))
        return -1;

    jmp = js_Emit3(cx, cg, op, JUMP_OFFSET_HI(off), JUMP_OFFSET_LO(off));
    if (jmp >= 0 && (extend || cg->spanDeps)) {
        pc = CG_CODE(cg, jmp);
        if (!AddSpanDep(cx, cg, pc, pc, off))
            return -1;
    }
    return jmp;
}

static ptrdiff_t
GetJumpOffset(JSCodeGenerator *cg, jsbytecode *pc)
{
    JSSpanDep *sd;
    JSJumpTarget *jt;
    ptrdiff_t top;

    if (!cg->spanDeps)
        return GET_JUMP_OFFSET(pc);

    sd = GetSpanDep(cg, pc);
    jt = sd->target;
    if (!JT_HAS_TAG(jt))
        return JT_TO_BPDELTA(jt);

    top = sd->top;
    while (--sd >= cg->spanDeps && sd->top == top)
        continue;
    sd++;
    return JT_CLR_TAG(jt)->offset - sd->offset;
}

JSBool
js_SetJumpOffset(JSContext *cx, JSCodeGenerator *cg, jsbytecode *pc,
                 ptrdiff_t off)
{
    if (!cg->spanDeps) {
        if (JUMP_OFFSET_MIN <= off && off <= JUMP_OFFSET_MAX) {
            SET_JUMP_OFFSET(pc, off);
            return JS_TRUE;
        }

        if (!BuildSpanDepTable(cx, cg))
            return JS_FALSE;
    }

    return SetSpanDepTarget(cx, cg, GetSpanDep(cg, pc), off);
}

bool
JSTreeContext::inStatement(JSStmtType type)
{
    for (JSStmtInfo *stmt = topStmt; stmt; stmt = stmt->down) {
        if (stmt->type == type)
            return true;
    }
    return false;
}

bool
JSTreeContext::ensureSharpSlots()
{
#if JS_HAS_SHARP_VARS
    JS_STATIC_ASSERT(SHARP_NSLOTS == 2);

    if (sharpSlotBase >= 0) {
        JS_ASSERT(flags & TCF_HAS_SHARPS);
        return true;
    }

    JS_ASSERT(!(flags & TCF_HAS_SHARPS));
    if (inFunction()) {
        JSContext *cx = parser->context;
        JSAtom *sharpArrayAtom = js_Atomize(cx, "#array", 6);
        JSAtom *sharpDepthAtom = js_Atomize(cx, "#depth", 6);
        if (!sharpArrayAtom || !sharpDepthAtom)
            return false;

        sharpSlotBase = bindings.countVars();
        if (!bindings.addVariable(cx, sharpArrayAtom))
            return false;
        if (!bindings.addVariable(cx, sharpDepthAtom))
            return false;
    } else {
        /*
         * Compiler::compileScript will rebase immediate operands indexing
         * the sharp slots to come at the end of the global script's |nfixed|
         * slots storage, after gvars and regexps.
         */
        sharpSlotBase = 0;
    }
    flags |= TCF_HAS_SHARPS;
#endif
    return true;
}

bool
JSTreeContext::skipSpansGenerator(unsigned skip)
{
    JSTreeContext *tc = this;
    for (unsigned i = 0; i < skip; ++i, tc = tc->parent) {
        if (!tc)
            return false;
        if (tc->flags & TCF_FUN_IS_GENERATOR)
            return true;
    }
    return false;
}

void
js_PushStatement(JSTreeContext *tc, JSStmtInfo *stmt, JSStmtType type,
                 ptrdiff_t top)
{
    stmt->type = type;
    stmt->flags = 0;
    stmt->blockid = tc->blockid();
    SET_STATEMENT_TOP(stmt, top);
    stmt->label = NULL;
    JS_ASSERT(!stmt->blockBox);
    stmt->down = tc->topStmt;
    tc->topStmt = stmt;
    if (STMT_LINKS_SCOPE(stmt)) {
        stmt->downScope = tc->topScopeStmt;
        tc->topScopeStmt = stmt;
    } else {
        stmt->downScope = NULL;
    }
}

void
js_PushBlockScope(JSTreeContext *tc, JSStmtInfo *stmt, JSObjectBox *blockBox,
                  ptrdiff_t top)
{
    js_PushStatement(tc, stmt, STMT_BLOCK, top);
    stmt->flags |= SIF_SCOPE;
    blockBox->parent = tc->blockChainBox;
    blockBox->object->setParent(tc->blockChain());
    stmt->downScope = tc->topScopeStmt;
    tc->topScopeStmt = stmt;
    tc->blockChainBox = blockBox;
    stmt->blockBox = blockBox;
}

/*
 * Emit a backpatch op with offset pointing to the previous jump of this type,
 * so that we can walk back up the chain fixing up the op and jump offset.
 */
static ptrdiff_t
EmitBackPatchOp(JSContext *cx, JSCodeGenerator *cg, JSOp op, ptrdiff_t *lastp)
{
    ptrdiff_t offset, delta;

    offset = CG_OFFSET(cg);
    delta = offset - *lastp;
    *lastp = offset;
    JS_ASSERT(delta > 0);
    return EmitJump(cx, cg, op, delta);
}

/* A macro for inlining at the top of js_EmitTree (whence it came). */
#define UPDATE_LINE_NUMBER_NOTES(cx, cg, line)                                \
    JS_BEGIN_MACRO                                                            \
        uintN line_ = (line);                                                 \
        uintN delta_ = line_ - CG_CURRENT_LINE(cg);                           \
        if (delta_ != 0) {                                                    \
            /*                                                                \
             * Encode any change in the current source line number by using   \
             * either several SRC_NEWLINE notes or just one SRC_SETLINE note, \
             * whichever consumes less space.                                 \
             *                                                                \
             * NB: We handle backward line number deltas (possible with for   \
             * loops where the update part is emitted after the body, but its \
             * line number is <= any line number in the body) here by letting \
             * unsigned delta_ wrap to a very large number, which triggers a  \
             * SRC_SETLINE.                                                   \
             */                                                               \
            CG_CURRENT_LINE(cg) = line_;                                      \
            if (delta_ >= (uintN)(2 + ((line_ > SN_3BYTE_OFFSET_MASK)<<1))) { \
                if (js_NewSrcNote2(cx, cg, SRC_SETLINE, (ptrdiff_t)line_) < 0)\
                    return JS_FALSE;                                          \
            } else {                                                          \
                do {                                                          \
                    if (js_NewSrcNote(cx, cg, SRC_NEWLINE) < 0)               \
                        return JS_FALSE;                                      \
                } while (--delta_ != 0);                                      \
            }                                                                 \
        }                                                                     \
    JS_END_MACRO

/* A function, so that we avoid macro-bloating all the other callsites. */
static JSBool
UpdateLineNumberNotes(JSContext *cx, JSCodeGenerator *cg, uintN line)
{
    UPDATE_LINE_NUMBER_NOTES(cx, cg, line);
    return JS_TRUE;
}

static ptrdiff_t
EmitTraceOp(JSContext *cx, JSCodeGenerator *cg, JSParseNode *nextpn)
{
    if (nextpn) {
        /*
         * Try to give the JSOP_TRACE the same line number as the next
         * instruction. nextpn is often a block, in which case the next
         * instruction typically comes from the first statement inside.
         */
        if (nextpn->isKind(TOK_LC) && nextpn->isArity(PN_LIST) && nextpn->pn_head)
            nextpn = nextpn->pn_head;
        if (!UpdateLineNumberNotes(cx, cg, nextpn->pn_pos.begin.lineno))
            return -1;
    }

    uint32 index = cg->traceIndex;
    if (index < UINT16_MAX)
        cg->traceIndex++;
    return js_Emit3(cx, cg, JSOP_TRACE, UINT16_HI(index), UINT16_LO(index));
}

/*
 * If op is JOF_TYPESET (see the type barriers comment in jsinfer.h), reserve
 * a type set to store its result.
 */
static inline void
CheckTypeSet(JSContext *cx, JSCodeGenerator *cg, JSOp op)
{
    if (js_CodeSpec[op].format & JOF_TYPESET) {
        if (cg->typesetCount < UINT16_MAX)
            cg->typesetCount++;
    }
}

/*
 * Macro to emit a bytecode followed by a uint16 immediate operand stored in
 * big-endian order, used for arg and var numbers as well as for atomIndexes.
 * NB: We use cx and cg from our caller's lexical environment, and return
 * false on error.
 */
#define EMIT_UINT16_IMM_OP(op, i)                                             \
    JS_BEGIN_MACRO                                                            \
        if (js_Emit3(cx, cg, op, UINT16_HI(i), UINT16_LO(i)) < 0)             \
            return JS_FALSE;                                                  \
        CheckTypeSet(cx, cg, op);                                             \
    JS_END_MACRO

#define EMIT_UINT16PAIR_IMM_OP(op, i, j)                                      \
    JS_BEGIN_MACRO                                                            \
        ptrdiff_t off_ = js_EmitN(cx, cg, op, 2 * UINT16_LEN);                \
        if (off_ < 0)                                                         \
            return JS_FALSE;                                                  \
        jsbytecode *pc_ = CG_CODE(cg, off_);                                  \
        SET_UINT16(pc_, i);                                                   \
        pc_ += UINT16_LEN;                                                    \
        SET_UINT16(pc_, j);                                                   \
    JS_END_MACRO

#define EMIT_UINT16_IN_PLACE(offset, op, i)                                   \
    JS_BEGIN_MACRO                                                            \
        CG_CODE(cg, offset)[0] = op;                                          \
        CG_CODE(cg, offset)[1] = UINT16_HI(i);                                \
        CG_CODE(cg, offset)[2] = UINT16_LO(i);                                \
    JS_END_MACRO

static JSBool
FlushPops(JSContext *cx, JSCodeGenerator *cg, intN *npops)
{
    JS_ASSERT(*npops != 0);
    if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0)
        return JS_FALSE;
    EMIT_UINT16_IMM_OP(JSOP_POPN, *npops);
    *npops = 0;
    return JS_TRUE;
}

/*
 * Emit additional bytecode(s) for non-local jumps.
 */
static JSBool
EmitNonLocalJumpFixup(JSContext *cx, JSCodeGenerator *cg, JSStmtInfo *toStmt)
{
    intN depth, npops;
    JSStmtInfo *stmt;

    /*
     * The non-local jump fixup we emit will unbalance cg->stackDepth, because
     * the fixup replicates balanced code such as JSOP_LEAVEWITH emitted at the
     * end of a with statement, so we save cg->stackDepth here and restore it
     * just before a successful return.
     */
    depth = cg->stackDepth;
    npops = 0;

#define FLUSH_POPS() if (npops && !FlushPops(cx, cg, &npops)) return JS_FALSE

    for (stmt = cg->topStmt; stmt != toStmt; stmt = stmt->down) {
        switch (stmt->type) {
          case STMT_FINALLY:
            FLUSH_POPS();
            if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0)
                return JS_FALSE;
            if (EmitBackPatchOp(cx, cg, JSOP_BACKPATCH, &GOSUBS(*stmt)) < 0)
                return JS_FALSE;
            break;

          case STMT_WITH:
            /* There's a With object on the stack that we need to pop. */
            FLUSH_POPS();
            if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0)
                return JS_FALSE;
            if (js_Emit1(cx, cg, JSOP_LEAVEWITH) < 0)
                return JS_FALSE;
            break;

          case STMT_FOR_IN_LOOP:
            /*
             * The iterator and the object being iterated need to be popped.
             */
            FLUSH_POPS();
            if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0)
                return JS_FALSE;
            if (js_Emit1(cx, cg, JSOP_ENDITER) < 0)
                return JS_FALSE;
            break;

          case STMT_SUBROUTINE:
            /*
             * There's a [exception or hole, retsub pc-index] pair on the
             * stack that we need to pop.
             */
            npops += 2;
            break;

          default:;
        }

        if (stmt->flags & SIF_SCOPE) {
            /* There is a Block object with locals on the stack to pop. */
            FLUSH_POPS();
            if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0)
                return JS_FALSE;
            if (!EmitLeaveBlock(cx, cg, JSOP_LEAVEBLOCK, stmt->blockBox))
                return JS_FALSE;
        }
    }

    FLUSH_POPS();
    cg->stackDepth = depth;
    return JS_TRUE;

#undef FLUSH_POPS
}

static JSBool
EmitKnownBlockChain(JSContext *cx, JSCodeGenerator *cg, JSObjectBox *box)
{
    if (box)
        return EmitIndexOp(cx, JSOP_BLOCKCHAIN, box->index, cg);
    return js_Emit1(cx, cg, JSOP_NULLBLOCKCHAIN) >= 0;
}

static JSBool
EmitBlockChain(JSContext *cx, JSCodeGenerator *cg)
{
    return EmitKnownBlockChain(cx, cg, cg->blockChainBox);
}

static const jsatomid INVALID_ATOMID = -1;

static ptrdiff_t
EmitGoto(JSContext *cx, JSCodeGenerator *cg, JSStmtInfo *toStmt,
         ptrdiff_t *lastp, jsatomid labelIndex = INVALID_ATOMID, JSSrcNoteType noteType = SRC_NULL)
{
    intN index;

    if (!EmitNonLocalJumpFixup(cx, cg, toStmt))
        return -1;

    if (labelIndex != INVALID_ATOMID)
        index = js_NewSrcNote2(cx, cg, noteType, ptrdiff_t(labelIndex));
    else if (noteType != SRC_NULL)
        index = js_NewSrcNote(cx, cg, noteType);
    else
        index = 0;
    if (index < 0)
        return -1;

    ptrdiff_t result = EmitBackPatchOp(cx, cg, JSOP_BACKPATCH, lastp);
    if (result < 0)
        return result;

    if (!EmitBlockChain(cx, cg))
        return -1;

    return result;
}

static JSBool
BackPatch(JSContext *cx, JSCodeGenerator *cg, ptrdiff_t last,
          jsbytecode *target, jsbytecode op)
{
    jsbytecode *pc, *stop;
    ptrdiff_t delta, span;

    pc = CG_CODE(cg, last);
    stop = CG_CODE(cg, -1);
    while (pc != stop) {
        delta = GetJumpOffset(cg, pc);
        span = target - pc;
        CHECK_AND_SET_JUMP_OFFSET(cx, cg, pc, span);

        /*
         * Set *pc after jump offset in case bpdelta didn't overflow, but span
         * does (if so, CHECK_AND_SET_JUMP_OFFSET might call BuildSpanDepTable
         * and need to see the JSOP_BACKPATCH* op at *pc).
         */
        *pc = op;
        pc -= delta;
    }
    return JS_TRUE;
}

void
js_PopStatement(JSTreeContext *tc)
{
    JSStmtInfo *stmt;

    stmt = tc->topStmt;
    tc->topStmt = stmt->down;
    if (STMT_LINKS_SCOPE(stmt)) {
        tc->topScopeStmt = stmt->downScope;
        if (stmt->flags & SIF_SCOPE) {
            tc->blockChainBox = stmt->blockBox->parent;
        }
    }
}

JSBool
js_PopStatementCG(JSContext *cx, JSCodeGenerator *cg)
{
    JSStmtInfo *stmt;

    stmt = cg->topStmt;
    if (!STMT_IS_TRYING(stmt) &&
        (!BackPatch(cx, cg, stmt->breaks, CG_NEXT(cg), JSOP_GOTO) ||
         !BackPatch(cx, cg, stmt->continues, CG_CODE(cg, stmt->update),
                    JSOP_GOTO))) {
        return JS_FALSE;
    }
    js_PopStatement(cg);
    return JS_TRUE;
}

JSBool
js_DefineCompileTimeConstant(JSContext *cx, JSCodeGenerator *cg, JSAtom *atom,
                             JSParseNode *pn)
{
    /* XXX just do numbers for now */
    if (pn->isKind(TOK_NUMBER)) {
        if (!cg->constMap.put(atom, NumberValue(pn->pn_dval)))
            return JS_FALSE;
    }
    return JS_TRUE;
}

JSStmtInfo *
js_LexicalLookup(JSTreeContext *tc, JSAtom *atom, jsint *slotp, JSStmtInfo *stmt)
{
    if (!stmt)
        stmt = tc->topScopeStmt;
    for (; stmt; stmt = stmt->downScope) {
        if (stmt->type == STMT_WITH)
            break;

        /* Skip "maybe scope" statements that don't contain let bindings. */
        if (!(stmt->flags & SIF_SCOPE))
            continue;

        JSObject *obj = stmt->blockBox->object;
        JS_ASSERT(obj->isStaticBlock());

        const Shape *shape = obj->nativeLookup(tc->parser->context, ATOM_TO_JSID(atom));
        if (shape) {
            JS_ASSERT(shape->hasShortID());

            if (slotp) {
                JS_ASSERT(obj->getSlot(JSSLOT_BLOCK_DEPTH).isInt32());
                *slotp = obj->getSlot(JSSLOT_BLOCK_DEPTH).toInt32() + shape->shortid;
            }
            return stmt;
        }
    }

    if (slotp)
        *slotp = -1;
    return stmt;
}

/*
 * The function sets vp to NO_CONSTANT when the atom does not corresponds to a
 * name defining a constant.
 */
static JSBool
LookupCompileTimeConstant(JSContext *cx, JSCodeGenerator *cg, JSAtom *atom,
                          Value *constp)
{
    JSStmtInfo *stmt;
    JSObject *obj;

    /*
     * Chase down the cg stack, but only until we reach the outermost cg.
     * This enables propagating consts from top-level into switch cases in a
     * function compiled along with the top-level script.
     */
    constp->setMagic(JS_NO_CONSTANT);
    do {
        if (cg->inFunction() || cg->compileAndGo()) {
            /* XXX this will need revising if 'const' becomes block-scoped. */
            stmt = js_LexicalLookup(cg, atom, NULL);
            if (stmt)
                return JS_TRUE;

            if (JSCodeGenerator::ConstMap::Ptr p = cg->constMap.lookup(atom)) {
                JS_ASSERT(!p->value.isMagic(JS_NO_CONSTANT));
                *constp = p->value;
                return JS_TRUE;
            }

            /*
             * Try looking in the variable object for a direct property that
             * is readonly and permanent.  We know such a property can't be
             * shadowed by another property on obj's prototype chain, or a
             * with object or catch variable; nor can prop's value be changed,
             * nor can prop be deleted.
             */
            if (cg->inFunction()) {
                if (cg->bindings.hasBinding(cx, atom))
                    break;
            } else {
                JS_ASSERT(cg->compileAndGo());
                obj = cg->scopeChain();

                const Shape *shape = obj->nativeLookup(cx, ATOM_TO_JSID(atom));
                if (shape) {
                    /*
                     * We're compiling code that will be executed immediately,
                     * not re-executed against a different scope chain and/or
                     * variable object.  Therefore we can get constant values
                     * from our variable object here.
                     */
                    if (!shape->writable() && !shape->configurable() &&
                        shape->hasDefaultGetter() && obj->containsSlot(shape->slot)) {
                        *constp = obj->getSlot(shape->slot);
                    }
                }

                if (shape)
                    break;
            }
        }
    } while (cg->parent && (cg = cg->parent->asCodeGenerator()));
    return JS_TRUE;
}

static inline bool
FitsWithoutBigIndex(uintN index)
{
    return index < JS_BIT(16);
}

/*
 * Return JSOP_NOP to indicate that index fits 2 bytes and no index segment
 * reset instruction is necessary, JSOP_FALSE to indicate an error or either
 * JSOP_RESETBASE0 or JSOP_RESETBASE1 to indicate the reset bytecode to issue
 * after the main bytecode sequence.
 */
static JSOp
EmitBigIndexPrefix(JSContext *cx, JSCodeGenerator *cg, uintN index)
{
    uintN indexBase;

    /*
     * We have max 3 bytes for indexes and check for INDEX_LIMIT overflow only
     * for big indexes.
     */
    JS_STATIC_ASSERT(INDEX_LIMIT <= JS_BIT(24));
    JS_STATIC_ASSERT(INDEX_LIMIT >=
                     (JSOP_INDEXBASE3 - JSOP_INDEXBASE1 + 2) << 16);

    if (FitsWithoutBigIndex(index))
        return JSOP_NOP;
    indexBase = index >> 16;
    if (indexBase <= JSOP_INDEXBASE3 - JSOP_INDEXBASE1 + 1) {
        if (js_Emit1(cx, cg, (JSOp)(JSOP_INDEXBASE1 + indexBase - 1)) < 0)
            return JSOP_FALSE;
        return JSOP_RESETBASE0;
    }

    if (index >= INDEX_LIMIT) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_TOO_MANY_LITERALS);
        return JSOP_FALSE;
    }

    if (js_Emit2(cx, cg, JSOP_INDEXBASE, (JSOp)indexBase) < 0)
        return JSOP_FALSE;
    return JSOP_RESETBASE;
}

/*
 * Emit a bytecode and its 2-byte constant index immediate operand. If the
 * index requires more than 2 bytes, emit a prefix op whose 8-bit immediate
 * operand effectively extends the 16-bit immediate of the prefixed opcode,
 * by changing index "segment" (see jsinterp.c). We optimize segments 1-3
 * with single-byte JSOP_INDEXBASE[123] codes.
 *
 * Such prefixing currently requires a suffix to restore the "zero segment"
 * register setting, but this could be optimized further.
 */
static bool
EmitIndexOp(JSContext *cx, JSOp op, uintN index, JSCodeGenerator *cg, JSOp *psuffix)
{
    JSOp bigSuffix;

    bigSuffix = EmitBigIndexPrefix(cx, cg, index);
    if (bigSuffix == JSOP_FALSE)
        return false;
    EMIT_UINT16_IMM_OP(op, index);

    /*
     * For decomposed ops, the suffix needs to go after the decomposed version.
     * This means the suffix will run in the interpreter in both the base
     * and decomposed paths, which works as suffix ops are idempotent.
     */
    JS_ASSERT(!!(js_CodeSpec[op].format & JOF_DECOMPOSE) == (psuffix != NULL));
    if (psuffix) {
        *psuffix = bigSuffix;
        return true;
    }

    return bigSuffix == JSOP_NOP || js_Emit1(cx, cg, bigSuffix) >= 0;
}

/*
 * Slight sugar for EmitIndexOp, again accessing cx and cg from the macro
 * caller's lexical environment, and embedding a false return on error.
 */
#define EMIT_INDEX_OP(op, index)                                              \
    JS_BEGIN_MACRO                                                            \
        if (!EmitIndexOp(cx, op, index, cg))                                  \
            return JS_FALSE;                                                  \
    JS_END_MACRO

static bool
EmitAtomOp(JSContext *cx, JSParseNode *pn, JSOp op, JSCodeGenerator *cg,
           JSOp *psuffix = NULL)
{
    JS_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);

    if (op == JSOP_GETPROP &&
        pn->pn_atom == cx->runtime->atomState.lengthAtom) {
        /* Specialize length accesses for the interpreter. */
        op = JSOP_LENGTH;
    }

    jsatomid index;
    if (!cg->makeAtomIndex(pn->pn_atom, &index))
        return false;

    return EmitIndexOp(cx, op, index, cg, psuffix);
}

static JSBool
EmitObjectOp(JSContext *cx, JSObjectBox *objbox, JSOp op,
             JSCodeGenerator *cg)
{
    JS_ASSERT(JOF_OPTYPE(op) == JOF_OBJECT);
    return EmitIndexOp(cx, op, cg->objectList.index(objbox), cg);
}

/*
 * What good are ARGNO_LEN and SLOTNO_LEN, you ask?  The answer is that, apart
 * from EmitSlotIndexOp, they abstract out the detail that both are 2, and in
 * other parts of the code there's no necessary relationship between the two.
 * The abstraction cracks here in order to share EmitSlotIndexOp code among
 * the JSOP_DEFLOCALFUN and JSOP_GET{ARG,VAR,LOCAL}PROP cases.
 */
JS_STATIC_ASSERT(ARGNO_LEN == 2);
JS_STATIC_ASSERT(SLOTNO_LEN == 2);

static JSBool
EmitSlotIndexOp(JSContext *cx, JSOp op, uintN slot, uintN index,
                JSCodeGenerator *cg)
{
    JSOp bigSuffix;
    ptrdiff_t off;
    jsbytecode *pc;

    JS_ASSERT(JOF_OPTYPE(op) == JOF_SLOTATOM ||
              JOF_OPTYPE(op) == JOF_SLOTOBJECT);
    bigSuffix = EmitBigIndexPrefix(cx, cg, index);
    if (bigSuffix == JSOP_FALSE)
        return JS_FALSE;

    /* Emit [op, slot, index]. */
    off = js_EmitN(cx, cg, op, 2 + INDEX_LEN);
    if (off < 0)
        return JS_FALSE;
    pc = CG_CODE(cg, off);
    SET_UINT16(pc, slot);
    pc += 2;
    SET_INDEX(pc, index);
    return bigSuffix == JSOP_NOP || js_Emit1(cx, cg, bigSuffix) >= 0;
}

bool
JSCodeGenerator::shouldNoteClosedName(JSParseNode *pn)
{
    return !callsEval() && pn->isDefn() && pn->isClosed();
}

/*
 * Adjust the slot for a block local to account for the number of variables
 * that share the same index space with locals. Due to the incremental code
 * generation for top-level script, we do the adjustment via code patching in
 * Compiler::compileScript; see comments there.
 *
 * The function returns -1 on failures.
 */
static jsint
AdjustBlockSlot(JSContext *cx, JSCodeGenerator *cg, jsint slot)
{
    JS_ASSERT((jsuint) slot < cg->maxStackDepth);
    if (cg->inFunction()) {
        slot += cg->bindings.countVars();
        if ((uintN) slot >= SLOTNO_LIMIT) {
            ReportCompileErrorNumber(cx, CG_TS(cg), NULL, JSREPORT_ERROR, JSMSG_TOO_MANY_LOCALS);
            slot = -1;
        }
    }
    return slot;
}

static bool
EmitEnterBlock(JSContext *cx, JSParseNode *pn, JSCodeGenerator *cg)
{
    JS_ASSERT(pn->isKind(TOK_LEXICALSCOPE));
    if (!EmitObjectOp(cx, pn->pn_objbox, JSOP_ENTERBLOCK, cg))
        return false;

    JSObject *blockObj = pn->pn_objbox->object;
    jsint depth = AdjustBlockSlot(cx, cg, OBJ_BLOCK_DEPTH(cx, blockObj));
    if (depth < 0)
        return false;

    uintN base = JSSLOT_FREE(&BlockClass);
    for (uintN slot = base, limit = base + OBJ_BLOCK_COUNT(cx, blockObj); slot < limit; slot++) {
        const Value &v = blockObj->getSlot(slot);

        /* Beware the empty destructuring dummy. */
        if (v.isUndefined()) {
            JS_ASSERT(slot + 1 <= limit);
            continue;
        }

        JSDefinition *dn = (JSDefinition *) v.toPrivate();
        JS_ASSERT(dn->isDefn());
        JS_ASSERT(uintN(dn->frameSlot() + depth) < JS_BIT(16));
        dn->pn_cookie.set(dn->pn_cookie.level(), uint16(dn->frameSlot() + depth));
#ifdef DEBUG
        for (JSParseNode *pnu = dn->dn_uses; pnu; pnu = pnu->pn_link) {
            JS_ASSERT(pnu->pn_lexdef == dn);
            JS_ASSERT(!(pnu->pn_dflags & PND_BOUND));
            JS_ASSERT(pnu->pn_cookie.isFree());
        }
#endif

        /*
         * If this variable is closed over, and |eval| is not present, then
         * then set a bit in dslots so the Method JIT can deoptimize this
         * slot.
         */
        bool isClosed = cg->shouldNoteClosedName(dn);
        blockObj->setSlot(slot, BooleanValue(isClosed));
    }

    /*
     * If clones of this block will have any extensible parents, then the clones
     * must get unique shapes; see the comments for js::Bindings::
     * extensibleParents.
     */
    if ((cg->flags & TCF_FUN_EXTENSIBLE_SCOPE) ||
        cg->bindings.extensibleParents())
        blockObj->setBlockOwnShape(cx);

    return true;
}

static JSBool
EmitLeaveBlock(JSContext *cx, JSCodeGenerator *cg, JSOp op,
               JSObjectBox *box)
{
    JSOp bigSuffix;
    uintN count = OBJ_BLOCK_COUNT(cx, box->object);
    
    bigSuffix = EmitBigIndexPrefix(cx, cg, box->index);
    if (bigSuffix == JSOP_FALSE)
        return JS_FALSE;
    if (js_Emit5(cx, cg, op, count, box->index) < 0)
        return JS_FALSE;
    return bigSuffix == JSOP_NOP || js_Emit1(cx, cg, bigSuffix) >= 0;
}

/*
 * Try to convert a *NAME op to a *GNAME op, which optimizes access to
 * undeclared globals. Return true if a conversion was made.
 *
 * This conversion is not made if we are in strict mode.  In eval code nested
 * within (strict mode) eval code, access to an undeclared "global" might
 * merely be to a binding local to that outer eval:
 *
 *   "use strict";
 *   var x = "global";
 *   eval('var x = "eval"; eval("x");'); // 'eval', not 'global'
 *
 * Outside eval code, access to an undeclared global is a strict mode error:
 *
 *   "use strict";
 *   function foo()
 *   {
 *     undeclared = 17; // throws ReferenceError
 *   }
 *   foo();
 */
static bool
TryConvertToGname(JSCodeGenerator *cg, JSParseNode *pn, JSOp *op)
{
    if (cg->compileAndGo() && 
        cg->compiler()->globalScope->globalObj &&
        !cg->mightAliasLocals() &&
        !pn->isDeoptimized() &&
        !(cg->flags & TCF_STRICT_MODE_CODE)) { 
        switch (*op) {
          case JSOP_NAME:     *op = JSOP_GETGNAME; break;
          case JSOP_SETNAME:  *op = JSOP_SETGNAME; break;
          case JSOP_INCNAME:  *op = JSOP_INCGNAME; break;
          case JSOP_NAMEINC:  *op = JSOP_GNAMEINC; break;
          case JSOP_DECNAME:  *op = JSOP_DECGNAME; break;
          case JSOP_NAMEDEC:  *op = JSOP_GNAMEDEC; break;
          case JSOP_SETCONST:
          case JSOP_DELNAME:
            /* Not supported. */
            return false;
          default: JS_NOT_REACHED("gname");
        }
        return true;
    }
    return false;
}

// Binds a global, given a |dn| that is known to have the PND_GVAR bit, and a pn
// that is |dn| or whose definition is |dn|. |pn->pn_cookie| is an outparam
// that will be free (meaning no binding), or a slot number.
static bool
BindKnownGlobal(JSContext *cx, JSCodeGenerator *cg, JSParseNode *dn, JSParseNode *pn, JSAtom *atom)
{
    // Cookie is an outparam; make sure caller knew to clear it.
    JS_ASSERT(pn->pn_cookie.isFree());

    if (cg->mightAliasLocals())
        return true;

    GlobalScope *globalScope = cg->compiler()->globalScope;

    jsatomid index;
    if (dn->pn_cookie.isFree()) {
        // The definition wasn't bound, so find its atom's index in the
        // mapping of defined globals.
        AtomIndexPtr p = globalScope->names.lookup(atom);
        JS_ASSERT(!!p);
        index = p.value();
    } else {
        JSCodeGenerator *globalcg = globalScope->cg;

        // If the definition is bound, and we're in the same cg, we can re-use
        // its cookie.
        if (globalcg == cg) {
            pn->pn_cookie = dn->pn_cookie;
            pn->pn_dflags |= PND_BOUND;
            return true;
        }

        // Otherwise, find the atom's index by using the originating cg's
        // global use table.
        index = globalcg->globalUses[dn->pn_cookie.asInteger()].slot;
    }

    if (!cg->addGlobalUse(atom, index, &pn->pn_cookie))
        return false;

    if (!pn->pn_cookie.isFree())
        pn->pn_dflags |= PND_BOUND;

    return true;
}

// See BindKnownGlobal()'s comment.
static bool
BindGlobal(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn, JSAtom *atom)
{
    pn->pn_cookie.makeFree();

    JSDefinition *dn;
    if (pn->isUsed()) {
        dn = pn->pn_lexdef;
    } else {
        if (!pn->isDefn())
            return true;
        dn = (JSDefinition *)pn;
    }

    // Only optimize for defined globals.
    if (!dn->isGlobal())
        return true;

    return BindKnownGlobal(cx, cg, dn, pn, atom);
}

/*
 * BindNameToSlot attempts to optimize name gets and sets to stack slot loads
 * and stores, given the compile-time information in cg and a TOK_NAME node pn.
 * It returns false on error, true on success.
 *
 * The caller can inspect pn->pn_cookie for FREE_UPVAR_COOKIE to tell whether
 * optimization occurred, in which case BindNameToSlot also updated pn->pn_op.
 * If pn->pn_cookie is still FREE_UPVAR_COOKIE on return, pn->pn_op still may
 * have been optimized, e.g., from JSOP_NAME to JSOP_CALLEE.  Whether or not
 * pn->pn_op was modified, if this function finds an argument or local variable
 * name, PND_CONST will be set in pn_dflags for read-only properties after a
 * successful return.
 *
 * NB: if you add more opcodes specialized from JSOP_NAME, etc., don't forget
 * to update the TOK_FOR (for-in) and TOK_ASSIGN (op=, e.g. +=) special cases
 * in js_EmitTree.
 */
static JSBool
BindNameToSlot(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn)
{
    JSDefinition *dn;
    JSOp op;
    JSAtom *atom;
    JSDefinition::Kind dn_kind;

    JS_ASSERT(pn->isKind(TOK_NAME));

    /* Idempotency tests come first, since we may be called more than once. */
    if (pn->pn_dflags & PND_BOUND)
        return JS_TRUE;

    /* No cookie initialized for these two, they're pre-bound by definition. */
    JS_ASSERT(!pn->isOp(JSOP_ARGUMENTS) && !pn->isOp(JSOP_CALLEE));

    /*
     * The parser linked all uses (including forward references) to their
     * definitions, unless a with statement or direct eval intervened.
     */
    if (pn->isUsed()) {
        JS_ASSERT(pn->pn_cookie.isFree());
        dn = pn->pn_lexdef;
        JS_ASSERT(dn->isDefn());
        if (pn->isDeoptimized())
            return JS_TRUE;
        pn->pn_dflags |= (dn->pn_dflags & PND_CONST);
    } else {
        if (!pn->isDefn())
            return JS_TRUE;
        dn = (JSDefinition *) pn;
    }

    op = pn->getOp();
    if (op == JSOP_NOP)
        return JS_TRUE;

    JS_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);
    atom = pn->pn_atom;
    UpvarCookie cookie = dn->pn_cookie;
    dn_kind = dn->kind();

    /*
     * Turn attempts to mutate const-declared bindings into get ops (for
     * pre-increment and pre-decrement ops, our caller will have to emit
     * JSOP_POS, JSOP_ONE, and JSOP_ADD as well).
     *
     * Turn JSOP_DELNAME into JSOP_FALSE if dn is known, as all declared
     * bindings visible to the compiler are permanent in JS unless the
     * declaration originates at top level in eval code.
     */
    switch (op) {
      case JSOP_NAME:
      case JSOP_SETCONST:
        break;
      case JSOP_DELNAME:
        if (dn_kind != JSDefinition::UNKNOWN) {
            if (cg->parser->callerFrame && dn->isTopLevel())
                JS_ASSERT(cg->compileAndGo());
            else
                pn->setOp(JSOP_FALSE);
            pn->pn_dflags |= PND_BOUND;
            return JS_TRUE;
        }
        break;
      default:
        if (pn->isConst()) {
            if (cg->needStrictChecks()) {
                JSAutoByteString name;
                if (!js_AtomToPrintableString(cx, atom, &name) ||
                    !ReportStrictModeError(cx, CG_TS(cg), cg, pn, JSMSG_READ_ONLY, name.ptr())) {
                    return JS_FALSE;
                }
            }
            pn->setOp(op = JSOP_NAME);
        }
    }

    if (dn->isGlobal()) {
        if (op == JSOP_NAME) {
            /*
             * If the definition is a defined global, not potentially aliased
             * by a local variable, and not mutating the variable, try and
             * optimize to a fast, unguarded global access.
             */
            if (!BindKnownGlobal(cx, cg, dn, pn, atom))
                return JS_FALSE;
            if (!pn->pn_cookie.isFree()) {
                pn->setOp(JSOP_GETGLOBAL);
                return JS_TRUE;
            }
        }

        /*
         * The locally stored cookie here should really come from |pn|, not
         * |dn|. For example, we could have a SETGNAME op's lexdef be a
         * GETGLOBAL op, and their cookies have very different meanings. As
         * a workaround, just make the cookie free.
         */
        cookie.makeFree();
    }

    if (cookie.isFree()) {
        StackFrame *caller = cg->parser->callerFrame;
        if (caller) {
            JS_ASSERT(cg->compileAndGo());

            /*
             * Don't generate upvars on the left side of a for loop. See
             * bug 470758.
             */
            if (cg->flags & TCF_IN_FOR_INIT)
                return JS_TRUE;

            JS_ASSERT(caller->isScriptFrame());

            /*
             * If this is an eval in the global scope, then unbound variables
             * must be globals, so try to use GNAME ops.
             */
            if (caller->isGlobalFrame() && TryConvertToGname(cg, pn, &op)) {
                jsatomid _;
                if (!cg->makeAtomIndex(atom, &_))
                    return JS_FALSE;

                pn->setOp(op);
                pn->pn_dflags |= PND_BOUND;
                return JS_TRUE;
            }

            /*
             * Out of tricks, so we must rely on PICs to optimize named
             * accesses from direct eval called from function code.
             */
            return JS_TRUE;
        }

        /* Optimize accesses to undeclared globals. */
        if (!cg->mightAliasLocals() && !TryConvertToGname(cg, pn, &op))
            return JS_TRUE;

        jsatomid _;
        if (!cg->makeAtomIndex(atom, &_))
            return JS_FALSE;

        pn->setOp(op);
        pn->pn_dflags |= PND_BOUND;

        return JS_TRUE;
    }

    uint16 level = cookie.level();
    JS_ASSERT(cg->staticLevel >= level);

    const uintN skip = cg->staticLevel - level;
    if (skip != 0) {
        JS_ASSERT(cg->inFunction());
        JS_ASSERT_IF(cookie.slot() != UpvarCookie::CALLEE_SLOT, cg->roLexdeps->lookup(atom));
        JS_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);
        JS_ASSERT(cg->fun()->u.i.skipmin <= skip);

        /*
         * If op is a mutating opcode, this upvar's lookup skips too many levels,
         * or the function is heavyweight, we fall back on JSOP_*NAME*.
         */
        if (op != JSOP_NAME)
            return JS_TRUE;
        if (skip >= UpvarCookie::UPVAR_LEVEL_LIMIT)
            return JS_TRUE;
        if (cg->flags & TCF_FUN_HEAVYWEIGHT)
            return JS_TRUE;

        if (!cg->fun()->isFlatClosure())
            return JS_TRUE;

        if (!cg->upvarIndices.ensureMap(cx))
            return JS_FALSE;

        AtomIndexAddPtr p = cg->upvarIndices->lookupForAdd(atom);
        jsatomid index;
        if (p) {
            index = p.value();
        } else {
            if (!cg->bindings.addUpvar(cx, atom))
                return JS_FALSE;

            index = cg->upvarIndices->count();
            if (!cg->upvarIndices->add(p, atom, index))
                return JS_FALSE;

            UpvarCookies &upvarMap = cg->upvarMap;
            /* upvarMap should have the same number of UpvarCookies as there are lexdeps. */
            size_t lexdepCount = cg->roLexdeps->count();

            JS_ASSERT_IF(!upvarMap.empty(), lexdepCount == upvarMap.length());
            if (upvarMap.empty()) {
                /* Lazily initialize the upvar map with exactly the necessary capacity. */
                if (lexdepCount <= upvarMap.sMaxInlineStorage) {
                    JS_ALWAYS_TRUE(upvarMap.growByUninitialized(lexdepCount));
                } else {
                    void *buf = upvarMap.allocPolicy().malloc_(lexdepCount * sizeof(UpvarCookie));
                    if (!buf)
                        return JS_FALSE;
                    upvarMap.replaceRawBuffer(static_cast<UpvarCookie *>(buf), lexdepCount);
                }
                for (size_t i = 0; i < lexdepCount; ++i)
                    upvarMap[i] = UpvarCookie();
            }

            uintN slot = cookie.slot();
            if (slot != UpvarCookie::CALLEE_SLOT && dn_kind != JSDefinition::ARG) {
                JSTreeContext *tc = cg;
                do {
                    tc = tc->parent;
                } while (tc->staticLevel != level);
                if (tc->inFunction())
                    slot += tc->fun()->nargs;
            }

            JS_ASSERT(index < upvarMap.length());
            upvarMap[index].set(skip, slot);
        }

        pn->setOp(JSOP_GETFCSLOT);
        JS_ASSERT((index & JS_BITMASK(16)) == index);
        pn->pn_cookie.set(0, index);
        pn->pn_dflags |= PND_BOUND;
        return JS_TRUE;
    }

    /*
     * We are compiling a function body and may be able to optimize name
     * to stack slot. Look for an argument or variable in the function and
     * rewrite pn_op and update pn accordingly.
     */
    switch (dn_kind) {
      case JSDefinition::UNKNOWN:
        return JS_TRUE;

      case JSDefinition::LET:
        switch (op) {
          case JSOP_NAME:     op = JSOP_GETLOCAL; break;
          case JSOP_SETNAME:  op = JSOP_SETLOCAL; break;
          case JSOP_INCNAME:  op = JSOP_INCLOCAL; break;
          case JSOP_NAMEINC:  op = JSOP_LOCALINC; break;
          case JSOP_DECNAME:  op = JSOP_DECLOCAL; break;
          case JSOP_NAMEDEC:  op = JSOP_LOCALDEC; break;
          default: JS_NOT_REACHED("let");
        }
        break;

      case JSDefinition::ARG:
        switch (op) {
          case JSOP_NAME:     op = JSOP_GETARG; break;
          case JSOP_SETNAME:  op = JSOP_SETARG; break;
          case JSOP_INCNAME:  op = JSOP_INCARG; break;
          case JSOP_NAMEINC:  op = JSOP_ARGINC; break;
          case JSOP_DECNAME:  op = JSOP_DECARG; break;
          case JSOP_NAMEDEC:  op = JSOP_ARGDEC; break;
          default: JS_NOT_REACHED("arg");
        }
        JS_ASSERT(!pn->isConst());
        break;

      case JSDefinition::VAR:
        if (dn->isOp(JSOP_CALLEE)) {
            JS_ASSERT(op != JSOP_CALLEE);
            JS_ASSERT((cg->fun()->flags & JSFUN_LAMBDA) && atom == cg->fun()->atom);

            /*
             * Leave pn->isOp(JSOP_NAME) if cg->fun is heavyweight to
             * address two cases: a new binding introduced by eval, and
             * assignment to the name in strict mode.
             *
             *   var fun = (function f(s) { eval(s); return f; });
             *   assertEq(fun("var f = 42"), 42);
             *
             * ECMAScript specifies that a function expression's name is bound
             * in a lexical environment distinct from that used to bind its
             * named parameters, the arguments object, and its variables.  The
             * new binding for "var f = 42" shadows the binding for the
             * function itself, so the name of the function will not refer to
             * the function.
             *
             *    (function f() { "use strict"; f = 12; })();
             *
             * Outside strict mode, assignment to a function expression's name
             * has no effect.  But in strict mode, this attempt to mutate an
             * immutable binding must throw a TypeError.  We implement this by
             * not optimizing such assignments and by marking such functions as
             * heavyweight, ensuring that the function name is represented in
             * the scope chain so that assignment will throw a TypeError.
             */
            JS_ASSERT(op != JSOP_DELNAME);
            if (!(cg->flags & TCF_FUN_HEAVYWEIGHT)) {
                op = JSOP_CALLEE;
                pn->pn_dflags |= PND_CONST;
            }

            pn->setOp(op);
            pn->pn_dflags |= PND_BOUND;
            return JS_TRUE;
        }
        /* FALL THROUGH */

      default:
        JS_ASSERT_IF(dn_kind != JSDefinition::FUNCTION,
                     dn_kind == JSDefinition::VAR ||
                     dn_kind == JSDefinition::CONST);
        switch (op) {
          case JSOP_NAME:     op = JSOP_GETLOCAL; break;
          case JSOP_SETNAME:  op = JSOP_SETLOCAL; break;
          case JSOP_SETCONST: op = JSOP_SETLOCAL; break;
          case JSOP_INCNAME:  op = JSOP_INCLOCAL; break;
          case JSOP_NAMEINC:  op = JSOP_LOCALINC; break;
          case JSOP_DECNAME:  op = JSOP_DECLOCAL; break;
          case JSOP_NAMEDEC:  op = JSOP_LOCALDEC; break;
          default: JS_NOT_REACHED("local");
        }
        JS_ASSERT_IF(dn_kind == JSDefinition::CONST, pn->pn_dflags & PND_CONST);
        break;
    }

    JS_ASSERT(!pn->isOp(op));
    pn->setOp(op);
    pn->pn_cookie.set(0, cookie.slot());
    pn->pn_dflags |= PND_BOUND;
    return JS_TRUE;
}

bool
JSCodeGenerator::addGlobalUse(JSAtom *atom, uint32 slot, UpvarCookie *cookie)
{
    if (!globalMap.ensureMap(context()))
        return false;

    AtomIndexAddPtr p = globalMap->lookupForAdd(atom);
    if (p) {
        jsatomid index = p.value();
        cookie->set(0, index);
        return true;
    }

    /* Don't bother encoding indexes >= uint16 */
    if (globalUses.length() >= UINT16_LIMIT) {
        cookie->makeFree();
        return true;
    }

    /* Find or add an existing atom table entry. */
    jsatomid allAtomIndex;
    if (!makeAtomIndex(atom, &allAtomIndex))
        return false;

    jsatomid globalUseIndex = globalUses.length();
    cookie->set(0, globalUseIndex);

    GlobalSlotArray::Entry entry = { allAtomIndex, slot };
    if (!globalUses.append(entry))
        return false;

    return globalMap->add(p, atom, globalUseIndex);
}

/*
 * If pn contains a useful expression, return true with *answer set to true.
 * If pn contains a useless expression, return true with *answer set to false.
 * Return false on error.
 *
 * The caller should initialize *answer to false and invoke this function on
 * an expression statement or similar subtree to decide whether the tree could
 * produce code that has any side effects.  For an expression statement, we
 * define useless code as code with no side effects, because the main effect,
 * the value left on the stack after the code executes, will be discarded by a
 * pop bytecode.
 */
static JSBool
CheckSideEffects(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn,
                 JSBool *answer)
{
    JSBool ok;
    JSParseNode *pn2;

    ok = JS_TRUE;
    if (!pn || *answer)
        return ok;

    switch (pn->getArity()) {
      case PN_FUNC:
        /*
         * A named function, contrary to ES3, is no longer useful, because we
         * bind its name lexically (using JSOP_CALLEE) instead of creating an
         * Object instance and binding a readonly, permanent property in it
         * (the object and binding can be detected and hijacked or captured).
         * This is a bug fix to ES3; it is fixed in ES3.1 drafts.
         */
        *answer = JS_FALSE;
        break;

      case PN_LIST:
        if (pn->isOp(JSOP_NOP) || pn->isOp(JSOP_OR) || pn->isOp(JSOP_AND) ||
            pn->isOp(JSOP_STRICTEQ) || pn->isOp(JSOP_STRICTNE)) {
            /*
             * Non-operators along with ||, &&, ===, and !== never invoke
             * toString or valueOf.
             */
            for (pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next)
                ok &= CheckSideEffects(cx, cg, pn2, answer);
        } else {
            /*
             * All invocation operations (construct: TOK_NEW, call: TOK_LP)
             * are presumed to be useful, because they may have side effects
             * even if their main effect (their return value) is discarded.
             *
             * TOK_LB binary trees of 3 or more nodes are flattened into lists
             * to avoid too much recursion.  All such lists must be presumed
             * to be useful because each index operation could invoke a getter
             * (the JSOP_ARGUMENTS special case below, in the PN_BINARY case,
             * does not apply here: arguments[i][j] might invoke a getter).
             *
             * Likewise, array and object initialisers may call prototype
             * setters (the __defineSetter__ built-in, and writable __proto__
             * on Array.prototype create this hazard). Initialiser list nodes
             * have JSOP_NEWINIT in their pn_op.
             */
            *answer = JS_TRUE;
        }
        break;

      case PN_TERNARY:
        ok = CheckSideEffects(cx, cg, pn->pn_kid1, answer) &&
             CheckSideEffects(cx, cg, pn->pn_kid2, answer) &&
             CheckSideEffects(cx, cg, pn->pn_kid3, answer);
        break;

      case PN_BINARY:
        if (pn->isKind(TOK_ASSIGN)) {
            /*
             * Assignment is presumed to be useful, even if the next operation
             * is another assignment overwriting this one's ostensible effect,
             * because the left operand may be a property with a setter that
             * has side effects.
             *
             * The only exception is assignment of a useless value to a const
             * declared in the function currently being compiled.
             */
            pn2 = pn->pn_left;
            if (!pn2->isKind(TOK_NAME)) {
                *answer = JS_TRUE;
            } else {
                if (!BindNameToSlot(cx, cg, pn2))
                    return JS_FALSE;
                if (!CheckSideEffects(cx, cg, pn->pn_right, answer))
                    return JS_FALSE;
                if (!*answer && (!pn->isOp(JSOP_NOP) || !pn2->isConst()))
                    *answer = JS_TRUE;
            }
        } else {
            if (pn->isOp(JSOP_OR) || pn->isOp(JSOP_AND) || pn->isOp(JSOP_STRICTEQ) ||
                pn->isOp(JSOP_STRICTNE)) {
                /*
                 * ||, &&, ===, and !== do not convert their operands via
                 * toString or valueOf method calls.
                 */
                ok = CheckSideEffects(cx, cg, pn->pn_left, answer) &&
                     CheckSideEffects(cx, cg, pn->pn_right, answer);
            } else {
                /*
                 * We can't easily prove that neither operand ever denotes an
                 * object with a toString or valueOf method.
                 */
                *answer = JS_TRUE;
            }
        }
        break;

      case PN_UNARY:
        switch (pn->getKind()) {
          case TOK_DELETE:
            pn2 = pn->pn_kid;
            switch (pn2->getKind()) {
              case TOK_NAME:
                if (!BindNameToSlot(cx, cg, pn2))
                    return JS_FALSE;
                if (pn2->isConst()) {
                    *answer = JS_FALSE;
                    break;
                }
                /* FALL THROUGH */
              case TOK_DOT:
#if JS_HAS_XML_SUPPORT
              case TOK_DBLDOT:
#endif
              case TOK_LP:
              case TOK_LB:
                /* All these delete addressing modes have effects too. */
                *answer = JS_TRUE;
                break;
              default:
                ok = CheckSideEffects(cx, cg, pn2, answer);
                break;
            }
            break;

          case TOK_UNARYOP:
            if (pn->isOp(JSOP_NOT)) {
                /* ! does not convert its operand via toString or valueOf. */
                ok = CheckSideEffects(cx, cg, pn->pn_kid, answer);
                break;
            }
            /* FALL THROUGH */

          default:
            /*
             * All of TOK_INC, TOK_DEC, TOK_THROW, TOK_YIELD, and TOK_DEFSHARP
             * have direct effects. Of the remaining unary-arity node types,
             * we can't easily prove that the operand never denotes an object
             * with a toString or valueOf method.
             */
            *answer = JS_TRUE;
            break;
        }
        break;

      case PN_NAME:
        /*
         * Take care to avoid trying to bind a label name (labels, both for
         * statements and property values in object initialisers, have pn_op
         * defaulted to JSOP_NOP).
         */
        if (pn->isKind(TOK_NAME) && !pn->isOp(JSOP_NOP)) {
            if (!BindNameToSlot(cx, cg, pn))
                return JS_FALSE;
            if (!pn->isOp(JSOP_ARGUMENTS) && !pn->isOp(JSOP_CALLEE) &&
                pn->pn_cookie.isFree()) {
                /*
                 * Not an argument or local variable use, and not a use of a
                 * unshadowed named function expression's given name, so this
                 * expression could invoke a getter that has side effects.
                 */
                *answer = JS_TRUE;
            }
        }
        pn2 = pn->maybeExpr();
        if (pn->isKind(TOK_DOT)) {
            if (pn2->isKind(TOK_NAME) && !BindNameToSlot(cx, cg, pn2))
                return JS_FALSE;
            if (!(pn2->isOp(JSOP_ARGUMENTS) &&
                  pn->pn_atom == cx->runtime->atomState.lengthAtom)) {
                /*
                 * Any dotted property reference could call a getter, except
                 * for arguments.length where arguments is unambiguous.
                 */
                *answer = JS_TRUE;
            }
        }
        ok = CheckSideEffects(cx, cg, pn2, answer);
        break;

      case PN_NAMESET:
        ok = CheckSideEffects(cx, cg, pn->pn_tree, answer);
        break;

      case PN_NULLARY:
        if (pn->isKind(TOK_DEBUGGER))
            *answer = JS_TRUE;
        break;
    }
    return ok;
}

static JSBool
EmitNameOp(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn,
           JSBool callContext)
{
    JSOp op;

    if (!BindNameToSlot(cx, cg, pn))
        return JS_FALSE;
    op = pn->getOp();

    if (callContext) {
        switch (op) {
          case JSOP_NAME:
            op = JSOP_CALLNAME;
            break;
          case JSOP_GETGNAME:
            op = JSOP_CALLGNAME;
            break;
          case JSOP_GETGLOBAL:
            op = JSOP_CALLGLOBAL;
            break;
          case JSOP_GETARG:
            op = JSOP_CALLARG;
            break;
          case JSOP_GETLOCAL:
            op = JSOP_CALLLOCAL;
            break;
          case JSOP_GETFCSLOT:
            op = JSOP_CALLFCSLOT;
            break;
          default:
            JS_ASSERT(op == JSOP_ARGUMENTS || op == JSOP_CALLEE);
            break;
        }
    }

    if (op == JSOP_ARGUMENTS || op == JSOP_CALLEE) {
        if (js_Emit1(cx, cg, op) < 0)
            return JS_FALSE;
        if (callContext && js_Emit1(cx, cg, JSOP_PUSH) < 0)
            return JS_FALSE;
    } else {
        if (!pn->pn_cookie.isFree()) {
            EMIT_UINT16_IMM_OP(op, pn->pn_cookie.asInteger());
        } else {
            if (!EmitAtomOp(cx, pn, op, cg))
                return JS_FALSE;
        }
    }

    return JS_TRUE;
}

#if JS_HAS_XML_SUPPORT
static JSBool
EmitXMLName(JSContext *cx, JSParseNode *pn, JSOp op, JSCodeGenerator *cg)
{
    JSParseNode *pn2;
    uintN oldflags;

    JS_ASSERT(pn->isKind(TOK_UNARYOP));
    JS_ASSERT(pn->isOp(JSOP_XMLNAME));
    JS_ASSERT(op == JSOP_XMLNAME || op == JSOP_CALLXMLNAME);

    pn2 = pn->pn_kid;
    oldflags = cg->flags;
    cg->flags &= ~TCF_IN_FOR_INIT;
    if (!js_EmitTree(cx, cg, pn2))
        return JS_FALSE;
    cg->flags |= oldflags & TCF_IN_FOR_INIT;
    if (js_NewSrcNote2(cx, cg, SRC_PCBASE,
                       CG_OFFSET(cg) - pn2->pn_offset) < 0) {
        return JS_FALSE;
    }

    return js_Emit1(cx, cg, op) >= 0;
}
#endif

static inline bool
EmitElemOpBase(JSContext *cx, JSCodeGenerator *cg, JSOp op)
{
    if (js_Emit1(cx, cg, op) < 0)
        return false;
    CheckTypeSet(cx, cg, op);
    return true;
}

static bool
EmitSpecialPropOp(JSContext *cx, JSParseNode *pn, JSOp op, JSCodeGenerator *cg)
{
    /*
     * Special case for obj.__proto__ to deoptimize away from fast paths in the
     * interpreter and trace recorder, which skip dense array instances by
     * going up to Array.prototype before looking up the property name.
     */
    jsatomid index;
    if (!cg->makeAtomIndex(pn->pn_atom, &index))
        return false;
    if (!EmitIndexOp(cx, JSOP_QNAMEPART, index, cg))
        return false;
    return EmitElemOpBase(cx, cg, op);
}

static JSBool
EmitPropOp(JSContext *cx, JSParseNode *pn, JSOp op, JSCodeGenerator *cg,
           JSBool callContext, JSOp *psuffix = NULL)
{
    JSParseNode *pn2, *pndot, *pnup, *pndown;
    ptrdiff_t top;

    JS_ASSERT(pn->isArity(PN_NAME));
    pn2 = pn->maybeExpr();

    /* Special case deoptimization for __proto__. */
    if ((op == JSOP_GETPROP || op == JSOP_CALLPROP) &&
        pn->pn_atom == cx->runtime->atomState.protoAtom) {
        if (pn2 && !js_EmitTree(cx, cg, pn2))
            return JS_FALSE;
        return EmitSpecialPropOp(cx, pn, callContext ? JSOP_CALLELEM : JSOP_GETELEM, cg);
    }

    if (callContext) {
        JS_ASSERT(pn->isKind(TOK_DOT));
        JS_ASSERT(op == JSOP_GETPROP);
        op = JSOP_CALLPROP;
    } else if (op == JSOP_GETPROP && pn->isKind(TOK_DOT)) {
        if (pn2->isKind(TOK_NAME)) {
            /*
             * Try to optimize arguments.length into JSOP_ARGCNT. If type
             * inference is enabled this is optimized separately.
             */
            if (!BindNameToSlot(cx, cg, pn2))
                return JS_FALSE;
            if (!cx->typeInferenceEnabled() &&
                pn->pn_atom == cx->runtime->atomState.lengthAtom) {
                if (pn2->isOp(JSOP_ARGUMENTS))
                    return js_Emit1(cx, cg, JSOP_ARGCNT) >= 0;
            }
        }
    }

    /*
     * If the object operand is also a dotted property reference, reverse the
     * list linked via pn_expr temporarily so we can iterate over it from the
     * bottom up (reversing again as we go), to avoid excessive recursion.
     */
    if (pn2->isKind(TOK_DOT)) {
        pndot = pn2;
        pnup = NULL;
        top = CG_OFFSET(cg);
        for (;;) {
            /* Reverse pndot->pn_expr to point up, not down. */
            pndot->pn_offset = top;
            JS_ASSERT(!pndot->isUsed());
            pndown = pndot->pn_expr;
            pndot->pn_expr = pnup;
            if (!pndown->isKind(TOK_DOT))
                break;
            pnup = pndot;
            pndot = pndown;
        }

        /* pndown is a primary expression, not a dotted property reference. */
        if (!js_EmitTree(cx, cg, pndown))
            return JS_FALSE;

        do {
            /* Walk back up the list, emitting annotated name ops. */
            if (js_NewSrcNote2(cx, cg, SRC_PCBASE,
                               CG_OFFSET(cg) - pndown->pn_offset) < 0) {
                return JS_FALSE;
            }

            /* Special case deoptimization on __proto__, as above. */
            if (pndot->isArity(PN_NAME) && pndot->pn_atom == cx->runtime->atomState.protoAtom) {
                if (!EmitSpecialPropOp(cx, pndot, JSOP_GETELEM, cg))
                    return JS_FALSE;
            } else if (!EmitAtomOp(cx, pndot, pndot->getOp(), cg)) {
                return JS_FALSE;
            }

            /* Reverse the pn_expr link again. */
            pnup = pndot->pn_expr;
            pndot->pn_expr = pndown;
            pndown = pndot;
        } while ((pndot = pnup) != NULL);
    } else {
        if (!js_EmitTree(cx, cg, pn2))
            return JS_FALSE;
    }

    if (js_NewSrcNote2(cx, cg, SRC_PCBASE,
                       CG_OFFSET(cg) - pn2->pn_offset) < 0) {
        return JS_FALSE;
    }

    return EmitAtomOp(cx, pn, op, cg, psuffix);
}

static bool
EmitPropIncDec(JSContext *cx, JSParseNode *pn, JSOp op, JSCodeGenerator *cg)
{
    JSOp suffix = JSOP_NOP;
    if (!EmitPropOp(cx, pn, op, cg, false, &suffix))
        return false;
    if (js_Emit1(cx, cg, JSOP_NOP) < 0)
        return false;

    /*
     * The stack is the same depth before/after INCPROP, so no balancing to do
     * before the decomposed version.
     */
    int start = CG_OFFSET(cg);

    if (suffix != JSOP_NOP && js_Emit1(cx, cg, suffix) < 0)
        return false;

    const JSCodeSpec *cs = &js_CodeSpec[op];
    JS_ASSERT(cs->format & JOF_PROP);
    JS_ASSERT(cs->format & (JOF_INC | JOF_DEC));

    bool post = (cs->format & JOF_POST);
    JSOp binop = (cs->format & JOF_INC) ? JSOP_ADD : JSOP_SUB;

                                                   // OBJ
    if (js_Emit1(cx, cg, JSOP_DUP) < 0)            // OBJ OBJ
        return false;
    if (!EmitAtomOp(cx, pn, JSOP_GETPROP, cg))     // OBJ V
        return false;
    if (js_Emit1(cx, cg, JSOP_POS) < 0)            // OBJ N
        return false;
    if (post && js_Emit1(cx, cg, JSOP_DUP) < 0)    // OBJ N? N
        return false;
    if (js_Emit1(cx, cg, JSOP_ONE) < 0)            // OBJ N? N 1
        return false;
    if (js_Emit1(cx, cg, binop) < 0)               // OBJ N? N+1
        return false;

    if (post) {
        if (js_Emit2(cx, cg, JSOP_PICK, (jsbytecode)2) < 0) // N? N+1 OBJ
            return false;
        if (js_Emit1(cx, cg, JSOP_SWAP) < 0)                // N? OBJ N+1
            return false;
    }

    if (!EmitAtomOp(cx, pn, JSOP_SETPROP, cg))     // N? N+1
        return false;
    if (post && js_Emit1(cx, cg, JSOP_POP) < 0)    // RESULT
        return false;

    UpdateDecomposeLength(cg, start);

    if (suffix != JSOP_NOP && js_Emit1(cx, cg, suffix) < 0)
        return false;

    return true;
}

static bool
EmitNameIncDec(JSContext *cx, JSParseNode *pn, JSOp op, JSCodeGenerator *cg)
{
    JSOp suffix = JSOP_NOP;
    if (!EmitAtomOp(cx, pn, op, cg, &suffix))
        return false;
    if (js_Emit1(cx, cg, JSOP_NOP) < 0)
        return false;

    /* Remove the result to restore the stack depth before the INCNAME. */
    cg->stackDepth--;

    int start = CG_OFFSET(cg);

    if (suffix != JSOP_NOP && js_Emit1(cx, cg, suffix) < 0)
        return false;

    const JSCodeSpec *cs = &js_CodeSpec[op];
    JS_ASSERT((cs->format & JOF_NAME) || (cs->format & JOF_GNAME));
    JS_ASSERT(cs->format & (JOF_INC | JOF_DEC));

    bool global = (cs->format & JOF_GNAME);
    bool post = (cs->format & JOF_POST);
    JSOp binop = (cs->format & JOF_INC) ? JSOP_ADD : JSOP_SUB;

    if (!EmitAtomOp(cx, pn, global ? JSOP_BINDGNAME : JSOP_BINDNAME, cg))  // OBJ
        return false;
    if (!EmitAtomOp(cx, pn, global ? JSOP_GETGNAME : JSOP_NAME, cg))       // OBJ V
        return false;
    if (js_Emit1(cx, cg, JSOP_POS) < 0)            // OBJ N
        return false;
    if (post && js_Emit1(cx, cg, JSOP_DUP) < 0)    // OBJ N? N
        return false;
    if (js_Emit1(cx, cg, JSOP_ONE) < 0)            // OBJ N? N 1
        return false;
    if (js_Emit1(cx, cg, binop) < 0)               // OBJ N? N+1
        return false;

    if (post) {
        if (js_Emit2(cx, cg, JSOP_PICK, (jsbytecode)2) < 0) // N? N+1 OBJ
            return false;
        if (js_Emit1(cx, cg, JSOP_SWAP) < 0)                // N? OBJ N+1
            return false;
    }

    if (!EmitAtomOp(cx, pn, global ? JSOP_SETGNAME : JSOP_SETNAME, cg))     // N? N+1
        return false;
    if (post && js_Emit1(cx, cg, JSOP_POP) < 0)    // RESULT
        return false;

    UpdateDecomposeLength(cg, start);

    if (suffix != JSOP_NOP && js_Emit1(cx, cg, suffix) < 0)
        return false;

    return true;
}

static JSBool
EmitElemOp(JSContext *cx, JSParseNode *pn, JSOp op, JSCodeGenerator *cg)
{
    ptrdiff_t top;
    JSParseNode *left, *right, *next, ltmp, rtmp;
    int32_t slot;

    top = CG_OFFSET(cg);
    if (pn->isArity(PN_LIST)) {
        /* Left-associative operator chain to avoid too much recursion. */
        JS_ASSERT(pn->isOp(JSOP_GETELEM));
        JS_ASSERT(pn->pn_count >= 3);
        left = pn->pn_head;
        right = pn->last();
        next = left->pn_next;
        JS_ASSERT(next != right);

        /*
         * Try to optimize arguments[0][j]... into JSOP_ARGSUB<0> followed by
         * one or more index expression and JSOP_GETELEM op pairs. If type
         * inference is enabled this is optimized separately.
         */
        if (left->isKind(TOK_NAME) && next->isKind(TOK_NUMBER)) {
            if (!BindNameToSlot(cx, cg, left))
                return JS_FALSE;
            if (left->isOp(JSOP_ARGUMENTS) &&
                JSDOUBLE_IS_INT32(next->pn_dval, &slot) &&
                jsuint(slot) < JS_BIT(16) &&
                !cx->typeInferenceEnabled() &&
                (!cg->inStrictMode() ||
                 (!cg->mutatesParameter() && !cg->callsEval()))) {
                /*
                 * arguments[i]() requires arguments object as "this".
                 * Check that we never generates list for that usage.
                 */
                JS_ASSERT(op != JSOP_CALLELEM || next->pn_next);
                left->pn_offset = next->pn_offset = top;
                EMIT_UINT16_IMM_OP(JSOP_ARGSUB, (jsatomid)slot);
                left = next;
                next = left->pn_next;
            }
        }

        /*
         * Check whether we generated JSOP_ARGSUB, just above, and have only
         * one more index expression to emit.  Given arguments[0][j], we must
         * skip the while loop altogether, falling through to emit code for j
         * (in the subtree referenced by right), followed by the annotated op,
         * at the bottom of this function.
         */
        JS_ASSERT(next != right || pn->pn_count == 3);
        if (left == pn->pn_head) {
            if (!js_EmitTree(cx, cg, left))
                return JS_FALSE;
        }
        while (next != right) {
            if (!js_EmitTree(cx, cg, next))
                return JS_FALSE;
            if (js_NewSrcNote2(cx, cg, SRC_PCBASE, CG_OFFSET(cg) - top) < 0)
                return JS_FALSE;
            if (!EmitElemOpBase(cx, cg, JSOP_GETELEM))
                return JS_FALSE;
            next = next->pn_next;
        }
    } else {
        if (pn->isArity(PN_NAME)) {
            /*
             * Set left and right so pn appears to be a TOK_LB node, instead
             * of a TOK_DOT node.  See the TOK_FOR/IN case in js_EmitTree, and
             * EmitDestructuringOps nearer below.  In the destructuring case,
             * the base expression (pn_expr) of the name may be null, which
             * means we have to emit a JSOP_BINDNAME.
             */
            left = pn->maybeExpr();
            if (!left) {
                left = &ltmp;
                left->setKind(TOK_STRING);
                left->setOp(JSOP_BINDNAME);
                left->setArity(PN_NULLARY);
                left->pn_pos = pn->pn_pos;
                left->pn_atom = pn->pn_atom;
            }
            right = &rtmp;
            right->setKind(TOK_STRING);
            right->setOp(js_IsIdentifier(pn->pn_atom) ? JSOP_QNAMEPART : JSOP_STRING);
            right->setArity(PN_NULLARY);
            right->pn_pos = pn->pn_pos;
            right->pn_atom = pn->pn_atom;
        } else {
            JS_ASSERT(pn->isArity(PN_BINARY));
            left = pn->pn_left;
            right = pn->pn_right;
        }

        /*
         * Try to optimize arguments[0] (e.g.) into JSOP_ARGSUB<0>. If type
         * inference is enabled this is optimized separately.
         */
        if (op == JSOP_GETELEM &&
            left->isKind(TOK_NAME) &&
            right->isKind(TOK_NUMBER)) {
            if (!BindNameToSlot(cx, cg, left))
                return JS_FALSE;
            if (left->isOp(JSOP_ARGUMENTS) &&
                JSDOUBLE_IS_INT32(right->pn_dval, &slot) &&
                jsuint(slot) < JS_BIT(16) &&
                !cx->typeInferenceEnabled() &&
                (!cg->inStrictMode() ||
                 (!cg->mutatesParameter() && !cg->callsEval()))) {
                left->pn_offset = right->pn_offset = top;
                EMIT_UINT16_IMM_OP(JSOP_ARGSUB, (jsatomid)slot);
                return JS_TRUE;
            }
        }

        if (!js_EmitTree(cx, cg, left))
            return JS_FALSE;
    }

    /* The right side of the descendant operator is implicitly quoted. */
    JS_ASSERT(op != JSOP_DESCENDANTS || !right->isKind(TOK_STRING) ||
              right->isOp(JSOP_QNAMEPART));
    if (!js_EmitTree(cx, cg, right))
        return JS_FALSE;
    if (js_NewSrcNote2(cx, cg, SRC_PCBASE, CG_OFFSET(cg) - top) < 0)
        return JS_FALSE;
    return EmitElemOpBase(cx, cg, op);
}

static bool
EmitElemIncDec(JSContext *cx, JSParseNode *pn, JSOp op, JSCodeGenerator *cg)
{
    if (pn) {
        if (!EmitElemOp(cx, pn, op, cg))
            return false;
    } else {
        if (!EmitElemOpBase(cx, cg, op))
            return false;
    }
    if (js_Emit1(cx, cg, JSOP_NOP) < 0)
        return false;

    /* INCELEM pops two values and pushes one, so restore the initial depth. */
    cg->stackDepth++;

    int start = CG_OFFSET(cg);

    const JSCodeSpec *cs = &js_CodeSpec[op];
    JS_ASSERT(cs->format & JOF_ELEM);
    JS_ASSERT(cs->format & (JOF_INC | JOF_DEC));

    bool post = (cs->format & JOF_POST);
    JSOp binop = (cs->format & JOF_INC) ? JSOP_ADD : JSOP_SUB;

    /*
     * We need to convert the key to an object id first, so that we do not do
     * it inside both the GETELEM and the SETELEM.
     */
                                                 // OBJ KEY*
    if (js_Emit1(cx, cg, JSOP_TOID) < 0)         // OBJ KEY
        return false;
    if (js_Emit1(cx, cg, JSOP_DUP2) < 0)         // OBJ KEY OBJ KEY
        return false;
    if (!EmitElemOpBase(cx, cg, JSOP_GETELEM))   // OBJ KEY V
        return false;
    if (js_Emit1(cx, cg, JSOP_POS) < 0)          // OBJ KEY N
        return false;
    if (post && js_Emit1(cx, cg, JSOP_DUP) < 0)  // OBJ KEY N? N
        return false;
    if (js_Emit1(cx, cg, JSOP_ONE) < 0)          // OBJ KEY N? N 1
        return false;
    if (js_Emit1(cx, cg, binop) < 0)             // OBJ KEY N? N+1
        return false;

    if (post) {
        if (js_Emit2(cx, cg, JSOP_PICK, (jsbytecode)3) < 0)  // KEY N N+1 OBJ
            return false;
        if (js_Emit2(cx, cg, JSOP_PICK, (jsbytecode)3) < 0)  // N N+1 OBJ KEY
            return false;
        if (js_Emit2(cx, cg, JSOP_PICK, (jsbytecode)2) < 0)  // N OBJ KEY N+1
            return false;
    }

    if (!EmitElemOpBase(cx, cg, JSOP_SETELEM))   // N? N+1
        return false;
    if (post && js_Emit1(cx, cg, JSOP_POP) < 0)  // RESULT
        return false;

    UpdateDecomposeLength(cg, start);

    return true;
}

static JSBool
EmitNumberOp(JSContext *cx, jsdouble dval, JSCodeGenerator *cg)
{
    int32_t ival;
    uint32 u;
    ptrdiff_t off;
    jsbytecode *pc;

    if (JSDOUBLE_IS_INT32(dval, &ival)) {
        if (ival == 0)
            return js_Emit1(cx, cg, JSOP_ZERO) >= 0;
        if (ival == 1)
            return js_Emit1(cx, cg, JSOP_ONE) >= 0;
        if ((jsint)(int8)ival == ival)
            return js_Emit2(cx, cg, JSOP_INT8, (jsbytecode)(int8)ival) >= 0;

        u = (uint32)ival;
        if (u < JS_BIT(16)) {
            EMIT_UINT16_IMM_OP(JSOP_UINT16, u);
        } else if (u < JS_BIT(24)) {
            off = js_EmitN(cx, cg, JSOP_UINT24, 3);
            if (off < 0)
                return JS_FALSE;
            pc = CG_CODE(cg, off);
            SET_UINT24(pc, u);
        } else {
            off = js_EmitN(cx, cg, JSOP_INT32, 4);
            if (off < 0)
                return JS_FALSE;
            pc = CG_CODE(cg, off);
            SET_INT32(pc, ival);
        }
        return JS_TRUE;
    }

    if (!cg->constList.append(DoubleValue(dval)))
        return JS_FALSE;

    return EmitIndexOp(cx, JSOP_DOUBLE, cg->constList.length() - 1, cg);
}

/*
 * To avoid bloating all parse nodes for the special case of switch, values are
 * allocated in the temp pool and pointed to by the parse node. These values
 * are not currently recycled (like parse nodes) and the temp pool is only
 * flushed at the end of compiling a script, so these values are technically
 * leaked. This would only be a problem for scripts containing a large number
 * of large switches, which seems unlikely.
 */
static Value *
AllocateSwitchConstant(JSContext *cx)
{
    Value *pv;
    JS_ARENA_ALLOCATE_TYPE(pv, Value, &cx->tempPool);
    return pv;
}

/*
 * Sometimes, let-slots are pushed to the JS stack before we logically enter
 * the let scope. For example,
 *     let (x = EXPR) BODY
 * compiles to roughly {enterblock; EXPR; setlocal x; BODY; leaveblock} even
 * though EXPR is evaluated in the enclosing scope; it does not see x.
 *
 * In those cases we use TempPopScope around the code to emit EXPR. It
 * temporarily removes the let-scope from the JSCodeGenerator's scope stack and
 * emits extra bytecode to ensure that js::GetBlockChain also finds the correct
 * scope at run time.
 */
class TempPopScope {
    JSStmtInfo *savedStmt;
    JSStmtInfo *savedScopeStmt;
    JSObjectBox *savedBlockBox;

  public:
    TempPopScope() : savedStmt(NULL), savedScopeStmt(NULL), savedBlockBox(NULL) {}

    bool popBlock(JSContext *cx, JSCodeGenerator *cg) {
        savedStmt = cg->topStmt;
        savedScopeStmt = cg->topScopeStmt;
        savedBlockBox = cg->blockChainBox;

        if (cg->topStmt->type == STMT_FOR_LOOP || cg->topStmt->type == STMT_FOR_IN_LOOP)
            js_PopStatement(cg);
        JS_ASSERT(STMT_LINKS_SCOPE(cg->topStmt));
        JS_ASSERT(cg->topStmt->flags & SIF_SCOPE);
        js_PopStatement(cg);

        /*
         * Since we have changed the block chain, emit an instruction marking
         * the change for the benefit of dynamic GetScopeChain callers such as
         * the debugger.
         *
         * FIXME bug 671360 - The JSOP_NOP instruction should not be necessary.
         */
        return js_Emit1(cx, cg, JSOP_NOP) >= 0 && EmitBlockChain(cx, cg);
    }

    bool repushBlock(JSContext *cx, JSCodeGenerator *cg) {
        JS_ASSERT(savedStmt);
        cg->topStmt = savedStmt;
        cg->topScopeStmt = savedScopeStmt;
        cg->blockChainBox = savedBlockBox;
        return js_Emit1(cx, cg, JSOP_NOP) >= 0 && EmitBlockChain(cx, cg);
    }
};

static JSBool
EmitSwitch(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn,
           JSStmtInfo *stmtInfo)
{
    JSOp switchOp;
    JSBool ok, hasDefault, constPropagated;
    ptrdiff_t top, off, defaultOffset;
    JSParseNode *pn2, *pn3, *pn4;
    uint32 caseCount, tableLength;
    JSParseNode **table;
    int32_t i, low, high;
    intN noteIndex;
    size_t switchSize, tableSize;
    jsbytecode *pc, *savepc;
#if JS_HAS_BLOCK_SCOPE
    JSObjectBox *box;
#endif

    /* Try for most optimal, fall back if not dense ints, and per ECMAv2. */
    switchOp = JSOP_TABLESWITCH;
    ok = JS_TRUE;
    hasDefault = constPropagated = JS_FALSE;
    defaultOffset = -1;

    /*
     * If the switch contains let variables scoped by its body, model the
     * resulting block on the stack first, before emitting the discriminant's
     * bytecode (in case the discriminant contains a stack-model dependency
     * such as a let expression).
     */
    pn2 = pn->pn_right;
#if JS_HAS_BLOCK_SCOPE
    TempPopScope tps;
    if (pn2->isKind(TOK_LEXICALSCOPE)) {
        /*
         * Push the body's block scope before discriminant code-gen to reflect
         * the order of slots on the stack. The block's locals must lie under
         * the discriminant on the stack so that case-dispatch bytecodes can
         * find the discriminant on top of stack.
         */
        box = pn2->pn_objbox;
        js_PushBlockScope(cg, stmtInfo, box, -1);
        stmtInfo->type = STMT_SWITCH;

        /* Emit JSOP_ENTERBLOCK before code to evaluate the discriminant. */
        if (!EmitEnterBlock(cx, pn2, cg))
            return JS_FALSE;

        /*
         * Pop the switch's statement info around discriminant code-gen, which
         * belongs in the enclosing scope.
         */
        if (!tps.popBlock(cx, cg))
            return JS_FALSE;
    }
#ifdef __GNUC__
    else {
        box = NULL;
    }
#endif
#endif

    /*
     * Emit code for the discriminant first (or nearly first, in the case of a
     * switch whose body is a block scope).
     */
    if (!js_EmitTree(cx, cg, pn->pn_left))
        return JS_FALSE;

    /* Switch bytecodes run from here till end of final case. */
    top = CG_OFFSET(cg);
#if !JS_HAS_BLOCK_SCOPE
    js_PushStatement(cg, stmtInfo, STMT_SWITCH, top);
#else
    if (pn2->isKind(TOK_LC)) {
        js_PushStatement(cg, stmtInfo, STMT_SWITCH, top);
    } else {
        /* Re-push the switch's statement info record. */
        if (!tps.repushBlock(cx, cg))
            return JS_FALSE;

        /*
         * Set the statement info record's idea of top. Reset top too, since
         * repushBlock emits code.
         */
        stmtInfo->update = top = CG_OFFSET(cg);

        /* Advance pn2 to refer to the switch case list. */
        pn2 = pn2->expr();
    }
#endif

    caseCount = pn2->pn_count;
    tableLength = 0;
    table = NULL;

    if (caseCount == 0 ||
        (caseCount == 1 &&
         (hasDefault = (pn2->pn_head->isKind(TOK_DEFAULT))))) {
        caseCount = 0;
        low = 0;
        high = -1;
    } else {
#define INTMAP_LENGTH   256
        jsbitmap intmap_space[INTMAP_LENGTH];
        jsbitmap *intmap = NULL;
        int32 intmap_bitlen = 0;

        low  = JSVAL_INT_MAX;
        high = JSVAL_INT_MIN;

        for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
            if (pn3->isKind(TOK_DEFAULT)) {
                hasDefault = JS_TRUE;
                caseCount--;    /* one of the "cases" was the default */
                continue;
            }

            JS_ASSERT(pn3->isKind(TOK_CASE));
            if (switchOp == JSOP_CONDSWITCH)
                continue;

            pn4 = pn3->pn_left;
            while (pn4->isKind(TOK_RP))
                pn4 = pn4->pn_kid;

            Value constVal;
            switch (pn4->getKind()) {
              case TOK_NUMBER:
                constVal.setNumber(pn4->pn_dval);
                break;
              case TOK_STRING:
                constVal.setString(pn4->pn_atom);
                break;
              case TOK_NAME:
                if (!pn4->maybeExpr()) {
                    ok = LookupCompileTimeConstant(cx, cg, pn4->pn_atom, &constVal);
                    if (!ok)
                        goto release;
                    if (!constVal.isMagic(JS_NO_CONSTANT)) {
                        if (constVal.isObject()) {
                            /*
                             * XXX JSOP_LOOKUPSWITCH does not support const-
                             * propagated object values, see bug 407186.
                             */
                            switchOp = JSOP_CONDSWITCH;
                            continue;
                        }
                        constPropagated = JS_TRUE;
                        break;
                    }
                }
                /* FALL THROUGH */
              case TOK_PRIMARY:
                if (pn4->isOp(JSOP_TRUE)) {
                    constVal.setBoolean(true);
                    break;
                }
                if (pn4->isOp(JSOP_FALSE)) {
                    constVal.setBoolean(false);
                    break;
                }
                if (pn4->isOp(JSOP_NULL)) {
                    constVal.setNull();
                    break;
                }
                /* FALL THROUGH */
              default:
                switchOp = JSOP_CONDSWITCH;
                continue;
            }
            JS_ASSERT(constVal.isPrimitive());

            pn3->pn_pval = AllocateSwitchConstant(cx);
            if (!pn3->pn_pval) {
                ok = JS_FALSE;
                goto release;
            }

            *pn3->pn_pval = constVal;

            if (switchOp != JSOP_TABLESWITCH)
                continue;
            if (!pn3->pn_pval->isInt32()) {
                switchOp = JSOP_LOOKUPSWITCH;
                continue;
            }
            i = pn3->pn_pval->toInt32();
            if ((jsuint)(i + (jsint)JS_BIT(15)) >= (jsuint)JS_BIT(16)) {
                switchOp = JSOP_LOOKUPSWITCH;
                continue;
            }
            if (i < low)
                low = i;
            if (high < i)
                high = i;

            /*
             * Check for duplicates, which require a JSOP_LOOKUPSWITCH.
             * We bias i by 65536 if it's negative, and hope that's a rare
             * case (because it requires a malloc'd bitmap).
             */
            if (i < 0)
                i += JS_BIT(16);
            if (i >= intmap_bitlen) {
                if (!intmap &&
                    i < (INTMAP_LENGTH << JS_BITS_PER_WORD_LOG2)) {
                    intmap = intmap_space;
                    intmap_bitlen = INTMAP_LENGTH << JS_BITS_PER_WORD_LOG2;
                } else {
                    /* Just grab 8K for the worst-case bitmap. */
                    intmap_bitlen = JS_BIT(16);
                    intmap = (jsbitmap *)
                        cx->malloc_((JS_BIT(16) >> JS_BITS_PER_WORD_LOG2)
                                   * sizeof(jsbitmap));
                    if (!intmap) {
                        JS_ReportOutOfMemory(cx);
                        return JS_FALSE;
                    }
                }
                memset(intmap, 0, intmap_bitlen >> JS_BITS_PER_BYTE_LOG2);
            }
            if (JS_TEST_BIT(intmap, i)) {
                switchOp = JSOP_LOOKUPSWITCH;
                continue;
            }
            JS_SET_BIT(intmap, i);
        }

      release:
        if (intmap && intmap != intmap_space)
            cx->free_(intmap);
        if (!ok)
            return JS_FALSE;

        /*
         * Compute table length and select lookup instead if overlarge or
         * more than half-sparse.
         */
        if (switchOp == JSOP_TABLESWITCH) {
            tableLength = (uint32)(high - low + 1);
            if (tableLength >= JS_BIT(16) || tableLength > 2 * caseCount)
                switchOp = JSOP_LOOKUPSWITCH;
        } else if (switchOp == JSOP_LOOKUPSWITCH) {
            /*
             * Lookup switch supports only atom indexes below 64K limit.
             * Conservatively estimate the maximum possible index during
             * switch generation and use conditional switch if it exceeds
             * the limit.
             */
            if (caseCount + cg->constList.length() > JS_BIT(16))
                switchOp = JSOP_CONDSWITCH;
        }
    }

    /*
     * Emit a note with two offsets: first tells total switch code length,
     * second tells offset to first JSOP_CASE if condswitch.
     */
    noteIndex = js_NewSrcNote3(cx, cg, SRC_SWITCH, 0, 0);
    if (noteIndex < 0)
        return JS_FALSE;

    if (switchOp == JSOP_CONDSWITCH) {
        /*
         * 0 bytes of immediate for unoptimized ECMAv2 switch.
         */
        switchSize = 0;
    } else if (switchOp == JSOP_TABLESWITCH) {
        /*
         * 3 offsets (len, low, high) before the table, 1 per entry.
         */
        switchSize = (size_t)(JUMP_OFFSET_LEN * (3 + tableLength));
    } else {
        /*
         * JSOP_LOOKUPSWITCH:
         * 1 offset (len) and 1 atom index (npairs) before the table,
         * 1 atom index and 1 jump offset per entry.
         */
        switchSize = (size_t)(JUMP_OFFSET_LEN + INDEX_LEN +
                              (INDEX_LEN + JUMP_OFFSET_LEN) * caseCount);
    }

    /*
     * Emit switchOp followed by switchSize bytes of jump or lookup table.
     *
     * If switchOp is JSOP_LOOKUPSWITCH or JSOP_TABLESWITCH, it is crucial
     * to emit the immediate operand(s) by which bytecode readers such as
     * BuildSpanDepTable discover the length of the switch opcode *before*
     * calling js_SetJumpOffset (which may call BuildSpanDepTable).  It's
     * also important to zero all unknown jump offset immediate operands,
     * so they can be converted to span dependencies with null targets to
     * be computed later (js_EmitN zeros switchSize bytes after switchOp).
     */
    if (js_EmitN(cx, cg, switchOp, switchSize) < 0)
        return JS_FALSE;

    off = -1;
    if (switchOp == JSOP_CONDSWITCH) {
        intN caseNoteIndex = -1;
        JSBool beforeCases = JS_TRUE;

        /* Emit code for evaluating cases and jumping to case statements. */
        for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
            pn4 = pn3->pn_left;
            if (pn4 && !js_EmitTree(cx, cg, pn4))
                return JS_FALSE;
            if (caseNoteIndex >= 0) {
                /* off is the previous JSOP_CASE's bytecode offset. */
                if (!js_SetSrcNoteOffset(cx, cg, (uintN)caseNoteIndex, 0,
                                         CG_OFFSET(cg) - off)) {
                    return JS_FALSE;
                }
            }
            if (!pn4) {
                JS_ASSERT(pn3->isKind(TOK_DEFAULT));
                continue;
            }
            caseNoteIndex = js_NewSrcNote2(cx, cg, SRC_PCDELTA, 0);
            if (caseNoteIndex < 0)
                return JS_FALSE;
            off = EmitJump(cx, cg, JSOP_CASE, 0);
            if (off < 0)
                return JS_FALSE;
            pn3->pn_offset = off;
            if (beforeCases) {
                uintN noteCount, noteCountDelta;

                /* Switch note's second offset is to first JSOP_CASE. */
                noteCount = CG_NOTE_COUNT(cg);
                if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 1, off - top)) {
                    return JS_FALSE;
                }
                noteCountDelta = CG_NOTE_COUNT(cg) - noteCount;
                if (noteCountDelta != 0)
                    caseNoteIndex += noteCountDelta;
                beforeCases = JS_FALSE;
            }
        }

        /*
         * If we didn't have an explicit default (which could fall in between
         * cases, preventing us from fusing this js_SetSrcNoteOffset with the
         * call in the loop above), link the last case to the implicit default
         * for the decompiler.
         */
        if (!hasDefault &&
            caseNoteIndex >= 0 &&
            !js_SetSrcNoteOffset(cx, cg, (uintN)caseNoteIndex, 0,
                                 CG_OFFSET(cg) - off)) {
            return JS_FALSE;
        }

        /* Emit default even if no explicit default statement. */
        defaultOffset = EmitJump(cx, cg, JSOP_DEFAULT, 0);
        if (defaultOffset < 0)
            return JS_FALSE;
    } else {
        pc = CG_CODE(cg, top + JUMP_OFFSET_LEN);

        if (switchOp == JSOP_TABLESWITCH) {
            /* Fill in switch bounds, which we know fit in 16-bit offsets. */
            SET_JUMP_OFFSET(pc, low);
            pc += JUMP_OFFSET_LEN;
            SET_JUMP_OFFSET(pc, high);
            pc += JUMP_OFFSET_LEN;

            /*
             * Use malloc to avoid arena bloat for programs with many switches.
             * We free table if non-null at label out, so all control flow must
             * exit this function through goto out or goto bad.
             */
            if (tableLength != 0) {
                tableSize = (size_t)tableLength * sizeof *table;
                table = (JSParseNode **) cx->malloc_(tableSize);
                if (!table)
                    return JS_FALSE;
                memset(table, 0, tableSize);
                for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
                    if (pn3->isKind(TOK_DEFAULT))
                        continue;
                    i = pn3->pn_pval->toInt32();
                    i -= low;
                    JS_ASSERT((uint32)i < tableLength);
                    table[i] = pn3;
                }
            }
        } else {
            JS_ASSERT(switchOp == JSOP_LOOKUPSWITCH);

            /* Fill in the number of cases. */
            SET_INDEX(pc, caseCount);
            pc += INDEX_LEN;
        }

        /*
         * After this point, all control flow involving JSOP_TABLESWITCH
         * must set ok and goto out to exit this function.  To keep things
         * simple, all switchOp cases exit that way.
         */
        MUST_FLOW_THROUGH("out");
        if (cg->spanDeps) {
            /*
             * We have already generated at least one big jump so we must
             * explicitly add span dependencies for the switch jumps. When
             * called below, js_SetJumpOffset can only do it when patching
             * the first big jump or when cg->spanDeps is null.
             */
            if (!AddSwitchSpanDeps(cx, cg, CG_CODE(cg, top)))
                goto bad;
        }

        if (constPropagated) {
            /*
             * Skip switchOp, as we are not setting jump offsets in the two
             * for loops below.  We'll restore CG_NEXT(cg) from savepc after,
             * unless there was an error.
             */
            savepc = CG_NEXT(cg);
            CG_NEXT(cg) = pc + 1;
            if (switchOp == JSOP_TABLESWITCH) {
                for (i = 0; i < (jsint)tableLength; i++) {
                    pn3 = table[i];
                    if (pn3 &&
                        (pn4 = pn3->pn_left) != NULL &&
                        pn4->isKind(TOK_NAME)) {
                        /* Note a propagated constant with the const's name. */
                        JS_ASSERT(!pn4->maybeExpr());
                        jsatomid index;
                        if (!cg->makeAtomIndex(pn4->pn_atom, &index))
                            goto bad;
                        CG_NEXT(cg) = pc;
                        if (js_NewSrcNote2(cx, cg, SRC_LABEL, ptrdiff_t(index)) < 0)
                            goto bad;
                    }
                    pc += JUMP_OFFSET_LEN;
                }
            } else {
                for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
                    pn4 = pn3->pn_left;
                    if (pn4 && pn4->isKind(TOK_NAME)) {
                        /* Note a propagated constant with the const's name. */
                        JS_ASSERT(!pn4->maybeExpr());
                        jsatomid index;
                        if (!cg->makeAtomIndex(pn4->pn_atom, &index))
                            goto bad;
                        CG_NEXT(cg) = pc;
                        if (js_NewSrcNote2(cx, cg, SRC_LABEL, ptrdiff_t(index)) < 0)
                            goto bad;
                    }
                    pc += INDEX_LEN + JUMP_OFFSET_LEN;
                }
            }
            CG_NEXT(cg) = savepc;
        }
    }

    /* Emit code for each case's statements, copying pn_offset up to pn3. */
    for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
        if (switchOp == JSOP_CONDSWITCH && !pn3->isKind(TOK_DEFAULT))
            CHECK_AND_SET_JUMP_OFFSET_AT_CUSTOM(cx, cg, pn3->pn_offset, goto bad);
        pn4 = pn3->pn_right;
        ok = js_EmitTree(cx, cg, pn4);
        if (!ok)
            goto out;
        pn3->pn_offset = pn4->pn_offset;
        if (pn3->isKind(TOK_DEFAULT))
            off = pn3->pn_offset - top;
    }

    if (!hasDefault) {
        /* If no default case, offset for default is to end of switch. */
        off = CG_OFFSET(cg) - top;
    }

    /* We better have set "off" by now. */
    JS_ASSERT(off != -1);

    /* Set the default offset (to end of switch if no default). */
    if (switchOp == JSOP_CONDSWITCH) {
        pc = NULL;
        JS_ASSERT(defaultOffset != -1);
        ok = js_SetJumpOffset(cx, cg, CG_CODE(cg, defaultOffset),
                              off - (defaultOffset - top));
        if (!ok)
            goto out;
    } else {
        pc = CG_CODE(cg, top);
        ok = js_SetJumpOffset(cx, cg, pc, off);
        if (!ok)
            goto out;
        pc += JUMP_OFFSET_LEN;
    }

    /* Set the SRC_SWITCH note's offset operand to tell end of switch. */
    off = CG_OFFSET(cg) - top;
    ok = js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 0, off);
    if (!ok)
        goto out;

    if (switchOp == JSOP_TABLESWITCH) {
        /* Skip over the already-initialized switch bounds. */
        pc += 2 * JUMP_OFFSET_LEN;

        /* Fill in the jump table, if there is one. */
        for (i = 0; i < (jsint)tableLength; i++) {
            pn3 = table[i];
            off = pn3 ? pn3->pn_offset - top : 0;
            ok = js_SetJumpOffset(cx, cg, pc, off);
            if (!ok)
                goto out;
            pc += JUMP_OFFSET_LEN;
        }
    } else if (switchOp == JSOP_LOOKUPSWITCH) {
        /* Skip over the already-initialized number of cases. */
        pc += INDEX_LEN;

        for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
            if (pn3->isKind(TOK_DEFAULT))
                continue;
            if (!cg->constList.append(*pn3->pn_pval))
                goto bad;
            SET_INDEX(pc, cg->constList.length() - 1);
            pc += INDEX_LEN;

            off = pn3->pn_offset - top;
            ok = js_SetJumpOffset(cx, cg, pc, off);
            if (!ok)
                goto out;
            pc += JUMP_OFFSET_LEN;
        }
    }

out:
    if (table)
        cx->free_(table);
    if (ok) {
        ok = js_PopStatementCG(cx, cg);

#if JS_HAS_BLOCK_SCOPE
        if (ok && pn->pn_right->isKind(TOK_LEXICALSCOPE))
            ok = EmitLeaveBlock(cx, cg, JSOP_LEAVEBLOCK, box);
#endif
    }
    return ok;

bad:
    ok = JS_FALSE;
    goto out;
}

JSBool
js_EmitFunctionScript(JSContext *cx, JSCodeGenerator *cg, JSParseNode *body)
{
    /*
     * The decompiler has assumptions about what may occur immediately after
     * script->main (e.g., in the case of destructuring params). Thus, put the
     * following ops into the range [script->code, script->main). Note:
     * execution starts from script->code, so this has no semantic effect.
     */

    if (cg->flags & TCF_FUN_IS_GENERATOR) {
        /* JSOP_GENERATOR must be the first instruction. */
        CG_SWITCH_TO_PROLOG(cg);
        JS_ASSERT(CG_NEXT(cg) == CG_BASE(cg));
        if (js_Emit1(cx, cg, JSOP_GENERATOR) < 0)
            return false;
        CG_SWITCH_TO_MAIN(cg);
    }

    /*
     * Strict mode functions' arguments objects copy initial parameter values.
     * We create arguments objects lazily -- but that doesn't work for strict
     * mode functions where a parameter might be modified and arguments might
     * be accessed. For such functions we synthesize an access to arguments to
     * initialize it with the original parameter values.
     */
    if (cg->needsEagerArguments()) {
        CG_SWITCH_TO_PROLOG(cg);
        if (js_Emit1(cx, cg, JSOP_ARGUMENTS) < 0 || js_Emit1(cx, cg, JSOP_POP) < 0)
            return false;
        CG_SWITCH_TO_MAIN(cg);
    }

    if (cg->flags & TCF_FUN_UNBRAND_THIS) {
        CG_SWITCH_TO_PROLOG(cg);
        if (js_Emit1(cx, cg, JSOP_UNBRANDTHIS) < 0)
            return false;
        CG_SWITCH_TO_MAIN(cg);
    }

    return js_EmitTree(cx, cg, body) &&
           js_Emit1(cx, cg, JSOP_STOP) >= 0 &&
           JSScript::NewScriptFromCG(cx, cg);
}

static bool
MaybeEmitVarDecl(JSContext *cx, JSCodeGenerator *cg, JSOp prologOp,
                 JSParseNode *pn, jsatomid *result)
{
    jsatomid atomIndex;

    if (!pn->pn_cookie.isFree()) {
        atomIndex = pn->pn_cookie.slot();
    } else {
        if (!cg->makeAtomIndex(pn->pn_atom, &atomIndex))
            return false;
    }

    if (JOF_OPTYPE(pn->getOp()) == JOF_ATOM &&
        (!cg->inFunction() || (cg->flags & TCF_FUN_HEAVYWEIGHT)) &&
        !(pn->pn_dflags & PND_GVAR))
    {
        CG_SWITCH_TO_PROLOG(cg);
        if (!UpdateLineNumberNotes(cx, cg, pn->pn_pos.begin.lineno))
            return false;
        EMIT_INDEX_OP(prologOp, atomIndex);
        CG_SWITCH_TO_MAIN(cg);
    }

    if (cg->inFunction() &&
        JOF_OPTYPE(pn->getOp()) == JOF_LOCAL &&
        pn->pn_cookie.slot() < cg->bindings.countVars() &&
        cg->shouldNoteClosedName(pn))
    {
        if (!cg->closedVars.append(pn->pn_cookie.slot()))
            return false;
    }

    if (result)
        *result = atomIndex;
    return true;
}

#if JS_HAS_DESTRUCTURING

typedef JSBool
(*DestructuringDeclEmitter)(JSContext *cx, JSCodeGenerator *cg, JSOp prologOp,
                            JSParseNode *pn);

static JSBool
EmitDestructuringDecl(JSContext *cx, JSCodeGenerator *cg, JSOp prologOp,
                      JSParseNode *pn)
{
    JS_ASSERT(pn->isKind(TOK_NAME));
    if (!BindNameToSlot(cx, cg, pn))
        return JS_FALSE;

    JS_ASSERT(!pn->isOp(JSOP_ARGUMENTS) && !pn->isOp(JSOP_CALLEE));
    return MaybeEmitVarDecl(cx, cg, prologOp, pn, NULL);
}

static JSBool
EmitDestructuringDecls(JSContext *cx, JSCodeGenerator *cg, JSOp prologOp,
                       JSParseNode *pn)
{
    JSParseNode *pn2, *pn3;
    DestructuringDeclEmitter emitter;

    if (pn->isKind(TOK_RB)) {
        for (pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
            if (pn2->isKind(TOK_COMMA))
                continue;
            emitter = (pn2->isKind(TOK_NAME))
                      ? EmitDestructuringDecl
                      : EmitDestructuringDecls;
            if (!emitter(cx, cg, prologOp, pn2))
                return JS_FALSE;
        }
    } else {
        JS_ASSERT(pn->isKind(TOK_RC));
        for (pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
            pn3 = pn2->pn_right;
            emitter = pn3->isKind(TOK_NAME) ? EmitDestructuringDecl : EmitDestructuringDecls;
            if (!emitter(cx, cg, prologOp, pn3))
                return JS_FALSE;
        }
    }
    return JS_TRUE;
}

static JSBool
EmitDestructuringOpsHelper(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn);

static JSBool
EmitDestructuringLHS(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn)
{
    /*
     * Now emit the lvalue opcode sequence.  If the lvalue is a nested
     * destructuring initialiser-form, call ourselves to handle it, then
     * pop the matched value.  Otherwise emit an lvalue bytecode sequence
     * ending with a JSOP_ENUMELEM or equivalent op.
     */
    if (pn->isKind(TOK_RB) || pn->isKind(TOK_RC)) {
        if (!EmitDestructuringOpsHelper(cx, cg, pn))
            return JS_FALSE;
        if (js_Emit1(cx, cg, JSOP_POP) < 0)
            return JS_FALSE;
    } else {
        if (pn->isKind(TOK_NAME)) {
            if (!BindNameToSlot(cx, cg, pn))
                return JS_FALSE;
            if (pn->isConst() && !pn->isInitialized())
                return js_Emit1(cx, cg, JSOP_POP) >= 0;
        }

        switch (pn->getOp()) {
          case JSOP_SETNAME:
          case JSOP_SETGNAME:
            /*
             * NB: pn is a PN_NAME node, not a PN_BINARY.  Nevertheless,
             * we want to emit JSOP_ENUMELEM, which has format JOF_ELEM.
             * So here and for JSOP_ENUMCONSTELEM, we use EmitElemOp.
             */
            if (!EmitElemOp(cx, pn, JSOP_ENUMELEM, cg))
                return JS_FALSE;
            break;

          case JSOP_SETCONST:
            if (!EmitElemOp(cx, pn, JSOP_ENUMCONSTELEM, cg))
                return JS_FALSE;
            break;

          case JSOP_SETLOCAL:
          {
            jsuint slot = pn->pn_cookie.asInteger();
            EMIT_UINT16_IMM_OP(JSOP_SETLOCALPOP, slot);
            break;
          }

          case JSOP_SETARG:
          {
            jsuint slot = pn->pn_cookie.asInteger();
            EMIT_UINT16_IMM_OP(pn->getOp(), slot);
            if (js_Emit1(cx, cg, JSOP_POP) < 0)
                return JS_FALSE;
            break;
          }

          default:
          {
            ptrdiff_t top;

            top = CG_OFFSET(cg);
            if (!js_EmitTree(cx, cg, pn))
                return JS_FALSE;
            if (js_NewSrcNote2(cx, cg, SRC_PCBASE, CG_OFFSET(cg) - top) < 0)
                return JS_FALSE;
            if (!EmitElemOpBase(cx, cg, JSOP_ENUMELEM))
                return JS_FALSE;
            break;
          }

          case JSOP_ENUMELEM:
            JS_ASSERT(0);
        }
    }

    return JS_TRUE;
}

/*
 * Recursive helper for EmitDestructuringOps.
 *
 * Given a value to destructure on the stack, walk over an object or array
 * initialiser at pn, emitting bytecodes to match property values and store
 * them in the lvalues identified by the matched property names.
 */
static JSBool
EmitDestructuringOpsHelper(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn)
{
    jsuint index;
    JSParseNode *pn2, *pn3;
    JSBool doElemOp;

#ifdef DEBUG
    intN stackDepth = cg->stackDepth;
    JS_ASSERT(stackDepth != 0);
    JS_ASSERT(pn->isArity(PN_LIST));
    JS_ASSERT(pn->isKind(TOK_RB) || pn->isKind(TOK_RC));
#endif

    if (pn->pn_count == 0) {
        /* Emit a DUP;POP sequence for the decompiler. */
        return js_Emit1(cx, cg, JSOP_DUP) >= 0 &&
               js_Emit1(cx, cg, JSOP_POP) >= 0;
    }

    index = 0;
    for (pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
        /*
         * Duplicate the value being destructured to use as a reference base.
         * If dup is not the first one, annotate it for the decompiler.
         */
        if (pn2 != pn->pn_head && js_NewSrcNote(cx, cg, SRC_CONTINUE) < 0)
            return JS_FALSE;
        if (js_Emit1(cx, cg, JSOP_DUP) < 0)
            return JS_FALSE;

        /*
         * Now push the property name currently being matched, which is either
         * the array initialiser's current index, or the current property name
         * "label" on the left of a colon in the object initialiser.  Set pn3
         * to the lvalue node, which is in the value-initializing position.
         */
        doElemOp = JS_TRUE;
        if (pn->isKind(TOK_RB)) {
            if (!EmitNumberOp(cx, index, cg))
                return JS_FALSE;
            pn3 = pn2;
        } else {
            JS_ASSERT(pn->isKind(TOK_RC));
            JS_ASSERT(pn2->isKind(TOK_COLON));
            pn3 = pn2->pn_left;
            if (pn3->isKind(TOK_NUMBER)) {
                /*
                 * If we are emitting an object destructuring initialiser,
                 * annotate the index op with SRC_INITPROP so we know we are
                 * not decompiling an array initialiser.
                 */
                if (js_NewSrcNote(cx, cg, SRC_INITPROP) < 0)
                    return JS_FALSE;
                if (!EmitNumberOp(cx, pn3->pn_dval, cg))
                    return JS_FALSE;
            } else {
                JS_ASSERT(pn3->isKind(TOK_STRING) || pn3->isKind(TOK_NAME));
                if (!EmitAtomOp(cx, pn3, JSOP_GETPROP, cg))
                    return JS_FALSE;
                doElemOp = JS_FALSE;
            }
            pn3 = pn2->pn_right;
        }

        if (doElemOp) {
            /*
             * Ok, get the value of the matching property name.  This leaves
             * that value on top of the value being destructured, so the stack
             * is one deeper than when we started.
             */
            if (!EmitElemOpBase(cx, cg, JSOP_GETELEM))
                return JS_FALSE;
            JS_ASSERT(cg->stackDepth == stackDepth + 1);
        }

        /* Nullary comma node makes a hole in the array destructurer. */
        if (pn3->isKind(TOK_COMMA) && pn3->isArity(PN_NULLARY)) {
            JS_ASSERT(pn->isKind(TOK_RB));
            JS_ASSERT(pn2 == pn3);
            if (js_Emit1(cx, cg, JSOP_POP) < 0)
                return JS_FALSE;
        } else {
            if (!EmitDestructuringLHS(cx, cg, pn3))
                return JS_FALSE;
        }

        JS_ASSERT(cg->stackDepth == stackDepth);
        ++index;
    }

    return JS_TRUE;
}

static ptrdiff_t
OpToDeclType(JSOp op)
{
    switch (op) {
      case JSOP_NOP:
        return SRC_DECL_LET;
      case JSOP_DEFCONST:
        return SRC_DECL_CONST;
      case JSOP_DEFVAR:
        return SRC_DECL_VAR;
      default:
        return SRC_DECL_NONE;
    }
}

static JSBool
EmitDestructuringOps(JSContext *cx, JSCodeGenerator *cg, JSOp prologOp,
                     JSParseNode *pn)
{
    /*
     * If we're called from a variable declaration, help the decompiler by
     * annotating the first JSOP_DUP that EmitDestructuringOpsHelper emits.
     * If the destructuring initialiser is empty, our helper will emit a
     * JSOP_DUP followed by a JSOP_POP for the decompiler.
     */
    if (js_NewSrcNote2(cx, cg, SRC_DESTRUCT, OpToDeclType(prologOp)) < 0)
        return JS_FALSE;

    /*
     * Call our recursive helper to emit the destructuring assignments and
     * related stack manipulations.
     */
    return EmitDestructuringOpsHelper(cx, cg, pn);
}

static JSBool
EmitGroupAssignment(JSContext *cx, JSCodeGenerator *cg, JSOp prologOp,
                    JSParseNode *lhs, JSParseNode *rhs)
{
    jsuint depth, limit, i, nslots;
    JSParseNode *pn;

    depth = limit = (uintN) cg->stackDepth;
    for (pn = rhs->pn_head; pn; pn = pn->pn_next) {
        if (limit == JS_BIT(16)) {
            ReportCompileErrorNumber(cx, CG_TS(cg), rhs, JSREPORT_ERROR, JSMSG_ARRAY_INIT_TOO_BIG);
            return JS_FALSE;
        }

        /* MaybeEmitGroupAssignment won't call us if rhs is holey. */
        JS_ASSERT(!(pn->isKind(TOK_COMMA) && pn->isArity(PN_NULLARY)));
        if (!js_EmitTree(cx, cg, pn))
            return JS_FALSE;
        ++limit;
    }

    if (js_NewSrcNote2(cx, cg, SRC_GROUPASSIGN, OpToDeclType(prologOp)) < 0)
        return JS_FALSE;

    i = depth;
    for (pn = lhs->pn_head; pn; pn = pn->pn_next, ++i) {
        /* MaybeEmitGroupAssignment requires lhs->pn_count <= rhs->pn_count. */
        JS_ASSERT(i < limit);
        jsint slot = AdjustBlockSlot(cx, cg, i);
        if (slot < 0)
            return JS_FALSE;
        EMIT_UINT16_IMM_OP(JSOP_GETLOCAL, slot);

        if (pn->isKind(TOK_COMMA) && pn->isArity(PN_NULLARY)) {
            if (js_Emit1(cx, cg, JSOP_POP) < 0)
                return JS_FALSE;
        } else {
            if (!EmitDestructuringLHS(cx, cg, pn))
                return JS_FALSE;
        }
    }

    nslots = limit - depth;
    EMIT_UINT16_IMM_OP(JSOP_POPN, nslots);
    cg->stackDepth = (uintN) depth;
    return JS_TRUE;
}

/*
 * Helper called with pop out param initialized to a JSOP_POP* opcode.  If we
 * can emit a group assignment sequence, which results in 0 stack depth delta,
 * we set *pop to JSOP_NOP so callers can veto emitting pn followed by a pop.
 */
static JSBool
MaybeEmitGroupAssignment(JSContext *cx, JSCodeGenerator *cg, JSOp prologOp,
                         JSParseNode *pn, JSOp *pop)
{
    JSParseNode *lhs, *rhs;

    JS_ASSERT(pn->isKind(TOK_ASSIGN));
    JS_ASSERT(*pop == JSOP_POP || *pop == JSOP_POPV);
    lhs = pn->pn_left;
    rhs = pn->pn_right;
    if (lhs->isKind(TOK_RB) && rhs->isKind(TOK_RB) &&
        !(rhs->pn_xflags & PNX_HOLEY) &&
        lhs->pn_count <= rhs->pn_count) {
        if (!EmitGroupAssignment(cx, cg, prologOp, lhs, rhs))
            return JS_FALSE;
        *pop = JSOP_NOP;
    }
    return JS_TRUE;
}

#endif /* JS_HAS_DESTRUCTURING */

static JSBool
EmitVariables(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn,
              JSBool inLetHead, ptrdiff_t *headNoteIndex)
{
    bool forInVar, first;
    ptrdiff_t off, noteIndex, tmp;
    JSParseNode *pn2, *pn3, *next;
    JSOp op;
    jsatomid atomIndex;
    uintN oldflags;

    /* Default in case of JS_HAS_BLOCK_SCOPE early return, below. */
    *headNoteIndex = -1;

    /*
     * Let blocks and expressions have a parenthesized head in which the new
     * scope is not yet open. Initializer evaluation uses the parent node's
     * lexical scope. If popScope is true below, then we hide the top lexical
     * block from any calls to BindNameToSlot hiding in pn2->pn_expr so that
     * it won't find any names in the new let block.
     *
     * The same goes for let declarations in the head of any kind of for loop.
     * Unlike a let declaration 'let x = i' within a block, where x is hoisted
     * to the start of the block, a 'for (let x = i...) ...' loop evaluates i
     * in the containing scope, and puts x in the loop body's scope.
     */
    DebugOnly<bool> let = (pn->isOp(JSOP_NOP));
    forInVar = (pn->pn_xflags & PNX_FORINVAR) != 0;

    off = noteIndex = -1;
    for (pn2 = pn->pn_head; ; pn2 = next) {
        first = pn2 == pn->pn_head;
        next = pn2->pn_next;

        if (!pn2->isKind(TOK_NAME)) {
#if JS_HAS_DESTRUCTURING
            if (pn2->isKind(TOK_RB) || pn2->isKind(TOK_RC)) {
                /*
                 * Emit variable binding ops, but not destructuring ops.
                 * The parser (see Variables, jsparse.c) has ensured that
                 * our caller will be the TOK_FOR/TOK_IN case in js_EmitTree,
                 * and that case will emit the destructuring code only after
                 * emitting an enumerating opcode and a branch that tests
                 * whether the enumeration ended.
                 */
                JS_ASSERT(forInVar);
                JS_ASSERT(pn->pn_count == 1);
                if (!EmitDestructuringDecls(cx, cg, pn->getOp(), pn2))
                    return JS_FALSE;
                break;
            }
#endif

            /*
             * A destructuring initialiser assignment preceded by var will
             * never occur to the left of 'in' in a for-in loop.  As with 'for
             * (var x = i in o)...', this will cause the entire 'var [a, b] =
             * i' to be hoisted out of the loop.
             */
            JS_ASSERT(pn2->isKind(TOK_ASSIGN));
            JS_ASSERT(!forInVar);

            /*
             * To allow the front end to rewrite var f = x; as f = x; when a
             * function f(){} precedes the var, detect simple name assignment
             * here and initialize the name.
             */
#if !JS_HAS_DESTRUCTURING
            JS_ASSERT(pn2->pn_left->isKind(TOK_NAME));
#else
            if (pn2->pn_left->isKind(TOK_NAME))
#endif
            {
                pn3 = pn2->pn_right;
                pn2 = pn2->pn_left;
                goto do_name;
            }

#if JS_HAS_DESTRUCTURING
            if (pn->pn_count == 1) {
                /*
                 * If this is the only destructuring assignment in the list,
                 * try to optimize to a group assignment.  If we're in a let
                 * head, pass JSOP_POP rather than the pseudo-prolog JSOP_NOP
                 * in pn->pn_op, to suppress a second (and misplaced) 'let'.
                 */
                JS_ASSERT(noteIndex < 0 && !pn2->pn_next);
                op = JSOP_POP;
                if (!MaybeEmitGroupAssignment(cx, cg,
                                              inLetHead ? JSOP_POP : pn->getOp(),
                                              pn2, &op)) {
                    return JS_FALSE;
                }
                if (op == JSOP_NOP) {
                    pn->pn_xflags = (pn->pn_xflags & ~PNX_POPVAR) | PNX_GROUPINIT;
                    break;
                }
            }

            pn3 = pn2->pn_left;
            if (!EmitDestructuringDecls(cx, cg, pn->getOp(), pn3))
                return JS_FALSE;

            if (!js_EmitTree(cx, cg, pn2->pn_right))
                return JS_FALSE;

            /*
             * Veto pn->pn_op if inLetHead to avoid emitting a SRC_DESTRUCT
             * that's redundant with respect to the SRC_DECL/SRC_DECL_LET that
             * we will emit at the bottom of this function.
             */
            if (!EmitDestructuringOps(cx, cg,
                                      inLetHead ? JSOP_POP : pn->getOp(),
                                      pn3)) {
                return JS_FALSE;
            }
            goto emit_note_pop;
#endif
        }

        /*
         * Load initializer early to share code above that jumps to do_name.
         * NB: if this var redeclares an existing binding, then pn2 is linked
         * on its definition's use-chain and pn_expr has been overlayed with
         * pn_lexdef.
         */
        pn3 = pn2->maybeExpr();

     do_name:
        if (!BindNameToSlot(cx, cg, pn2))
            return JS_FALSE;

        op = pn2->getOp();
        if (op == JSOP_ARGUMENTS) {
            /* JSOP_ARGUMENTS => no initializer */
            JS_ASSERT(!pn3 && !let);
            pn3 = NULL;
#ifdef __GNUC__
            atomIndex = 0;            /* quell GCC overwarning */
#endif
        } else {
            JS_ASSERT(op != JSOP_CALLEE);
            JS_ASSERT(!pn2->pn_cookie.isFree() || !let);
            if (!MaybeEmitVarDecl(cx, cg, pn->getOp(), pn2, &atomIndex))
                return JS_FALSE;

            if (pn3) {
                JS_ASSERT(!forInVar);
                if (op == JSOP_SETNAME) {
                    JS_ASSERT(!let);
                    EMIT_INDEX_OP(JSOP_BINDNAME, atomIndex);
                } else if (op == JSOP_SETGNAME) {
                    JS_ASSERT(!let);
                    EMIT_INDEX_OP(JSOP_BINDGNAME, atomIndex);
                }
                if (pn->isOp(JSOP_DEFCONST) &&
                    !js_DefineCompileTimeConstant(cx, cg, pn2->pn_atom, pn3)) {
                    return JS_FALSE;
                }

                oldflags = cg->flags;
                cg->flags &= ~TCF_IN_FOR_INIT;
                if (!js_EmitTree(cx, cg, pn3))
                    return JS_FALSE;
                cg->flags |= oldflags & TCF_IN_FOR_INIT;
            }
        }

        /*
         * The parser rewrites 'for (var x = i in o)' to hoist 'var x = i' --
         * likewise 'for (let x = i in o)' becomes 'i; for (let x in o)' using
         * a TOK_SEQ node to make the two statements appear as one. Therefore
         * if this declaration is part of a for-in loop head, we do not need to
         * emit op or any source note. Our caller, the TOK_FOR/TOK_IN case in
         * js_EmitTree, will annotate appropriately.
         */
        JS_ASSERT_IF(pn2->isDefn(), pn3 == pn2->pn_expr);
        if (forInVar) {
            JS_ASSERT(pn->pn_count == 1);
            JS_ASSERT(!pn3);
            break;
        }

        if (first &&
            !inLetHead &&
            js_NewSrcNote2(cx, cg, SRC_DECL,
                           (pn->isOp(JSOP_DEFCONST))
                           ? SRC_DECL_CONST
                           : (pn->isOp(JSOP_DEFVAR))
                           ? SRC_DECL_VAR
                           : SRC_DECL_LET) < 0) {
            return JS_FALSE;
        }
        if (op == JSOP_ARGUMENTS) {
            if (js_Emit1(cx, cg, op) < 0)
                return JS_FALSE;
        } else if (!pn2->pn_cookie.isFree()) {
            EMIT_UINT16_IMM_OP(op, atomIndex);
        } else {
            EMIT_INDEX_OP(op, atomIndex);
        }

#if JS_HAS_DESTRUCTURING
    emit_note_pop:
#endif
        tmp = CG_OFFSET(cg);
        if (noteIndex >= 0) {
            if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 0, tmp-off))
                return JS_FALSE;
        }
        if (!next)
            break;
        off = tmp;
        noteIndex = js_NewSrcNote2(cx, cg, SRC_PCDELTA, 0);
        if (noteIndex < 0 || js_Emit1(cx, cg, JSOP_POP) < 0)
            return JS_FALSE;
    }

    /* If this is a let head, emit and return a srcnote on the pop. */
    if (inLetHead) {
        *headNoteIndex = js_NewSrcNote(cx, cg, SRC_DECL);
        if (*headNoteIndex < 0)
            return JS_FALSE;
        if (!(pn->pn_xflags & PNX_POPVAR))
            return js_Emit1(cx, cg, JSOP_NOP) >= 0;
    }

    return !(pn->pn_xflags & PNX_POPVAR) || js_Emit1(cx, cg, JSOP_POP) >= 0;
}

static bool
EmitAssignment(JSContext *cx, JSCodeGenerator *cg, JSParseNode *lhs, JSOp op, JSParseNode *rhs)
{
    ptrdiff_t top = CG_OFFSET(cg);

    /*
     * Check left operand type and generate specialized code for it.
     * Specialize to avoid ECMA "reference type" values on the operand
     * stack, which impose pervasive runtime "GetValue" costs.
     */
    jsatomid atomIndex = (jsatomid) -1;              /* quell GCC overwarning */
    jsbytecode offset = 1;

    switch (lhs->getKind()) {
      case TOK_NAME:
        if (!BindNameToSlot(cx, cg, lhs))
            return false;
        if (!lhs->pn_cookie.isFree()) {
            atomIndex = lhs->pn_cookie.asInteger();
        } else {
            if (!cg->makeAtomIndex(lhs->pn_atom, &atomIndex))
                return false;
            if (!lhs->isConst()) {
                JSOp op = lhs->isOp(JSOP_SETGNAME) ? JSOP_BINDGNAME : JSOP_BINDNAME;
                EMIT_INDEX_OP(op, atomIndex);
                offset++;
            }
        }
        break;
      case TOK_DOT:
        if (!js_EmitTree(cx, cg, lhs->expr()))
            return false;
        offset++;
        if (!cg->makeAtomIndex(lhs->pn_atom, &atomIndex))
            return false;
        break;
      case TOK_LB:
        JS_ASSERT(lhs->isArity(PN_BINARY));
        if (!js_EmitTree(cx, cg, lhs->pn_left))
            return false;
        if (!js_EmitTree(cx, cg, lhs->pn_right))
            return false;
        offset += 2;
        break;
#if JS_HAS_DESTRUCTURING
      case TOK_RB:
      case TOK_RC:
        break;
#endif
      case TOK_LP:
        if (!js_EmitTree(cx, cg, lhs))
            return false;
        offset++;
        break;
#if JS_HAS_XML_SUPPORT
      case TOK_UNARYOP:
        JS_ASSERT(lhs->isOp(JSOP_SETXMLNAME));
        if (!js_EmitTree(cx, cg, lhs->pn_kid))
            return false;
        if (js_Emit1(cx, cg, JSOP_BINDXMLNAME) < 0)
            return false;
        offset++;
        break;
#endif
      default:
        JS_ASSERT(0);
    }

    if (op != JSOP_NOP) {
        JS_ASSERT(rhs);
        switch (lhs->getKind()) {
          case TOK_NAME:
            if (lhs->isConst()) {
                if (lhs->isOp(JSOP_CALLEE)) {
                    if (js_Emit1(cx, cg, JSOP_CALLEE) < 0)
                        return false;
                } else {
                    EMIT_INDEX_OP(lhs->getOp(), atomIndex);
                }
            } else if (lhs->isOp(JSOP_SETNAME)) {
                if (js_Emit1(cx, cg, JSOP_DUP) < 0)
                    return false;
                EMIT_INDEX_OP(JSOP_GETXPROP, atomIndex);
            } else if (lhs->isOp(JSOP_SETGNAME)) {
                if (!BindGlobal(cx, cg, lhs, lhs->pn_atom))
                    return false;
                if (lhs->pn_cookie.isFree())
                    EmitAtomOp(cx, lhs, JSOP_GETGNAME, cg);
                else
                    EMIT_UINT16_IMM_OP(JSOP_GETGLOBAL, lhs->pn_cookie.asInteger());
            } else {
                EMIT_UINT16_IMM_OP(lhs->isOp(JSOP_SETARG) ? JSOP_GETARG : JSOP_GETLOCAL, atomIndex);
            }
            break;
          case TOK_DOT:
            if (js_Emit1(cx, cg, JSOP_DUP) < 0)
                return false;
            if (lhs->pn_atom == cx->runtime->atomState.protoAtom) {
                if (!EmitIndexOp(cx, JSOP_QNAMEPART, atomIndex, cg))
                    return false;
                if (!EmitElemOpBase(cx, cg, JSOP_GETELEM))
                    return false;
            } else {
                bool isLength = (lhs->pn_atom == cx->runtime->atomState.lengthAtom);
                EMIT_INDEX_OP(isLength ? JSOP_LENGTH : JSOP_GETPROP, atomIndex);
            }
            break;
          case TOK_LB:
          case TOK_LP:
#if JS_HAS_XML_SUPPORT
          case TOK_UNARYOP:
#endif
            if (js_Emit1(cx, cg, JSOP_DUP2) < 0)
                return false;
            if (!EmitElemOpBase(cx, cg, JSOP_GETELEM))
                return false;
            break;
          default:;
        }
    }

    /* Now emit the right operand (it may affect the namespace). */
    if (rhs) {
        if (!js_EmitTree(cx, cg, rhs))
            return false;
    } else {
        /* The value to assign is the next enumeration value in a for-in loop. */
        if (js_Emit2(cx, cg, JSOP_ITERNEXT, offset) < 0)
            return false;
    }

    /* If += etc., emit the binary operator with a decompiler note. */
    if (op != JSOP_NOP) {
        /*
         * Take care to avoid SRC_ASSIGNOP if the left-hand side is a const
         * declared in the current compilation unit, as in this case (just
         * a bit further below) we will avoid emitting the assignment op.
         */
        if (!lhs->isKind(TOK_NAME) || !lhs->isConst()) {
            if (js_NewSrcNote(cx, cg, SRC_ASSIGNOP) < 0)
                return false;
        }
        if (js_Emit1(cx, cg, op) < 0)
            return false;
    }

    /* Left parts such as a.b.c and a[b].c need a decompiler note. */
    if (!lhs->isKind(TOK_NAME) &&
#if JS_HAS_DESTRUCTURING
        !lhs->isKind(TOK_RB) &&
        !lhs->isKind(TOK_RC) &&
#endif
        js_NewSrcNote2(cx, cg, SRC_PCBASE, CG_OFFSET(cg) - top) < 0) {
        return false;
    }

    /* Finally, emit the specialized assignment bytecode. */
    switch (lhs->getKind()) {
      case TOK_NAME:
        if (lhs->isConst()) {
            if (!rhs) {
                ReportCompileErrorNumber(cx, CG_TS(cg), lhs, JSREPORT_ERROR,
                                         JSMSG_BAD_FOR_LEFTSIDE);
                return false;
            }
            break;
        }
        /* FALL THROUGH */
      case TOK_DOT:
        EMIT_INDEX_OP(lhs->getOp(), atomIndex);
        break;
      case TOK_LB:
      case TOK_LP:
        if (js_Emit1(cx, cg, JSOP_SETELEM) < 0)
            return false;
        break;
#if JS_HAS_DESTRUCTURING
      case TOK_RB:
      case TOK_RC:
        if (!EmitDestructuringOps(cx, cg, JSOP_SETNAME, lhs))
            return false;
        break;
#endif
#if JS_HAS_XML_SUPPORT
      case TOK_UNARYOP:
        if (js_Emit1(cx, cg, JSOP_SETXMLNAME) < 0)
            return false;
        break;
#endif
      default:
        JS_ASSERT(0);
    }
    return true;
}

#if defined DEBUG_brendan || defined DEBUG_mrbkap
static JSBool
GettableNoteForNextOp(JSCodeGenerator *cg)
{
    ptrdiff_t offset, target;
    jssrcnote *sn, *end;

    offset = 0;
    target = CG_OFFSET(cg);
    for (sn = CG_NOTES(cg), end = sn + CG_NOTE_COUNT(cg); sn < end;
         sn = SN_NEXT(sn)) {
        if (offset == target && SN_IS_GETTABLE(sn))
            return JS_TRUE;
        offset += SN_DELTA(sn);
    }
    return JS_FALSE;
}
#endif

/* Top-level named functions need a nop for decompilation. */
static JSBool
EmitFunctionDefNop(JSContext *cx, JSCodeGenerator *cg, uintN index)
{
    return js_NewSrcNote2(cx, cg, SRC_FUNCDEF, (ptrdiff_t)index) >= 0 &&
           js_Emit1(cx, cg, JSOP_NOP) >= 0;
}

static bool
EmitNewInit(JSContext *cx, JSCodeGenerator *cg, JSProtoKey key, JSParseNode *pn, int sharpnum)
{
    if (js_Emit3(cx, cg, JSOP_NEWINIT, (jsbytecode) key, 0) < 0)
        return false;
#if JS_HAS_SHARP_VARS
    if (cg->hasSharps()) {
        if (pn->pn_count != 0)
            EMIT_UINT16_IMM_OP(JSOP_SHARPINIT, cg->sharpSlotBase);
        if (sharpnum >= 0)
            EMIT_UINT16PAIR_IMM_OP(JSOP_DEFSHARP, cg->sharpSlotBase, sharpnum);
    } else {
        JS_ASSERT(sharpnum < 0);
    }
#endif
    return true;
}

static bool
EmitEndInit(JSContext *cx, JSCodeGenerator *cg, uint32 count)
{
#if JS_HAS_SHARP_VARS
    /* Emit an op for sharp array cleanup and decompilation. */
    if (cg->hasSharps() && count != 0)
        EMIT_UINT16_IMM_OP(JSOP_SHARPINIT, cg->sharpSlotBase);
#endif
    return js_Emit1(cx, cg, JSOP_ENDINIT) >= 0;
}

bool
JSParseNode::getConstantValue(JSContext *cx, bool strictChecks, Value *vp)
{
    switch (getKind()) {
      case TOK_NUMBER:
        vp->setNumber(pn_dval);
        return true;
      case TOK_STRING:
        vp->setString(pn_atom);
        return true;
      case TOK_PRIMARY:
        switch (getOp()) {
          case JSOP_NULL:
            vp->setNull();
            return true;
          case JSOP_FALSE:
            vp->setBoolean(false);
            return true;
          case JSOP_TRUE:
            vp->setBoolean(true);
            return true;
          default:
            JS_NOT_REACHED("Unexpected node");
            return false;
        }
      case TOK_RB: {
        JS_ASSERT(isOp(JSOP_NEWINIT) && !(pn_xflags & PNX_NONCONST));
 
        JSObject *obj = NewDenseAllocatedArray(cx, pn_count);
        if (!obj)
            return false;

        unsigned idx = 0;
        for (JSParseNode *pn = pn_head; pn; idx++, pn = pn->pn_next) {
            Value value;
            if (!pn->getConstantValue(cx, strictChecks, &value))
                return false;
            if (!obj->defineProperty(cx, INT_TO_JSID(idx), value, NULL, NULL, JSPROP_ENUMERATE))
                return false;
        }
        JS_ASSERT(idx == pn_count);

        types::FixArrayType(cx, obj);
        vp->setObject(*obj);
        return true;
      }
      case TOK_RC: {
        JS_ASSERT(isOp(JSOP_NEWINIT) && !(pn_xflags & PNX_NONCONST));

        gc::AllocKind kind = GuessObjectGCKind(pn_count, false);
        JSObject *obj = NewBuiltinClassInstance(cx, &ObjectClass, kind);
        if (!obj)
            return false;

        for (JSParseNode *pn = pn_head; pn; pn = pn->pn_next) {
            Value value;
            if (!pn->pn_right->getConstantValue(cx, strictChecks, &value))
                return false;

            JSParseNode *pnid = pn->pn_left;
            if (pnid->isKind(TOK_NUMBER)) {
                Value idvalue = NumberValue(pnid->pn_dval);
                jsid id;
                if (idvalue.isInt32() && INT_FITS_IN_JSID(idvalue.toInt32()))
                    id = INT_TO_JSID(idvalue.toInt32());
                else if (!js_InternNonIntElementId(cx, obj, idvalue, &id))
                    return false;
                if (!obj->defineProperty(cx, id, value, NULL, NULL, JSPROP_ENUMERATE))
                    return false;
            } else {
                JS_ASSERT(pnid->isKind(TOK_NAME) ||
                          pnid->isKind(TOK_STRING));
                JS_ASSERT(pnid->pn_atom != cx->runtime->atomState.protoAtom);
                jsid id = ATOM_TO_JSID(pnid->pn_atom);
                if (!DefineNativeProperty(cx, obj, id, value, NULL, NULL,
                                          JSPROP_ENUMERATE, 0, 0)) {
                    return false;
                }
            }
        }

        types::FixObjectType(cx, obj);
        vp->setObject(*obj);
        return true;
      }
      default:
        JS_NOT_REACHED("Unexpected node");
    }
    return false;
}

static bool
EmitSingletonInitialiser(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn)
{
    Value value;
    if (!pn->getConstantValue(cx, cg->needStrictChecks(), &value))
        return false;

    JS_ASSERT(value.isObject());
    JSObjectBox *objbox = cg->parser->newObjectBox(&value.toObject());
    if (!objbox)
        return false;

    return EmitObjectOp(cx, objbox, JSOP_OBJECT, cg);
}

/* See the SRC_FOR source note offsetBias comments later in this file. */
JS_STATIC_ASSERT(JSOP_NOP_LENGTH == 1);
JS_STATIC_ASSERT(JSOP_POP_LENGTH == 1);

class EmitLevelManager
{
private:
    JSCodeGenerator *cg;
    
public:
    EmitLevelManager(JSCodeGenerator *cg) : cg(cg) { cg->emitLevel++; }

    ~EmitLevelManager() { cg->emitLevel--; }
};

JSBool
js_EmitTree(JSContext *cx, JSCodeGenerator *cg, JSParseNode *pn)
{
    JSBool ok, useful, wantval;
    JSStmtInfo *stmt, stmtInfo;
    ptrdiff_t top, off, tmp, beq, jmp, tmp2, tmp3;
    JSParseNode *pn2, *pn3;
    JSAtom *atom;
    jsatomid atomIndex;
    uintN index;
    ptrdiff_t noteIndex, noteIndex2;
    JSSrcNoteType noteType;
    jsbytecode *pc;
    JSOp op;
    TokenKind type;
    uint32 argc;
    EmitLevelManager elm(cg);
#if JS_HAS_SHARP_VARS
    jsint sharpnum;
#endif

    JS_CHECK_RECURSION(cx, return JS_FALSE);

    ok = JS_TRUE;
    pn->pn_offset = top = CG_OFFSET(cg);

    /* Emit notes to tell the current bytecode's source line number. */
    UPDATE_LINE_NUMBER_NOTES(cx, cg, pn->pn_pos.begin.lineno);

    switch (pn->getKind()) {
      case TOK_FUNCTION:
      {
        JSFunction *fun;
        uintN slot;

#if JS_HAS_XML_SUPPORT
        if (pn->isArity(PN_NULLARY)) {
            if (js_Emit1(cx, cg, JSOP_GETFUNNS) < 0)
                return JS_FALSE;
            break;
        }
#endif

        fun = pn->pn_funbox->function();
        JS_ASSERT(fun->isInterpreted());
        if (fun->script()) {
            /*
             * This second pass is needed to emit JSOP_NOP with a source note
             * for the already-emitted function definition prolog opcode. See
             * comments in the TOK_LC case.
             */
            JS_ASSERT(pn->isOp(JSOP_NOP));
            JS_ASSERT(cg->inFunction());
            if (!EmitFunctionDefNop(cx, cg, pn->pn_index))
                return JS_FALSE;
            break;
        }

        JS_ASSERT_IF(pn->pn_funbox->tcflags & TCF_FUN_HEAVYWEIGHT,
                     fun->kind() == JSFUN_INTERPRETED);

        /* Generate code for the function's body. */
        JSCodeGenerator *cg2 = cx->new_<JSCodeGenerator>(cg->parser, pn->pn_pos.begin.lineno);
        if (!cg2) {
            js_ReportOutOfMemory(cx);
            return JS_FALSE;
        }
        if (!cg2->init(cx))
            return JS_FALSE;

        cg2->flags = pn->pn_funbox->tcflags | TCF_COMPILING | TCF_IN_FUNCTION |
                     (cg->flags & TCF_FUN_MIGHT_ALIAS_LOCALS);
        cg2->bindings.transfer(cx, &pn->pn_funbox->bindings);
#if JS_HAS_SHARP_VARS
        if (cg2->flags & TCF_HAS_SHARPS) {
            cg2->sharpSlotBase = cg2->bindings.sharpSlotBase(cx);
            if (cg2->sharpSlotBase < 0)
                return JS_FALSE;
        }
#endif
        cg2->setFunction(fun);
        cg2->funbox = pn->pn_funbox;
        cg2->parent = cg;

        /*
         * jsparse.cpp:SetStaticLevel limited static nesting depth to fit in 16
         * bits and to reserve the all-ones value, thereby reserving the magic
         * FREE_UPVAR_COOKIE value. Note the cg2->staticLevel assignment below.
         */
        JS_ASSERT(cg->staticLevel < JS_BITMASK(16) - 1);
        cg2->staticLevel = cg->staticLevel + 1;

        /* We measured the max scope depth when we parsed the function. */
        if (!js_EmitFunctionScript(cx, cg2, pn->pn_body))
            pn = NULL;

        cx->delete_(cg2);
        cg2 = NULL;
        if (!pn)
            return JS_FALSE;

        /* Make the function object a literal in the outer script's pool. */
        index = cg->objectList.index(pn->pn_funbox);

        /* Emit a bytecode pointing to the closure object in its immediate. */
        op = pn->getOp();
        if (op != JSOP_NOP) {
            if ((pn->pn_funbox->tcflags & TCF_GENEXP_LAMBDA) &&
                js_NewSrcNote(cx, cg, SRC_GENEXP) < 0) {
                return JS_FALSE;
            }
            EMIT_INDEX_OP(op, index);

            /* Make blockChain determination quicker. */
            if (EmitBlockChain(cx, cg) < 0)
                return JS_FALSE;
            break;
        }

        /*
         * For a script we emit the code as we parse. Thus the bytecode for
         * top-level functions should go in the prolog to predefine their
         * names in the variable object before the already-generated main code
         * is executed. This extra work for top-level scripts is not necessary
         * when we emit the code for a function. It is fully parsed prior to
         * invocation of the emitter and calls to js_EmitTree for function
         * definitions can be scheduled before generating the rest of code.
         */
        if (!cg->inFunction()) {
            JS_ASSERT(!cg->topStmt);
            if (!BindGlobal(cx, cg, pn, fun->atom))
                return false;
            if (pn->pn_cookie.isFree()) {
                CG_SWITCH_TO_PROLOG(cg);
                op = fun->isFlatClosure() ? JSOP_DEFFUN_FC : JSOP_DEFFUN;
                EMIT_INDEX_OP(op, index);

                /* Make blockChain determination quicker. */
                if (EmitBlockChain(cx, cg) < 0)
                    return JS_FALSE;
                CG_SWITCH_TO_MAIN(cg);
            }

            /* Emit NOP for the decompiler. */
            if (!EmitFunctionDefNop(cx, cg, index))
                return JS_FALSE;
        } else {
            DebugOnly<BindingKind> kind = cg->bindings.lookup(cx, fun->atom, &slot);
            JS_ASSERT(kind == VARIABLE || kind == CONSTANT);
            JS_ASSERT(index < JS_BIT(20));
            pn->pn_index = index;
            op = fun->isFlatClosure() ? JSOP_DEFLOCALFUN_FC : JSOP_DEFLOCALFUN;
            if (pn->isClosed() &&
                !cg->callsEval() &&
                !cg->closedVars.append(pn->pn_cookie.slot())) {
                return JS_FALSE;
            }
            if (!EmitSlotIndexOp(cx, op, slot, index, cg))
                return JS_FALSE;

            /* Make blockChain determination quicker. */
            if (EmitBlockChain(cx, cg) < 0)
                return JS_FALSE;
        }
        break;
      }

      case TOK_ARGSBODY:
      {
        JSParseNode *pnlast = pn->last();
        for (JSParseNode *pn2 = pn->pn_head; pn2 != pnlast; pn2 = pn2->pn_next) {
            if (!pn2->isDefn())
                continue;
            if (!BindNameToSlot(cx, cg, pn2))
                return JS_FALSE;
            if (JOF_OPTYPE(pn2->getOp()) == JOF_QARG && cg->shouldNoteClosedName(pn2)) {
                if (!cg->closedArgs.append(pn2->pn_cookie.slot()))
                    return JS_FALSE;
            }
        }
        ok = js_EmitTree(cx, cg, pnlast);
        break;
      }

      case TOK_UPVARS:
        JS_ASSERT(pn->pn_names->count() != 0);
        cg->roLexdeps = pn->pn_names;
        ok = js_EmitTree(cx, cg, pn->pn_tree);
        cg->roLexdeps.clearMap();
        pn->pn_names.releaseMap(cx);
        break;

      case TOK_IF:
        /* Initialize so we can detect else-if chains and avoid recursion. */
        stmtInfo.type = STMT_IF;
        beq = jmp = -1;
        noteIndex = -1;

      if_again:
        /* Emit code for the condition before pushing stmtInfo. */
        if (!js_EmitTree(cx, cg, pn->pn_kid1))
            return JS_FALSE;
        top = CG_OFFSET(cg);
        if (stmtInfo.type == STMT_IF) {
            js_PushStatement(cg, &stmtInfo, STMT_IF, top);
        } else {
            /*
             * We came here from the goto further below that detects else-if
             * chains, so we must mutate stmtInfo back into a STMT_IF record.
             * Also (see below for why) we need a note offset for SRC_IF_ELSE
             * to help the decompiler.  Actually, we need two offsets, one for
             * decompiling any else clause and the second for decompiling an
             * else-if chain without bracing, overindenting, or incorrectly
             * scoping let declarations.
             */
            JS_ASSERT(stmtInfo.type == STMT_ELSE);
            stmtInfo.type = STMT_IF;
            stmtInfo.update = top;
            if (!js_SetSrcNoteOffset(cx, cg, noteIndex, 0, jmp - beq))
                return JS_FALSE;
            if (!js_SetSrcNoteOffset(cx, cg, noteIndex, 1, top - beq))
                return JS_FALSE;
        }

        /* Emit an annotated branch-if-false around the then part. */
        pn3 = pn->pn_kid3;
        noteIndex = js_NewSrcNote(cx, cg, pn3 ? SRC_IF_ELSE : SRC_IF);
        if (noteIndex < 0)
            return JS_FALSE;
        beq = EmitJump(cx, cg, JSOP_IFEQ, 0);
        if (beq < 0)
            return JS_FALSE;

        /* Emit code for the then and optional else parts. */
        if (!js_EmitTree(cx, cg, pn->pn_kid2))
            return JS_FALSE;
        if (pn3) {
            /* Modify stmtInfo so we know we're in the else part. */
            stmtInfo.type = STMT_ELSE;

            /*
             * Emit a JSOP_BACKPATCH op to jump from the end of our then part
             * around the else part.  The js_PopStatementCG call at the bottom
             * of this switch case will fix up the backpatch chain linked from
             * stmtInfo.breaks.
             */
            jmp = EmitGoto(cx, cg, &stmtInfo, &stmtInfo.breaks);
            if (jmp < 0)
                return JS_FALSE;

            /* Ensure the branch-if-false comes here, then emit the else. */
            CHECK_AND_SET_JUMP_OFFSET_AT(cx, cg, beq);
            if (pn3->isKind(TOK_IF)) {
                pn = pn3;
                goto if_again;
            }

            if (!js_EmitTree(cx, cg, pn3))
                return JS_FALSE;

            /*
             * Annotate SRC_IF_ELSE with the offset from branch to jump, for
             * the decompiler's benefit.  We can't just "back up" from the pc
             * of the else clause, because we don't know whether an extended
             * jump was required to leap from the end of the then clause over
             * the else clause.
             */
            if (!js_SetSrcNoteOffset(cx, cg, noteIndex, 0, jmp - beq))
                return JS_FALSE;
        } else {
            /* No else part, fixup the branch-if-false to come here. */
            CHECK_AND_SET_JUMP_OFFSET_AT(cx, cg, beq);
        }
        ok = js_PopStatementCG(cx, cg);
        break;

      case TOK_SWITCH:
        /* Out of line to avoid bloating js_EmitTree's stack frame size. */
        ok = EmitSwitch(cx, cg, pn, &stmtInfo);
        break;

      case TOK_WHILE:
        /*
         * Minimize bytecodes issued for one or more iterations by jumping to
         * the condition below the body and closing the loop if the condition
         * is true with a backward branch. For iteration count i:
         *
         *  i    test at the top                 test at the bottom
         *  =    ===============                 ==================
         *  0    ifeq-pass                       goto; ifne-fail
         *  1    ifeq-fail; goto; ifne-pass      goto; ifne-pass; ifne-fail
         *  2    2*(ifeq-fail; goto); ifeq-pass  goto; 2*ifne-pass; ifne-fail
         *  . . .
         *  N    N*(ifeq-fail; goto); ifeq-pass  goto; N*ifne-pass; ifne-fail
         *
         * SpiderMonkey, pre-mozilla.org, emitted while parsing and so used
         * test at the top. When JSParseNode trees were added during the ES3
         * work (1998-9), the code generation scheme was not optimized, and
         * the decompiler continued to take advantage of the branch and jump
         * that bracketed the body. But given the SRC_WHILE note, it is easy
         * to support the more efficient scheme.
         */
        js_PushStatement(cg, &stmtInfo, STMT_WHILE_LOOP, top);
        noteIndex = js_NewSrcNote(cx, cg, SRC_WHILE);
        if (noteIndex < 0)
            return JS_FALSE;
        jmp = EmitJump(cx, cg, JSOP_GOTO, 0);
        if (jmp < 0)
            return JS_FALSE;
        noteIndex2 = js_NewSrcNote(cx, cg, SRC_TRACE);
        if (noteIndex2 < 0)
            return JS_FALSE;
        top = EmitTraceOp(cx, cg, pn->pn_right);
        if (top < 0)
            return JS_FALSE;
        if (!js_EmitTree(cx, cg, pn->pn_right))
            return JS_FALSE;
        CHECK_AND_SET_JUMP_OFFSET_AT(cx, cg, jmp);
        if (!js_EmitTree(cx, cg, pn->pn_left))
            return JS_FALSE;
        beq = EmitJump(cx, cg, JSOP_IFNE, top - CG_OFFSET(cg));
        if (beq < 0)
            return JS_FALSE;
        /*
         * Be careful: We must set noteIndex2 before noteIndex in case the noteIndex
         * note gets bigger.
         */
        if (!js_SetSrcNoteOffset(cx, cg, noteIndex2, 0, beq - top))
            return JS_FALSE;
        if (!js_SetSrcNoteOffset(cx, cg, noteIndex, 0, beq - jmp))
            return JS_FALSE;
        ok = js_PopStatementCG(cx, cg);
        break;

      case TOK_DO:
        /* Emit an annotated nop so we know to decompile a 'do' keyword. */
        noteIndex = js_NewSrcNote(cx, cg, SRC_WHILE);
        if (noteIndex < 0 || js_Emit1(cx, cg, JSOP_NOP) < 0)
            return JS_FALSE;

        noteIndex2 = js_NewSrcNote(cx, cg, SRC_TRACE);
        if (noteIndex2 < 0)
            return JS_FALSE;

        /* Compile the loop body. */
        top = EmitTraceOp(cx, cg, pn->pn_left);
        if (top < 0)
            return JS_FALSE;
        js_PushStatement(cg, &stmtInfo, STMT_DO_LOOP, top);
        if (!js_EmitTree(cx, cg, pn->pn_left))
            return JS_FALSE;

        /* Set loop and enclosing label update offsets, for continue. */
        off = CG_OFFSET(cg);
        stmt = &stmtInfo;
        do {
            stmt->update = off;
        } while ((stmt = stmt->down) != NULL && stmt->type == STMT_LABEL);

        /* Compile the loop condition, now that continues know where to go. */
        if (!js_EmitTree(cx, cg, pn->pn_right))
            return JS_FALSE;

        /*
         * Since we use JSOP_IFNE for other purposes as well as for do-while
         * loops, we must store 1 + (beq - top) in the SRC_WHILE note offset,
         * and the decompiler must get that delta and decompile recursively.
         */
        beq = EmitJump(cx, cg, JSOP_IFNE, top - CG_OFFSET(cg));
        if (beq < 0)
            return JS_FALSE;
        /*
         * Be careful: We must set noteIndex2 before noteIndex in case the noteIndex
         * note gets bigger.
         */
        if (!js_SetSrcNoteOffset(cx, cg, noteIndex2, 0, beq - top))
            return JS_FALSE;
        if (!js_SetSrcNoteOffset(cx, cg, noteIndex, 0, 1 + (off - top)))
            return JS_FALSE;
        ok = js_PopStatementCG(cx, cg);
        break;

      case TOK_FOR:
        beq = 0;                /* suppress gcc warnings */
        jmp = -1;
        pn2 = pn->pn_left;
        js_PushStatement(cg, &stmtInfo, STMT_FOR_LOOP, top);

        if (pn2->isKind(TOK_IN)) {
            /* Set stmtInfo type for later testing. */
            stmtInfo.type = STMT_FOR_IN_LOOP;

            /*
             * If the left part is 'var x', emit code to define x if necessary
             * using a prolog opcode, but do not emit a pop. If the left part
             * was originally 'var x = i', the parser will have rewritten it;
             * see Parser::forStatement. 'for (let x = i in o)' is mercifully
             * banned.
             */
            bool forLet = false;
            if (JSParseNode *decl = pn2->pn_kid1) {
                JS_ASSERT(TokenKindIsDecl(decl->getKind()));
                forLet = decl->isKind(TOK_LET);
                cg->flags |= TCF_IN_FOR_INIT;
                if (!js_EmitTree(cx, cg, decl))
                    return JS_FALSE;
                cg->flags &= ~TCF_IN_FOR_INIT;
            }

            /* Compile the object expression to the right of 'in'. */
            {
                TempPopScope tps;
                if (forLet && !tps.popBlock(cx, cg))
                    return JS_FALSE;
                if (!js_EmitTree(cx, cg, pn2->pn_kid3))
                    return JS_FALSE;
                if (forLet && !tps.repushBlock(cx, cg))
                    return JS_FALSE;
            }

            /*
             * Emit a bytecode to convert top of stack value to the iterator
             * object depending on the loop variant (for-in, for-each-in, or
             * destructuring for-in).
             */
            JS_ASSERT(pn->isOp(JSOP_ITER));
            if (js_Emit2(cx, cg, JSOP_ITER, (uint8) pn->pn_iflags) < 0)
                return JS_FALSE;

            /* Annotate so the decompiler can find the loop-closing jump. */
            noteIndex = js_NewSrcNote(cx, cg, SRC_FOR_IN);
            if (noteIndex < 0)
                return JS_FALSE;

            /*
             * Jump down to the loop condition to minimize overhead assuming at
             * least one iteration, as the other loop forms do.
             */
            jmp = EmitJump(cx, cg, JSOP_GOTO, 0);
            if (jmp < 0)
                return JS_FALSE;

            noteIndex2 = js_NewSrcNote(cx, cg, SRC_TRACE);
            if (noteIndex2 < 0)
                return JS_FALSE;
            
            top = CG_OFFSET(cg);
            SET_STATEMENT_TOP(&stmtInfo, top);
            if (EmitTraceOp(cx, cg, NULL) < 0)
                return JS_FALSE;

#ifdef DEBUG
            intN loopDepth = cg->stackDepth;
#endif

            /*
             * Emit code to get the next enumeration value and assign it to the
             * left hand side. The JSOP_POP after this assignment is annotated
             * so that the decompiler can distinguish 'for (x in y)' from
             * 'for (var x in y)'.
             */
            if (!EmitAssignment(cx, cg, pn2->pn_kid2, JSOP_NOP, NULL))
                return false;
            tmp2 = CG_OFFSET(cg);
            if (pn2->pn_kid1 && js_NewSrcNote2(cx, cg, SRC_DECL,
                                               (pn2->pn_kid1->isOp(JSOP_DEFVAR))
                                               ? SRC_DECL_VAR
                                               : SRC_DECL_LET) < 0) {
                return false;
            }
            if (js_Emit1(cx, cg, JSOP_POP) < 0)
                return false;

            /* The stack should be balanced around the assignment opcode sequence. */
            JS_ASSERT(cg->stackDepth == loopDepth);

            /* Emit code for the loop body. */
            if (!js_EmitTree(cx, cg, pn->pn_right))
                return JS_FALSE;

            /* Set loop and enclosing "update" offsets, for continue. */
            stmt = &stmtInfo;
            do {
                stmt->update = CG_OFFSET(cg);
            } while ((stmt = stmt->down) != NULL && stmt->type == STMT_LABEL);

            /*
             * Fixup the goto that starts the loop to jump down to JSOP_MOREITER.
             */
            CHECK_AND_SET_JUMP_OFFSET_AT(cx, cg, jmp);
            if (js_Emit1(cx, cg, JSOP_MOREITER) < 0)
                return JS_FALSE;
            beq = EmitJump(cx, cg, JSOP_IFNE, top - CG_OFFSET(cg));
            if (beq < 0)
                return JS_FALSE;

            /*
             * Be careful: We must set noteIndex2 before noteIndex in case the noteIndex
             * note gets bigger.
             */
            if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex2, 0, beq - top))
                return JS_FALSE;
            /* Set the first srcnote offset so we can find the start of the loop body. */
            if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 0, tmp2 - jmp))
                return JS_FALSE;
            /* Set the second srcnote offset so we can find the closing jump. */
            if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 1, beq - jmp))
                return JS_FALSE;
        } else {
            /* C-style for (init; cond; update) ... loop. */
            op = JSOP_POP;
            pn3 = pn2->pn_kid1;
            if (!pn3) {
                /* No initializer: emit an annotated nop for the decompiler. */
                op = JSOP_NOP;
            } else {
                cg->flags |= TCF_IN_FOR_INIT;
#if JS_HAS_DESTRUCTURING
                if (pn3->isKind(TOK_ASSIGN) &&
                    !MaybeEmitGroupAssignment(cx, cg, op, pn3, &op)) {
                    return JS_FALSE;
                }
#endif
                if (op == JSOP_POP) {
                    if (!js_EmitTree(cx, cg, pn3))
                        return JS_FALSE;
                    if (TokenKindIsDecl(pn3->getKind())) {
                        /*
                         * Check whether a destructuring-initialized var decl
                         * was optimized to a group assignment.  If so, we do
                         * not need to emit a pop below, so switch to a nop,
                         * just for the decompiler.
                         */
                        JS_ASSERT(pn3->isArity(PN_LIST));
                        if (pn3->pn_xflags & PNX_GROUPINIT)
                            op = JSOP_NOP;
                    }
                }
                cg->flags &= ~TCF_IN_FOR_INIT;
            }

            /*
             * NB: the SRC_FOR note has offsetBias 1 (JSOP_{NOP,POP}_LENGTH).
             * Use tmp to hold the biased srcnote "top" offset, which differs
             * from the top local variable by the length of the JSOP_GOTO{,X}
             * emitted in between tmp and top if this loop has a condition.
             */
            noteIndex = js_NewSrcNote(cx, cg, SRC_FOR);
            if (noteIndex < 0 || js_Emit1(cx, cg, op) < 0)
                return JS_FALSE;
            tmp = CG_OFFSET(cg);

            if (pn2->pn_kid2) {
                /* Goto the loop condition, which branches back to iterate. */
                jmp = EmitJump(cx, cg, JSOP_GOTO, 0);
                if (jmp < 0)
                    return JS_FALSE;
            }

            top = CG_OFFSET(cg);
            SET_STATEMENT_TOP(&stmtInfo, top);

            noteIndex2 = js_NewSrcNote(cx, cg, SRC_TRACE);
            if (noteIndex2 < 0)
                return JS_FALSE;
            
            /* Emit code for the loop body. */
            if (EmitTraceOp(cx, cg, pn->pn_right) < 0)
                return JS_FALSE;
            if (!js_EmitTree(cx, cg, pn->pn_right))
                return JS_FALSE;

            /* Set the second note offset so we can find the update part. */
            JS_ASSERT(noteIndex != -1);
            tmp2 = CG_OFFSET(cg);

            /* Set loop and enclosing "update" offsets, for continue. */
            stmt = &stmtInfo;
            do {
                stmt->update = CG_OFFSET(cg);
            } while ((stmt = stmt->down) != NULL && stmt->type == STMT_LABEL);

            /* Check for update code to do before the condition (if any). */
            pn3 = pn2->pn_kid3;
            if (pn3) {
                op = JSOP_POP;
#if JS_HAS_DESTRUCTURING
                if (pn3->isKind(TOK_ASSIGN) &&
                    !MaybeEmitGroupAssignment(cx, cg, op, pn3, &op)) {
                    return JS_FALSE;
                }
#endif
                if (op == JSOP_POP && !js_EmitTree(cx, cg, pn3))
                    return JS_FALSE;

                /* Always emit the POP or NOP, to help the decompiler. */
                if (js_Emit1(cx, cg, op) < 0)
                    return JS_FALSE;

                /* Restore the absolute line number for source note readers. */
                off = (ptrdiff_t) pn->pn_pos.end.lineno;
                if (CG_CURRENT_LINE(cg) != (uintN) off) {
                    if (js_NewSrcNote2(cx, cg, SRC_SETLINE, off) < 0)
                        return JS_FALSE;
                    CG_CURRENT_LINE(cg) = (uintN) off;
                }
            }

            tmp3 = CG_OFFSET(cg);

            if (pn2->pn_kid2) {
                /* Fix up the goto from top to target the loop condition. */
                JS_ASSERT(jmp >= 0);
                CHECK_AND_SET_JUMP_OFFSET_AT(cx, cg, jmp);

                if (!js_EmitTree(cx, cg, pn2->pn_kid2))
                    return JS_FALSE;
            }

            /*
             * Be careful: We must set noteIndex2 before noteIndex in case the noteIndex
             * note gets bigger.
             */
            if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex2, 0,
                                     CG_OFFSET(cg) - top)) {
                return JS_FALSE;
            }
            /* Set the first note offset so we can find the loop condition. */
            if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 0,
                                     tmp3 - tmp)) {
                return JS_FALSE;
            }
            if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 1,
                                     tmp2 - tmp)) {
                return JS_FALSE;
            }
            /* The third note offset helps us find the loop-closing jump. */
            if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 2,
                                     CG_OFFSET(cg) - tmp)) {
                return JS_FALSE;
            }

            if (pn2->pn_kid2) {
                beq = EmitJump(cx, cg, JSOP_IFNE, top - CG_OFFSET(cg));
                if (beq < 0)
                    return JS_FALSE;
            } else {
                /* No loop condition -- emit the loop-closing jump. */
                jmp = EmitJump(cx, cg, JSOP_GOTO, top - CG_OFFSET(cg));
                if (jmp < 0)
                    return JS_FALSE;
            }
        }

        /* Now fixup all breaks and continues (before for/in's JSOP_ENDITER). */
        if (!js_PopStatementCG(cx, cg))
            return JS_FALSE;

        if (pn2->isKind(TOK_IN)) {
            if (!NewTryNote(cx, cg, JSTRY_ITER, cg->stackDepth, top, CG_OFFSET(cg)) ||
                js_Emit1(cx, cg, JSOP_ENDITER) < 0) {
                return JS_FALSE;
            }
        }
        break;

      case TOK_BREAK: {
        stmt = cg->topStmt;
        atom = pn->pn_atom;

        jsatomid labelIndex;
        if (atom) {
            if (!cg->makeAtomIndex(atom, &labelIndex))
                return JS_FALSE;

            while (stmt->type != STMT_LABEL || stmt->label != atom)
                stmt = stmt->down;
            noteType = SRC_BREAK2LABEL;
        } else {
            labelIndex = INVALID_ATOMID;
            while (!STMT_IS_LOOP(stmt) && stmt->type != STMT_SWITCH)
                stmt = stmt->down;
            noteType = (stmt->type == STMT_SWITCH) ? SRC_SWITCHBREAK : SRC_BREAK;
        }

        if (EmitGoto(cx, cg, stmt, &stmt->breaks, labelIndex, noteType) < 0)
            return JS_FALSE;
        break;
      }

      case TOK_CONTINUE: {
        stmt = cg->topStmt;
        atom = pn->pn_atom;

        jsatomid labelIndex;
        if (atom) {
            /* Find the loop statement enclosed by the matching label. */
            JSStmtInfo *loop = NULL;
            if (!cg->makeAtomIndex(atom, &labelIndex))
                return JS_FALSE;
            while (stmt->type != STMT_LABEL || stmt->label != atom) {
                if (STMT_IS_LOOP(stmt))
                    loop = stmt;
                stmt = stmt->down;
            }
            stmt = loop;
            noteType = SRC_CONT2LABEL;
        } else {
            labelIndex = INVALID_ATOMID;
            while (!STMT_IS_LOOP(stmt))
                stmt = stmt->down;
            noteType = SRC_CONTINUE;
        }

        if (EmitGoto(cx, cg, stmt, &stmt->continues, labelIndex, noteType) < 0)
            return JS_FALSE;
        break;
      }

      case TOK_WITH:
        if (!js_EmitTree(cx, cg, pn->pn_left))
            return JS_FALSE;
        js_PushStatement(cg, &stmtInfo, STMT_WITH, CG_OFFSET(cg));
        if (js_Emit1(cx, cg, JSOP_ENTERWITH) < 0)
            return JS_FALSE;

        /* Make blockChain determination quicker. */
        if (EmitBlockChain(cx, cg) < 0)
            return JS_FALSE;
        if (!js_EmitTree(cx, cg, pn->pn_right))
            return JS_FALSE;
        if (js_Emit1(cx, cg, JSOP_LEAVEWITH) < 0)
            return JS_FALSE;
        ok = js_PopStatementCG(cx, cg);
        break;

      case TOK_TRY:
      {
        ptrdiff_t tryStart, tryEnd, catchJump, finallyStart;
        intN depth;
        JSParseNode *lastCatch;

        catchJump = -1;

        /*
         * Push stmtInfo to track jumps-over-catches and gosubs-to-finally
         * for later fixup.
         *
         * When a finally block is active (STMT_FINALLY in our tree context),
         * non-local jumps (including jumps-over-catches) result in a GOSUB
         * being written into the bytecode stream and fixed-up later (c.f.
         * EmitBackPatchOp and BackPatch).
         */
        js_PushStatement(cg, &stmtInfo,
                         pn->pn_kid3 ? STMT_FINALLY : STMT_TRY,
                         CG_OFFSET(cg));

        /*
         * Since an exception can be thrown at any place inside the try block,
         * we need to restore the stack and the scope chain before we transfer
         * the control to the exception handler.
         *
         * For that we store in a try note associated with the catch or
         * finally block the stack depth upon the try entry. The interpreter
         * uses this depth to properly unwind the stack and the scope chain.
         */
        depth = cg->stackDepth;

        /* Mark try location for decompilation, then emit try block. */
        if (js_Emit1(cx, cg, JSOP_TRY) < 0)
            return JS_FALSE;
        tryStart = CG_OFFSET(cg);
        if (!js_EmitTree(cx, cg, pn->pn_kid1))
            return JS_FALSE;
        JS_ASSERT(depth == cg->stackDepth);

        /* GOSUB to finally, if present. */
        if (pn->pn_kid3) {
            if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0)
                return JS_FALSE;
            jmp = EmitBackPatchOp(cx, cg, JSOP_BACKPATCH, &GOSUBS(stmtInfo));
            if (jmp < 0)
                return JS_FALSE;
        }

        /* Emit (hidden) jump over catch and/or finally. */
        if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0)
            return JS_FALSE;
        jmp = EmitBackPatchOp(cx, cg, JSOP_BACKPATCH, &catchJump);
        if (jmp < 0)
            return JS_FALSE;

        tryEnd = CG_OFFSET(cg);

        JSObjectBox *prevBox = NULL;
        /* If this try has a catch block, emit it. */
        pn2 = pn->pn_kid2;
        lastCatch = NULL;
        if (pn2) {
            uintN count = 0;    /* previous catch block's population */
            
            /*
             * The emitted code for a catch block looks like:
             *
             * blockchain
             * [throwing]                          only if 2nd+ catch block
             * [leaveblock]                        only if 2nd+ catch block
             * enterblock                          with SRC_CATCH
             * exception
             * [dup]                               only if catchguard
             * setlocalpop <slot>                  or destructuring code
             * [< catchguard code >]               if there's a catchguard
             * [ifeq <offset to next catch block>]         " "
             * [pop]                               only if catchguard
             * < catch block contents >
             * leaveblock
             * goto <end of catch blocks>          non-local; finally applies
             *
             * If there's no catch block without a catchguard, the last
             * <offset to next catch block> points to rethrow code.  This
             * code will [gosub] to the finally code if appropriate, and is
             * also used for the catch-all trynote for capturing exceptions
             * thrown from catch{} blocks.
             */
            for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
                ptrdiff_t guardJump, catchNote;

                JS_ASSERT(cg->stackDepth == depth);
                guardJump = GUARDJUMP(stmtInfo);
                if (guardJump != -1) {
                    if (EmitKnownBlockChain(cx, cg, prevBox) < 0)
                        return JS_FALSE;
                
                    /* Fix up and clean up previous catch block. */
                    CHECK_AND_SET_JUMP_OFFSET_AT(cx, cg, guardJump);

                    /*
                     * Account for JSOP_ENTERBLOCK (whose block object count
                     * is saved below) and pushed exception object that we
                     * still have after the jumping from the previous guard.
                     */
                    cg->stackDepth = depth + count + 1;

                    /*
                     * Move exception back to cx->exception to prepare for
                     * the next catch. We hide [throwing] from the decompiler
                     * since it compensates for the hidden JSOP_DUP at the
                     * start of the previous guarded catch.
                     */
                    if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0 ||
                        js_Emit1(cx, cg, JSOP_THROWING) < 0) {
                        return JS_FALSE;
                    }
                    if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0)
                        return JS_FALSE;
                    if (!EmitLeaveBlock(cx, cg, JSOP_LEAVEBLOCK, prevBox))
                        return JS_FALSE;
                    JS_ASSERT(cg->stackDepth == depth);
                }

                /*
                 * Annotate the JSOP_ENTERBLOCK that's about to be generated
                 * by the call to js_EmitTree immediately below.  Save this
                 * source note's index in stmtInfo for use by the TOK_CATCH:
                 * case, where the length of the catch guard is set as the
                 * note's offset.
                 */
                catchNote = js_NewSrcNote2(cx, cg, SRC_CATCH, 0);
                if (catchNote < 0)
                    return JS_FALSE;
                CATCHNOTE(stmtInfo) = catchNote;

                /*
                 * Emit the lexical scope and catch body.  Save the catch's
                 * block object population via count, for use when targeting
                 * guardJump at the next catch (the guard mismatch case).
                 */
                JS_ASSERT(pn3->isKind(TOK_LEXICALSCOPE));
                count = OBJ_BLOCK_COUNT(cx, pn3->pn_objbox->object);
                prevBox = pn3->pn_objbox;
                if (!js_EmitTree(cx, cg, pn3))
                    return JS_FALSE;

                /* gosub <finally>, if required */
                if (pn->pn_kid3) {
                    jmp = EmitBackPatchOp(cx, cg, JSOP_BACKPATCH,
                                          &GOSUBS(stmtInfo));
                    if (jmp < 0)
                        return JS_FALSE;
                    JS_ASSERT(cg->stackDepth == depth);
                }

                /*
                 * Jump over the remaining catch blocks.  This will get fixed
                 * up to jump to after catch/finally.
                 */
                if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0)
                    return JS_FALSE;
                jmp = EmitBackPatchOp(cx, cg, JSOP_BACKPATCH, &catchJump);
                if (jmp < 0)
                    return JS_FALSE;

                /*
                 * Save a pointer to the last catch node to handle try-finally
                 * and try-catch(guard)-finally special cases.
                 */
                lastCatch = pn3->expr();
            }
        }

        /*
         * Last catch guard jumps to the rethrow code sequence if none of the
         * guards match. Target guardJump at the beginning of the rethrow
         * sequence, just in case a guard expression throws and leaves the
         * stack unbalanced.
         */
        if (lastCatch && lastCatch->pn_kid2) {
            if (EmitKnownBlockChain(cx, cg, prevBox) < 0)
                return JS_FALSE;
            
            CHECK_AND_SET_JUMP_OFFSET_AT(cx, cg, GUARDJUMP(stmtInfo));

            /* Sync the stack to take into account pushed exception. */
            JS_ASSERT(cg->stackDepth == depth);
            cg->stackDepth = depth + 1;

            /*
             * Rethrow the exception, delegating executing of finally if any
             * to the exception handler.
             */
            if (js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0 ||
                js_Emit1(cx, cg, JSOP_THROW) < 0) {
                return JS_FALSE;
            }

            if (EmitBlockChain(cx, cg) < 0)
                return JS_FALSE;
        }

        JS_ASSERT(cg->stackDepth == depth);

        /* Emit finally handler if any. */
        finallyStart = 0;   /* to quell GCC uninitialized warnings */
        if (pn->pn_kid3) {
            /*
             * Fix up the gosubs that might have been emitted before non-local
             * jumps to the finally code.
             */
            if (!BackPatch(cx, cg, GOSUBS(stmtInfo), CG_NEXT(cg), JSOP_GOSUB))
                return JS_FALSE;

            finallyStart = CG_OFFSET(cg);

            /* Indicate that we're emitting a subroutine body. */
            stmtInfo.type = STMT_SUBROUTINE;
            if (!UpdateLineNumberNotes(cx, cg, pn->pn_kid3->pn_pos.begin.lineno))
                return JS_FALSE;
            if (js_Emit1(cx, cg, JSOP_FINALLY) < 0 ||
                !js_EmitTree(cx, cg, pn->pn_kid3) ||
                js_Emit1(cx, cg, JSOP_RETSUB) < 0) {
                return JS_FALSE;
            }
            JS_ASSERT(cg->stackDepth == depth);
        }
        if (!js_PopStatementCG(cx, cg))
            return JS_FALSE;

        if (js_NewSrcNote(cx, cg, SRC_ENDBRACE) < 0 ||
            js_Emit1(cx, cg, JSOP_NOP) < 0) {
            return JS_FALSE;
        }

        /* Fix up the end-of-try/catch jumps to come here. */
        if (!BackPatch(cx, cg, catchJump, CG_NEXT(cg), JSOP_GOTO))
            return JS_FALSE;

        /*
         * Add the try note last, to let post-order give us the right ordering
         * (first to last for a given nesting level, inner to outer by level).
         */
        if (pn->pn_kid2 &&
            !NewTryNote(cx, cg, JSTRY_CATCH, depth, tryStart, tryEnd)) {
            return JS_FALSE;
        }

        /*
         * If we've got a finally, mark try+catch region with additional
         * trynote to catch exceptions (re)thrown from a catch block or
         * for the try{}finally{} case.
         */
        if (pn->pn_kid3 &&
            !NewTryNote(cx, cg, JSTRY_FINALLY, depth, tryStart, finallyStart)) {
            return JS_FALSE;
        }
        break;
      }

      case TOK_CATCH:
      {
        ptrdiff_t catchStart, guardJump;

        /*
         * Morph STMT_BLOCK to STMT_CATCH, note the block entry code offset,
         * and save the block object atom.
         */
        stmt = cg->topStmt;
        JS_ASSERT(stmt->type == STMT_BLOCK && (stmt->flags & SIF_SCOPE));
        stmt->type = STMT_CATCH;
        catchStart = stmt->update;

        /* Go up one statement info record to the TRY or FINALLY record. */
        stmt = stmt->down;
        JS_ASSERT(stmt->type == STMT_TRY || stmt->type == STMT_FINALLY);

        /* Pick up the pending exception and bind it to the catch variable. */
        if (js_Emit1(cx, cg, JSOP_EXCEPTION) < 0)
            return JS_FALSE;

        /*
         * Dup the exception object if there is a guard for rethrowing to use
         * it later when rethrowing or in other catches.
         */
        if (pn->pn_kid2 && js_Emit1(cx, cg, JSOP_DUP) < 0)
            return JS_FALSE;

        pn2 = pn->pn_kid1;
        switch (pn2->getKind()) {
#if JS_HAS_DESTRUCTURING
          case TOK_RB:
          case TOK_RC:
            if (!EmitDestructuringOps(cx, cg, JSOP_NOP, pn2))
                return JS_FALSE;
            if (js_Emit1(cx, cg, JSOP_POP) < 0)
                return JS_FALSE;
            break;
#endif

          case TOK_NAME:
            /* Inline and specialize BindNameToSlot for pn2. */
            JS_ASSERT(!pn2->pn_cookie.isFree());
            EMIT_UINT16_IMM_OP(JSOP_SETLOCALPOP, pn2->pn_cookie.asInteger());
            break;

          default:
            JS_ASSERT(0);
        }

        /* Emit the guard expression, if there is one. */
        if (pn->pn_kid2) {
            if (!js_EmitTree(cx, cg, pn->pn_kid2))
                return JS_FALSE;
            if (!js_SetSrcNoteOffset(cx, cg, CATCHNOTE(*stmt), 0,
                                     CG_OFFSET(cg) - catchStart)) {
                return JS_FALSE;
            }
            /* ifeq <next block> */
            guardJump = EmitJump(cx, cg, JSOP_IFEQ, 0);
            if (guardJump < 0)
                return JS_FALSE;
            GUARDJUMP(*stmt) = guardJump;

            /* Pop duplicated exception object as we no longer need it. */
            if (js_Emit1(cx, cg, JSOP_POP) < 0)
                return JS_FALSE;
        }

        /* Emit the catch body. */
        if (!js_EmitTree(cx, cg, pn->pn_kid3))
            return JS_FALSE;

        /*
         * Annotate the JSOP_LEAVEBLOCK that will be emitted as we unwind via
         * our TOK_LEXICALSCOPE parent, so the decompiler knows to pop.
         */
        off = cg->stackDepth;
        if (js_NewSrcNote2(cx, cg, SRC_CATCH, off) < 0)
            return JS_FALSE;
        break;
      }

      case TOK_VAR:
        if (!EmitVariables(cx, cg, pn, JS_FALSE, &noteIndex))
            return JS_FALSE;
        break;

      case TOK_RETURN:
        /* Push a return value */
        pn2 = pn->pn_kid;
        if (pn2) {
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;
        } else {
            if (js_Emit1(cx, cg, JSOP_PUSH) < 0)
                return JS_FALSE;
        }

        /*
         * EmitNonLocalJumpFixup may add fixup bytecode to close open try
         * blocks having finally clauses and to exit intermingled let blocks.
         * We can't simply transfer control flow to our caller in that case,
         * because we must gosub to those finally clauses from inner to outer,
         * with the correct stack pointer (i.e., after popping any with,
         * for/in, etc., slots nested inside the finally's try).
         *
         * In this case we mutate JSOP_RETURN into JSOP_SETRVAL and add an
         * extra JSOP_RETRVAL after the fixups.
         */
        top = CG_OFFSET(cg);
        if (js_Emit1(cx, cg, JSOP_RETURN) < 0)
            return JS_FALSE;
        if (!EmitNonLocalJumpFixup(cx, cg, NULL))
            return JS_FALSE;
        if (top + JSOP_RETURN_LENGTH != CG_OFFSET(cg)) {
            CG_BASE(cg)[top] = JSOP_SETRVAL;
            if (js_Emit1(cx, cg, JSOP_RETRVAL) < 0)
                return JS_FALSE;
            if (EmitBlockChain(cx, cg) < 0)
                return JS_FALSE;
        }
        break;

#if JS_HAS_GENERATORS
      case TOK_YIELD:
        JS_ASSERT(cg->inFunction());
        if (pn->pn_kid) {
            if (!js_EmitTree(cx, cg, pn->pn_kid))
                return JS_FALSE;
        } else {
            if (js_Emit1(cx, cg, JSOP_PUSH) < 0)
                return JS_FALSE;
        }
        if (pn->pn_hidden && js_NewSrcNote(cx, cg, SRC_HIDDEN) < 0)
            return JS_FALSE;
        if (js_Emit1(cx, cg, JSOP_YIELD) < 0)
            return JS_FALSE;
        break;
#endif

      case TOK_LC:
      {
#if JS_HAS_XML_SUPPORT
        if (pn->isArity(PN_UNARY)) {
            if (!js_EmitTree(cx, cg, pn->pn_kid))
                return JS_FALSE;
            if (js_Emit1(cx, cg, pn->getOp()) < 0)
                return JS_FALSE;
            break;
        }
#endif

        JS_ASSERT(pn->isArity(PN_LIST));

        noteIndex = -1;
        tmp = CG_OFFSET(cg);
        if (pn->pn_xflags & PNX_NEEDBRACES) {
            noteIndex = js_NewSrcNote2(cx, cg, SRC_BRACE, 0);
            if (noteIndex < 0 || js_Emit1(cx, cg, JSOP_NOP) < 0)
                return JS_FALSE;
        }

        js_PushStatement(cg, &stmtInfo, STMT_BLOCK, top);

        JSParseNode *pnchild = pn->pn_head;
        if (pn->pn_xflags & PNX_FUNCDEFS) {
            /*
             * This block contains top-level function definitions. To ensure
             * that we emit the bytecode defining them before the rest of code
             * in the block we use a separate pass over functions. During the
             * main pass later the emitter will add JSOP_NOP with source notes
             * for the function to preserve the original functions position
             * when decompiling.
             *
             * Currently this is used only for functions, as compile-as-we go
             * mode for scripts does not allow separate emitter passes.
             */
            JS_ASSERT(cg->inFunction());
            if (pn->pn_xflags & PNX_DESTRUCT) {
                /*
                 * Assign the destructuring arguments before defining any
                 * functions, see bug 419662.
                 */
                JS_ASSERT(pnchild->isKind(TOK_SEMI));
                JS_ASSERT(pnchild->pn_kid->isKind(TOK_VAR));
                if (!js_EmitTree(cx, cg, pnchild))
                    return JS_FALSE;
                pnchild = pnchild->pn_next;
            }

            for (pn2 = pnchild; pn2; pn2 = pn2->pn_next) {
                if (pn2->isKind(TOK_FUNCTION)) {
                    if (pn2->isOp(JSOP_NOP)) {
                        if (!js_EmitTree(cx, cg, pn2))
                            return JS_FALSE;
                    } else {
                        /*
                         * JSOP_DEFFUN in a top-level block with function
                         * definitions appears, for example, when "if (true)"
                         * is optimized away from "if (true) function x() {}".
                         * See bug 428424.
                         */
                        JS_ASSERT(pn2->isOp(JSOP_DEFFUN));
                    }
                }
            }
        }
        for (pn2 = pnchild; pn2; pn2 = pn2->pn_next) {
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;
        }

        if (noteIndex >= 0 &&
            !js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 0,
                                 CG_OFFSET(cg) - tmp)) {
            return JS_FALSE;
        }

        ok = js_PopStatementCG(cx, cg);
        break;
      }

      case TOK_SEQ:
        JS_ASSERT(pn->isArity(PN_LIST));
        js_PushStatement(cg, &stmtInfo, STMT_SEQ, top);
        for (pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;
        }
        ok = js_PopStatementCG(cx, cg);
        break;

      case TOK_SEMI:
        pn2 = pn->pn_kid;
        if (pn2) {
            /*
             * Top-level or called-from-a-native JS_Execute/EvaluateScript,
             * debugger, and eval frames may need the value of the ultimate
             * expression statement as the script's result, despite the fact
             * that it appears useless to the compiler.
             *
             * API users may also set the JSOPTION_NO_SCRIPT_RVAL option when
             * calling JS_Compile* to suppress JSOP_POPV.
             */
            useful = wantval = !(cg->flags & (TCF_IN_FUNCTION | TCF_NO_SCRIPT_RVAL));

            /* Don't eliminate expressions with side effects. */
            if (!useful) {
                if (!CheckSideEffects(cx, cg, pn2, &useful))
                    return JS_FALSE;
            }

            /*
             * Don't eliminate apparently useless expressions if they are
             * labeled expression statements.  The tc->topStmt->update test
             * catches the case where we are nesting in js_EmitTree for a
             * labeled compound statement.
             */
            if (!useful &&
                cg->topStmt &&
                cg->topStmt->type == STMT_LABEL &&
                cg->topStmt->update >= CG_OFFSET(cg)) {
                useful = true;
            }

            if (!useful) {
                /* Don't complain about directive prologue members; just don't emit their code. */
                if (!pn->isDirectivePrologueMember()) {
                    CG_CURRENT_LINE(cg) = pn2->pn_pos.begin.lineno;
                    if (!ReportCompileErrorNumber(cx, CG_TS(cg), pn2,
                                                  JSREPORT_WARNING | JSREPORT_STRICT,
                                                  JSMSG_USELESS_EXPR)) {
                        return JS_FALSE;
                    }
                }
            } else {
                op = wantval ? JSOP_POPV : JSOP_POP;
#if JS_HAS_DESTRUCTURING
                if (!wantval &&
                    pn2->isKind(TOK_ASSIGN) &&
                    !MaybeEmitGroupAssignment(cx, cg, op, pn2, &op)) {
                    return JS_FALSE;
                }
#endif
                if (op != JSOP_NOP) {
                    /*
                     * Specialize JSOP_SETPROP to JSOP_SETMETHOD to defer or
                     * avoid null closure cloning. Do this only for assignment
                     * statements that are not completion values wanted by a
                     * script evaluator, to ensure that the joined function
                     * can't escape directly.
                     */
                    if (!wantval &&
                        pn2->isKind(TOK_ASSIGN) &&
                        pn2->isOp(JSOP_NOP) &&
                        pn2->pn_left->isOp(JSOP_SETPROP) &&
                        pn2->pn_right->isOp(JSOP_LAMBDA) &&
                        pn2->pn_right->pn_funbox->joinable()) {
                        pn2->pn_left->setOp(JSOP_SETMETHOD);
                    }
                    if (!js_EmitTree(cx, cg, pn2))
                        return JS_FALSE;
                    if (js_Emit1(cx, cg, op) < 0)
                        return JS_FALSE;
                }
            }
        }
        break;

      case TOK_COLON:
        /* Emit an annotated nop so we know to decompile a label. */
        atom = pn->pn_atom;

        jsatomid index;
        if (!cg->makeAtomIndex(atom, &index))
            return JS_FALSE;

        pn2 = pn->expr();
        noteType = (pn2->isKind(TOK_LC) ||
                    (pn2->isKind(TOK_LEXICALSCOPE) &&
                     pn2->expr()->isKind(TOK_LC)))
                   ? SRC_LABELBRACE
                   : SRC_LABEL;
        noteIndex = js_NewSrcNote2(cx, cg, noteType, ptrdiff_t(index));
        if (noteIndex < 0 ||
            js_Emit1(cx, cg, JSOP_NOP) < 0) {
            return JS_FALSE;
        }

        /* Emit code for the labeled statement. */
        js_PushStatement(cg, &stmtInfo, STMT_LABEL, CG_OFFSET(cg));
        stmtInfo.label = atom;
        if (!js_EmitTree(cx, cg, pn2))
            return JS_FALSE;
        if (!js_PopStatementCG(cx, cg))
            return JS_FALSE;

        /* If the statement was compound, emit a note for the end brace. */
        if (noteType == SRC_LABELBRACE) {
            if (js_NewSrcNote(cx, cg, SRC_ENDBRACE) < 0 ||
                js_Emit1(cx, cg, JSOP_NOP) < 0) {
                return JS_FALSE;
            }
        }
        break;

      case TOK_COMMA:
        /*
         * Emit SRC_PCDELTA notes on each JSOP_POP between comma operands.
         * These notes help the decompiler bracket the bytecodes generated
         * from each sub-expression that follows a comma.
         */
        off = noteIndex = -1;
        for (pn2 = pn->pn_head; ; pn2 = pn2->pn_next) {
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;
            tmp = CG_OFFSET(cg);
            if (noteIndex >= 0) {
                if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 0, tmp-off))
                    return JS_FALSE;
            }
            if (!pn2->pn_next)
                break;
            off = tmp;
            noteIndex = js_NewSrcNote2(cx, cg, SRC_PCDELTA, 0);
            if (noteIndex < 0 ||
                js_Emit1(cx, cg, JSOP_POP) < 0) {
                return JS_FALSE;
            }
        }
        break;

      case TOK_ASSIGN:
        if (!EmitAssignment(cx, cg, pn->pn_left, pn->getOp(), pn->pn_right))
            return false;
        break;

      case TOK_HOOK:
        /* Emit the condition, then branch if false to the else part. */
        if (!js_EmitTree(cx, cg, pn->pn_kid1))
            return JS_FALSE;
        noteIndex = js_NewSrcNote(cx, cg, SRC_COND);
        if (noteIndex < 0)
            return JS_FALSE;
        beq = EmitJump(cx, cg, JSOP_IFEQ, 0);
        if (beq < 0 || !js_EmitTree(cx, cg, pn->pn_kid2))
            return JS_FALSE;

        /* Jump around else, fixup the branch, emit else, fixup jump. */
        jmp = EmitJump(cx, cg, JSOP_GOTO, 0);
        if (jmp < 0)
            return JS_FALSE;
        CHECK_AND_SET_JUMP_OFFSET_AT(cx, cg, beq);

        /*
         * Because each branch pushes a single value, but our stack budgeting
         * analysis ignores branches, we now have to adjust cg->stackDepth to
         * ignore the value pushed by the first branch.  Execution will follow
         * only one path, so we must decrement cg->stackDepth.
         *
         * Failing to do this will foil code, such as the try/catch/finally
         * exception handling code generator, that samples cg->stackDepth for
         * use at runtime (JSOP_SETSP), or in let expression and block code
         * generation, which must use the stack depth to compute local stack
         * indexes correctly.
         */
        JS_ASSERT(cg->stackDepth > 0);
        cg->stackDepth--;
        if (!js_EmitTree(cx, cg, pn->pn_kid3))
            return JS_FALSE;
        CHECK_AND_SET_JUMP_OFFSET_AT(cx, cg, jmp);
        if (!js_SetSrcNoteOffset(cx, cg, noteIndex, 0, jmp - beq))
            return JS_FALSE;
        break;

      case TOK_OR:
      case TOK_AND:
        /*
         * JSOP_OR converts the operand on the stack to boolean, and if true,
         * leaves the original operand value on the stack and jumps; otherwise
         * it pops and falls into the next bytecode, which evaluates the right
         * operand.  The jump goes around the right operand evaluation.
         *
         * JSOP_AND converts the operand on the stack to boolean, and if false,
         * leaves the original operand value on the stack and jumps; otherwise
         * it pops and falls into the right operand's bytecode.
         */
        if (pn->isArity(PN_BINARY)) {
            if (!js_EmitTree(cx, cg, pn->pn_left))
                return JS_FALSE;
            top = EmitJump(cx, cg, JSOP_BACKPATCH_POP, 0);
            if (top < 0)
                return JS_FALSE;
            if (!js_EmitTree(cx, cg, pn->pn_right))
                return JS_FALSE;
            off = CG_OFFSET(cg);
            pc = CG_CODE(cg, top);
            CHECK_AND_SET_JUMP_OFFSET(cx, cg, pc, off - top);
            *pc = pn->getOp();
        } else {
            JS_ASSERT(pn->isArity(PN_LIST));
            JS_ASSERT(pn->pn_head->pn_next->pn_next);

            /* Left-associative operator chain: avoid too much recursion. */
            pn2 = pn->pn_head;
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;
            top = EmitJump(cx, cg, JSOP_BACKPATCH_POP, 0);
            if (top < 0)
                return JS_FALSE;

            /* Emit nodes between the head and the tail. */
            jmp = top;
            while ((pn2 = pn2->pn_next)->pn_next) {
                if (!js_EmitTree(cx, cg, pn2))
                    return JS_FALSE;
                off = EmitJump(cx, cg, JSOP_BACKPATCH_POP, 0);
                if (off < 0)
                    return JS_FALSE;
                if (!SetBackPatchDelta(cx, cg, CG_CODE(cg, jmp), off - jmp))
                    return JS_FALSE;
                jmp = off;

            }
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;

            pn2 = pn->pn_head;
            off = CG_OFFSET(cg);
            do {
                pc = CG_CODE(cg, top);
                tmp = GetJumpOffset(cg, pc);
                CHECK_AND_SET_JUMP_OFFSET(cx, cg, pc, off - top);
                *pc = pn->getOp();
                top += tmp;
            } while ((pn2 = pn2->pn_next)->pn_next);
        }
        break;

      case TOK_PLUS:
      case TOK_BITOR:
      case TOK_BITXOR:
      case TOK_BITAND:
      case TOK_EQOP:
      case TOK_RELOP:
      case TOK_IN:
      case TOK_INSTANCEOF:
      case TOK_SHOP:
      case TOK_MINUS:
      case TOK_STAR:
      case TOK_DIVOP:
        if (pn->isArity(PN_LIST)) {
            /* Left-associative operator chain: avoid too much recursion. */
            pn2 = pn->pn_head;
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;
            op = pn->getOp();
            while ((pn2 = pn2->pn_next) != NULL) {
                if (!js_EmitTree(cx, cg, pn2))
                    return JS_FALSE;
                if (js_Emit1(cx, cg, op) < 0)
                    return JS_FALSE;
            }
        } else {
#if JS_HAS_XML_SUPPORT
            uintN oldflags;

      case TOK_DBLCOLON:
            if (pn->isArity(PN_NAME)) {
                if (!js_EmitTree(cx, cg, pn->expr()))
                    return JS_FALSE;
                if (!EmitAtomOp(cx, pn, pn->getOp(), cg))
                    return JS_FALSE;
                break;
            }

            /*
             * Binary :: has a right operand that brackets arbitrary code,
             * possibly including a let (a = b) ... expression.  We must clear
             * TCF_IN_FOR_INIT to avoid mis-compiling such beasts.
             */
            oldflags = cg->flags;
            cg->flags &= ~TCF_IN_FOR_INIT;
#endif

            /* Binary operators that evaluate both operands unconditionally. */
            if (!js_EmitTree(cx, cg, pn->pn_left))
                return JS_FALSE;
            if (!js_EmitTree(cx, cg, pn->pn_right))
                return JS_FALSE;
#if JS_HAS_XML_SUPPORT
            cg->flags |= oldflags & TCF_IN_FOR_INIT;
#endif
            if (js_Emit1(cx, cg, pn->getOp()) < 0)
                return JS_FALSE;
        }
        break;

      case TOK_THROW:
#if JS_HAS_XML_SUPPORT
      case TOK_AT:
      case TOK_DEFAULT:
        JS_ASSERT(pn->isArity(PN_UNARY));
        /* FALL THROUGH */
#endif
      case TOK_UNARYOP:
      {
        uintN oldflags;

        /* Unary op, including unary +/-. */
        op = pn->getOp();
#if JS_HAS_XML_SUPPORT
        if (op == JSOP_XMLNAME) {
            if (!EmitXMLName(cx, pn, op, cg))
                return JS_FALSE;
            break;
        }
#endif
        pn2 = pn->pn_kid;

        if (op == JSOP_TYPEOF && !pn2->isKind(TOK_NAME))
            op = JSOP_TYPEOFEXPR;

        oldflags = cg->flags;
        cg->flags &= ~TCF_IN_FOR_INIT;
        if (!js_EmitTree(cx, cg, pn2))
            return JS_FALSE;
        cg->flags |= oldflags & TCF_IN_FOR_INIT;
        if (js_Emit1(cx, cg, op) < 0)
            return JS_FALSE;
        break;
      }

      case TOK_INC:
      case TOK_DEC:
        /* Emit lvalue-specialized code for ++/-- operators. */
        pn2 = pn->pn_kid;
        JS_ASSERT(!pn2->isKind(TOK_RP));
        op = pn->getOp();
        switch (pn2->getKind()) {
          default:
            JS_ASSERT(pn2->isKind(TOK_NAME));
            pn2->setOp(op);
            if (!BindNameToSlot(cx, cg, pn2))
                return JS_FALSE;
            op = pn2->getOp();
            if (op == JSOP_CALLEE) {
                if (js_Emit1(cx, cg, op) < 0)
                    return JS_FALSE;
            } else if (!pn2->pn_cookie.isFree()) {
                atomIndex = pn2->pn_cookie.asInteger();
                EMIT_UINT16_IMM_OP(op, atomIndex);
            } else {
                JS_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);
                if (js_CodeSpec[op].format & (JOF_INC | JOF_DEC)) {
                    if (!EmitNameIncDec(cx, pn2, op, cg))
                        return JS_FALSE;
                } else {
                    if (!EmitAtomOp(cx, pn2, op, cg))
                        return JS_FALSE;
                }
                break;
            }
            if (pn2->isConst()) {
                if (js_Emit1(cx, cg, JSOP_POS) < 0)
                    return JS_FALSE;
                op = pn->getOp();
                if (!(js_CodeSpec[op].format & JOF_POST)) {
                    if (js_Emit1(cx, cg, JSOP_ONE) < 0)
                        return JS_FALSE;
                    op = (js_CodeSpec[op].format & JOF_INC) ? JSOP_ADD : JSOP_SUB;
                    if (js_Emit1(cx, cg, op) < 0)
                        return JS_FALSE;
                }
            }
            break;
          case TOK_DOT:
            if (!EmitPropIncDec(cx, pn2, op, cg))
                return JS_FALSE;
            break;
          case TOK_LB:
            if (!EmitElemIncDec(cx, pn2, op, cg))
                return JS_FALSE;
            break;
          case TOK_LP:
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;
            if (js_NewSrcNote2(cx, cg, SRC_PCBASE,
                               CG_OFFSET(cg) - pn2->pn_offset) < 0) {
                return JS_FALSE;
            }
            if (js_Emit1(cx, cg, op) < 0)
                return JS_FALSE;
            /*
             * This is dead code for the decompiler, don't generate
             * a decomposed version of the opcode. We do need to balance
             * the stacks in the decomposed version.
             */
            JS_ASSERT(js_CodeSpec[op].format & JOF_DECOMPOSE);
            JS_ASSERT(js_CodeSpec[op].format & JOF_ELEM);
            if (js_Emit1(cx, cg, (JSOp)1) < 0)
                return JS_FALSE;
            if (js_Emit1(cx, cg, JSOP_POP) < 0)
                return JS_FALSE;
            break;
#if JS_HAS_XML_SUPPORT
          case TOK_UNARYOP:
            JS_ASSERT(pn2->isOp(JSOP_SETXMLNAME));
            if (!js_EmitTree(cx, cg, pn2->pn_kid))
                return JS_FALSE;
            if (js_Emit1(cx, cg, JSOP_BINDXMLNAME) < 0)
                return JS_FALSE;
            if (!EmitElemIncDec(cx, NULL, op, cg))
                return JS_FALSE;
            break;
#endif
        }
        break;

      case TOK_DELETE:
        /*
         * Under ECMA 3, deleting a non-reference returns true -- but alas we
         * must evaluate the operand if it appears it might have side effects.
         */
        pn2 = pn->pn_kid;
        switch (pn2->getKind()) {
          case TOK_NAME:
            if (!BindNameToSlot(cx, cg, pn2))
                return JS_FALSE;
            op = pn2->getOp();
            if (op == JSOP_FALSE) {
                if (js_Emit1(cx, cg, op) < 0)
                    return JS_FALSE;
            } else {
                if (!EmitAtomOp(cx, pn2, op, cg))
                    return JS_FALSE;
            }
            break;
          case TOK_DOT:
            if (!EmitPropOp(cx, pn2, JSOP_DELPROP, cg, JS_FALSE))
                return JS_FALSE;
            break;
#if JS_HAS_XML_SUPPORT
          case TOK_DBLDOT:
            if (!EmitElemOp(cx, pn2, JSOP_DELDESC, cg))
                return JS_FALSE;
            break;
#endif
          case TOK_LB:
            if (!EmitElemOp(cx, pn2, JSOP_DELELEM, cg))
                return JS_FALSE;
            break;
          default:
            /*
             * If useless, just emit JSOP_TRUE; otherwise convert delete foo()
             * to foo(), true (a comma expression, requiring SRC_PCDELTA).
             */
            useful = JS_FALSE;
            if (!CheckSideEffects(cx, cg, pn2, &useful))
                return JS_FALSE;
            if (!useful) {
                off = noteIndex = -1;
            } else {
                JS_ASSERT_IF(pn2->isKind(TOK_LP), !(pn2->pn_xflags & PNX_SETCALL));
                if (!js_EmitTree(cx, cg, pn2))
                    return JS_FALSE;
                off = CG_OFFSET(cg);
                noteIndex = js_NewSrcNote2(cx, cg, SRC_PCDELTA, 0);
                if (noteIndex < 0 || js_Emit1(cx, cg, JSOP_POP) < 0)
                    return JS_FALSE;
            }
            if (js_Emit1(cx, cg, JSOP_TRUE) < 0)
                return JS_FALSE;
            if (noteIndex >= 0) {
                tmp = CG_OFFSET(cg);
                if (!js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 0, tmp-off))
                    return JS_FALSE;
            }
        }
        break;

#if JS_HAS_XML_SUPPORT
      case TOK_FILTER:
        if (!js_EmitTree(cx, cg, pn->pn_left))
            return JS_FALSE;
        jmp = EmitJump(cx, cg, JSOP_FILTER, 0);
        if (jmp < 0)
            return JS_FALSE;
        top = EmitTraceOp(cx, cg, pn->pn_right);
        if (top < 0)
            return JS_FALSE;
        if (!js_EmitTree(cx, cg, pn->pn_right))
            return JS_FALSE;
        CHECK_AND_SET_JUMP_OFFSET_AT(cx, cg, jmp);
        if (EmitJump(cx, cg, JSOP_ENDFILTER, top - CG_OFFSET(cg)) < 0)
            return JS_FALSE;

        /* Make blockChain determination quicker. */
        if (EmitBlockChain(cx, cg) < 0)
            return JS_FALSE;
        break;
#endif

      case TOK_DOT:
        /*
         * Pop a stack operand, convert it to object, get a property named by
         * this bytecode's immediate-indexed atom operand, and push its value
         * (not a reference to it).
         */
        ok = EmitPropOp(cx, pn, pn->getOp(), cg, JS_FALSE);
        break;

      case TOK_LB:
#if JS_HAS_XML_SUPPORT
      case TOK_DBLDOT:
#endif
        /*
         * Pop two operands, convert the left one to object and the right one
         * to property name (atom or tagged int), get the named property, and
         * push its value.  Set the "obj" register to the result of ToObject
         * on the left operand.
         */
        ok = EmitElemOp(cx, pn, pn->getOp(), cg);
        break;

      case TOK_NEW:
      case TOK_LP:
      {
        bool callop = pn->isKind(TOK_LP);

        /*
         * Emit callable invocation or operator new (constructor call) code.
         * First, emit code for the left operand to evaluate the callable or
         * constructable object expression.
         *
         * For operator new applied to other expressions than E4X ones, we emit
         * JSOP_GETPROP instead of JSOP_CALLPROP, etc. This is necessary to
         * interpose the lambda-initialized method read barrier -- see the code
         * in jsinterp.cpp for JSOP_LAMBDA followed by JSOP_{SET,INIT}PROP.
         *
         * Then (or in a call case that has no explicit reference-base
         * object) we emit JSOP_PUSH to produce the |this| slot required
         * for calls (which non-strict mode functions will box into the
         * global object).
         */
        pn2 = pn->pn_head;
        switch (pn2->getKind()) {
          case TOK_NAME:
            if (!EmitNameOp(cx, cg, pn2, callop))
                return JS_FALSE;
            break;
          case TOK_DOT:
            if (!EmitPropOp(cx, pn2, pn2->getOp(), cg, callop))
                return JS_FALSE;
            break;
          case TOK_LB:
            JS_ASSERT(pn2->isOp(JSOP_GETELEM));
            if (!EmitElemOp(cx, pn2, callop ? JSOP_CALLELEM : JSOP_GETELEM, cg))
                return JS_FALSE;
            break;
          case TOK_UNARYOP:
#if JS_HAS_XML_SUPPORT
            if (pn2->isOp(JSOP_XMLNAME)) {
                if (!EmitXMLName(cx, pn2, JSOP_CALLXMLNAME, cg))
                    return JS_FALSE;
                callop = true;          /* suppress JSOP_PUSH after */
                break;
            }
#endif
            /* FALL THROUGH */
          default:
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;
            callop = false;             /* trigger JSOP_PUSH after */
            break;
        }
        if (!callop && js_Emit1(cx, cg, JSOP_PUSH) < 0)
            return JS_FALSE;

        /* Remember start of callable-object bytecode for decompilation hint. */
        off = top;

        /*
         * Emit code for each argument in order, then emit the JSOP_*CALL or
         * JSOP_NEW bytecode with a two-byte immediate telling how many args
         * were pushed on the operand stack.
         */
        uintN oldflags = cg->flags;
        cg->flags &= ~TCF_IN_FOR_INIT;
        for (pn3 = pn2->pn_next; pn3; pn3 = pn3->pn_next) {
            if (!js_EmitTree(cx, cg, pn3))
                return JS_FALSE;
        }
        cg->flags |= oldflags & TCF_IN_FOR_INIT;
        if (js_NewSrcNote2(cx, cg, SRC_PCBASE, CG_OFFSET(cg) - off) < 0)
            return JS_FALSE;

        argc = pn->pn_count - 1;
        if (js_Emit3(cx, cg, pn->getOp(), ARGC_HI(argc), ARGC_LO(argc)) < 0)
            return JS_FALSE;
        CheckTypeSet(cx, cg, pn->getOp());
        if (pn->isOp(JSOP_EVAL)) {
            EMIT_UINT16_IMM_OP(JSOP_LINENO, pn->pn_pos.begin.lineno);
            if (EmitBlockChain(cx, cg) < 0)
                return JS_FALSE;
        }
        if (pn->pn_xflags & PNX_SETCALL) {
            if (js_Emit1(cx, cg, JSOP_SETCALL) < 0)
                return JS_FALSE;
        }
        break;
      }

      case TOK_LEXICALSCOPE:
      {
        JSObjectBox *objbox;

        objbox = pn->pn_objbox;
        js_PushBlockScope(cg, &stmtInfo, objbox, CG_OFFSET(cg));

        /*
         * If this lexical scope is not for a catch block, let block or let
         * expression, or any kind of for loop (where the scope starts in the
         * head after the first part if for (;;), else in the body if for-in);
         * and if our container is top-level but not a function body, or else
         * a block statement; then emit a SRC_BRACE note.  All other container
         * statements get braces by default from the decompiler.
         */
        noteIndex = -1;
        type = pn->expr()->getKind();
        if (type != TOK_CATCH && type != TOK_LET && type != TOK_FOR &&
            (!(stmt = stmtInfo.down)
             ? !cg->inFunction()
             : stmt->type == STMT_BLOCK)) {
#if defined DEBUG_brendan || defined DEBUG_mrbkap
            /* There must be no source note already output for the next op. */
            JS_ASSERT(CG_NOTE_COUNT(cg) == 0 ||
                      CG_LAST_NOTE_OFFSET(cg) != CG_OFFSET(cg) ||
                      !GettableNoteForNextOp(cg));
#endif
            noteIndex = js_NewSrcNote2(cx, cg, SRC_BRACE, 0);
            if (noteIndex < 0)
                return JS_FALSE;
        }

        JS_ASSERT(CG_OFFSET(cg) == top);
        if (!EmitEnterBlock(cx, pn, cg))
            return JS_FALSE;

        if (!js_EmitTree(cx, cg, pn->pn_expr))
            return JS_FALSE;

        op = pn->getOp();
        if (op == JSOP_LEAVEBLOCKEXPR) {
            if (js_NewSrcNote2(cx, cg, SRC_PCBASE, CG_OFFSET(cg) - top) < 0)
                return JS_FALSE;
        } else {
            if (noteIndex >= 0 &&
                !js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 0,
                                     CG_OFFSET(cg) - top)) {
                return JS_FALSE;
            }
        }

        /* Emit the JSOP_LEAVEBLOCK or JSOP_LEAVEBLOCKEXPR opcode. */
        if (!EmitLeaveBlock(cx, cg, op, objbox))
            return JS_FALSE;

        ok = js_PopStatementCG(cx, cg);
        break;
      }

#if JS_HAS_BLOCK_SCOPE
      case TOK_LET: {
        /*
         * pn represents one of these syntactic constructs:
         *   let-expression:                        (let (x = y) EXPR)
         *   let-statement:                         let (x = y) { ... }
         *   let-declaration in statement context:  let x = y;
         *   let-declaration in for-loop head:      for (let ...) ...
         *
         * Let-expressions and let-statements are represented as binary nodes
         * with their variable declarations on the left and the body on the
         * right.
         */
        if (pn->isArity(PN_BINARY)) {
            pn2 = pn->pn_right;
            pn = pn->pn_left;
        } else {
            pn2 = NULL;
        }

        /*
         * Non-null pn2 means that pn is the variable list from a let head.
         *
         * Use TempPopScope to evaluate the expressions in the enclosing scope.
         * This also causes the initializing assignments to be emitted in the
         * enclosing scope, but the assignment opcodes emitted here
         * (essentially just setlocal, though destructuring assignment uses
         * other additional opcodes) do not care about the block chain.
         */
        JS_ASSERT(pn->isArity(PN_LIST));
        TempPopScope tps;
        bool popScope = pn2 || (cg->flags & TCF_IN_FOR_INIT);
        if (popScope && !tps.popBlock(cx, cg))
            return JS_FALSE;
        if (!EmitVariables(cx, cg, pn, pn2 != NULL, &noteIndex))
            return JS_FALSE;
        tmp = CG_OFFSET(cg);
        if (popScope && !tps.repushBlock(cx, cg))
            return JS_FALSE;

        /* Thus non-null pn2 is the body of the let block or expression. */
        if (pn2 && !js_EmitTree(cx, cg, pn2))
            return JS_FALSE;

        if (noteIndex >= 0 &&
            !js_SetSrcNoteOffset(cx, cg, (uintN)noteIndex, 0,
                                 CG_OFFSET(cg) - tmp)) {
            return JS_FALSE;
        }
        break;
      }
#endif /* JS_HAS_BLOCK_SCOPE */

#if JS_HAS_GENERATORS
      case TOK_ARRAYPUSH: {
        jsint slot;

        /*
         * The array object's stack index is in cg->arrayCompDepth. See below
         * under the array initialiser code generator for array comprehension
         * special casing.
         */
        if (!js_EmitTree(cx, cg, pn->pn_kid))
            return JS_FALSE;
        slot = AdjustBlockSlot(cx, cg, cg->arrayCompDepth);
        if (slot < 0)
            return JS_FALSE;
        EMIT_UINT16_IMM_OP(pn->getOp(), slot);
        break;
      }
#endif

      case TOK_RB:
#if JS_HAS_GENERATORS
      case TOK_ARRAYCOMP:
#endif
        /*
         * Emit code for [a, b, c] that is equivalent to constructing a new
         * array and in source order evaluating each element value and adding
         * it to the array, without invoking latent setters.  We use the
         * JSOP_NEWINIT and JSOP_INITELEM bytecodes to ignore setters and to
         * avoid dup'ing and popping the array as each element is added, as
         * JSOP_SETELEM/JSOP_SETPROP would do.
         */
#if JS_HAS_SHARP_VARS
        sharpnum = -1;
      do_emit_array:
#endif

#if JS_HAS_GENERATORS
        if (pn->isKind(TOK_ARRAYCOMP)) {
            uintN saveDepth;

            if (!EmitNewInit(cx, cg, JSProto_Array, pn, sharpnum))
                return JS_FALSE;

            /*
             * Pass the new array's stack index to the TOK_ARRAYPUSH case via
             * cg->arrayCompDepth, then simply traverse the TOK_FOR node and
             * its kids under pn2 to generate this comprehension.
             */
            JS_ASSERT(cg->stackDepth > 0);
            saveDepth = cg->arrayCompDepth;
            cg->arrayCompDepth = (uint32) (cg->stackDepth - 1);
            if (!js_EmitTree(cx, cg, pn->pn_head))
                return JS_FALSE;
            cg->arrayCompDepth = saveDepth;

            /* Emit the usual op needed for decompilation. */
            if (!EmitEndInit(cx, cg, 1))
                return JS_FALSE;
            break;
        }
#endif /* JS_HAS_GENERATORS */

        if (!cg->hasSharps() && !(pn->pn_xflags & PNX_NONCONST) && pn->pn_head &&
            cg->checkSingletonContext()) {
            if (!EmitSingletonInitialiser(cx, cg, pn))
                return JS_FALSE;
            break;
        }

        /* Use the slower NEWINIT for arrays in scripts containing sharps. */
        if (cg->hasSharps()) {
            if (!EmitNewInit(cx, cg, JSProto_Array, pn, sharpnum))
                return JS_FALSE;
        } else {
            ptrdiff_t off = js_EmitN(cx, cg, JSOP_NEWARRAY, 3);
            if (off < 0)
                return JS_FALSE;
            pc = CG_CODE(cg, off);
            SET_UINT24(pc, pn->pn_count);
        }

        pn2 = pn->pn_head;
        for (atomIndex = 0; pn2; atomIndex++, pn2 = pn2->pn_next) {
            if (!EmitNumberOp(cx, atomIndex, cg))
                return JS_FALSE;
            if (pn2->isKind(TOK_COMMA) && pn2->isArity(PN_NULLARY)) {
                if (js_Emit1(cx, cg, JSOP_HOLE) < 0)
                    return JS_FALSE;
            } else {
                if (!js_EmitTree(cx, cg, pn2))
                    return JS_FALSE;
            }
            if (js_Emit1(cx, cg, JSOP_INITELEM) < 0)
                return JS_FALSE;
        }
        JS_ASSERT(atomIndex == pn->pn_count);

        if (pn->pn_xflags & PNX_ENDCOMMA) {
            /* Emit a source note so we know to decompile an extra comma. */
            if (js_NewSrcNote(cx, cg, SRC_CONTINUE) < 0)
                return JS_FALSE;
        }

        /*
         * Emit an op to finish the array and, secondarily, to aid in sharp
         * array cleanup (if JS_HAS_SHARP_VARS) and decompilation.
         */
        if (!EmitEndInit(cx, cg, atomIndex))
            return JS_FALSE;
        break;

      case TOK_RC: {
#if JS_HAS_SHARP_VARS
        sharpnum = -1;
      do_emit_object:
#endif
#if JS_HAS_DESTRUCTURING_SHORTHAND
        if (pn->pn_xflags & PNX_DESTRUCT) {
            ReportCompileErrorNumber(cx, CG_TS(cg), pn, JSREPORT_ERROR, JSMSG_BAD_OBJECT_INIT);
            return JS_FALSE;
        }
#endif

        if (!cg->hasSharps() && !(pn->pn_xflags & PNX_NONCONST) && pn->pn_head &&
            cg->checkSingletonContext()) {
            if (!EmitSingletonInitialiser(cx, cg, pn))
                return JS_FALSE;
            break;
        }

        /*
         * Emit code for {p:a, '%q':b, 2:c} that is equivalent to constructing
         * a new object and in source order evaluating each property value and
         * adding the property to the object, without invoking latent setters.
         * We use the JSOP_NEWINIT and JSOP_INITELEM/JSOP_INITPROP bytecodes to
         * ignore setters and to avoid dup'ing and popping the object as each
         * property is added, as JSOP_SETELEM/JSOP_SETPROP would do.
         */
        ptrdiff_t offset = CG_NEXT(cg) - CG_BASE(cg);
        if (!EmitNewInit(cx, cg, JSProto_Object, pn, sharpnum))
            return JS_FALSE;

        /*
         * Try to construct the shape of the object as we go, so we can emit a
         * JSOP_NEWOBJECT with the final shape instead.
         */
        JSObject *obj = NULL;
        if (!cg->hasSharps() && cg->compileAndGo()) {
            gc::AllocKind kind = GuessObjectGCKind(pn->pn_count, false);
            obj = NewBuiltinClassInstance(cx, &ObjectClass, kind);
            if (!obj)
                return JS_FALSE;
        }

        uintN methodInits = 0, slowMethodInits = 0;
        for (pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
            /* Emit an index for t[2] for later consumption by JSOP_INITELEM. */
            pn3 = pn2->pn_left;
            if (pn3->isKind(TOK_NUMBER)) {
                if (!EmitNumberOp(cx, pn3->pn_dval, cg))
                    return JS_FALSE;
            }

            /* Emit code for the property initializer. */
            if (!js_EmitTree(cx, cg, pn2->pn_right))
                return JS_FALSE;

            op = pn2->getOp();
            if (op == JSOP_GETTER || op == JSOP_SETTER) {
                obj = NULL;
                if (js_Emit1(cx, cg, op) < 0)
                    return JS_FALSE;
            }

            /* Annotate JSOP_INITELEM so we decompile 2:c and not just c. */
            if (pn3->isKind(TOK_NUMBER)) {
                obj = NULL;
                if (js_NewSrcNote(cx, cg, SRC_INITPROP) < 0)
                    return JS_FALSE;
                if (js_Emit1(cx, cg, JSOP_INITELEM) < 0)
                    return JS_FALSE;
            } else {
                JS_ASSERT(pn3->isKind(TOK_NAME) ||
                          pn3->isKind(TOK_STRING));
                jsatomid index;
                if (!cg->makeAtomIndex(pn3->pn_atom, &index))
                    return JS_FALSE;

                /* Check whether we can optimize to JSOP_INITMETHOD. */
                JSParseNode *init = pn2->pn_right;
                bool lambda = init->isOp(JSOP_LAMBDA);
                if (lambda)
                    ++methodInits;
                if (op == JSOP_INITPROP && lambda && init->pn_funbox->joinable()) {
                    obj = NULL;
                    op = JSOP_INITMETHOD;
                    pn2->setOp(op);
                } else {
                    /*
                     * Disable NEWOBJECT on initializers that set __proto__, which has
                     * a non-standard setter on objects.
                     */
                    if (pn3->pn_atom == cx->runtime->atomState.protoAtom)
                        obj = NULL;
                    op = JSOP_INITPROP;
                    if (lambda)
                        ++slowMethodInits;
                }

                if (obj) {
                    JS_ASSERT(!obj->inDictionaryMode());
                    if (!DefineNativeProperty(cx, obj, ATOM_TO_JSID(pn3->pn_atom),
                                              UndefinedValue(), NULL, NULL,
                                              JSPROP_ENUMERATE, 0, 0)) {
                        return false;
                    }
                    if (obj->inDictionaryMode())
                        obj = NULL;
                }

                EMIT_INDEX_OP(op, index);
            }
        }

        if (cg->funbox && cg->funbox->shouldUnbrand(methodInits, slowMethodInits)) {
            obj = NULL;
            if (js_Emit1(cx, cg, JSOP_UNBRAND) < 0)
                return JS_FALSE;
        }
        if (!EmitEndInit(cx, cg, pn->pn_count))
            return JS_FALSE;

        if (obj) {
            /*
             * The object survived and has a predictable shape.  Update the original bytecode,
             * as long as we can do so without using a big index prefix/suffix.
             */
            JSObjectBox *objbox = cg->parser->newObjectBox(obj);
            if (!objbox)
                return JS_FALSE;
            unsigned index = cg->objectList.index(objbox);
            if (FitsWithoutBigIndex(index))
                EMIT_UINT16_IN_PLACE(offset, JSOP_NEWOBJECT, uint16(index));
        }

        break;
      }

#if JS_HAS_SHARP_VARS
      case TOK_DEFSHARP:
        JS_ASSERT(cg->hasSharps());
        sharpnum = pn->pn_num;
        pn = pn->pn_kid;
        if (pn->isKind(TOK_RB))
            goto do_emit_array;
# if JS_HAS_GENERATORS
        if (pn->isKind(TOK_ARRAYCOMP))
            goto do_emit_array;
# endif
        if (pn->isKind(TOK_RC))
            goto do_emit_object;

        if (!js_EmitTree(cx, cg, pn))
            return JS_FALSE;
        EMIT_UINT16PAIR_IMM_OP(JSOP_DEFSHARP, cg->sharpSlotBase, (jsatomid) sharpnum);
        break;

      case TOK_USESHARP:
        JS_ASSERT(cg->hasSharps());
        EMIT_UINT16PAIR_IMM_OP(JSOP_USESHARP, cg->sharpSlotBase, (jsatomid) pn->pn_num);
        break;
#endif /* JS_HAS_SHARP_VARS */

      case TOK_NAME:
        /*
         * Cope with a left-over function definition that was replaced by a use
         * of a later function definition of the same name. See FunctionDef and
         * MakeDefIntoUse in jsparse.cpp.
         */
        if (pn->isOp(JSOP_NOP))
            break;
        if (!EmitNameOp(cx, cg, pn, JS_FALSE))
            return JS_FALSE;
        break;

#if JS_HAS_XML_SUPPORT
      case TOK_XMLATTR:
      case TOK_XMLSPACE:
      case TOK_XMLTEXT:
      case TOK_XMLCDATA:
      case TOK_XMLCOMMENT:
#endif
      case TOK_STRING:
        ok = EmitAtomOp(cx, pn, pn->getOp(), cg);
        break;

      case TOK_NUMBER:
        ok = EmitNumberOp(cx, pn->pn_dval, cg);
        break;

      case TOK_REGEXP: {
        JS_ASSERT(pn->isOp(JSOP_REGEXP));
        ok = EmitIndexOp(cx, JSOP_REGEXP,
                         cg->regexpList.index(pn->pn_objbox),
                         cg);
        break;
      }

#if JS_HAS_XML_SUPPORT
      case TOK_ANYNAME:
#endif
      case TOK_PRIMARY:
        if (js_Emit1(cx, cg, pn->getOp()) < 0)
            return JS_FALSE;
        break;

      case TOK_DEBUGGER:
        if (js_Emit1(cx, cg, JSOP_DEBUGGER) < 0)
            return JS_FALSE;
        break;

#if JS_HAS_XML_SUPPORT
      case TOK_XMLELEM:
      case TOK_XMLLIST:
        JS_ASSERT(pn->isKind(TOK_XMLLIST) || pn->pn_count != 0);
        switch (pn->pn_head ? pn->pn_head->getKind() : TOK_XMLLIST) {
          case TOK_XMLETAGO:
            JS_ASSERT(0);
            /* FALL THROUGH */
          case TOK_XMLPTAGC:
          case TOK_XMLSTAGO:
            break;
          default:
            if (js_Emit1(cx, cg, JSOP_STARTXML) < 0)
                return JS_FALSE;
        }

        for (pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
            if (pn2->isKind(TOK_LC) &&
                js_Emit1(cx, cg, JSOP_STARTXMLEXPR) < 0) {
                return JS_FALSE;
            }
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;
            if (pn2 != pn->pn_head && js_Emit1(cx, cg, JSOP_ADD) < 0)
                return JS_FALSE;
        }

        if (pn->pn_xflags & PNX_XMLROOT) {
            if (pn->pn_count == 0) {
                JS_ASSERT(pn->isKind(TOK_XMLLIST));
                atom = cx->runtime->atomState.emptyAtom;
                jsatomid index;
                if (!cg->makeAtomIndex(atom, &index))
                    return JS_FALSE;
                EMIT_INDEX_OP(JSOP_STRING, index);
            }
            if (js_Emit1(cx, cg, pn->getOp()) < 0)
                return JS_FALSE;
        }
#ifdef DEBUG
        else
            JS_ASSERT(pn->pn_count != 0);
#endif
        break;

      case TOK_XMLPTAGC:
      case TOK_XMLSTAGO:
      case TOK_XMLETAGO:
      {
        uint32 i;

        if (js_Emit1(cx, cg, JSOP_STARTXML) < 0)
            return JS_FALSE;

        {
            jsatomid index;
            JSAtom *tmp = (pn->isKind(TOK_XMLETAGO)) ? cx->runtime->atomState.etagoAtom
                                                     : cx->runtime->atomState.stagoAtom;
            if (!cg->makeAtomIndex(tmp, &index))
                return JS_FALSE;
            EMIT_INDEX_OP(JSOP_STRING, index);
        }

        JS_ASSERT(pn->pn_count != 0);
        pn2 = pn->pn_head;
        if (pn2->isKind(TOK_LC) && js_Emit1(cx, cg, JSOP_STARTXMLEXPR) < 0)
            return JS_FALSE;
        if (!js_EmitTree(cx, cg, pn2))
            return JS_FALSE;
        if (js_Emit1(cx, cg, JSOP_ADD) < 0)
            return JS_FALSE;

        for (pn2 = pn2->pn_next, i = 0; pn2; pn2 = pn2->pn_next, i++) {
            if (pn2->isKind(TOK_LC) &&
                js_Emit1(cx, cg, JSOP_STARTXMLEXPR) < 0) {
                return JS_FALSE;
            }
            if (!js_EmitTree(cx, cg, pn2))
                return JS_FALSE;
            if ((i & 1) && pn2->isKind(TOK_LC)) {
                if (js_Emit1(cx, cg, JSOP_TOATTRVAL) < 0)
                    return JS_FALSE;
            }
            if (js_Emit1(cx, cg,
                         (i & 1) ? JSOP_ADDATTRVAL : JSOP_ADDATTRNAME) < 0) {
                return JS_FALSE;
            }
        }

        {
            jsatomid index;
            JSAtom *tmp = (pn->isKind(TOK_XMLPTAGC)) ? cx->runtime->atomState.ptagcAtom
                                                     : cx->runtime->atomState.tagcAtom;
            if (!cg->makeAtomIndex(tmp, &index))
                return JS_FALSE;
            EMIT_INDEX_OP(JSOP_STRING, index);
        }
        if (js_Emit1(cx, cg, JSOP_ADD) < 0)
            return JS_FALSE;

        if ((pn->pn_xflags & PNX_XMLROOT) && js_Emit1(cx, cg, pn->getOp()) < 0)
            return JS_FALSE;
        break;
      }

      case TOK_XMLNAME:
        if (pn->isArity(PN_LIST)) {
            JS_ASSERT(pn->pn_count != 0);
            for (pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
                if (pn2->isKind(TOK_LC) &&
                    js_Emit1(cx, cg, JSOP_STARTXMLEXPR) < 0) {
                    return JS_FALSE;
                }
                if (!js_EmitTree(cx, cg, pn2))
                    return JS_FALSE;
                if (pn2 != pn->pn_head && js_Emit1(cx, cg, JSOP_ADD) < 0)
                    return JS_FALSE;
            }
        } else {
            JS_ASSERT(pn->isArity(PN_NULLARY));
            ok = pn->isOp(JSOP_OBJECT)
                 ? EmitObjectOp(cx, pn->pn_objbox, pn->getOp(), cg)
                 : EmitAtomOp(cx, pn, pn->getOp(), cg);
        }
        break;

      case TOK_XMLPI: {
        jsatomid index;
        if (!cg->makeAtomIndex(pn->pn_atom2, &index))
            return false;
        if (!EmitIndexOp(cx, JSOP_QNAMEPART, index, cg))
            return JS_FALSE;
        if (!EmitAtomOp(cx, pn, JSOP_XMLPI, cg))
            return JS_FALSE;
        break;
      }
#endif /* JS_HAS_XML_SUPPORT */

      default:
        JS_ASSERT(0);
    }

    /* cg->emitLevel == 1 means we're last on the stack, so finish up. */
    if (ok && cg->emitLevel == 1) {
        if (cg->spanDeps)
            ok = OptimizeSpanDeps(cx, cg);
        if (!UpdateLineNumberNotes(cx, cg, pn->pn_pos.end.lineno))
            return JS_FALSE;
    }

    return ok;
}

/*
 * We should try to get rid of offsetBias (always 0 or 1, where 1 is
 * JSOP_{NOP,POP}_LENGTH), which is used only by SRC_FOR and SRC_DECL.
 */
JS_FRIEND_DATA(JSSrcNoteSpec) js_SrcNoteSpec[] = {
    {"null",            0,      0,      0},
    {"if",              0,      0,      0},
    {"if-else",         2,      0,      1},
    {"for",             3,      1,      1},
    {"while",           1,      0,      1},
    {"continue",        0,      0,      0},
    {"decl",            1,      1,      1},
    {"pcdelta",         1,      0,      1},
    {"assignop",        0,      0,      0},
    {"cond",            1,      0,      1},
    {"brace",           1,      0,      1},
    {"hidden",          0,      0,      0},
    {"pcbase",          1,      0,     -1},
    {"label",           1,      0,      0},
    {"labelbrace",      1,      0,      0},
    {"endbrace",        0,      0,      0},
    {"break2label",     1,      0,      0},
    {"cont2label",      1,      0,      0},
    {"switch",          2,      0,      1},
    {"funcdef",         1,      0,      0},
    {"catch",           1,      0,      1},
    {"extended",       -1,      0,      0},
    {"newline",         0,      0,      0},
    {"setline",         1,      0,      0},
    {"xdelta",          0,      0,      0},
};

static intN
AllocSrcNote(JSContext *cx, JSCodeGenerator *cg)
{
    jssrcnote *notes = CG_NOTES(cg);
    jssrcnote *newnotes;
    uintN index = CG_NOTE_COUNT(cg);
    uintN max = CG_NOTE_LIMIT(cg);

    if (index == max) {
        size_t newlength;
        if (!notes) {
            JS_ASSERT(!index && !max);
            newlength = SRCNOTE_CHUNK_LENGTH;
            newnotes = (jssrcnote *) cx->malloc_(SRCNOTE_SIZE(newlength));
        } else {
            JS_ASSERT(index <= max);
            newlength = max * 2;
            newnotes = (jssrcnote *) cx->realloc_(notes, SRCNOTE_SIZE(newlength));
        }
        if (!newnotes) {
            js_ReportOutOfMemory(cx);
            return -1;
        }
        CG_NOTES(cg) = newnotes;
        CG_NOTE_LIMIT(cg) = newlength;
    }

    CG_NOTE_COUNT(cg) = index + 1;
    return (intN)index;
}

intN
js_NewSrcNote(JSContext *cx, JSCodeGenerator *cg, JSSrcNoteType type)
{
    intN index, n;
    jssrcnote *sn;
    ptrdiff_t offset, delta, xdelta;

    /*
     * Claim a note slot in CG_NOTES(cg) by growing it if necessary and then
     * incrementing CG_NOTE_COUNT(cg).
     */
    index = AllocSrcNote(cx, cg);
    if (index < 0)
        return -1;
    sn = &CG_NOTES(cg)[index];

    /*
     * Compute delta from the last annotated bytecode's offset.  If it's too
     * big to fit in sn, allocate one or more xdelta notes and reset sn.
     */
    offset = CG_OFFSET(cg);
    delta = offset - CG_LAST_NOTE_OFFSET(cg);
    CG_LAST_NOTE_OFFSET(cg) = offset;
    if (delta >= SN_DELTA_LIMIT) {
        do {
            xdelta = JS_MIN(delta, SN_XDELTA_MASK);
            SN_MAKE_XDELTA(sn, xdelta);
            delta -= xdelta;
            index = AllocSrcNote(cx, cg);
            if (index < 0)
                return -1;
            sn = &CG_NOTES(cg)[index];
        } while (delta >= SN_DELTA_LIMIT);
    }

    /*
     * Initialize type and delta, then allocate the minimum number of notes
     * needed for type's arity.  Usually, we won't need more, but if an offset
     * does take two bytes, js_SetSrcNoteOffset will grow CG_NOTES(cg).
     */
    SN_MAKE_NOTE(sn, type, delta);
    for (n = (intN)js_SrcNoteSpec[type].arity; n > 0; n--) {
        if (js_NewSrcNote(cx, cg, SRC_NULL) < 0)
            return -1;
    }
    return index;
}

intN
js_NewSrcNote2(JSContext *cx, JSCodeGenerator *cg, JSSrcNoteType type,
               ptrdiff_t offset)
{
    intN index;

    index = js_NewSrcNote(cx, cg, type);
    if (index >= 0) {
        if (!js_SetSrcNoteOffset(cx, cg, index, 0, offset))
            return -1;
    }
    return index;
}

intN
js_NewSrcNote3(JSContext *cx, JSCodeGenerator *cg, JSSrcNoteType type,
               ptrdiff_t offset1, ptrdiff_t offset2)
{
    intN index;

    index = js_NewSrcNote(cx, cg, type);
    if (index >= 0) {
        if (!js_SetSrcNoteOffset(cx, cg, index, 0, offset1))
            return -1;
        if (!js_SetSrcNoteOffset(cx, cg, index, 1, offset2))
            return -1;
    }
    return index;
}

static JSBool
GrowSrcNotes(JSContext *cx, JSCodeGenerator *cg)
{
    size_t newlength = CG_NOTE_LIMIT(cg) * 2;
    jssrcnote *newnotes = (jssrcnote *) cx->realloc_(CG_NOTES(cg), newlength);
    if (!newnotes) {
        js_ReportOutOfMemory(cx);
        return JS_FALSE;
    }
    CG_NOTES(cg) = newnotes;
    CG_NOTE_LIMIT(cg) = newlength;
    return JS_TRUE;
}

jssrcnote *
js_AddToSrcNoteDelta(JSContext *cx, JSCodeGenerator *cg, jssrcnote *sn,
                     ptrdiff_t delta)
{
    ptrdiff_t base, limit, newdelta, diff;
    intN index;

    /*
     * Called only from OptimizeSpanDeps and js_FinishTakingSrcNotes to add to
     * main script note deltas, and only by a small positive amount.
     */
    JS_ASSERT(cg->current == &cg->main);
    JS_ASSERT((unsigned) delta < (unsigned) SN_XDELTA_LIMIT);

    base = SN_DELTA(sn);
    limit = SN_IS_XDELTA(sn) ? SN_XDELTA_LIMIT : SN_DELTA_LIMIT;
    newdelta = base + delta;
    if (newdelta < limit) {
        SN_SET_DELTA(sn, newdelta);
    } else {
        index = sn - cg->main.notes;
        if (cg->main.noteCount == cg->main.noteLimit) {
            if (!GrowSrcNotes(cx, cg))
                return NULL;
            sn = cg->main.notes + index;
        }
        diff = cg->main.noteCount - index;
        cg->main.noteCount++;
        memmove(sn + 1, sn, SRCNOTE_SIZE(diff));
        SN_MAKE_XDELTA(sn, delta);
        sn++;
    }
    return sn;
}

JS_FRIEND_API(uintN)
js_SrcNoteLength(jssrcnote *sn)
{
    uintN arity;
    jssrcnote *base;

    arity = (intN)js_SrcNoteSpec[SN_TYPE(sn)].arity;
    for (base = sn++; arity; sn++, arity--) {
        if (*sn & SN_3BYTE_OFFSET_FLAG)
            sn += 2;
    }
    return sn - base;
}

JS_FRIEND_API(ptrdiff_t)
js_GetSrcNoteOffset(jssrcnote *sn, uintN which)
{
    /* Find the offset numbered which (i.e., skip exactly which offsets). */
    JS_ASSERT(SN_TYPE(sn) != SRC_XDELTA);
    JS_ASSERT((intN) which < js_SrcNoteSpec[SN_TYPE(sn)].arity);
    for (sn++; which; sn++, which--) {
        if (*sn & SN_3BYTE_OFFSET_FLAG)
            sn += 2;
    }
    if (*sn & SN_3BYTE_OFFSET_FLAG) {
        return (ptrdiff_t)(((uint32)(sn[0] & SN_3BYTE_OFFSET_MASK) << 16)
                           | (sn[1] << 8)
                           | sn[2]);
    }
    return (ptrdiff_t)*sn;
}

JSBool
js_SetSrcNoteOffset(JSContext *cx, JSCodeGenerator *cg, uintN index,
                    uintN which, ptrdiff_t offset)
{
    jssrcnote *sn;
    ptrdiff_t diff;

    if ((jsuword)offset >= (jsuword)((ptrdiff_t)SN_3BYTE_OFFSET_FLAG << 16)) {
        ReportStatementTooLarge(cx, cg);
        return JS_FALSE;
    }

    /* Find the offset numbered which (i.e., skip exactly which offsets). */
    sn = &CG_NOTES(cg)[index];
    JS_ASSERT(SN_TYPE(sn) != SRC_XDELTA);
    JS_ASSERT((intN) which < js_SrcNoteSpec[SN_TYPE(sn)].arity);
    for (sn++; which; sn++, which--) {
        if (*sn & SN_3BYTE_OFFSET_FLAG)
            sn += 2;
    }

    /* See if the new offset requires three bytes. */
    if (offset > (ptrdiff_t)SN_3BYTE_OFFSET_MASK) {
        /* Maybe this offset was already set to a three-byte value. */
        if (!(*sn & SN_3BYTE_OFFSET_FLAG)) {
            /* Losing, need to insert another two bytes for this offset. */
            index = sn - CG_NOTES(cg);

            /*
             * Test to see if the source note array must grow to accommodate
             * either the first or second byte of additional storage required
             * by this 3-byte offset.
             */
            if (CG_NOTE_COUNT(cg) + 1 >= CG_NOTE_LIMIT(cg)) {
                if (!GrowSrcNotes(cx, cg))
                    return JS_FALSE;
                sn = CG_NOTES(cg) + index;
            }
            CG_NOTE_COUNT(cg) += 2;

            diff = CG_NOTE_COUNT(cg) - (index + 3);
            JS_ASSERT(diff >= 0);
            if (diff > 0)
                memmove(sn + 3, sn + 1, SRCNOTE_SIZE(diff));
        }
        *sn++ = (jssrcnote)(SN_3BYTE_OFFSET_FLAG | (offset >> 16));
        *sn++ = (jssrcnote)(offset >> 8);
    }
    *sn = (jssrcnote)offset;
    return JS_TRUE;
}

#ifdef DEBUG_notme
#define DEBUG_srcnotesize
#endif

#ifdef DEBUG_srcnotesize
#define NBINS 10
static uint32 hist[NBINS];

void DumpSrcNoteSizeHist()
{
    static FILE *fp;
    int i, n;

    if (!fp) {
        fp = fopen("/tmp/srcnotes.hist", "w");
        if (!fp)
            return;
        setvbuf(fp, NULL, _IONBF, 0);
    }
    fprintf(fp, "SrcNote size histogram:\n");
    for (i = 0; i < NBINS; i++) {
        fprintf(fp, "%4u %4u ", JS_BIT(i), hist[i]);
        for (n = (int) JS_HOWMANY(hist[i], 10); n > 0; --n)
            fputc('*', fp);
        fputc('\n', fp);
    }
    fputc('\n', fp);
}
#endif

/*
 * Fill in the storage at notes with prolog and main srcnotes; the space at
 * notes was allocated using the CG_COUNT_FINAL_SRCNOTES macro from jsemit.h.
 * SO DON'T CHANGE THIS FUNCTION WITHOUT AT LEAST CHECKING WHETHER jsemit.h's
 * CG_COUNT_FINAL_SRCNOTES MACRO NEEDS CORRESPONDING CHANGES!
 */
JSBool
js_FinishTakingSrcNotes(JSContext *cx, JSCodeGenerator *cg, jssrcnote *notes)
{
    uintN prologCount, mainCount, totalCount;
    ptrdiff_t offset, delta;
    jssrcnote *sn;

    JS_ASSERT(cg->current == &cg->main);

    prologCount = cg->prolog.noteCount;
    if (prologCount && cg->prolog.currentLine != cg->firstLine) {
        CG_SWITCH_TO_PROLOG(cg);
        if (js_NewSrcNote2(cx, cg, SRC_SETLINE, (ptrdiff_t)cg->firstLine) < 0)
            return JS_FALSE;
        prologCount = cg->prolog.noteCount;
        CG_SWITCH_TO_MAIN(cg);
    } else {
        /*
         * Either no prolog srcnotes, or no line number change over prolog.
         * We don't need a SRC_SETLINE, but we may need to adjust the offset
         * of the first main note, by adding to its delta and possibly even
         * prepending SRC_XDELTA notes to it to account for prolog bytecodes
         * that came at and after the last annotated bytecode.
         */
        offset = CG_PROLOG_OFFSET(cg) - cg->prolog.lastNoteOffset;
        JS_ASSERT(offset >= 0);
        if (offset > 0 && cg->main.noteCount != 0) {
            /* NB: Use as much of the first main note's delta as we can. */
            sn = cg->main.notes;
            delta = SN_IS_XDELTA(sn)
                    ? SN_XDELTA_MASK - (*sn & SN_XDELTA_MASK)
                    : SN_DELTA_MASK - (*sn & SN_DELTA_MASK);
            if (offset < delta)
                delta = offset;
            for (;;) {
                if (!js_AddToSrcNoteDelta(cx, cg, sn, delta))
                    return JS_FALSE;
                offset -= delta;
                if (offset == 0)
                    break;
                delta = JS_MIN(offset, SN_XDELTA_MASK);
                sn = cg->main.notes;
            }
        }
    }

    mainCount = cg->main.noteCount;
    totalCount = prologCount + mainCount;
    if (prologCount)
        memcpy(notes, cg->prolog.notes, SRCNOTE_SIZE(prologCount));
    memcpy(notes + prologCount, cg->main.notes, SRCNOTE_SIZE(mainCount));
    SN_MAKE_TERMINATOR(&notes[totalCount]);

#ifdef DEBUG_notme
  { int bin = JS_CeilingLog2(totalCount);
    if (bin >= NBINS)
        bin = NBINS - 1;
    ++hist[bin];
  }
#endif
    return JS_TRUE;
}

static JSBool
NewTryNote(JSContext *cx, JSCodeGenerator *cg, JSTryNoteKind kind,
           uintN stackDepth, size_t start, size_t end)
{
    JSTryNode *tryNode;

    JS_ASSERT((uintN)(uint16)stackDepth == stackDepth);
    JS_ASSERT(start <= end);
    JS_ASSERT((size_t)(uint32)start == start);
    JS_ASSERT((size_t)(uint32)end == end);

    JS_ARENA_ALLOCATE_TYPE(tryNode, JSTryNode, &cx->tempPool);
    if (!tryNode) {
        js_ReportOutOfMemory(cx);
        return JS_FALSE;
    }

    tryNode->note.kind = kind;
    tryNode->note.stackDepth = (uint16)stackDepth;
    tryNode->note.start = (uint32)start;
    tryNode->note.length = (uint32)(end - start);
    tryNode->prev = cg->lastTryNode;
    cg->lastTryNode = tryNode;
    cg->ntrynotes++;
    return JS_TRUE;
}

void
js_FinishTakingTryNotes(JSCodeGenerator *cg, JSTryNoteArray *array)
{
    JSTryNode *tryNode;
    JSTryNote *tn;

    JS_ASSERT(array->length > 0 && array->length == cg->ntrynotes);
    tn = array->vector + array->length;
    tryNode = cg->lastTryNode;
    do {
        *--tn = tryNode->note;
    } while ((tryNode = tryNode->prev) != NULL);
    JS_ASSERT(tn == array->vector);
}

/*
 * Find the index of the given object for code generator.
 *
 * Since the emitter refers to each parsed object only once, for the index we
 * use the number of already indexes objects. We also add the object to a list
 * to convert the list to a fixed-size array when we complete code generation,
 * see JSCGObjectList::finish below.
 *
 * Most of the objects go to JSCodeGenerator.objectList but for regexp we use a
 * separated JSCodeGenerator.regexpList. In this way the emitted index can be
 * directly used to store and fetch a reference to a cloned RegExp object that
 * shares the same JSRegExp private data created for the object literal in
 * objbox. We need a cloned object to hold lastIndex and other direct properties
 * that should not be shared among threads sharing a precompiled function or
 * script.
 *
 * If the code being compiled is function code, allocate a reserved slot in
 * the cloned function object that shares its precompiled script with other
 * cloned function objects and with the compiler-created clone-parent. There
 * are nregexps = script->regexps()->length such reserved slots in each
 * function object cloned from fun->object. NB: during compilation, a funobj
 * slots element must never be allocated, because JSObject::allocSlot could
 * hand out one of the slots that should be given to a regexp clone.
 *
 * If the code being compiled is global code, the cloned regexp are stored in
 * fp->vars slot and to protect regexp slots from GC we set fp->nvars to
 * nregexps.
 *
 * The slots initially contain undefined or null. We populate them lazily when
 * JSOP_REGEXP is executed for the first time.
 *
 * Why clone regexp objects?  ECMA specifies that when a regular expression
 * literal is scanned, a RegExp object is created.  In the spec, compilation
 * and execution happen indivisibly, but in this implementation and many of
 * its embeddings, code is precompiled early and re-executed in multiple
 * threads, or using multiple global objects, or both, for efficiency.
 *
 * In such cases, naively following ECMA leads to wrongful sharing of RegExp
 * objects, which makes for collisions on the lastIndex property (especially
 * for global regexps) and on any ad-hoc properties.  Also, __proto__ refers to
 * the pre-compilation prototype, a pigeon-hole problem for instanceof tests.
 */
uintN
JSCGObjectList::index(JSObjectBox *objbox)
{
    JS_ASSERT(!objbox->emitLink);
    objbox->emitLink = lastbox;
    lastbox = objbox;
    objbox->index = length++;
    return objbox->index;
}

void
JSCGObjectList::finish(JSObjectArray *array)
{
    JSObject **cursor;
    JSObjectBox *objbox;

    JS_ASSERT(length <= INDEX_LIMIT);
    JS_ASSERT(length == array->length);

    cursor = array->vector + array->length;
    objbox = lastbox;
    do {
        --cursor;
        JS_ASSERT(!*cursor);
        *cursor = objbox->object;
    } while ((objbox = objbox->emitLink) != NULL);
    JS_ASSERT(cursor == array->vector);
}

void
JSGCConstList::finish(JSConstArray *array)
{
    JS_ASSERT(array->length == list.length());
    Value *src = list.begin(), *srcend = list.end();
    Value *dst = array->vector;
    for (; src != srcend; ++src, ++dst)
        *dst = *src;
}
