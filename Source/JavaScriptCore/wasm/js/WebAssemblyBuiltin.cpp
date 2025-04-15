#if ENABLE(WEBASSEMBLY)

#include "WebAssemblyBuiltin.h"

#include "wtf/text/MakeString.h"

#define GET_CALL_FRAME() std::bit_cast<CallFrame*>(__builtin_frame_address(0))
#define FETCH_WASM_INSTANCE(callFrame) std::bit_cast<JSWebAssemblyInstance*>((frame)->codeBlock())

namespace JSC {

WebAssemblyBuiltin* WebAssemblyBuiltinSet::findBuiltin(const String& name) const
{
    for (auto* builtin : m_builtins) {
        if (name == builtin->name())
            return builtin;
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
    VM* vm = &wasmInstance->vm();

    return JSValue::encode(jsString(*vm, String::fromLatin1("Hello from a builtin!")));
}

static WebAssemblyBuiltin builtinJsStringHello = { ASCIILiteral("hello"), jsStringHello };

static std::array<WebAssemblyBuiltin*, 1> allJsStringBuiltins = { &builtinJsStringHello };

static WebAssemblyBuiltinSet jsStringBuiltinSet = { ASCIILiteral("js-string"), ASCIILiteral("wasm:js-string"), std::span(allJsStringBuiltins) };

/*
        All builtin sets
*/

static std::array<WebAssemblyBuiltinSet*, 1> allBuiltinSets = { &jsStringBuiltinSet };

WebAssemblyBuiltinSet* WebAssemblyBuiltinSet::findBySimpleName(const String& name)
{
    for (auto* builtinSet : allBuiltinSets ) {
        if (name == builtinSet->simpleName())
            return builtinSet;
    }
    return nullptr;
}

WebAssemblyBuiltinSet* WebAssemblyBuiltinSet::findByQualifiedName(const String& name)
{
    for (auto* builtinSet : allBuiltinSets ) {
        if (name == builtinSet->qualifiedName())
            return builtinSet;
    }
    return nullptr;
}

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
