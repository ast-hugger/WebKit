#pragma once

#include <optional>

#include "IteratorOperations.h"
#include "wtf/text/WTFString.h"

namespace JSC {

class WebAssemblyCompileOptions {
public:
    /// Create an instance and populate it from the given optionsObject.
    static WebAssemblyCompileOptions create(JSGlobalObject*, JSObject* optionsObject);

    ~WebAssemblyCompileOptions() = default;

    /// Return a pointer to the 'importedStringConstants' option value, or a `nullptr` if the option was not specified.
    /// The recipient MUST NOT create additional references to the string.
    const String* importedStringConstants() const;
    /// Return a vector of pointers to the names specified in the 'builtins' option.
    /// The recipient MUST NOT create additional references to the strings.
    Vector<const String*> builtins() const;
    
    /// As defined by the standard's Editor's Draft of 23 August 2024, 
    /// "to validate builtins and imported string for a WebAssembly module".
    bool validateBuiltinsAndImportedString(Wasm::ModuleInformation& module) const;

    private:
    bool validateBuiltinSetNames() const;
    bool validateImportForBuiltin(const Wasm::Import&) const;

    WebAssemblyCompileOptions();

    std::optional<String> m_importedStringConstants;
    Vector<String> m_builtins;
};

} // namespace JSC
