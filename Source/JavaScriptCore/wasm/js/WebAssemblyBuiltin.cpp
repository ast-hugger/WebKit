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

#include "WebAssemblyBuiltin.h"

#include "wtf/text/MakeString.h"

#define GET_CALL_FRAME() std::bit_cast<CallFrame*>(__builtin_frame_address(0))
#define FETCH_WASM_INSTANCE(callFrame) std::bit_cast<JSWebAssemblyInstance*>((callFrame)->codeBlock())
#define AS_IMPLEMENTATION_POINTER(function) reinterpret_cast<WebAssemblyBuiltin::ImplementationPtr>(reinterpret_cast<void*>(function))

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

static EncodedJSValue jsStringHello()
{
    CallFrame* frame = GET_CALL_FRAME();
    JSWebAssemblyInstance* wasmInstance = FETCH_WASM_INSTANCE(frame);
    VM& vm = wasmInstance->vm();

    return JSValue::encode(jsString(vm, String::fromLatin1("Hello from the 'wasm:js-string:hello' builtin!")));
}

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#js-string-concat
 *
 * Summary: if the two values are both strings, return the result of concatenating them.
 * Otherwise, throw a RuntimeError.
 */
static EncodedJSValue jsStringConcat(JSValue left, JSValue right)
{
    CallFrame* callFrame = GET_CALL_FRAME();
    JSWebAssemblyInstance* wasmInstance = FETCH_WASM_INSTANCE(callFrame);
    VM& vm = wasmInstance->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!left.isString() || !right.isString()) {
        // how do we throw here without a globalObject?
        ASSERT(false);
    }

    auto leftString = asString(left)->tryGetValue();
    RETURN_IF_EXCEPTION(scope, { });
    auto rightString = asString(right)->tryGetValue();
    RETURN_IF_EXCEPTION(scope, { });

    String result = makeString(StringView(leftString), StringView(rightString));

    RELEASE_AND_RETURN(scope, JSValue::encode(jsString(vm, WTFMove(result))));
}

/*
        All builtin sets
*/

WebAssemblyBuiltinSet WebAssemblyBuiltinSet::createJSStringBuiltinSet()
{
    return WebAssemblyBuiltinSet(
        ASCIILiteral("js-string"),
        ASCIILiteral("wasm:js-string"),
        {
            {
                ASCIILiteral("hello"),
                Wasm::TypeInformation::typeDefinitionForFunction({ Wasm::externrefType() }, { }),
                jsStringHello
            },
            {
                ASCIILiteral("concat"),
                Wasm::TypeInformation::typeDefinitionForFunction({ Wasm::externrefType() }, { Wasm::externrefType(), Wasm::externrefType() }),
                AS_IMPLEMENTATION_POINTER(jsStringConcat)
            }
        });
}

static Vector<WebAssemblyBuiltinSet>& allBuiltinSets() {
    static bool populated = false;
    static Vector<WebAssemblyBuiltinSet>* sets;
    if (!populated) {
        sets = new Vector<WebAssemblyBuiltinSet>(); // leaked, but not really
        sets->append(WebAssemblyBuiltinSet::createJSStringBuiltinSet());
    }
    return *sets;
}

WebAssemblyBuiltinSet* WebAssemblyBuiltinSet::findBySimpleName(const String& name)
{
    for (auto& builtinSet : allBuiltinSets() ) {
        if (name == builtinSet.simpleName())
            return &builtinSet;
    }
    return nullptr;
}

WebAssemblyBuiltinSet* WebAssemblyBuiltinSet::findByQualifiedName(const String& name)
{
    for (auto& builtinSet : allBuiltinSets() ) {
        if (name == builtinSet.qualifiedName())
            return &builtinSet;
    }
    return nullptr;
}

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
