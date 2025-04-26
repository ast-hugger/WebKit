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
#define FETCH_WASM_INSTANCE(callFrame) std::bit_cast<JSWebAssemblyInstance*>((callFrame)->codeBlock())
// FIXME(vb): use _callFrame->wasmInstance() in the macro below, but it's only possible when we have a proper callee which 'isNativeCallee()'
#define BUILTIN_PROLOGUE(_vm, _globalObject) \
    CallFrame* _callFrame = GET_CALL_FRAME(); \
    JSWebAssemblyInstance* _wasmInstance = FETCH_WASM_INSTANCE(_callFrame); \
    VM& _vm = _wasmInstance->vm(); \
    JSGlobalObject* _globalObject = _wasmInstance->globalObject();

#define IMPLEMENTATION_POINTER(function) reinterpret_cast<WebAssemblyBuiltin::ImplementationPtr>(reinterpret_cast<void*>(function))

#define DEFINE_WASM_BUILTIN(name, ...) \
    extern "C" EncodedJSValue SYSV_ABI name(__VA_ARGS__) REFERENCED_FROM_ASM WTF_INTERNAL; \
    extern "C" EncodedJSValue SYSV_ABI name(__VA_ARGS__)

namespace JSC {

const WebAssemblyBuiltin* WebAssemblyBuiltinSet::findBuiltin(const String& name) const
{
    for (auto& builtin : m_builtins) {
        if (name == builtin.name())
            return &builtin;
    }
    return nullptr;
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
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-concat
 *
 * Summary: if both values are strings, return the result of concatenating them.
 * Otherwise, throw a RuntimeError.
 */
DEFINE_WASM_BUILTIN(jsString_concat, EncodedJSValue arg0, EncodedJSValue arg1)
{
    BUILTIN_PROLOGUE(vm, globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue left = JSValue::decode(arg0);
    JSValue right = JSValue::decode(arg1);
    if (!left.isString() || !right.isString()) {
        JSObject* error = createJSWebAssemblyRuntimeError(globalObject, vm, "invalid concat() arguments: both must be strings"_s);
        return throwVMError(globalObject, scope, error);
    }

    JSString* result = jsString(globalObject, asString(left), asString(right)); // creates a rope string
    RELEASE_AND_RETURN(scope, JSValue::encode(result));
}

class WebAssemblyBuiltinSetJSString final : public WebAssemblyBuiltinSet {
    friend class WebAssemblyBuiltinRegistry;

    WebAssemblyBuiltinSetJSString() : WebAssemblyBuiltinSet(ASCIILiteral("wasm:js-string"))
    {
        m_builtins.append(WebAssemblyBuiltin(
            ASCIILiteral("hello"),
            Wasm::TypeInformation::typeDefinitionForFunction(
                { Wasm::externrefType() },
                { }),
            IMPLEMENTATION_POINTER(jsString_hello)
        ));
        m_builtins.append(WebAssemblyBuiltin(
            ASCIILiteral("concat"),
            Wasm::TypeInformation::typeDefinitionForFunction(
                { Wasm::externrefType() },
                { Wasm::externrefType(), Wasm::externrefType() }),
            IMPLEMENTATION_POINTER(jsString_concat)
        ));
    }
};

/*
        Registry
*/

static LazyNeverDestroyed<WebAssemblyBuiltinRegistry> registry;

WebAssemblyBuiltinRegistry& WebAssemblyBuiltinRegistry::singleton()
{
    return registry.get();
}

void WebAssemblyBuiltinRegistry::initialize()
{
    registry.construct();
}

WebAssemblyBuiltinRegistry::WebAssemblyBuiltinRegistry()
{
    m_builtinSets.append(WebAssemblyBuiltinSetJSString());
}

const WebAssemblyBuiltinSet* WebAssemblyBuiltinRegistry::findByQualifiedName(const String& name)
{
    Locker locker { m_lock };
    for (auto& builtinSet : m_builtinSets) {
        if (name == builtinSet.qualifiedName())
            return &builtinSet;
    }
    return nullptr;
}

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
