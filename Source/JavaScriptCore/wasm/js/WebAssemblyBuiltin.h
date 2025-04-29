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
 * An individual builtin. A member of a builtin set.
 */
class WebAssemblyBuiltin {
public:
    using ImplementationPtr = EncodedJSValue (*)();

    WebAssemblyBuiltin(ASCIILiteral name, RefPtr<Wasm::TypeDefinition> type, ImplementationPtr implementation, NativeFunction implementationForReexports)
        : m_name(name)
        , m_type(type)
        , m_implementation(implementation)
        , m_reexportImplementation(implementationForReexports)
    {
    }
    WebAssemblyBuiltin(WebAssemblyBuiltin&&) = default;

    const ASCIILiteral& name() const
    {
        return m_name;
    }
    const Wasm::FunctionSignature* signature() const
    {
        return m_type->as<Wasm::FunctionSignature>();
    }
    ImplementationPtr implementation() const
    {
        return m_implementation;
    }
    /// A re-export representative is a JS function with the same behavior as the builtin,
    /// that can be used in place of the builtin if it happens to be re-exported by the module.
    JSObject* reExportRepresentative(JSGlobalObject*) const;

private:
    ASCIILiteral m_name;
    RefPtr<Wasm::TypeDefinition> m_type;
    ImplementationPtr m_implementation;
    NativeFunction m_reexportImplementation;
};

/**
 * A collection of builtins such as `wasm:js-string`.
 *
 * Sets are created and managed by a builtin registry. Use
 * `WebAssemblyBuiltinRegistry::singleton().findByQualifiedName` to get an instance.
 */
class WebAssemblyBuiltinSet {
public:
    friend class WebAssemblyBuiltinRegistry;

    WebAssemblyBuiltinSet(WebAssemblyBuiltinSet&&) = default;

    /// The set name with the "wasm:" prefix.
    const ASCIILiteral& qualifiedName() const
    {
        return m_qualifiedName;
    }
    /// Search in the set for a builtin with the given name.
    /// Return a pointer to the builtin or nullptr if not found.
    const WebAssemblyBuiltin* findBuiltin(const String& name) const;

private:
    /// Create and return the `wasm:js-string` builtin set.
    static WebAssemblyBuiltinSet jsString();

    WebAssemblyBuiltinSet(ASCIILiteral qualifiedName) : m_qualifiedName(qualifiedName)
    {
    }

    void add(WebAssemblyBuiltin&&);

    ASCIILiteral m_qualifiedName;
    Vector<WebAssemblyBuiltin> m_builtins;
    UncheckedKeyHashMap<String, WebAssemblyBuiltin*> m_builtinsByName;
};

class WebAssemblyBuiltinRegistry {
public:
    static const WebAssemblyBuiltinRegistry& singleton();

    WebAssemblyBuiltinRegistry();

    /// Look for a builtin set instance with the specified qualified name.
    /// Return a pointer to the set, or nullptr if not found.
    const WebAssemblyBuiltinSet* findByQualifiedName(const String&) const;

private:
    Vector<WebAssemblyBuiltinSet> m_builtinSets;
};

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
