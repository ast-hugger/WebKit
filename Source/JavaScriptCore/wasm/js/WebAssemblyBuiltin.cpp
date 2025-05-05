/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

 #if ENABLE(WEBASSEMBLY)

#include "JITExceptions.h"
#include "JSString.h"
#include "JSWebAssemblyRuntimeError.h"
#include "WebAssemblyBuiltin.h"

#include "wtf/text/MakeString.h"

namespace JSC {

JSObject* WebAssemblyBuiltin::jsRepresentative(JSGlobalObject* globalObject) const
{
    return JSFunction::create(globalObject->vm(), globalObject, 0, m_name, m_reexportImplementation, ImplementationVisibility::Public, JSC::NoIntrinsic);
}

const WebAssemblyBuiltin* WebAssemblyBuiltinSet::findBuiltin(const String& name) const
{
    auto it = m_builtinsByName.find(name);
    return it != m_builtinsByName.end() ? it->value : nullptr;
}

void WebAssemblyBuiltinSet::add(WebAssemblyBuiltin&& builtin)
{
    m_builtins.append(WTFMove(builtin));
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN  // for memcpy()

void WebAssemblyBuiltinSet::finalizeCreation()
{
    // Should not be called repeatedly
    ASSERT(m_builtinsByName.isEmpty());
    ASSERT(m_nameSection->functionNames.isEmpty());

    size_t builtinCount = m_builtins.size();
    m_nameSection->functionNames.resize(builtinCount);
    for (size_t i = 0; i < builtinCount; i++) {
        auto& builtin = m_builtins[i];
        String builtinName = String(builtin.name());

        // Enter the builtin into the lookup table
        m_builtinsByName.add(builtinName, &builtin);

        // Create a simulated wasm name section entry for the builtin
        String fullName = makeString(String(m_qualifiedName), "."_s, builtinName);
        auto span = fullName.span8();
        Wasm::Name name;
        name.tryReserveCapacity(span.size());
        name.grow(span.size());
        memcpy(name.data(), span.data(), span.size());
        m_nameSection->functionNames[i] = WTFMove(name);
        builtin.m_wasmName = &m_nameSection->functionNames[i];
        builtin.m_nameSection = m_nameSection.ptr();
    }
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

const WebAssemblyBuiltinRegistry& WebAssemblyBuiltinRegistry::singleton()
{
    static NeverDestroyed<WebAssemblyBuiltinRegistry> registry;
    return registry.get();
}

WebAssemblyBuiltinRegistry::WebAssemblyBuiltinRegistry()
{
    m_builtinSets.append(WebAssemblyBuiltinSet::jsString());
}

// The registry is immutable once constructed so we don't worry about concurrency.
const WebAssemblyBuiltinSet* WebAssemblyBuiltinRegistry::findByQualifiedName(const String& name) const
{
    size_t index = m_builtinSets.findIf([&](auto& builtinSet) {
        return name == builtinSet.qualifiedName();
    });
    return index != notFound ? &m_builtinSets[index] : nullptr;
}

/*
        Builtin implementation macrology
*/

// A builtin host function is called by a wrapper (ipint_host_function_call_trampoline)
// so the relevant CallFrame is the call frame of the caller.
#define HOST_FUNCTION_PROLOGUE(vm, globalObject) \
    CallFrame* _thisFrame = std::bit_cast<CallFrame*>(__builtin_frame_address(0)); \
    CallFrame* _callFrame = _thisFrame->callerFrame(); \
    JSWebAssemblyInstance* _wasmInstance = _callFrame->wasmInstance(); \
    VM& vm = _wasmInstance->vm(); \
    JSGlobalObject* globalObject = _wasmInstance->globalObject();

#define IMPLEMENTATION_POINTER(function) reinterpret_cast<WebAssemblyBuiltin::ImplementationPtr>(reinterpret_cast<void*>(function))

// A host wasm function that returns an externref (EncodedJSValue)
#define DEFINE_WASM_BUILTIN_HOST_FUNCTION(name, ...) \
    static EncodedJSValue SYSV_ABI name(__VA_ARGS__) REFERENCED_FROM_ASM WTF_INTERNAL; \
    EncodedJSValue SYSV_ABI name(__VA_ARGS__)

// A host wasm function that returns an i32 (int32_t)
#define DEFINE_WASM_BUILTIN_HOST_FUNCTION_I32(name, ...) \
    static int32_t SYSV_ABI name(__VA_ARGS__) REFERENCED_FROM_ASM WTF_INTERNAL; \
    int32_t SYSV_ABI name(__VA_ARGS__)

#define JSVALUE_TO_I32(valueVar, i32Var) \
    int32_t i32Var; \
    if (valueVar.isInt32()) { \
        i32Var = valueVar.asInt32(); \
    } else { \
        double doubleArg1 = valueVar.toIntegerOrInfinity(globalObject); \
        RETURN_IF_EXCEPTION(scope,  { }); \
        i32Var = static_cast<int32_t>(doubleArg1); \
    }

// (result externref) (param externref externref)

#define DEFINE_BUILTIN_ENTRY_R_RR(name, impl) \
    DEFINE_WASM_BUILTIN_HOST_FUNCTION(name, EncodedJSValue arg0, EncodedJSValue arg1) \
    { \
        HOST_FUNCTION_PROLOGUE(vm, globalObject); \
        JSValue left = JSValue::decode(arg0); \
        JSValue right = JSValue::decode(arg1); \
        return JSValue::encode(impl(vm, globalObject, left, right)); \
    }

#define DEFINE_BUILTIN_JS_ENTRY_R_RR(name, impl) \
    static JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        JSValue left = callFrame->argument(0); \
        JSValue right = callFrame->argument(1); \
        return JSValue::encode(impl(globalObject->vm(), globalObject, left, right)); \
    }

// (result externref) (param externref externref)

#define DEFINE_BUILTIN_ENTRY_R_RII(name, impl) \
    DEFINE_WASM_BUILTIN_HOST_FUNCTION(name, EncodedJSValue arg0, int32_t arg1, int32_t arg2) \
    { \
        HOST_FUNCTION_PROLOGUE(vm, globalObject); \
        JSValue value0 = JSValue::decode(arg0); \
        return JSValue::encode(impl(vm, globalObject, value0, arg1, arg2)); \
    }

#define DEFINE_BUILTIN_JS_ENTRY_R_RII(name, impl) \
    static JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm()); \
        JSValue arg0 = callFrame->argument(0); \
        JSValue arg1 = callFrame->argument(1); \
        JSValue arg2 = callFrame->argument(2); \
        JSVALUE_TO_I32(arg1, intArg1); \
        JSVALUE_TO_I32(arg2, intArg2); \
        RELEASE_AND_RETURN(scope, JSValue::encode(impl(globalObject->vm(), globalObject, arg0, intArg1, intArg2))); \
    }

// (result externref) (param externref)

#define DEFINE_BUILTIN_ENTRY_R_R(name, impl) \
    DEFINE_WASM_BUILTIN_HOST_FUNCTION(name, EncodedJSValue arg) \
    { \
        HOST_FUNCTION_PROLOGUE(vm, globalObject); \
        JSValue value = JSValue::decode(arg); \
        JSValue result = impl(vm, globalObject, value); \
        return JSValue::encode(result); \
    }

#define DEFINE_BUILTIN_JS_ENTRY_R_R(name, impl) \
    static JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        JSValue value = callFrame->argument(0); \
        return JSValue::encode(impl(globalObject->vm(), globalObject, value)); \
    }

// (result i32) (param externref)

#define DEFINE_BUILTIN_ENTRY_I_R(name, impl) \
    DEFINE_WASM_BUILTIN_HOST_FUNCTION_I32(name, EncodedJSValue arg) \
    { \
        HOST_FUNCTION_PROLOGUE(vm, globalObject); \
        JSValue value = JSValue::decode(arg); \
        return impl(vm, globalObject, value); \
    }

#define DEFINE_BUILTIN_JS_ENTRY_I_R(name, impl) \
    static JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        JSValue value = callFrame->argument(0); \
        JSValue result = JSValue(impl(globalObject->vm(), globalObject, value)); \
        return JSValue::encode(result); \
    }

// (result externref) (param i32)

#define DEFINE_BUILTIN_ENTRY_R_I(name, impl) \
    DEFINE_WASM_BUILTIN_HOST_FUNCTION(name, int32_t arg) \
    { \
        HOST_FUNCTION_PROLOGUE(vm, globalObject); \
        return JSValue::encode(impl(vm, globalObject, arg)); \
    }

#define DEFINE_BUILTIN_JS_ENTRY_R_I(name, impl) \
    static JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm()); \
        JSValue arg = callFrame->argument(0); \
        JSVALUE_TO_I32(arg, intArg); \
        JSValue result = JSValue(impl(globalObject->vm(), globalObject, intArg)); \
        RELEASE_AND_RETURN(scope, JSValue::encode(result)); \
    }

// (result i32) (param externref i32)

#define DEFINE_BUILTIN_ENTRY_I_RI(name, impl) \
    DEFINE_WASM_BUILTIN_HOST_FUNCTION_I32(name, EncodedJSValue arg0, int32_t arg1) \
    { \
        HOST_FUNCTION_PROLOGUE(vm, globalObject); \
        JSValue value0 = JSValue::decode(arg0); \
        return impl(vm, globalObject, value0, arg1); \
    }

#define DEFINE_BUILTIN_JS_ENTRY_I_RI(name, impl) \
    static JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm()); \
        JSValue arg0 = callFrame->argument(0); \
        JSValue arg1 = callFrame->argument(1); \
        JSVALUE_TO_I32(arg1, intArg1); \
        int32_t result = impl(globalObject->vm(), globalObject, arg0, intArg1); \
        RELEASE_AND_RETURN(scope, JSValue::encode(JSValue(result))); \
    }

// (result i32) (param externref externref)

#define DEFINE_BUILTIN_ENTRY_I_RR(name, impl) \
    DEFINE_WASM_BUILTIN_HOST_FUNCTION_I32(name, EncodedJSValue arg0, EncodedJSValue arg1) \
    { \
        HOST_FUNCTION_PROLOGUE(vm, globalObject); \
        JSValue value0 = JSValue::decode(arg0); \
        JSValue value1 = JSValue::decode(arg1); \
        return impl(vm, globalObject, value0, value1); \
    }

#define DEFINE_BUILTIN_JS_ENTRY_I_RR(name, impl) \
    static JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        JSValue arg0 = callFrame->argument(0); \
        JSValue arg1 = callFrame->argument(1); \
        int32_t result = impl(globalObject->vm(), globalObject, arg0, arg1); \
        return JSValue::encode(JSValue(result)); \
    }

/*
        wasm:js-string builtin set

        Builtins in this set resemble the standard JS functions defined in String.prototype and
        String constructor object (StringPrototype.cpp and StringConstructor.cpp). However,
        differences in the exact semantics make it difficult to directly share implementations. For
        example, String.prototype.charCodeAt returns NaN if the position is out of bounds, while
        wasm:js-string.charCodeAt throws an exception. In JavaScript the argument can be anything,
        but in wasm it's an `int32`. That's on top of the differences in calling conventions: most
        JS string operations operate on 'this' while there is no such special argument in wasm
        functions.

        For this reason, builtin implementations here are independent from StringPrototype and
        StringConstructor. That also applies to JS representatives of builtin functions (JS
        functions exported when a builtin is re-expored). Again, that's because even when
        re-exported as a JS function, a Wasm builtin receives arguments in a different way and in
        general behaves slightly differently.
*/


/**
 * See: https://webassembly.github.io/js-string-builtins/js-api/#js-string-cast
 *
 * Summary: if the arg is a string, return it, otherwise throw a RuntimeError.
 */
static ALWAYS_INLINE JSValue doCast(VM& vm, JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!value.isString()) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "the value is not a string"_s);
        return throwException(globalObject, scope, error);
    }

    RELEASE_AND_RETURN(scope, value);
}

DEFINE_BUILTIN_ENTRY_R_R(wasmJsStringCast, doCast)
DEFINE_BUILTIN_JS_ENTRY_R_R(wasmJsStringCastJS, doCast)

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-test
 *
 * Summary: return 1 if the arg is a string, 0 otherwise.
 */
DEFINE_WASM_BUILTIN_HOST_FUNCTION_I32(wasmJsStringTest, EncodedJSValue arg)
{
    JSValue value = JSValue::decode(arg);
    return value.isString() ? 1 : 0;
}

JSC_DECLARE_HOST_FUNCTION(wasmJsStringTestJS);
JSC_DEFINE_HOST_FUNCTION(wasmJsStringTestJS, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    UNUSED_PARAM(globalObject);
    JSValue value = callFrame->argument(0);
    JSValue result = value.isString() ? JSValue(1) : JSValue(0);
    return JSValue::encode(result);
}

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-fromCharCode
 */

static ALWAYS_INLINE JSValue doFromCharCode(VM& vm, JSGlobalObject* globalObject, int32_t arg)
{
    UNUSED_PARAM(globalObject);
    UChar code = static_cast<UChar>(arg);
    return jsSingleCharacterString(vm, code);
}

DEFINE_BUILTIN_ENTRY_R_I(wasmJsStringFromCharCode, doFromCharCode);
DEFINE_BUILTIN_JS_ENTRY_R_I(wasmJsStringFromCharCodeJS, doFromCharCode);

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-fromCodePoint
 */

static ALWAYS_INLINE JSValue doFromCodePoint(VM& vm, JSGlobalObject* globalObject, int32_t arg)
{
    UNUSED_PARAM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    uint32_t codePoint = static_cast<uint32_t>(arg);
    if (codePoint > 0x10ffff) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "code point out of range"_s);
        return throwException(globalObject, scope, error);
    }

    StringBuilder builder;
    if (U_IS_BMP(codePoint)) {
        builder.append(static_cast<UChar>(codePoint));
    } else {
        builder.append(U16_LEAD(codePoint));
        builder.append(U16_TRAIL(codePoint));
    }
    RELEASE_AND_RETURN(scope, jsString(vm, builder.toString()));
}

DEFINE_BUILTIN_ENTRY_R_I(wasmJsStringFromCodePoint, doFromCodePoint);
DEFINE_BUILTIN_JS_ENTRY_R_I(wasmJsStringFromCodePointJS, doFromCodePoint);

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-charCodeAt
 *
 * The proposal states that if the index is greater than or equal to the string length, an exception
 * is thrown. However, it does not say what happens if the index is negative. Step 4 is not
 * applicable because it states "Return CharCodeAt(string, index)", and CharCodeAt behaves as
 * String.prototype.charCodeAt, which would return NaN given a negative index. I'm assuming the
 * intended but underspecified behavior here is also to throw if the index is negative.
 */

static ALWAYS_INLINE int32_t doCharCodeAt(VM& vm, JSGlobalObject* globalObject, JSValue arg, int32_t index)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!arg.isString()) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "charCodeAt() first argument is not a string"_s);
        throwException(globalObject, scope, error);
        return 0; // actual returned value doesn't matter
    }

    auto* string = arg.toString(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    if (index < 0 || static_cast<int32_t>(view->length()) <= index) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "charCodeAt() index is out of bounds"_s);
        throwException(globalObject, scope, error);
        return 0; // actual returned value doesn't matter
    }

    UChar result = view[index];
    RELEASE_AND_RETURN(scope, result);
}

DEFINE_BUILTIN_ENTRY_I_RI(wasmJsStringCharCodeAt, doCharCodeAt);
DEFINE_BUILTIN_JS_ENTRY_I_RI(wasmJsStringCharCodeAtJS, doCharCodeAt);

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-codePointAt
 *
 * See the note on index validation in charCodeAt.
 */

static ALWAYS_INLINE int32_t doCodePointAt(VM& vm, JSGlobalObject* globalObject, JSValue arg, int32_t index)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!arg.isString()) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "charCodeAt() first argument is not a string"_s);
        throwException(globalObject, scope, error);
        return 0; // actual returned value doesn't matter
    }

    auto* string = arg.toString(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    unsigned length = view->length();
    if (index < 0 || static_cast<int32_t>(length) <= index) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "charCodeAt() index is out of bounds"_s);
        throwException(globalObject, scope, error);
        return 0; // actual returned value doesn't matter
    }

    int32_t result;
    if (view->is8Bit()) {
        result = view->span8()[index];
    } else {
        char32_t character;
        auto characters = view->span16();
        unsigned i = static_cast<unsigned>(index);
        U16_NEXT(characters, i, length, character);
        result = static_cast<int32_t>(character);
    }
    RELEASE_AND_RETURN(scope, result);
}

DEFINE_BUILTIN_ENTRY_I_RI(wasmJsStringCodePointAt, doCodePointAt);
DEFINE_BUILTIN_JS_ENTRY_I_RI(wasmJsStringCodePointAtJS, doCodePointAt);

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-length
 *
 * Summary: if the value is a string, return its length.
 * Otherwise, throw a RuntimeError.
 */

static ALWAYS_INLINE int32_t doLength(VM& vm, JSGlobalObject* globalObject, JSValue arg)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!arg.isString()) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "invalid length() argument"_s);
        throwException(globalObject, scope, error);
        return 0; // actual returned value doesn't matter
    }

    unsigned length = asString(arg)->length();
    if (length > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        length = std::numeric_limits<int32_t>::max();
    }
    RELEASE_AND_RETURN(scope, length);
}

DEFINE_BUILTIN_ENTRY_I_R(wasmJsStringLength, doLength);
DEFINE_BUILTIN_JS_ENTRY_I_R(wasmJsStringLengthJS, doLength);

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-concat
 *
 * Summary: if both values are strings, return the result of concatenating them.
 * Otherwise, throw a RuntimeError.
 */

static ALWAYS_INLINE JSValue doConcat(VM& vm, JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!left.isString() || !right.isString()) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "invalid concat() arguments: both must be strings"_s);
        return throwException(globalObject, scope, error);
    }

    JSString* result = jsString(globalObject, asString(left), asString(right)); // creates a rope string
    RELEASE_AND_RETURN(scope, result);
}

DEFINE_BUILTIN_ENTRY_R_RR(wasmJsStringConcat, doConcat)
DEFINE_BUILTIN_JS_ENTRY_R_RR(wasmJsStringConcatJS, doConcat)

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-substring
 *
 * The spec does not specify the interpretation of negative indices, and redundantly
 * specifies the behavior for 'start > end' and 'start > length', which already
 * follows from the spec for String.prototype.substring. What's implemented here
 * is what String.prototype.substring does.
 */

static ALWAYS_INLINE JSValue doSubstring(VM& vm, JSGlobalObject* globalObject, JSValue arg, int32_t start, int32_t end)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!arg.isString()) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "substring() first argument must be a string"_s);
        return throwException(globalObject, scope, error);
    }

    JSString* string = asString(arg);
    unsigned length = string->length();
    unsigned substringStart = start >= 0 ? static_cast<unsigned>(start) : 0;
    unsigned substringEnd = end >= 0 ? static_cast<unsigned>(end) : 0;
    if (substringStart >= length)
        substringStart = length;
    if (substringEnd > length)
        substringEnd = length;
    if (substringStart > substringEnd)
        substringStart = substringEnd;
    unsigned substringLength = substringEnd - substringStart;
    RELEASE_AND_RETURN(scope, jsSubstring(globalObject, string, substringStart, substringLength));
}

DEFINE_BUILTIN_ENTRY_R_RII(wasmJsStringSubstring, doSubstring)
DEFINE_BUILTIN_JS_ENTRY_R_RII(wasmJsStringSubstringJS, doSubstring)

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-equals
 *
 * Summary: each argument must be a string or a null, or an exception is thrown.
 * If this initial type check passes, comparison behaves as tc39's IsStrictlyEqual,
 * see https://tc39.es/ecma262/#sec-isstrictlyequal
 */

static ALWAYS_INLINE int32_t doEquals(VM& vm, JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    bool leftIsString = left.isString();
    bool leftIsNull = left.isNull();
    bool rightIsString = right.isString();
    bool rightIsNull = right.isNull();
    if (!(leftIsString || leftIsNull) || !(rightIsString || rightIsNull)) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "invalid equals() arguments"_s);
        throwException(globalObject, scope, error);
        return 0; // actual returned value doesn't matter
    }
    if (!((leftIsString && rightIsString) || (leftIsNull && rightIsNull))) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "invalid equals() arguments"_s);
        throwException(globalObject, scope, error);
        return 0; // actual returned value doesn't matter
    }

    int32_t result;
    if (leftIsNull) {
        result = true;
    } else {
        JSString* leftString = asString(left);
        JSString* rightString = asString(right);
        result = leftString->equal(globalObject, rightString);
    }
    RELEASE_AND_RETURN(scope, result);
}

DEFINE_BUILTIN_ENTRY_I_RR(wasmJsStringEquals, doEquals)
DEFINE_BUILTIN_JS_ENTRY_I_RR(wasmJsStringEqualsJS, doEquals)

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-compare
 *
 * Unlike 'equals', the spec says the args may only be strings--no nulls.
 */

 static ALWAYS_INLINE int32_t doCompare(VM& vm, JSGlobalObject* globalObject, JSValue left, JSValue right)
 {
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!left.isString() || !right.isString()) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "invalid concat() arguments: both must be strings"_s);
        throwException(globalObject, scope, error);
        return 0;
    }

    JSString* leftString = left.toStringOrNull(globalObject);
    JSString* rightString = right.toStringOrNull(globalObject);
    auto leftView = leftString->view(globalObject);
    auto rightView = rightString->view(globalObject);
    int32_t result = codePointCompare(StringView(leftView), StringView(rightView));
    RELEASE_AND_RETURN(scope, result);
 }

 DEFINE_BUILTIN_ENTRY_I_RR(wasmJsStringCompare, doCompare)
 DEFINE_BUILTIN_JS_ENTRY_I_RR(wasmJsStringCompareJS, doCompare)

WebAssemblyBuiltinSet WebAssemblyBuiltinSet::jsString()
{
    WebAssemblyBuiltinSet builtinSet = WebAssemblyBuiltinSet("wasm:js-string");
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("cast"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::externrefType() },
            { Wasm::externrefType() }),
        IMPLEMENTATION_POINTER(wasmJsStringCast),
        wasmJsStringCastJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("test"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::Types::I32 },
            { Wasm::externrefType() }),
        IMPLEMENTATION_POINTER(wasmJsStringTest),
        wasmJsStringTestJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("fromCharCode"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::externrefType() },
            { Wasm::Types::I32 }),
        IMPLEMENTATION_POINTER(wasmJsStringFromCharCode),
        wasmJsStringFromCharCodeJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("fromCodePoint"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::externrefType() },
            { Wasm::Types::I32 }),
        IMPLEMENTATION_POINTER(wasmJsStringFromCodePoint),
        wasmJsStringFromCodePointJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("concat"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::externrefType() },
            { Wasm::externrefType(), Wasm::externrefType() }),
        IMPLEMENTATION_POINTER(wasmJsStringConcat),
        wasmJsStringConcatJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("substring"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::externrefType() },
            { Wasm::externrefType(), Wasm::Types::I32, Wasm::Types::I32 }),
        IMPLEMENTATION_POINTER(wasmJsStringSubstring),
        wasmJsStringSubstringJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("charCodeAt"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::Types::I32 },
            { Wasm::externrefType(), Wasm::Types::I32 }),
        IMPLEMENTATION_POINTER(wasmJsStringCharCodeAt),
        wasmJsStringCharCodeAtJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("codePointAt"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::Types::I32 },
            { Wasm::externrefType(), Wasm::Types::I32 }),
        IMPLEMENTATION_POINTER(wasmJsStringCodePointAt),
        wasmJsStringCodePointAtJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("length"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::Types::I32 },
            { Wasm::externrefType() }),
        IMPLEMENTATION_POINTER(wasmJsStringLength),
        wasmJsStringLengthJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("equals"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::Types::I32 },
            { Wasm::externrefType(), Wasm::externrefType() }),
        IMPLEMENTATION_POINTER(wasmJsStringEquals),
        wasmJsStringEqualsJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("compare"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::Types::I32 },
            { Wasm::externrefType(), Wasm::externrefType() }),
        IMPLEMENTATION_POINTER(wasmJsStringCompare),
        wasmJsStringCompareJS
    ));
    builtinSet.finalizeCreation();
    return builtinSet;
}

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
