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

    WebAssemblyBuiltin(ASCIILiteral name, RefPtr<Wasm::TypeDefinition> type, ImplementationPtr implementation);
    ~WebAssemblyBuiltin() = default;

    const ASCIILiteral& name() const;
    const Wasm::FunctionSignature* signature() const;
    ImplementationPtr implementation() const;

private:
    ASCIILiteral m_name;
    RefPtr<Wasm::TypeDefinition> m_type;
    ImplementationPtr m_implementation;
};

inline WebAssemblyBuiltin::WebAssemblyBuiltin(ASCIILiteral name, RefPtr<Wasm::TypeDefinition> type, ImplementationPtr implementation)
    : m_name(name)
    , m_type(type)
    , m_implementation(implementation)
{
}

inline const ASCIILiteral& WebAssemblyBuiltin::name() const
{
    return m_name;
}

inline const Wasm::FunctionSignature* WebAssemblyBuiltin::signature() const {
    return m_type->as<Wasm::FunctionSignature>();
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
    static WebAssemblyBuiltinSet createJSStringBuiltinSet();

    /// Look for a builtin set instance with the specified qualified name.
    /// Return a pointer to the set, or nullptr if not found.
    static WebAssemblyBuiltinSet* findByQualifiedName(const String& name);

    ~WebAssemblyBuiltinSet() = default;

    /// The set name with the "wasm:" prefix.
    const ASCIILiteral& qualifiedName() const;
    /// Search in the set for a builtin with the given name.
    /// Return a pointer to the builtin or nullptr if not found.
    const WebAssemblyBuiltin* findBuiltin(const String& name) const;

private:
    WebAssemblyBuiltinSet(ASCIILiteral qualifiedName, Vector<WebAssemblyBuiltin>&& builtins);

    ASCIILiteral m_qualifiedName;
    Vector<WebAssemblyBuiltin> m_builtins;
};

inline WebAssemblyBuiltinSet::WebAssemblyBuiltinSet(ASCIILiteral qualifiedName, Vector<WebAssemblyBuiltin>&& builtins)
    : m_qualifiedName(qualifiedName)
    , m_builtins(WTFMove(builtins))
{
}

inline const ASCIILiteral& WebAssemblyBuiltinSet::qualifiedName() const
{
    return m_qualifiedName;
}

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
