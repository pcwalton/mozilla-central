/* -*- Mode: c++; c-basic-offset: 4; tab-width: 40; indent-tabs-mode: nil -*- */
/* vim: set ts=40 sw=4 et tw=99: */
/* ***** BEGIN LICENSE BLOCK *****
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
 * The Original Code is Mozilla WebGL impl
 *
 * The Initial Developer of the Original Code is
 *   Mozilla Foundation
 * Portions created by the Initial Developer are Copyright (C) 2009
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Vladimir Vukicevic <vladimir@pobox.com>
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

#ifndef jstypedarray_h
#define jstypedarray_h

#include "jsapi.h"
#include "jsvalue.h"

typedef struct JSProperty JSProperty;

namespace js {

/*
 * ArrayBuffer
 *
 * This class holds the underlying raw buffer that the TypedArray
 * subclasses access.  It can be created explicitly and passed to a
 * TypedArray subclass, or can be created implicitly by constructing a
 * TypedArray with a size.
 */
struct JS_FRIEND_API(ArrayBuffer) {
    static Class slowClass;
    static JSPropertySpec jsprops[];

    static JSBool prop_getByteLength(JSContext *cx, JSObject *obj, jsid id, Value *vp);

    static JSBool class_constructor(JSContext *cx, uintN argc, Value *vp);

    static JSObject *create(JSContext *cx, int32 nbytes);

    ArrayBuffer()
    {
    }

    ~ArrayBuffer();

    static void
    obj_trace(JSTracer *trc, JSObject *obj);

    static JSBool
    obj_lookupProperty(JSContext *cx, JSObject *obj, jsid id,
                       JSObject **objp, JSProperty **propp);

    static JSBool
    obj_lookupElement(JSContext *cx, JSObject *obj, uint32 index,
                      JSObject **objp, JSProperty **propp);

    static JSBool
    obj_defineProperty(JSContext *cx, JSObject *obj, jsid id, const Value *v,
                       PropertyOp getter, StrictPropertyOp setter, uintN attrs);

    static JSBool
    obj_defineElement(JSContext *cx, JSObject *obj, uint32 index, const Value *v,
                      PropertyOp getter, StrictPropertyOp setter, uintN attrs);

    static JSBool
    obj_getProperty(JSContext *cx, JSObject *obj, JSObject *receiver, jsid id, Value *vp);

    static JSBool
    obj_getElement(JSContext *cx, JSObject *obj, JSObject *receiver, uint32 index, Value *vp);

    static JSBool
    obj_setProperty(JSContext *cx, JSObject *obj, jsid id, Value *vp, JSBool strict);

    static JSBool
    obj_setElement(JSContext *cx, JSObject *obj, uint32 index, Value *vp, JSBool strict);

    static JSBool
    obj_getAttributes(JSContext *cx, JSObject *obj, jsid id, uintN *attrsp);

    static JSBool
    obj_getElementAttributes(JSContext *cx, JSObject *obj, uint32 index, uintN *attrsp);

    static JSBool
    obj_setAttributes(JSContext *cx, JSObject *obj, jsid id, uintN *attrsp);

    static JSBool
    obj_setElementAttributes(JSContext *cx, JSObject *obj, uint32 index, uintN *attrsp);

    static JSBool
    obj_deleteProperty(JSContext *cx, JSObject *obj, jsid id, Value *rval, JSBool strict);

    static JSBool
    obj_deleteElement(JSContext *cx, JSObject *obj, uint32 index, Value *rval, JSBool strict);

    static JSBool
    obj_enumerate(JSContext *cx, JSObject *obj, JSIterateOp enum_op,
                  Value *statep, jsid *idp);

    static JSType
    obj_typeOf(JSContext *cx, JSObject *obj);

    static JSObject *
    getArrayBuffer(JSObject *obj);
};

/*
 * TypedArray
 *
 * The non-templated base class for the specific typed implementations.
 * This class holds all the member variables that are used by
 * the subclasses.
 */

struct JS_FRIEND_API(TypedArray) {
    enum {
        TYPE_INT8 = 0,
        TYPE_UINT8,
        TYPE_INT16,
        TYPE_UINT16,
        TYPE_INT32,
        TYPE_UINT32,
        TYPE_FLOAT32,
        TYPE_FLOAT64,

        /*
         * Special type that's a uint8, but assignments are clamped to 0 .. 255.
         * Treat the raw data type as a uint8.
         */
        TYPE_UINT8_CLAMPED,

        TYPE_MAX
    };

    enum {
        /* Properties of the typed array stored in reserved slots. */
        FIELD_LENGTH = 0,
        FIELD_BYTEOFFSET,
        FIELD_BYTELENGTH,
        FIELD_TYPE,
        FIELD_BUFFER,
        FIELD_MAX
    };

    // and MUST NOT be used to construct new objects.
    static Class fastClasses[TYPE_MAX];

    // These are the slow/original classes, used
    // fo constructing new objects
    static Class slowClasses[TYPE_MAX];

    static JSPropertySpec jsprops[];

    static JSObject *getTypedArray(JSObject *obj);

    static JSBool prop_getBuffer(JSContext *cx, JSObject *obj, jsid id, Value *vp);
    static JSBool prop_getByteOffset(JSContext *cx, JSObject *obj, jsid id, Value *vp);
    static JSBool prop_getByteLength(JSContext *cx, JSObject *obj, jsid id, Value *vp);
    static JSBool prop_getLength(JSContext *cx, JSObject *obj, jsid id, Value *vp);

    static JSBool obj_lookupProperty(JSContext *cx, JSObject *obj, jsid id,
                                     JSObject **objp, JSProperty **propp);
    static JSBool obj_lookupElement(JSContext *cx, JSObject *obj, uint32 index,
                                    JSObject **objp, JSProperty **propp);

    static JSBool obj_getAttributes(JSContext *cx, JSObject *obj, jsid id, uintN *attrsp);
    static JSBool obj_getElementAttributes(JSContext *cx, JSObject *obj, uint32 index, uintN *attrsp);

    static JSBool obj_setAttributes(JSContext *cx, JSObject *obj, jsid id, uintN *attrsp);
    static JSBool obj_setElementAttributes(JSContext *cx, JSObject *obj, uint32 index, uintN *attrsp);

    static JSUint32 getLength(JSObject *obj);
    static JSUint32 getByteOffset(JSObject *obj);
    static JSUint32 getByteLength(JSObject *obj);
    static JSUint32 getType(JSObject *obj);
    static JSObject * getBuffer(JSObject *obj);
    static void * getDataOffset(JSObject *obj);

  public:
    static bool
    isArrayIndex(JSContext *cx, JSObject *obj, jsid id, jsuint *ip = NULL);

    static inline uint32 slotWidth(int atype) {
        switch (atype) {
          case js::TypedArray::TYPE_INT8:
          case js::TypedArray::TYPE_UINT8:
          case js::TypedArray::TYPE_UINT8_CLAMPED:
            return 1;
          case js::TypedArray::TYPE_INT16:
          case js::TypedArray::TYPE_UINT16:
            return 2;
          case js::TypedArray::TYPE_INT32:
          case js::TypedArray::TYPE_UINT32:
          case js::TypedArray::TYPE_FLOAT32:
            return 4;
          case js::TypedArray::TYPE_FLOAT64:
            return 8;
          default:
            JS_NOT_REACHED("invalid typed array type");
            return 0;
        }
    }

    static inline int slotWidth(JSObject *obj) {
        return slotWidth(getType(obj));
    }

    static int lengthOffset();
    static int dataOffset();
};

extern bool
IsFastTypedArrayClass(const Class *clasp);

} // namespace js

/* Friend API methods */

JS_FRIEND_API(JSObject *)
js_InitTypedArrayClasses(JSContext *cx, JSObject *obj);

JS_FRIEND_API(JSBool)
js_IsTypedArray(JSObject *obj);

JS_FRIEND_API(JSBool)
js_IsArrayBuffer(JSObject *obj);

JS_FRIEND_API(JSObject *)
js_CreateArrayBuffer(JSContext *cx, jsuint nbytes);

/*
 * Create a new typed array of type atype (one of the TypedArray
 * enumerant values above), with nelements elements.
 */
JS_FRIEND_API(JSObject *)
js_CreateTypedArray(JSContext *cx, jsint atype, jsuint nelements);

/*
 * Create a new typed array of type atype (one of the TypedArray
 * enumerant values above), and copy in values from the given JSObject,
 * which must either be a typed array or an array-like object.
 */
JS_FRIEND_API(JSObject *)
js_CreateTypedArrayWithArray(JSContext *cx, jsint atype, JSObject *arrayArg);

/*
 * Create a new typed array of type atype (one of the TypedArray
 * enumerant values above), using a given ArrayBuffer for storage.
 * The byteoffset and length values are optional; if -1 is passed, an
 * offset of 0 and enough elements to use up the remainder of the byte
 * array are used as the default values.
 */
JS_FRIEND_API(JSObject *)
js_CreateTypedArrayWithBuffer(JSContext *cx, jsint atype, JSObject *bufArg,
                              jsint byteoffset, jsint length);

extern int32 JS_FASTCALL
js_TypedArray_uint8_clamp_double(const double x);

JS_FRIEND_API(JSUint32)
JS_GetArrayBufferByteLength(JSObject *obj);

JS_FRIEND_API(uint8 *)
JS_GetArrayBufferData(JSObject *obj);

JS_FRIEND_API(JSUint32)
JS_GetTypedArrayLength(JSObject *obj);

JS_FRIEND_API(JSUint32)
JS_GetTypedArrayByteOffset(JSObject *obj);

JS_FRIEND_API(JSUint32)
JS_GetTypedArrayByteLength(JSObject *obj);

JS_FRIEND_API(JSUint32)
JS_GetTypedArrayType(JSObject *obj);

JS_FRIEND_API(JSObject *)
JS_GetTypedArrayBuffer(JSObject *obj);

JS_FRIEND_API(void *)
JS_GetTypedArrayData(JSObject *obj);

#endif /* jstypedarray_h */
