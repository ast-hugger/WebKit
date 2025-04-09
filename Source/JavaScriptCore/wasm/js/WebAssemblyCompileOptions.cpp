#include "IteratorOperations.h"
#include "WasmModuleInformation.h"
#include "WebAssemblyCompileOptions.h"

namespace JSC {

WebAssemblyCompileOptions WebAssemblyCompileOptions::create(JSGlobalObject* globalObject, JSObject *optionsObject)
{
    WebAssemblyCompileOptions options;
    if (optionsObject) {
        VM& vm = globalObject->vm();
        
        // Check for and acquire 'importedStringConstants'.
        // Acquire means create an isolated copy of the original string, owned exclusively by this object.
        JSValue importedStringConstantsValue = optionsObject->get(globalObject, PropertyName(Identifier::fromString(vm, "importedStringConstants"_s)));
        if (importedStringConstantsValue.isString()) {
            auto contents = asString(importedStringConstantsValue)->value(globalObject);
            options.m_importedStringConstants = contents.data.isolatedCopy();
        }

        // Check for and acquire 'builtins'.
        JSValue builtinsValue = optionsObject->get(globalObject, PropertyName(Identifier::fromString(vm, "builtins"_s)));
        forEachInIterable(globalObject, builtinsValue, [&] (VM&, JSGlobalObject* globalObject, JSValue nextValue) {
            if (nextValue.isString()) {
                auto contents = asString(nextValue)->value(globalObject);
                options.m_builtins.append(contents.data.isolatedCopy());
            }
        });
    }
    return options;
}

WebAssemblyCompileOptions::WebAssemblyCompileOptions()
    : m_importedStringConstants(std::nullopt)
    , m_builtins(Vector<String>())
{
}

const String* WebAssemblyCompileOptions::importedStringConstants() const 
{
    return m_importedStringConstants ? &*m_importedStringConstants : nullptr;
}

Vector<const String*> WebAssemblyCompileOptions::builtins() const 
{
    Vector<const String*> result;
    for (const auto& name : m_builtins) {
        result.append(&name);
    }
    return result;
}

// Check that the type is 'global const (ref extern)'
static bool validateImportedConstantsExternType(const Wasm::Import& import) 
{
    UNUSED_PARAM(import);
    return true;
}

bool WebAssemblyCompileOptions::validateBuiltinsAndImportedString(Wasm::ModuleInformation& module) const
{
    if (!validateBuiltinSetNames())
        return false;
    for (const auto& import : module.imports) {
        String importName = String::fromUTF8WithLatin1Fallback(import.module.span());
        if (m_importedStringConstants && *m_importedStringConstants == importName) {
            if (!validateImportedConstantsExternType(import))
                return false;
        } else {
            if (!validateImportForBuiltin(import))
                return false;
        }
    }
    return true;
}

bool WebAssemblyCompileOptions::validateBuiltinSetNames() const 
{
    UncheckedKeyHashSet<String> seen;
    for (const auto& name : m_builtins) {
        if (seen.contains(name))
            return false;
        seen.add(name);
    }
    return true;
}

bool WebAssemblyCompileOptions::validateImportForBuiltin(const Wasm::Import& import) const
{
    UNUSED_PARAM(import);
    return true; // FIXME
}

} // namespace JSC
