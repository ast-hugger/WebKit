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

#include "JSWebAssemblyRuntimeError.h"
#include "WebAssemblyBuiltin.h"

#include "wtf/text/MakeString.h"

#define GET_CALL_FRAME() std::bit_cast<CallFrame*>(__builtin_frame_address(0))
#define BUILTIN_PROLOGUE(vm, globalObject) \
    CallFrame* _callFrame = GET_CALL_FRAME(); \
    JSWebAssemblyInstance* _wasmInstance = _callFrame->wasmInstance(); \
    VM& vm = _wasmInstance->vm(); \
    JSGlobalObject* globalObject = _wasmInstance->globalObject();

#define IMPLEMENTATION_POINTER(function) reinterpret_cast<WebAssemblyBuiltin::ImplementationPtr>(reinterpret_cast<void*>(function))

/// Define a Wasm builtin returning an externref.
#define DEFINE_WASM_BUILTIN(name, ...) \
    static EncodedJSValue SYSV_ABI name(__VA_ARGS__) REFERENCED_FROM_ASM WTF_INTERNAL; \
    EncodedJSValue SYSV_ABI name(__VA_ARGS__)

/// Define a Wasm builtin returning an int32.
#define DEFINE_WASM_BUILTIN_I32(name, ...) \
    static int32_t SYSV_ABI name(__VA_ARGS__) REFERENCED_FROM_ASM WTF_INTERNAL; \
    int32_t SYSV_ABI name(__VA_ARGS__)

#define DEFINE_BUILTIN_ENTRY_R_RR(name, impl) \
    DEFINE_WASM_BUILTIN(name, EncodedJSValue arg0, EncodedJSValue arg1) \
    { \
        BUILTIN_PROLOGUE(vm, globalObject); \
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

#define DEFINE_BUILTIN_ENTRY_R_R(name, impl) \
    DEFINE_WASM_BUILTIN(name, EncodedJSValue arg) \
    { \
        BUILTIN_PROLOGUE(vm, globalObject); \
        JSValue value = JSValue::decode(arg); \
        return JSValue::encode(impl(vm, globalObject, value)); \
    }

#define DEFINE_BUILTIN_JS_ENTRY_R_R(name, impl) \
    static JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        JSValue value = callFrame->argument(0); \
        return JSValue::encode(impl(globalObject->vm(), globalObject, value)); \
    }


namespace JSC {

JSObject* WebAssemblyBuiltin::reExportRepresentative(JSGlobalObject* globalObject) const
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
    auto* added = &m_builtins.last();
    String name = String(added->name());
    m_builtinsByName.add(name, added);
}

/*
        wasm:js-string builtin set
*/

/**
 * A scratch builtin for trying things out, not part of the spec.
 */
DEFINE_WASM_BUILTIN(jsString_hello)
{
    BUILTIN_PROLOGUE(vm, globalObject);
    UNUSED_PARAM(globalObject);

    return JSValue::encode(jsString(vm, String::fromLatin1("Hello from the 'wasm:js-string:hello' builtin!")));
}

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

DEFINE_BUILTIN_ENTRY_R_R(jsString_cast, doCast)
DEFINE_BUILTIN_JS_ENTRY_R_R(jsString_castJS, doCast)

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-test
 *
 * Summary: return 1 if the arg is a string, 0 otherwise.
 */
DEFINE_WASM_BUILTIN_I32(jsString_test, EncodedJSValue arg)
{
    JSValue value = JSValue::decode(arg);
    return value.isString() ? 1 : 0;
}

JSC_DECLARE_HOST_FUNCTION(jsString_testJS);
JSC_DEFINE_HOST_FUNCTION(jsString_testJS, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    UNUSED_PARAM(globalObject);
    JSValue value = callFrame->argument(0);
    JSValue result = value.isString() ? JSValue(1) : JSValue(0);
    return JSValue::encode(result);
}

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

DEFINE_BUILTIN_ENTRY_R_RR(jsString_concat, doConcat)
DEFINE_BUILTIN_JS_ENTRY_R_RR(jsString_concatJS, doConcat)

WebAssemblyBuiltinSet WebAssemblyBuiltinSet::jsString()
{
    WebAssemblyBuiltinSet builtinSet = WebAssemblyBuiltinSet("wasm:js-string");
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("hello"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::externrefType() },
            { }),
        IMPLEMENTATION_POINTER(jsString_hello),
        nullptr
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("cast"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::externrefType() },
            { Wasm::externrefType() }),
        IMPLEMENTATION_POINTER(jsString_cast),
        jsString_castJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("test"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::Types::I32 },
            { Wasm::externrefType() }),
        IMPLEMENTATION_POINTER(jsString_test),
        jsString_testJS
    ));
    builtinSet.add(WebAssemblyBuiltin(
        ASCIILiteral("concat"),
        Wasm::TypeInformation::typeDefinitionForFunction(
            { Wasm::externrefType() },
            { Wasm::externrefType(), Wasm::externrefType() }),
        IMPLEMENTATION_POINTER(jsString_concat),
        jsString_concatJS
    ));
    return builtinSet;
}

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

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
