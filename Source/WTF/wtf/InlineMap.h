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

#include <array>
#include <cstddef>
#include <memory>
#include <wtf/Assertions.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashTraits.h>
#include <wtf/KeyValuePair.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace WTF {

// A map that stores entries inline with linear lookup for small sizes and switches to hashed heap
// storage only after growing past a certain point. The switchover happens when the map size exceeds
// the InlineCapacity template parameter. InlineCapacity does not need to be any special number such
// as a power of 2.

template<typename KeyArg, typename ValueArg, typename HashArg = PtrHash<KeyArg>, typename KeyTraitsArg = HashTraits<KeyArg>, typename MappedTraitsArg = HashTraits<ValueArg>, unsigned InlineCapacity = 5>
class InlineMap {
public:
    using KeyType = KeyArg;
    using KeyTraits = KeyTraitsArg;
    using MappedType = ValueArg;
    using MappedTraits = MappedTraitsArg;
    using Entry = KeyValuePair<KeyType, MappedType>;
    using EntryTraits = KeyValuePairHashTraits<KeyTraits, MappedTraits>;

    InlineMap()
        : m_size(0)
        , m_capacity(InlineCapacity)
    {
    }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

    InlineMap(InlineMap&& other)
        : m_size(other.m_size)
        , m_capacity(other.m_capacity)
    {
        if (isInline()) {
            for (unsigned i = 0; i < m_size; ++i)
                new (&inlineStorage()[i]) Entry(WTF::move(other.inlineStorage()[i]));
        } else {
            m_storage.heapEntries = other.m_storage.heapEntries;
            other.m_storage.heapEntries = nullptr;
        }
        other.m_size = 0;
        other.m_capacity = InlineCapacity;
    }

    InlineMap(const InlineMap& other)
        : m_size(other.m_size)
        , m_capacity(other.m_capacity)
    {
        if (isInline()) {
            for (unsigned i = 0; i < m_size; ++i)
                new (&inlineStorage()[i]) Entry(other.inlineStorage()[i]);
            return;
        }

        m_storage.heapEntries = static_cast<Entry*>(FastMalloc::malloc(allocationSizeForCapacity(m_capacity)));
        Entry* srcEntries = other.m_storage.heapEntries;
        Entry* destEntries = m_storage.heapEntries;

        for (unsigned i = 0; i < m_capacity; ++i) {
            if (isEmptyEntry(srcEntries[i]))
                constructEmptyEntry(destEntries[i]);
            else if (isDeletedEntry(srcEntries[i]))
                constructDeletedEntry(destEntries[i]);
            else
                new (&destEntries[i]) Entry(srcEntries[i]);
        }
    }

    InlineMap& operator=(InlineMap&& other)
    {
        if (this != &other) {
            std::destroy_at(this);
            new (this) InlineMap(WTF::move(other));
        }
        return *this;
    }

    InlineMap& operator=(const InlineMap& other)
    {
        if (this != &other) {
            std::destroy_at(this);
            new (this) InlineMap(other);
        }
        return *this;
    }

    ~InlineMap()
    {
        if (isInline()) {
            std::destroy_n(inlineStorage(), m_size);
        } else if (m_storage.heapEntries) {
            for (unsigned i = 0; i < m_capacity; ++i) {
                if (!isEmptyOrDeletedEntry(m_storage.heapEntries[i]))
                    std::destroy_at(&m_storage.heapEntries[i]);
            }
            FastMalloc::free(m_storage.heapEntries);
        }
    }

    class ValuesIterator;

    class iterator {
        friend class ValuesIterator;
    public:
        iterator() = default;

        iterator(Entry* current, Entry* end)
            : m_current(current)
            , m_end(end)
        {
        }

        Entry& operator*() const { return *m_current; }
        Entry* operator->() const { return m_current; }

        iterator& operator++()
        {
            ++m_current;
            skipEmpty();
            return *this;
        }

        bool operator==(const iterator& other) const { return m_current == other.m_current; }

        void skipEmpty()
        {
            while (m_current != m_end && isEmptyOrDeletedEntry(*m_current))
                ++m_current;
        }

        ValuesIterator values() const { return ValuesIterator(*this); }

    private:
        Entry* m_current { nullptr };
        Entry* m_end { nullptr };
    };

    class ValuesIterator {
    public:
        ValuesIterator() = default;
        ValuesIterator(const iterator& it) : m_impl(it) { }

        MappedType& operator*() const { return m_impl.m_current->value; }
        MappedType* operator->() const { return &m_impl.m_current->value; }

        ValuesIterator& operator++()
        {
            ++m_impl;
            return *this;
        }

        bool operator==(const ValuesIterator& other) const { return m_impl == other.m_impl; }

    private:
        iterator m_impl;
    };

    struct ValuesRange {
        ValuesIterator m_begin;
        ValuesIterator m_end;

        ValuesIterator begin() const { return m_begin; }
        ValuesIterator end() const { return m_end; }
    };

    using const_iterator = iterator;

    struct AddResult {
        iterator iterator;
        bool isNewEntry;
    };

    template<typename K, typename V>
    AddResult add(K&& key, V&& value) LIFETIME_BOUND
    {
        ASSERT(!isEmptyKey(key));

        unsigned size = m_size;
        if (isInline()) [[likely]] {
            Entry* entryStorage = inlineStorage();
            Entry* end = entryStorage + size;
            for (unsigned i = 0; i < size; ++i) {
                if (HashArg::equal(entryStorage[i].key, key))
                    return { iterator { &entryStorage[i], end }, false };
            }

            if (size < m_capacity) {
                Entry* slot = &entryStorage[size];
                new (slot) Entry(std::forward<K>(key), std::forward<V>(value));
                ++m_size;
                return { iterator { slot, entriesEnd() }, true };
            }

            // At capacity in linear mode: become hashed and fall through to the hashed add
            transitionToHashed();
        }

        if (m_size * loadFactorDenominator >= m_capacity * loadFactorNumerator) [[unlikely]]
            grow();

        Entry* end = entriesEnd();
        Entry* slot = findBucketForInsertion(key);
        if (!isEmptyOrDeletedEntry(*slot))
            return { iterator { slot, end }, false };

        // Slot is either empty or deleted - we can insert here
        new (slot) Entry(std::forward<K>(key), std::forward<V>(value));
        ++m_size;
        return { iterator { slot, end }, true };
    }

    template<typename K>
    bool contains(const K& key) const
    {
        if (m_size == 0)
            return false;

        if (isInline()) [[likely]] {
            Entry* entryStorage = const_cast<InlineMap*>(this)->inlineStorage();
            for (unsigned i = 0; i < m_size; ++i) {
                if (HashArg::equal(entryStorage[i].key, key))
                    return true;
            }
            return false;
        }

        return !isEmptyOrDeletedEntry(*findBucketForLookup(key));
    }

    template<typename K>
    iterator find(const K& key) LIFETIME_BOUND
    {
        if (m_size == 0)
            return iterator { nullptr, nullptr };

        if (isInline()) [[likely]] {
            Entry* entryStorage = inlineStorage();
            Entry* end = entryStorage + m_size;
            for (unsigned i = 0; i < m_size; ++i) {
                if (HashArg::equal(entryStorage[i].key, key))
                    return iterator { &entryStorage[i], end };
            }
            return iterator { end, end };
        }

        Entry* slot = findBucketForLookup(key);
        Entry* end = m_storage.heapEntries + m_capacity;
        if (isEmptyOrDeletedEntry(*slot))
            return iterator { end, end };
        return iterator { slot, end };
    }

    template<typename K>
    const_iterator find(const K& key) const LIFETIME_BOUND
    {
        return const_cast<InlineMap*>(this)->find(key);
    }

    template<typename K>
    bool remove(const K& key)
    {
        if (m_size == 0) [[unlikely]]
            return false;

        Entry* entryStorage = entries();

        if (isInline()) {
            // Find and swap with last entry. Inline mode should not use deleted entries.
            for (unsigned i = 0; i < m_size; ++i) {
                if (HashArg::equal(entryStorage[i].key, key)) [[unlikely]] {
                    std::destroy_at(&entryStorage[i]);
                    --m_size;
                    if (i != m_size) {
                        // Move last entry to fill the gap
                        new (&entryStorage[i]) Entry(WTF::move(entryStorage[m_size]));
                        // Assuming a moved-from entry doesn't need destruction
                    }
                    return true;
                }
            }
            return false;
        }

        // Hashed mode - mark as deleted
        Entry* slot = findBucketForLookup(key);
        if (isEmptyOrDeletedEntry(*slot))
            return false;

        std::destroy_at(slot);
        constructDeletedEntry(*slot);
        --m_size;
        return true;
    }

    iterator begin() LIFETIME_BOUND
    {
        if (m_size == 0)
            return iterator { nullptr, nullptr };

        Entry* entryStorage = entries();
        Entry* end = entriesEnd();
        iterator it { entryStorage, end };
        it.skipEmpty();
        return it;
    }

    const_iterator begin() const LIFETIME_BOUND
    {
        return const_cast<InlineMap*>(this)->begin();
    }

    iterator end() LIFETIME_BOUND
    {
        if (m_size == 0)
            return iterator { nullptr, nullptr };
        Entry* end = entriesEnd();
        return iterator { end, end };
    }

    const_iterator end() const LIFETIME_BOUND
    {
        return const_cast<InlineMap*>(this)->end();
    }

    ValuesRange values() LIFETIME_BOUND
    {
        return { begin().values(), end().values() };
    }

    ValuesRange values() const LIFETIME_BOUND
    {
        return const_cast<InlineMap*>(this)->values();
    }

    ALWAYS_INLINE unsigned size() const
    {
        return m_size;
    }

    ALWAYS_INLINE bool isEmpty() const { return m_size == 0; }

    void clear()
    {
        if (m_size == 0)
            return;

        Entry* entryStorage = entries();
        if (isInline())
            std::destroy_n(entryStorage, m_size);
        else {
            for (unsigned i = 0; i < m_capacity; ++i) {
                if (!isEmptyOrDeletedEntry(entryStorage[i]))
                    std::destroy_at(&entryStorage[i]);
                constructEmptyEntry(entryStorage[i]);
            }
        }
        m_size = 0;
    }

    ALWAYS_INLINE void swap(InlineMap& other)
    {
        // For now, implement as three moves
        InlineMap temp(WTF::move(*this));
        *this = WTF::move(other);
        other = WTF::move(temp);
    }

    void reserveInitialCapacity(unsigned keyCount)
    {
        RELEASE_ASSERT(m_size == 0 && m_capacity == InlineCapacity); // expecting a brand new map

        if (keyCount <= InlineCapacity) {
            // Already have inline storage
            return;
        }

        unsigned capacity = roundUpToPowerOfTwo(2 * keyCount * loadFactorDenominator / loadFactorNumerator);
        m_capacity = capacity;
        m_storage.heapEntries = static_cast<Entry*>(FastMalloc::malloc(allocationSizeForCapacity(capacity)));

        Entry* entryStorage = m_storage.heapEntries;
        for (unsigned i = 0; i < capacity; ++i)
            constructEmptyEntry(entryStorage[i]);
    }

private:
    friend class InlineMapAccessForTesting;

    ALWAYS_INLINE bool isInline() const { return m_capacity == InlineCapacity; }

    ALWAYS_INLINE Entry* inlineStorage()
    {
        return reinterpret_cast<Entry*>(&m_storage.inlineEntries);
    }

    ALWAYS_INLINE const Entry* inlineStorage() const
    {
        return reinterpret_cast<const Entry*>(&m_storage.inlineEntries);
    }

    ALWAYS_INLINE Entry* entries()
    {
        return isInline() ? inlineStorage() : m_storage.heapEntries;
    }

    ALWAYS_INLINE const Entry* entries() const
    {
        return isInline() ? inlineStorage() : m_storage.heapEntries;
    }

    ALWAYS_INLINE Entry* entriesEnd()
    {
        Entry* entryStorage = entries();
        return isInline()
            ? entryStorage + m_size
            : entryStorage + m_capacity;
    }

    ALWAYS_INLINE const Entry* entriesEnd() const
    {
        const Entry* entryStorage = entries();
        return isInline()
            ? entryStorage + m_size
            : entryStorage + m_capacity;
    }

    ALWAYS_INLINE static constexpr size_t allocationSizeForCapacity(size_t capacity)
    {
        return sizeof(Entry) * capacity;
    }

    ALWAYS_INLINE static bool isEmptyKey(const KeyType& key)
    {
        return isHashTraitsEmptyValue<HashTraits<KeyType>>(key);
    }

    ALWAYS_INLINE static bool isEmptyEntry(const Entry& entry)
    {
        return isEmptyKey(entry.key);
    }

    ALWAYS_INLINE static bool isDeletedEntry(const Entry& entry)
    {
        return HashTraits<KeyType>::isDeletedValue(entry.key);
    }

    ALWAYS_INLINE static bool isEmptyOrDeletedEntry(const Entry& entry)
    {
        return isEmptyEntry(entry) || isDeletedEntry(entry);
    }

    ALWAYS_INLINE static void constructDeletedEntry(Entry& entry)
    {
        if constexpr (EntryTraits::emptyValueIsZero) {
            // For zero-initialized types, we can zero the memory first, then set the deleted marker
            zeroBytes(entry);
            KeyTraits::constructDeletedValue(entry.key);
        } else {
            // Create the deleted key value separately to avoid construction/destruction cycle
            typename KeyTraits::TraitType deletedKey = KeyTraits::emptyValue();
            KeyTraits::constructDeletedValue(deletedKey);

            // Construct the Entry directly with the deleted key and empty value
            // This avoids constructing an empty Entry first and then overwriting it
            new (NotNull, std::addressof(entry)) Entry(WTF::move(deletedKey), MappedTraits::emptyValue());
        }
    }

    ALWAYS_INLINE static void constructEmptyEntry(Entry& entry)
    {
        if constexpr (EntryTraits::emptyValueIsZero)
            zeroBytes(entry);
        else
            new (NotNull, std::addressof(entry)) Entry(KeyTraits::emptyValue(), MappedTraits::emptyValue());
    }

    static constexpr unsigned loadFactorNumerator = 3;
    static constexpr unsigned loadFactorDenominator = 4;

    void transitionToHashed()
    {
        ASSERT(isInline());
        unsigned oldSize = m_size;
        Entry* oldEntries = inlineStorage();

        // New capacity should allow having twice as many entries before hitting the load factor.
        unsigned newCapacity = roundUpToPowerOfTwo(oldSize * 2 * loadFactorDenominator / loadFactorNumerator);
        Entry* newEntries = static_cast<Entry*>(FastMalloc::malloc(allocationSizeForCapacity(newCapacity)));

        // Initialize new buffer with empty entries
        for (unsigned i = 0; i < newCapacity; ++i)
            constructEmptyEntry(newEntries[i]);

        // Move old entries directly into new buffer
        for (unsigned i = 0; i < oldSize; ++i) {
            Entry* slot = findBucketForLookupInStorage(oldEntries[i].key, newEntries, newCapacity);
            ASSERT(isEmptyEntry(*slot));
            new (slot) Entry(WTF::move(oldEntries[i]));
        }

        // We assume the moved-from entries in the old storage don't need destruction
        m_capacity = newCapacity;
        m_storage.heapEntries = newEntries;
    }

    void grow()
    {
        ASSERT(!isInline());
        unsigned oldCapacity = m_capacity;
        Entry* oldEntries = m_storage.heapEntries;

        unsigned newCapacity = oldCapacity * 2;
        Entry* newEntries = static_cast<Entry*>(FastMalloc::malloc(allocationSizeForCapacity(newCapacity)));

        // Initialize new buffer with empty entries
        for (unsigned i = 0; i < newCapacity; ++i)
            constructEmptyEntry(newEntries[i]);

        // Rehash old entries into new buffer
        for (unsigned i = 0; i < oldCapacity; ++i) {
            if (!isEmptyOrDeletedEntry(oldEntries[i])) {
                Entry* slot = findBucketForLookupInStorage(oldEntries[i].key, newEntries, newCapacity);
                ASSERT(isEmptyEntry(*slot));
                new (slot) Entry(WTF::move(oldEntries[i]));
            }
        }

        m_capacity = newCapacity;
        m_storage.heapEntries = newEntries;
        // We assume the moved-from entries in the old storage don't need destruction
        FastMalloc::free(oldEntries);
    }

    template<typename K>
    ALWAYS_INLINE static Entry* findBucketForLookupInStorage(const K& key, Entry* data, unsigned capacity)
    {
        ASSERT(isPowerOfTwo(capacity));

        unsigned hash = HashArg::hash(key);
        unsigned mask = capacity - 1;
        unsigned bucket = hash & mask;
        unsigned probe = 0;

        while (true) {
            Entry* entry = &data[bucket];
            if (isEmptyEntry(*entry)) [[likely]]
                return entry;
            if (!isDeletedEntry(*entry) && HashArg::equal(entry->key, key))
                return entry;
            ++probe;
            bucket = (bucket + probe) & mask;
        }
    }

    // If the key is not found, return the empty bucket where the search ended. This is for lookup
    // only. For insertions we want to use the "for insertions" counterpart.
    template<typename K>
    Entry* findBucketForLookup(const K& key) const
    {
        ASSERT(!isInline());
        return findBucketForLookupInStorage(key, m_storage.heapEntries, m_capacity);
    }

    // If the key is not found but there was a deleted bucket on the search path, return that
    // bucket so it gets filled instead.
    Entry* findBucketForInsertion(const KeyType& key) const
    {
        ASSERT(!isInline());
        ASSERT(isPowerOfTwo(m_capacity));

        Entry* data = m_storage.heapEntries;
        unsigned hash = HashArg::hash(key);
        unsigned mask = m_capacity - 1;
        unsigned bucket = hash & mask;
        unsigned probe = 0;
        Entry* firstDeletedSlot = nullptr;

        while (true) {
            Entry* entry = &data[bucket];
            if (isEmptyEntry(*entry)) [[likely]]
                return firstDeletedSlot ? firstDeletedSlot : entry;
            if (isDeletedEntry(*entry)) {
                if (!firstDeletedSlot)
                    firstDeletedSlot = entry;
            } else if (HashArg::equal(entry->key, key))
                return entry;
            ++probe;
            bucket = (bucket + probe) & mask;
        }
    }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    unsigned m_size;
    unsigned m_capacity;
    union Storage {
        struct alignas(Entry) EntryBytes {
            std::byte data[sizeof(Entry)];
        };
        std::array<EntryBytes, InlineCapacity> inlineEntries;
        Entry* heapEntries;
    } m_storage;
};

class InlineMapAccessForTesting {
public:
    template<typename KeyArg, typename ValueArg, typename HashArg, typename KeyTraitsArg, typename MappedTraitsArg, unsigned InlineCapacity>
    static bool isInline(InlineMap<KeyArg, ValueArg, HashArg, KeyTraitsArg, MappedTraitsArg, InlineCapacity>& map)
    {
        return map.isInline();
    }

    template<typename KeyArg, typename ValueArg, typename HashArg, typename KeyTraitsArg, typename MappedTraitsArg, unsigned InlineCapacity>
    static unsigned capacity(InlineMap<KeyArg, ValueArg, HashArg, KeyTraitsArg, MappedTraitsArg, InlineCapacity>& map)
    {
        return map.m_capacity;
    }
};

} // namespace WTF

using WTF::InlineMap;
