/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "EagerIIFERegistry.h"

#include "BytecodeGenerator.h"
#include "Options.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "VM.h"
#include "VariableEnvironment.h"
#include <wtf/Vector.h>

namespace JSC {

EagerIIFERegistry::~EagerIIFERegistry()
{
    clear();
}

void EagerIIFERegistry::clear()
{
    m_pendingNodes.clear();
    m_executables.clear();
}

void EagerIIFERegistry::addPendingNode(int sourcePosition, std::unique_ptr<FunctionNode> node, FunctionMetadataNode* metadata)
{
    ASSERT(!m_pendingNodes.contains(sourcePosition));
    m_pendingNodes.add(sourcePosition, PendingEntry { WTF::move(node), metadata });
}

void EagerIIFERegistry::addExecutable(VM& vm, int sourcePosition, UnlinkedFunctionExecutable* executable)
{
    ASSERT(!m_executables.contains(sourcePosition));
    m_executables.add(sourcePosition, Strong<UnlinkedFunctionExecutable>(vm, executable));
}

UnlinkedFunctionExecutable* EagerIIFERegistry::takeExecutable(int sourcePosition)
{
    auto strong = m_executables.take(sourcePosition);
    return strong.get();
}

void performEagerIIFECompileForProgram(VM& vm, EagerIIFERegistry& registry, ProgramNode& programNode, const SourceCode& parentSource, OptionSet<CodeGenerationMode> codeGenerationMode, JSParserScriptMode scriptMode)
{
    if (!Options::useEagerIIFECompile())
        return;
    if (!registry.hasPendingNodes())
        return;

    // Build parent TDZ from program-scope lexical variables. This matches what
    // BytecodeGenerator::makeFunction would see via getVariablesUnderTDZ() for an
    // IIFE at top-level program scope: the program pushes its lexicalVariables
    // onto m_TDZStack via pushLexicalScope, and any nested function captures
    // that environment.
    RefPtr<TDZEnvironmentLink> parentScopeTDZVariables;
    {
        TDZEnvironment environment;
        for (const auto& entry : programNode.lexicalVariables()) {
            if (entry.value.isFunction())
                continue;
            environment.add(entry.key.get());
        }
        if (!environment.isEmpty())
            parentScopeTDZVariables = TDZEnvironmentLink::create(vm.m_compactVariableMap->get(environment), nullptr);
    }

    auto pending = registry.takePendingNodes();
    // Keys are source-position offsets. Process entries in source-order so
    // we can skip nested ones: once an outer IIFE's range is determined,
    // any pending entry whose position falls within that range is nested
    // inside it and must NOT be eager-compiled here (we don't have its
    // correct parent source). The outer's BCG will handle it via the normal
    // makeFunction path, which in turn consults the registry for the outer's
    // own pending bytecode and lazy-reparses inner ones via the
    // parse<FunctionNode> short-circuit.
    Vector<std::pair<int, EagerIIFERegistry::PendingEntry>> entries;
    entries.reserveInitialCapacity(pending.size());
    for (auto& entry : pending)
        entries.append(std::pair { entry.key, WTF::move(entry.value) });
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    unsigned nestedCutoffEndOffset = 0;
    for (auto& [sourcePosition, pendingEntry] : entries) {
        std::unique_ptr<FunctionNode> functionNode = WTF::move(pendingEntry.node);
        FunctionMetadataNode* metadata = pendingEntry.metadata;
        if (!functionNode || !metadata) [[unlikely]]
            continue;

        // Skip IIFEs nested inside a previously-compiled one. Their parent
        // source is not the program; the outer IIFE's BCG will lazy-compile
        // them with the correct parent.
        if (static_cast<unsigned>(sourcePosition) < nestedCutoffEndOffset) {
            // Put it back into pending so the lazy reparse short-circuit can
            // consume it when BCG recursively compiles the outer IIFE.
            registry.addPendingNode(sourcePosition, WTF::move(functionNode), metadata);
            continue;
        }

        SourceParseMode parseMode = metadata->parseMode();
        ConstructAbility constructAbility = constructAbilityForParseMode(parseMode);

        UnlinkedFunctionExecutable* executable = UnlinkedFunctionExecutable::create(
            vm, parentSource, metadata, UnlinkedNormalFunction, constructAbility,
            InlineAttribute::None, scriptMode, parentScopeTDZVariables,
            std::nullopt /* generatorOrAsyncWrapperFunctionParameterNames */,
            std::nullopt /* parentPrivateNameEnvironment */,
            DerivedContextType::None, EvalContextType::FunctionEvalContext,
            NeedsClassFieldInitializer::No, PrivateBrandRequirement::None);

        SourceCode functionSource = executable->linkedSourceCode(parentSource);

        ParserError error;
        UnlinkedFunctionCodeBlock* unlinkedCodeBlock = compileFunctionNodeToUnlinkedCodeBlock(
            vm, executable, functionNode.get(), functionSource,
            CodeSpecializationKind::CodeForCall, codeGenerationMode,
            UnlinkedNormalFunction, error, parseMode);

        // AST (FunctionNode + its ParserArena) is freed when the local unique_ptr
        // goes out of scope at the end of this iteration.

        if (error.isValid() || !unlinkedCodeBlock) [[unlikely]]
            continue; // Fall through to the lazy re-parse path.

        executable->installUnlinkedCodeBlockForCall(vm, unlinkedCodeBlock);
        registry.addExecutable(vm, sourcePosition, executable);

        // Any pending entries whose sourcePosition is within this IIFE's source
        // range are nested inside it and must be left for the outer's BCG to
        // handle (via the lazy FunctionNode short-circuit).
        unsigned iifeEnd = metadata->source().startOffset() + metadata->source().length();
        nestedCutoffEndOffset = std::max(nestedCutoffEndOffset, iifeEnd);
    }
}

} // namespace JSC
