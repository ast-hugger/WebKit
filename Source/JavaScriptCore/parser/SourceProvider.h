/*
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Compiler.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include <JavaScriptCore/CachedBytecode.h>
#include <JavaScriptCore/CodeBlockHash.h>
#include <JavaScriptCore/CodeSpecializationKind.h>
#include <JavaScriptCore/SourceOrigin.h>
#include <JavaScriptCore/SourceTaintedOrigin.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/SIMDHelpers.h>
#include <wtf/Vector.h>
#include <wtf/text/TextPosition.h>
#include <wtf/text/WTFString.h>
#include <optional>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

namespace JSC {

class SourceCode;
class UnlinkedFunctionExecutable;
class UnlinkedFunctionCodeBlock;

enum class SourceProviderSourceType : uint8_t {
    Program,
    Module,
    WebAssembly,
    JSON,
    Text,
    ImportMap,
};

using BytecodeCacheGenerator = Function<RefPtr<CachedBytecode>()>;

// The line terminators of ECMA-262 #11.3: LF, CR, LS (U+2028) and PS (U+2029), of which only the
// first two can occur in an 8-bit source.
//
// These live here, beside the line-start table that consumes them, rather than only on Lexer<T>,
// because two very different traversals have to agree on them: the lexer walks a cursor and
// consumes the terminator it is sitting on, while the table sweeps a whole buffer looking for
// every terminator. A second copy of the CRLF rule in particular would diverge silently -- it
// only shows up on sources that use CRLF, and JSTests contains none.
template<typename CharType> ALWAYS_INLINE bool isLineTerminator(CharType);

template<> ALWAYS_INLINE bool isLineTerminator<Latin1Character>(Latin1Character character)
{
    return character == '\r' || character == '\n';
}

template<> ALWAYS_INLINE bool isLineTerminator<char16_t>(char16_t character)
{
    // (c & ~1) == 0x2028 covers both U+2028 and U+2029.
    return character == '\r' || character == '\n' || (character & ~1) == 0x2028;
}

// CRLF is a single line terminator, not two. This is the one decision the lexer and the table must
// agree on and cannot share code for -- the lexer has a cursor and two characters in hand, the table
// has a buffer and an index -- so the decision itself is factored out here and both call it.
template<typename CharType>
ALWAYS_INLINE bool isCRLFPair(CharType first, CharType second)
{
    return first == '\r' && second == '\n';
}

// Given an index at a line terminator, the offset at which the next line begins.
template<typename CharType>
ALWAYS_INLINE size_t lineStartAfterTerminator(std::span<const CharType> text, size_t indexOfTerminator)
{
    ASSERT(indexOfTerminator < text.size());
    ASSERT(isLineTerminator(text[indexOfTerminator]));
    if (indexOfTerminator + 1 < text.size() && isCRLFPair(text[indexOfTerminator], text[indexOfTerminator + 1]))
        return indexOfTerminator + 2;
    return indexOfTerminator + 1;
}

// The first line terminator in `text`, or one past the end if there is none.
//
// The vector form of isLineTerminator above, kept beside it so the scalar and SIMD spellings of one
// rule cannot drift apart -- a scalar predicate cannot be used on a vector, so a second encoding is
// unavoidable, but a second *location* is not. Used by the line-start table and by the lexer's
// single-line-comment scanner, which want the same thing over different buffers.
//
// Cheaper than most predicates in the lexer: two compares for an 8-bit source, four for 16-bit,
// against the two range checks and two equality tests that scanning an identifier needs.
template<typename CharType>
ALWAYS_INLINE const CharType* findLineTerminator(std::span<const CharType> text)
{
    using UnsignedType = SameSizeUnsignedInteger<CharType>;
    auto vectorMatch = [](auto input) ALWAYS_INLINE_LAMBDA {
        constexpr auto lineFeedMask = SIMD::splat<UnsignedType>('\n');
        constexpr auto carriageReturnMask = SIMD::splat<UnsignedType>('\r');
        auto matches = SIMD::bitOr(SIMD::equal(input, lineFeedMask), SIMD::equal(input, carriageReturnMask));
        if constexpr (!std::is_same_v<CharType, Latin1Character>) {
            // LS and PS are single UTF-16 code units, so they compare directly in a 16-bit lane.
            constexpr auto lineSeparatorMask = SIMD::splat<UnsignedType>(static_cast<UnsignedType>(0x2028));
            constexpr auto paragraphSeparatorMask = SIMD::splat<UnsignedType>(static_cast<UnsignedType>(0x2029));
            matches = SIMD::bitOr(matches, SIMD::equal(input, lineSeparatorMask), SIMD::equal(input, paragraphSeparatorMask));
        }
        return SIMD::findFirstNonZeroIndex(matches);
    };
    auto scalarMatch = [](CharType character) ALWAYS_INLINE_LAMBDA {
        return isLineTerminator(character);
    };
    return SIMD::find(text, vectorMatch, scalarMatch);
}


// A source's line boundaries, derived from its text on first use and never rebuilt.
//
// Deliberately not maintained incrementally. The lexer keeps a running line number, but setOffset()
// jumps it for the source-provider-cache fast path and for save points, with the line restored
// separately; a derivation from the text has no such bookkeeping and so cannot drift from it.
//
// Queries are rare by construction -- stack traces, the debugger, the inspector -- so a lock per
// query is the right trade against the complexity of publishing the table without one. The lock is
// needed rather than optional: the sampling profiler and the compiler threads reach this off the
// main thread.
class LineStartTable {
    WTF_MAKE_NONCOPYABLE(LineStartTable);
public:
    LineStartTable() = default;

    // Where an offset falls in the source. All four come out of one lookup, so unlike the parser's
    // separately maintained line and lineStart they cannot disagree with each other. Zero-based and
    // free of embedding bias: a caller wanting one-based user-facing coordinates, or the column
    // offset an inline <script> starts at, applies that itself, once.
    struct PositionInfo {
        unsigned line { 0 };
        unsigned column { 0 };
        unsigned lineStart { 0 };  // offset of the line's first character
        unsigned lineEnd { 0 };    // one past the line's last character, excluding its terminator,
                                   // so [lineStart, lineEnd) is the line's text
    };

    // `text` must be the same source on every call -- the table is built from the first one it sees
    // and reused thereafter. Asserted below.
    JS_EXPORT_PRIVATE PositionInfo positionInfoForOffset(StringView text, unsigned offset);
    JS_EXPORT_PRIVATE unsigned lineCount(StringView text);

    // Whether anything has forced the table yet. The whole design rests on this staying false
    // through an ordinary parse, so it is worth being able to assert on.
    bool isBuilt() const
    {
        Locker locker { m_lock };
        return !!m_lineStarts;
    }

private:
    template<typename CharType> static Vector<unsigned> build(std::span<const CharType>);
    const Vector<unsigned>& ensureBuilt(StringView) WTF_REQUIRES_LOCK(m_lock);

    mutable Lock m_lock;
    std::optional<Vector<unsigned>> m_lineStarts WTF_GUARDED_BY_LOCK(m_lock);
    unsigned m_builtForLength WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

class SourceProvider : public ThreadSafeRefCounted<SourceProvider> {
public:
    static const intptr_t nullID = 1;

    JS_EXPORT_PRIVATE SourceProvider(const SourceOrigin&, String&& sourceURL, String&& preRedirectURL, SourceTaintedOrigin, const TextPosition& startPosition, SourceProviderSourceType);

    JS_EXPORT_PRIVATE virtual ~SourceProvider();

    virtual unsigned hash() const = 0;
    virtual StringView source() const = 0;
    virtual RefPtr<CachedBytecode> cachedBytecode() const { return nullptr; }
    virtual void cacheBytecode(const BytecodeCacheGenerator&) const { }
    virtual void updateCache(const UnlinkedFunctionExecutable*, const SourceCode&, CodeSpecializationKind, const UnlinkedFunctionCodeBlock*) const { }
    virtual void commitCachedBytecode() const { }

    StringView getRange(int start, int end) const LIFETIME_BOUND
    {
        return source().substring(start, end - start);
    }

    const SourceOrigin& sourceOrigin() const LIFETIME_BOUND { return m_sourceOrigin; }

    // This is NOT the path that should be used for computing relative paths from a script. Use SourceOrigin's URL for that, the values may or may not be the same...
    const String& sourceURL() const LIFETIME_BOUND { return m_sourceURL; }
    const String& sourceURLStripped();
    const String& preRedirectURL() const LIFETIME_BOUND { return m_preRedirectURL; }
    const String& sourceURLDirective() const LIFETIME_BOUND { return m_sourceURLDirective; }
    const String& sourceMappingURLDirective() const LIFETIME_BOUND { return m_sourceMappingURLDirective; }

    TextPosition startPosition() const { return m_startPosition; }
    SourceProviderSourceType sourceType() const { return m_sourceType; }
    bool isModuleType() const
    {
        switch (m_sourceType) {
        case SourceProviderSourceType::Module:
        case SourceProviderSourceType::JSON:
        case SourceProviderSourceType::Text:
            return true;
        default:
            return false;
        }
    }

    SourceID asID()
    {
        if (!m_id)
            getID();
        return m_id;
    }

    void setSourceURLDirective(const String& sourceURLDirective) { m_sourceURLDirective = sourceURLDirective; }
    void setSourceMappingURLDirective(const String& sourceMappingURLDirective) { m_sourceMappingURLDirective = sourceMappingURLDirective; }
    void setSourceTaintedOrigin(SourceTaintedOrigin taintedness) { m_taintedness = taintedness; }

    SourceTaintedOrigin sourceTaintedOrigin() const { return m_taintedness; }
    bool couldBeTainted() const { return m_taintedness != SourceTaintedOrigin::Untainted; }

    JS_EXPORT_PRIVATE void lockUnderlyingBuffer();
    JS_EXPORT_PRIVATE void unlockUnderlyingBuffer();
    JS_EXPORT_PRIVATE virtual CodeBlockHash codeBlockHashConcurrently(int startOffset, int endOffset, CodeSpecializationKind);

    virtual bool isScriptBufferSourceProvider() const { return false; }

    JS_EXPORT_PRIVATE CString sourceCodeDumpFilePath(const CString& dumpDirectory);

    // Line and column for an offset, derived from the text rather than tracked. See LineStartTable.
    LineStartTable::PositionInfo positionInfoForOffset(unsigned offset)
    {
        return m_lineStartTable.positionInfoForOffset(source(), offset);
    }

    // Number of lines in the source. Forces the table, so it is for testing and for callers already
    // paying for a lookup.
    unsigned lineCount() { return m_lineStartTable.lineCount(source()); }

    bool lineStartTableIsBuilt() const { return m_lineStartTable.isBuilt(); }

private:
    JS_EXPORT_PRIVATE virtual void lockUnderlyingBufferImpl();
    JS_EXPORT_PRIVATE virtual void unlockUnderlyingBufferImpl();

    JS_EXPORT_PRIVATE void NODELETE getID();

    std::atomic<unsigned> m_lockingCount { 0 };
    SourceProviderSourceType m_sourceType;
    SourceOrigin m_sourceOrigin;
    String m_sourceURL;
    String m_sourceURLStripped;
    String m_preRedirectURL;
    String m_sourceURLDirective;
    String m_sourceMappingURLDirective;
    TextPosition m_startPosition;
    SourceID m_id { 0 };
    SourceTaintedOrigin m_taintedness;

    std::atomic<bool> m_sourceCodeDumped { false };
    Lock m_sourceCodeDumpLock;
    CString m_sourceCodeDumpFilePath WTF_GUARDED_BY_LOCK(m_sourceCodeDumpLock);

    LineStartTable m_lineStartTable;
};

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(StringSourceProvider);
class StringSourceProvider : public SourceProvider {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED_WITH_HEAP_IDENTIFIER(StringSourceProvider, StringSourceProvider);
public:
    static Ref<StringSourceProvider> create(const String& source, const SourceOrigin& sourceOrigin, String sourceURL, SourceTaintedOrigin taintedness, const TextPosition& startPosition = TextPosition(), SourceProviderSourceType sourceType = SourceProviderSourceType::Program)
    {
        return adoptRef(*new StringSourceProvider(source, sourceOrigin, taintedness, WTF::move(sourceURL), startPosition, sourceType));
    }

    unsigned hash() const override
    {
        return m_source.get().hash();
    }

    StringView source() const override
    {
        return m_source.get();
    }

protected:
    StringSourceProvider(const String& source, const SourceOrigin& sourceOrigin, SourceTaintedOrigin taintedness, String&& sourceURL, const TextPosition& startPosition, SourceProviderSourceType sourceType)
        : SourceProvider(sourceOrigin, WTF::move(sourceURL), String(), taintedness, startPosition, sourceType)
        , m_source(source.isNull() ? *StringImpl::empty() : *source.impl())
    {
    }

private:
    const Ref<StringImpl> m_source;
};

#if ENABLE(WEBASSEMBLY)
class BaseWebAssemblySourceProvider : public SourceProvider {
public:
    virtual const uint8_t* data() = 0;
    virtual size_t size() const = 0;
protected:
    JS_EXPORT_PRIVATE BaseWebAssemblySourceProvider(const SourceOrigin&, String&& sourceURL);
};

class WebAssemblySourceProvider final : public BaseWebAssemblySourceProvider {
public:
    static Ref<WebAssemblySourceProvider> create(Vector<uint8_t>&& data, const SourceOrigin& sourceOrigin, String sourceURL)
    {
        return adoptRef(*new WebAssemblySourceProvider(WTF::move(data), sourceOrigin, WTF::move(sourceURL)));
    }

    unsigned hash() const final
    {
        return m_source.impl()->hash();
    }

    StringView source() const final
    {
        return m_source;
    }

    const uint8_t* data() final
    {
        return m_data.span().data();
    }

    size_t size() const final
    {
        return m_data.size();
    }

    const Vector<uint8_t>& dataVector() const
    {
        return m_data;
    }

private:
    WebAssemblySourceProvider(Vector<uint8_t>&& data, const SourceOrigin& sourceOrigin, String&& sourceURL)
        : BaseWebAssemblySourceProvider(sourceOrigin, WTF::move(sourceURL))
        , m_source("[WebAssembly source]"_s)
        , m_data(WTF::move(data))
    {
    }

    String m_source;
    Vector<uint8_t> m_data;
};
#endif

// RAII class for managing a source provider's underlying buffer.
class SourceProviderBufferGuard {
public:
    explicit SourceProviderBufferGuard(SourceProvider* sourceProvider)
        : m_sourceProvider(sourceProvider)
    {
        if (m_sourceProvider)
            m_sourceProvider->lockUnderlyingBuffer();
    }

    ~SourceProviderBufferGuard()
    {
        if (m_sourceProvider)
            m_sourceProvider->unlockUnderlyingBuffer();
    }

    SourceProvider* provider() { return m_sourceProvider; }

private:
    // This must not be RefPtr. It is possible that this is used by the concurrent compiler and
    // we are ensuring that this does not go away with different mechanism. But SourceProvider etc. can have main-thread-only affinity.
    SourceProvider* m_sourceProvider { nullptr };
};

} // namespace JSC
