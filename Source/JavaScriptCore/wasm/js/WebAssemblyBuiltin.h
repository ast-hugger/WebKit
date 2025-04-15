#pragma once

#if ENABLE(WEBASSEMBLY)

#include "JSCJSValue.h"
#include "JSWebAssemblyInstance.h"

#include "wtf/text/ASCIILiteral.h"
#include "wtf/text/WTFString.h"

namespace JSC {

/**
 * An individual builtin contained in a builtin set.
 */
class WebAssemblyBuiltin {
public:
    using ImplementationPtr = EncodedJSValue (*)();

    constexpr WebAssemblyBuiltin(ASCIILiteral name, ImplementationPtr implementation);
    ~WebAssemblyBuiltin() = default;

    const ASCIILiteral& name() const;
    ImplementationPtr implementation() const;

private:
    ASCIILiteral m_name;
    ImplementationPtr m_implementation;
};

inline constexpr WebAssemblyBuiltin::WebAssemblyBuiltin(ASCIILiteral name, ImplementationPtr implementation)
    : m_name(name)
    , m_implementation(implementation)
{
}

inline const ASCIILiteral& WebAssemblyBuiltin::name() const
{
    return m_name;
}

inline WebAssemblyBuiltin::ImplementationPtr WebAssemblyBuiltin::implementation() const
{
    return m_implementation;
}

/**
 * A collection of builtins. Made available for importing if listed in the
 * "builtins" compile option under its simple name. Module imports reference
 * it by the qualified name.
 */
class WebAssemblyBuiltinSet {
public:
    /// Look for a builtin set instance with the specified simple name.
    /// Return a pointer to the set, or nullptr if not found.
    static WebAssemblyBuiltinSet* findBySimpleName(const String& name);
    /// Look for a builtin set instance with the specified qualified name.
    /// Return a pointer to the set, or nullptr if not found.
    static WebAssemblyBuiltinSet* findByQualifiedName(const String& name);

    constexpr WebAssemblyBuiltinSet(ASCIILiteral simpleName, ASCIILiteral qualifiedName, std::span<WebAssemblyBuiltin*> m_builtins);
    ~WebAssemblyBuiltinSet() = default;

    /// The set name without the "wasm:" prefix.
    const ASCIILiteral& simpleName() const;
    /// The set name with the "wasm:" prefix.
    const ASCIILiteral& qualifiedName() const;
    /// Search in the set for a builtin with the given name.
    /// Return a pointer to the builtin on nullptr if not found.
    WebAssemblyBuiltin* findBuiltin(const String& name) const;

private:
    ASCIILiteral m_simpleName;
    ASCIILiteral m_qualifiedName;

    std::span<WebAssemblyBuiltin*> m_builtins;
};

inline constexpr WebAssemblyBuiltinSet::WebAssemblyBuiltinSet(ASCIILiteral simpleName, ASCIILiteral qualifiedName, std::span<WebAssemblyBuiltin*> builtins)
    : m_simpleName(simpleName)
    , m_qualifiedName(qualifiedName)
    , m_builtins(builtins)
{
}

inline const ASCIILiteral& WebAssemblyBuiltinSet::simpleName() const
{
    return m_simpleName;
}

inline const ASCIILiteral& WebAssemblyBuiltinSet::qualifiedName() const
{
    return m_qualifiedName;
}

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
