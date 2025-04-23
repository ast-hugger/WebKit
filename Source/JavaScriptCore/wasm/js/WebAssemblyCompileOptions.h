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

#include <optional>

#include "IteratorOperations.h"
#include "WasmModule.h"
#include "wtf/text/WTFString.h"

namespace JSC {

/**
 * Captures the information extracted from the optional compilation options argument added by the
 * js-string builtins proposal to `WebAssembly.Module` constructor and a number of other APIs.
 *
 * The names listed in the "builtins" option are qualified: "foo" becomes "wasm:foo".
 * The contents of JSStrings in the options object are deep-copied into isolated copies. Eventually
 * the strings are moved into the `Wasm::Module` that ends with their full ownership.
 */
class WebAssemblyCompileOptions {
public:
    /**
     * Create an instance if `optionsObject` is not a nullptr.
     */
    static std::optional<WebAssemblyCompileOptions> create(JSGlobalObject*, JSObject* optionsObject);

    WebAssemblyCompileOptions(WebAssemblyCompileOptions&& other)
        : m_importedStringConstants(WTFMove(other.m_importedStringConstants))
        , m_qualifiedBuiltinSetNames(WTFMove(other.m_qualifiedBuiltinSetNames))
    {
    }
    ~WebAssemblyCompileOptions() = default;

    // See https://webassembly.github.io/js-string-builtins/js-api/#validate-builtins-and-imported-string-for-a-webassembly-module
    bool validateBuiltinsAndImportedStrings(const Wasm::Module&) const;
    // See https://webassembly.github.io/js-string-builtins/js-api/#validate-builtin-set-names
    bool validateBuiltinSetNames() const;

    void moveOptionsInto(Wasm::Module& module);

private:
    WebAssemblyCompileOptions();
    bool validateImportForBuiltinSetNames(const Wasm::Import& import, const String& importModuleName, const Wasm::ModuleInformation& moduleInfo) const;

    std::optional<String> m_importedStringConstants;
    Vector<String> m_qualifiedBuiltinSetNames;
};

} // namespace JSC
