#pragma once

#include <optional>

#include "IteratorOperations.h"
#include "wtf/text/WTFString.h"

namespace JSC {

/**
 * Captures the information extracted in the optional compilation options argument added by the
 * js-string builtins proposal to `WebAssembly.Module` constructor and a number of other APIs. 
 * The contents of the original JSStrings are deep-copied into isolated copies fully owned by this
 * object. The intent is to eventually move these isolated copies into a `Wasm::Module` to be
 * fully owned by it, where they can be treated as read-only data without touching string refcounts.
 */
class WebAssemblyCompileOptions {
public:
    static WebAssemblyCompileOptions create(JSGlobalObject*, JSObject* optionsObject);

    ~WebAssemblyCompileOptions() = default;

    const String* importedStringConstants() const;
    Vector<const String*> builtins() const;

    /**
     * Implements the "to validate builtins and imported string for a WebAssembly module" algorithm
     * of the proposal.
     */
    bool validateBuiltinsAndImportedString(Wasm::ModuleInformation& module) const;

    private:
    bool validateBuiltinSetNames() const;
    bool validateImportForBuiltin(const Wasm::Import&) const;

    WebAssemblyCompileOptions();

    std::optional<String> m_importedStringConstants;
    Vector<String> m_builtins;
};

} // namespace JSC
