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

#include "IteratorOperations.h"
#include "WasmModuleInformation.h"
#include "WebAssemblyCompileOptions.h"

namespace JSC {

std::optional<WebAssemblyCompileOptions> WebAssemblyCompileOptions::create(JSGlobalObject* globalObject, JSObject *optionsObject)
{
    if (!optionsObject) {
        return std::nullopt;
    }
    WebAssemblyCompileOptions options;
    VM& vm = globalObject->vm();

    // Check for and acquire 'importedStringConstants'.
    // Acquire means create an isolated copy of the original string, owned exclusively by this object.
    JSValue importedStringConstantsValue = optionsObject->get(globalObject, PropertyName(Identifier::fromString(vm, "importedStringConstants"_s)));
    if (importedStringConstantsValue.isString()) {
        auto contents = asString(importedStringConstantsValue)->value(globalObject);
        options.m_importedStringConstants = String(contents).isolatedCopy();
    }

    // Check for and acquire 'builtins'.
    JSValue builtinsValue = optionsObject->get(globalObject, PropertyName(Identifier::fromString(vm, "builtins"_s)));
    forEachInIterable(globalObject, builtinsValue, [&] (VM&, JSGlobalObject* globalObject, JSValue nextValue) {
        if (nextValue.isString()) {
            auto contents = asString(nextValue)->value(globalObject);
            String qualifiedName = makeString("wasm:"_s, StringView(contents));
            options.m_qualifiedBuiltinSetNames.append(qualifiedName);
        }
    });
    return options;
}

WebAssemblyCompileOptions::WebAssemblyCompileOptions()
    : m_importedStringConstants(std::nullopt)
    , m_qualifiedBuiltinSetNames(Vector<String>())
{
}

static bool namesInclude(const String& expected, const Vector<String>& names)
{
    for (auto& name : names) {
        if (name == expected) return true;
    }
    return false;
}

/**
 * See step 2.1 of: https://webassembly.github.io/js-string-builtins/js-api/#validate-builtins-and-imported-string-for-a-webassembly-module
 */
static bool validateImportedStringConstant(const Wasm::Import& import, const Wasm::ModuleInformation& moduleInformation)
{
    // auto importType = moduleInformation.typeSignatures[import.kindIndex]; // FIXME: is this the right way of getting the type?
    // auto expectedType = Wasm::TypeInformation::
    UNUSED_PARAM(import);
    UNUSED_PARAM(moduleInformation);
    return true;
}

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#validate-an-import-for-builtins
 *
 * Summary:
 * Fail the validation if:
 *  - there is a builtin set whose simple name appears in builtinSetNames, and
 *  - the qualified name of the builtin set matches the import module name, and
 *  - the builtin set contains a builtin matching the function name, and
 *  - the builtin type does not match the import type.
 */
bool WebAssemblyCompileOptions::validateImportForBuiltinSetNames(const Wasm::Import& import, const String& importModuleName, const Wasm::ModuleInformation& moduleInfo) const
{
    if (!namesInclude(importModuleName, m_qualifiedBuiltinSetNames)) {
        return true;
    }
    WebAssemblyBuiltinSet *builtinSet = WebAssemblyBuiltinSet::findByQualifiedName(importModuleName);
    if (!builtinSet) {
        return true;
    }
    String importName = makeString(import.field);
    const WebAssemblyBuiltin* builtin = builtinSet->findBuiltin(importName);
    if (!builtin) {
        return true;
    }
    const Wasm::FunctionSignature* builtinSig = builtin->signature();
    Wasm::TypeIndex typeIndex = moduleInfo.importFunctionTypeIndices[import.kindIndex];
    const Wasm::FunctionSignature* importSig = Wasm::TypeInformation::get(typeIndex).as<Wasm::FunctionSignature>();
    return *builtinSig == *importSig;
}

bool WebAssemblyCompileOptions::validateBuiltinsAndImportedStrings(const Wasm::Module& module) const
{
    if (!validateBuiltinSetNames()) {
        return false;
    }
    auto& moduleInfo = module.moduleInformation();
    for (const auto& import : moduleInfo.imports) {
        String importModuleName = makeString(import.module);
        if (m_importedStringConstants && *m_importedStringConstants == importModuleName) {
            if (!validateImportedStringConstant(import, moduleInfo)) {
                return false;
            }
        } else {
            if (!validateImportForBuiltinSetNames(import, importModuleName, moduleInfo)) {
                return false;
            }
        }
    }
    return true;
}

/**
 * See https://webassembly.github.io/js-string-builtins/js-api/#validate-builtin-set-names
 *
 * Summary: the builtin set names should not include duplicates.
 */
bool WebAssemblyCompileOptions::validateBuiltinSetNames() const
{
    UncheckedKeyHashSet<String> seen;
    for (const auto& name : m_qualifiedBuiltinSetNames) {
        if (seen.contains(name)) {
            return false;
        }
        seen.add(name);
    }
    return true;
}

void WebAssemblyCompileOptions::moveOptionsInto(Wasm::Module& module)
{
    module.setCompileOptions(WTFMove(m_importedStringConstants), WTFMove(m_qualifiedBuiltinSetNames));
}

} // namespace JSC
