// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/string-table.h"

#include <atomic>

#include "src/base/atomicops.h"
#include "src/base/macros.h"
#include "src/common/assert-scope.h"
#include "src/common/globals.h"
#include "src/common/ptr-compr-inl.h"
#include "src/execution/isolate-utils-inl.h"
#include "src/heap/safepoint.h"
#include "src/objects/internal-index.h"
#include "src/objects/object-list-macros.h"
#include "src/objects/slots-inl.h"
#include "src/objects/slots.h"
#include "src/objects/string-inl.h"
#include "src/objects/string-table-inl.h"
#include "src/snapshot/deserializer.h"
#include "src/utils/allocation.h"
#include "src/utils/ostreams.h"

namespace v8 {
namespace internal {

namespace {

static constexpr int kStringTableMaxEmptyFactor = 4;
static constexpr int kStringTableMinCapacity = 2048;

bool StringTableHasSufficientCapacityToAdd(int capacity, int number_of_elements,
                                           int number_of_deleted_elements,
                                           int number_of_additional_elements) {
  int nof = number_of_elements + number_of_additional_elements;
  // Return true if:
  //   50% is still free after adding number_of_additional_elements elements and
  //   at most 50% of the free elements are deleted elements.
  if ((nof < capacity) &&
      ((number_of_deleted_elements <= (capacity - nof) / 2))) {
    int needed_free = nof / 2;
    if (nof + needed_free <= capacity) return true;
  }
  return false;
}

int ComputeStringTableCapacity(int at_least_space_for) {
  // Add 50% slack to make slot collisions sufficiently unlikely.
  // See matching computation in StringTableHasSufficientCapacityToAdd().
  int raw_capacity = at_least_space_for + (at_least_space_for >> 1);
  int capacity = base::bits::RoundUpToPowerOfTwo32(raw_capacity);
  return std::max(capacity, kStringTableMinCapacity);
}

int ComputeStringTableCapacityWithShrink(int current_capacity,
                                         int at_least_room_for) {
  // Only shrink if the table is very empty to avoid performance penalty.
  DCHECK_GE(current_capacity, kStringTableMinCapacity);
  if (at_least_room_for > (current_capacity / kStringTableMaxEmptyFactor))
    return current_capacity;

  // Recalculate the smaller capacity actually needed.
  int new_capacity = ComputeStringTableCapacity(at_least_room_for);
  DCHECK_GE(new_capacity, at_least_room_for);
  // Don't go lower than room for {kStringTableMinCapacity} elements.
  if (new_capacity < kStringTableMinCapacity) return current_capacity;
  return new_capacity;
}

template <typename IsolateT, typename StringTableKey>
bool KeyIsMatch(IsolateT* isolate, StringTableKey* key, String string) {
  if (string.hash() != key->hash()) return false;
  if (string.length() != key->length()) return false;
  return key->IsMatch(isolate, string);
}

}  // namespace

// Data holds the actual data of the string table, including capacity and number
// of elements.
//
// It is a variable sized structure, with a "header" followed directly in memory
// by the elements themselves. These are accessed as offsets from the elements_
// field, which itself provides storage for the first element.
//
// The elements themselves are stored as an open-addressed hash table, with
// quadratic probing and Smi 0 and Smi 1 as the empty and deleted sentinels,
// respectively.
class StringTable::Data {
 public:
  static std::unique_ptr<Data> New(int capacity);
  static std::unique_ptr<Data> Resize(PtrComprCageBase cage_base,
                                      std::unique_ptr<Data> data, int capacity);

  OffHeapObjectSlot slot(InternalIndex index) const {
    return OffHeapObjectSlot(&elements_[index.as_uint32()]);
  }

  Object Get(PtrComprCageBase cage_base, InternalIndex index) const {
    return slot(index).Acquire_Load(cage_base);
  }

  void Set(InternalIndex index, String entry) {
    slot(index).Release_Store(entry);
  }

  void ElementAdded() {
    DCHECK_LT(number_of_elements_ + 1, capacity());
    DCHECK(StringTableHasSufficientCapacityToAdd(
        capacity(), number_of_elements(), number_of_deleted_elements(), 1));

    number_of_elements_++;
  }
  void DeletedElementOverwritten() {
    DCHECK_LT(number_of_elements_ + 1, capacity());
    DCHECK(StringTableHasSufficientCapacityToAdd(
        capacity(), number_of_elements(), number_of_deleted_elements() - 1, 1));

    number_of_elements_++;
    number_of_deleted_elements_--;
  }
  void ElementsRemoved(int count) {
    DCHECK_LE(count, number_of_elements_);
    number_of_elements_ -= count;
    number_of_deleted_elements_ += count;
  }

  void* operator new(size_t size, int capacity);
  void* operator new(size_t size) = delete;
  void operator delete(void* description);

  int capacity() const { return capacity_; }
  int number_of_elements() const { return number_of_elements_; }
  int number_of_deleted_elements() const { return number_of_deleted_elements_; }

  template <typename IsolateT, typename StringTableKey>
  InternalIndex FindEntry(IsolateT* isolate, StringTableKey* key,
                          uint32_t hash) const;

  InternalIndex FindInsertionEntry(PtrComprCageBase cage_base,
                                   uint32_t hash) const;

  template <typename IsolateT, typename StringTableKey>
  InternalIndex FindEntryOrInsertionEntry(IsolateT* isolate,
                                          StringTableKey* key,
                                          uint32_t hash) const;

  // Helper method for StringTable::TryStringToIndexOrLookupExisting.
  template <typename Char>
  static Address TryStringToIndexOrLookupExisting(Isolate* isolate,
                                                  String string, String source,
                                                  size_t start);

  void IterateElements(RootVisitor* visitor);

  Data* PreviousData() { return previous_data_.get(); }
  void DropPreviousData() { previous_data_.reset(); }

  void Print(PtrComprCageBase cage_base) const;
  size_t GetCurrentMemoryUsage() const;

 private:
  explicit Data(int capacity);

  // Returns probe entry.
  inline static InternalIndex FirstProbe(uint32_t hash, uint32_t size) {
    return InternalIndex(hash & (size - 1));
  }

  inline static InternalIndex NextProbe(InternalIndex last, uint32_t number,
                                        uint32_t size) {
    return InternalIndex((last.as_uint32() + number) & (size - 1));
  }

 private:
  std::unique_ptr<Data> previous_data_;
  int number_of_elements_;
  int number_of_deleted_elements_;
  const int capacity_;
  Tagged_t elements_[1];
};

void* StringTable::Data::operator new(size_t size, int capacity) {
  // Make sure the size given is the size of the Data structure.
  DCHECK_EQ(size, sizeof(StringTable::Data));
  // Make sure that the elements_ array is at the end of Data, with no padding,
  // so that subsequent elements can be accessed as offsets from elements_.
  static_assert(offsetof(StringTable::Data, elements_) ==
                sizeof(StringTable::Data) - sizeof(Tagged_t));
  // Make sure that elements_ is aligned when StringTable::Data is aligned.
  static_assert(
      (alignof(StringTable::Data) + offsetof(StringTable::Data, elements_)) %
          kTaggedSize ==
      0);

  // Subtract 1 from capacity, as the member elements_ already supplies the
  // storage for the first element.
  return AlignedAlloc(size + (capacity - 1) * sizeof(Tagged_t),
                      alignof(StringTable::Data));
}

void StringTable::Data::operator delete(void* table) { AlignedFree(table); }

size_t StringTable::Data::GetCurrentMemoryUsage() const {
  size_t usage = sizeof(*this) + (capacity_ - 1) * sizeof(Tagged_t);
  if (previous_data_) {
    usage += previous_data_->GetCurrentMemoryUsage();
  }
  return usage;
}

StringTable::Data::Data(int capacity)
    : previous_data_(nullptr),
      number_of_elements_(0),
      number_of_deleted_elements_(0),
      capacity_(capacity) {
  OffHeapObjectSlot first_slot = slot(InternalIndex(0));
  MemsetTagged(first_slot, empty_element(), capacity);
}

std::unique_ptr<StringTable::Data> StringTable::Data::New(int capacity) {
  return std::unique_ptr<Data>(new (capacity) Data(capacity));
}

std::unique_ptr<StringTable::Data> StringTable::Data::Resize(
    PtrComprCageBase cage_base, std::unique_ptr<Data> data, int capacity) {
  std::unique_ptr<Data> new_data(new (capacity) Data(capacity));

  DCHECK_LT(data->number_of_elements(), new_data->capacity());
  DCHECK(StringTableHasSufficientCapacityToAdd(
      new_data->capacity(), new_data->number_of_elements(),
      new_data->number_of_deleted_elements(), data->number_of_elements()));

  // Rehash the elements.
  for (InternalIndex i : InternalIndex::Range(data->capacity())) {
    Object element = data->Get(cage_base, i);
    if (element == empty_element() || element == deleted_element()) continue;
    String string = String::cast(element);
    uint32_t hash = string.hash();
    InternalIndex insertion_index =
        new_data->FindInsertionEntry(cage_base, hash);
    new_data->Set(insertion_index, string);
  }
  new_data->number_of_elements_ = data->number_of_elements();

  new_data->previous_data_ = std::move(data);
  return new_data;
}

template <typename IsolateT, typename StringTableKey>
InternalIndex StringTable::Data::FindEntry(IsolateT* isolate,
                                           StringTableKey* key,
                                           uint32_t hash) const {
  uint32_t count = 1;
  // EnsureCapacity will guarantee the hash table is never full.
  for (InternalIndex entry = FirstProbe(hash, capacity_);;
       entry = NextProbe(entry, count++, capacity_)) {
    // TODO(leszeks): Consider delaying the decompression until after the
    // comparisons against empty/deleted.
    Object element = Get(isolate, entry);
    if (element == empty_element()) return InternalIndex::NotFound();
    if (element == deleted_element()) continue;
    String string = String::cast(element);
    if (KeyIsMatch(isolate, key, string)) return entry;
  }
}

InternalIndex StringTable::Data::FindInsertionEntry(PtrComprCageBase cage_base,
                                                    uint32_t hash) const {
  uint32_t count = 1;
  // EnsureCapacity will guarantee the hash table is never full.
  for (InternalIndex entry = FirstProbe(hash, capacity_);;
       entry = NextProbe(entry, count++, capacity_)) {
    // TODO(leszeks): Consider delaying the decompression until after the
    // comparisons against empty/deleted.
    Object element = Get(cage_base, entry);
    if (element == empty_element() || element == deleted_element())
      return entry;
  }
}

template <typename IsolateT, typename StringTableKey>
InternalIndex StringTable::Data::FindEntryOrInsertionEntry(
    IsolateT* isolate, StringTableKey* key, uint32_t hash) const {
  InternalIndex insertion_entry = InternalIndex::NotFound();
  uint32_t count = 1;
  // EnsureCapacity will guarantee the hash table is never full.
  for (InternalIndex entry = FirstProbe(hash, capacity_);;
       entry = NextProbe(entry, count++, capacity_)) {
    // TODO(leszeks): Consider delaying the decompression until after the
    // comparisons against empty/deleted.
    Object element = Get(isolate, entry);
    if (element == empty_element()) {
      // Empty entry, it's our insertion entry if there was no previous Hole.
      if (insertion_entry.is_not_found()) return entry;
      return insertion_entry;
    }

    if (element == deleted_element()) {
      // Holes are potential insertion candidates, but we continue the search
      // in case we find the actual matching entry.
      if (insertion_entry.is_not_found()) insertion_entry = entry;
      continue;
    }

    String string = String::cast(element);
    if (KeyIsMatch(isolate, key, string)) return entry;
  }
}

void StringTable::Data::IterateElements(RootVisitor* visitor) {
  OffHeapObjectSlot first_slot = slot(InternalIndex(0));
  OffHeapObjectSlot end_slot = slot(InternalIndex(capacity_));
  visitor->VisitRootPointers(Root::kStringTable, nullptr, first_slot, end_slot);
}

void StringTable::Data::Print(PtrComprCageBase cage_base) const {
  OFStream os(stdout);
  os << "StringTable {" << std::endl;
  for (InternalIndex i : InternalIndex::Range(capacity_)) {
    os << "  " << i.as_uint32() << ": " << Brief(Get(cage_base, i))
       << std::endl;
  }
  os << "}" << std::endl;
}

StringTable::StringTable(Isolate* isolate)
    : data_(Data::New(kStringTableMinCapacity).release()), isolate_(isolate) {}

StringTable::~StringTable() { delete data_; }

int StringTable::Capacity() const {
  return data_.load(std::memory_order_acquire)->capacity();
}
int StringTable::NumberOfElements() const {
  {
    base::MutexGuard table_write_guard(&write_mutex_);
    return data_.load(std::memory_order_relaxed)->number_of_elements();
  }
}

// InternalizedStringKey carries a string/internalized-string object as key.
class InternalizedStringKey final : public StringTableKey {
 public:
  explicit InternalizedStringKey(Handle<String> string, uint32_t hash)
      : StringTableKey(hash, string->length()), string_(string) {
    // When sharing the string table, it's possible that another thread already
    // internalized the key, in which case StringTable::LookupKey will perform a
    // redundant lookup and return the already internalized copy.
    DCHECK_IMPLIES(!FLAG_shared_string_table, !string->IsInternalizedString());
    DCHECK(string->IsFlat());
    DCHECK(String::IsHashFieldComputed(hash));
  }

  bool IsMatch(Isolate* isolate, String string) {
    DCHECK(!SharedStringAccessGuardIfNeeded::IsNeeded(string));
    return string_->SlowEquals(string);
  }

  void PrepareForInsertion(Isolate* isolate) {
    StringTransitionStrategy strategy =
        isolate->factory()->ComputeInternalizationStrategyForString(
            string_, &maybe_internalized_map_);
    switch (strategy) {
      case StringTransitionStrategy::kCopy:
        break;
      case StringTransitionStrategy::kInPlace:
        // In-place transition will be done in GetHandleForInsertion, when we
        // are sure that we are going to insert the string into the table.
        return;
      case StringTransitionStrategy::kAlreadyTransitioned:
        // We can see already internalized strings here only when sharing the
        // string table and allowing concurrent internalization.
        DCHECK(FLAG_shared_string_table);
        return;
    }

    // Copying the string here is always threadsafe, as no instance type
    // requiring a copy can transition any further.
    StringShape shape(*string_);
    // External strings get special treatment, to avoid copying their
    // contents as long as they are not uncached.
    if (shape.IsExternalOneByte() && !shape.IsUncachedExternal()) {
      // TODO(syg): External strings not yet supported.
      DCHECK(!FLAG_shared_string_table);
      string_ =
          isolate->factory()->InternalizeExternalString<ExternalOneByteString>(
              string_);
    } else if (shape.IsExternalTwoByte() && !shape.IsUncachedExternal()) {
      // TODO(syg): External strings not yet supported.
      DCHECK(!FLAG_shared_string_table);
      string_ =
          isolate->factory()->InternalizeExternalString<ExternalTwoByteString>(
              string_);
    } else {
      // Otherwise allocate a new internalized string.
      string_ = isolate->factory()->NewInternalizedStringImpl(
          string_, string_->length(), string_->raw_hash_field());
    }
  }

  Handle<String> GetHandleForInsertion() {
    Handle<Map> internalized_map;
    // When preparing the string, the strategy was to in-place migrate it.
    if (maybe_internalized_map_.ToHandle(&internalized_map)) {
      // It is always safe to overwrite the map. The only transition possible
      // is another thread migrated the string to internalized already.
      // Migrations to thin are impossible, as we only call this method on table
      // misses inside the critical section.
      string_->set_map_no_write_barrier(*internalized_map);
      DCHECK(string_->IsInternalizedString());
      return string_;
    }
    // We prepared an internalized copy for the string or the string was already
    // internalized.
    // In theory we could have created a copy of a SeqString in young generation
    // that has been promoted to old space by now. In that case we could
    // in-place migrate the original string instead of internalizing the copy
    // and migrating the original string to a ThinString. This scenario doesn't
    // seem to be common enough to justify re-computing the strategy here.
    return string_;
  }

 private:
  Handle<String> string_;
  MaybeHandle<Map> maybe_internalized_map_;
};

namespace {

void SetInternalizedReference(Isolate* isolate, String string,
                              String internalized) {
  // TODO(v8:12007): Support external strings.
  if ((string.IsShared() || FLAG_always_use_string_forwarding_table) &&
      !string.IsExternalString()) {
    uint32_t field = string.raw_hash_field();
    // Don't use the forwarding table for strings that have an integer index.
    // Using the hash field for the integer index is more beneficial than
    // using it to store the forwarding index to the internalized string.
    if (Name::IsIntegerIndex(field)) return;

    const int forwarding_index =
        isolate->string_forwarding_table()->Add(isolate, string, internalized);
    string.set_raw_hash_field(
        String::CreateHashFieldValue(forwarding_index,
                                     String::HashFieldType::kForwardingIndex),
        kReleaseStore);
  } else {
    string.MakeThin(isolate, internalized);
  }
}

}  // namespace

Handle<String> StringTable::LookupString(Isolate* isolate,
                                         Handle<String> string) {
  // When sharing the string table, internalization is allowed to be concurrent
  // from multiple Isolates, assuming that:
  //
  //  - All in-place internalizable strings (i.e. old-generation flat strings)
  //    and internalized strings are in the shared heap.
  //  - LookupKey supports concurrent access (see comment below).
  //
  // These assumptions guarantee the following properties:
  //
  //  - String::Flatten is not threadsafe but is only called on non-shared
  //    strings, since non-flat strings are not shared.
  //
  //  - String::ComputeAndSetHash is threadsafe on flat strings. This is safe
  //    because the characters are immutable and the same hash will be
  //    computed. The hash field is set with relaxed memory order. A thread that
  //    doesn't see the hash may do redundant work but will not be incorrect.
  //
  //  - In-place internalizable strings do not incur a copy regardless of string
  //    table sharing. The map mutation is threadsafe even with relaxed memory
  //    order, because for concurrent table lookups, the "losing" thread will be
  //    correctly ordered by LookupKey's write mutex and see the updated map
  //    during the re-lookup.
  //
  // For lookup misses, the internalized string map is the same map in RO space
  // regardless of which thread is doing the lookup.
  //
  // For lookup hits, we use the StringForwardingTable for shared strings to
  // delay the transition into a ThinString to the next stop-the-world GC.
  Handle<String> result = String::Flatten(isolate, string);
  if (!result->IsInternalizedString()) {
    result->EnsureHash();
    uint32_t raw_hash_field = result->raw_hash_field(kAcquireLoad);

    if (String::IsForwardingIndex(raw_hash_field)) {
      const int index = String::HashBits::decode(raw_hash_field);
      result = handle(
          isolate->string_forwarding_table()->GetForwardString(isolate, index),
          isolate);
    } else {
      InternalizedStringKey key(result, raw_hash_field);
      result = LookupKey(isolate, &key);
    }
  }
  if (*string != *result && !string->IsThinString()) {
    SetInternalizedReference(isolate, *string, *result);
  }
  return result;
}

template <typename StringTableKey, typename IsolateT>
Handle<String> StringTable::LookupKey(IsolateT* isolate, StringTableKey* key) {
  // String table lookups are allowed to be concurrent, assuming that:
  //
  //   - The Heap access is allowed to be concurrent (using LocalHeap or
  //     similar),
  //   - All writes to the string table are guarded by the Isolate string table
  //     mutex,
  //   - Resizes of the string table first copies the old contents to the new
  //     table, and only then sets the new string table pointer to the new
  //     table,
  //   - Only GCs can remove elements from the string table.
  //
  // These assumptions allow us to make the following statement:
  //
  //   "Reads are allowed when not holding the lock, as long as false negatives
  //    (misses) are ok. We will never get a false positive (hit of an entry no
  //    longer in the table)"
  //
  // This is because we _know_ that if we find an entry in the string table, any
  // entry will also be in all reallocations of that tables. This is required
  // for strong consistency of internalized string equality implying reference
  // equality.
  //
  // We therefore try to optimistically read from the string table without
  // taking the lock (both here and in the NoAllocate version of the lookup),
  // and on a miss we take the lock and try to write the entry, with a second
  // read lookup in case the non-locked read missed a write.
  //
  // One complication is allocation -- we don't want to allocate while holding
  // the string table lock. This applies to both allocation of new strings, and
  // re-allocation of the string table on resize. So, we optimistically allocate
  // (without copying values) outside the lock, and potentially discard the
  // allocation if another write also did an allocation. This assumes that
  // writes are rarer than reads.

  // Load the current string table data, in case another thread updates the
  // data while we're reading.
  const Data* current_data = data_.load(std::memory_order_acquire);

  // First try to find the string in the table. This is safe to do even if the
  // table is now reallocated; we won't find a stale entry in the old table
  // because the new table won't delete it's corresponding entry until the
  // string is dead, in which case it will die in this table too and worst
  // case we'll have a false miss.
  InternalIndex entry = current_data->FindEntry(isolate, key, key->hash());
  if (entry.is_found()) {
    Handle<String> result(String::cast(current_data->Get(isolate, entry)),
                          isolate);
    DCHECK_IMPLIES(FLAG_shared_string_table, result->InSharedHeap());
    return result;
  }

  // No entry found, so adding new string.
  key->PrepareForInsertion(isolate);
  {
    base::MutexGuard table_write_guard(&write_mutex_);

    Data* data = EnsureCapacity(isolate, 1);

    // Check one last time if the key is present in the table, in case it was
    // added after the check.
    entry = data->FindEntryOrInsertionEntry(isolate, key, key->hash());

    Object element = data->Get(isolate, entry);
    if (element == empty_element()) {
      // This entry is empty, so write it and register that we added an
      // element.
      Handle<String> new_string = key->GetHandleForInsertion();
      DCHECK_IMPLIES(FLAG_shared_string_table, new_string->IsShared());
      data->Set(entry, *new_string);
      data->ElementAdded();
      return new_string;
    } else if (element == deleted_element()) {
      // This entry was deleted, so overwrite it and register that we
      // overwrote a deleted element.
      Handle<String> new_string = key->GetHandleForInsertion();
      DCHECK_IMPLIES(FLAG_shared_string_table, new_string->IsShared());
      data->Set(entry, *new_string);
      data->DeletedElementOverwritten();
      return new_string;
    } else {
      // Return the existing string as a handle.
      return handle(String::cast(element), isolate);
    }
  }
}

template Handle<String> StringTable::LookupKey(Isolate* isolate,
                                               OneByteStringKey* key);
template Handle<String> StringTable::LookupKey(Isolate* isolate,
                                               TwoByteStringKey* key);
template Handle<String> StringTable::LookupKey(Isolate* isolate,
                                               SeqOneByteSubStringKey* key);
template Handle<String> StringTable::LookupKey(Isolate* isolate,
                                               SeqTwoByteSubStringKey* key);

template Handle<String> StringTable::LookupKey(LocalIsolate* isolate,
                                               OneByteStringKey* key);
template Handle<String> StringTable::LookupKey(LocalIsolate* isolate,
                                               TwoByteStringKey* key);

template Handle<String> StringTable::LookupKey(Isolate* isolate,
                                               StringTableInsertionKey* key);
template Handle<String> StringTable::LookupKey(LocalIsolate* isolate,
                                               StringTableInsertionKey* key);

StringTable::Data* StringTable::EnsureCapacity(PtrComprCageBase cage_base,
                                               int additional_elements) {
  // This call is only allowed while the write mutex is held.
  write_mutex_.AssertHeld();

  // This load can be relaxed as the table pointer can only be modified while
  // the lock is held.
  Data* data = data_.load(std::memory_order_relaxed);

  // Grow or shrink table if needed. We first try to shrink the table, if it
  // is sufficiently empty; otherwise we make sure to grow it so that it has
  // enough space.
  int current_capacity = data->capacity();
  int current_nof = data->number_of_elements();
  int capacity_after_shrinking =
      ComputeStringTableCapacityWithShrink(current_capacity, current_nof + 1);

  int new_capacity = -1;
  if (capacity_after_shrinking < current_capacity) {
    DCHECK(StringTableHasSufficientCapacityToAdd(capacity_after_shrinking,
                                                 current_nof, 0, 1));
    new_capacity = capacity_after_shrinking;
  } else if (!StringTableHasSufficientCapacityToAdd(
                 current_capacity, current_nof,
                 data->number_of_deleted_elements(), 1)) {
    new_capacity = ComputeStringTableCapacity(current_nof + 1);
  }

  if (new_capacity != -1) {
    std::unique_ptr<Data> new_data =
        Data::Resize(cage_base, std::unique_ptr<Data>(data), new_capacity);
    // `new_data` is the new owner of `data`.
    DCHECK_EQ(new_data->PreviousData(), data);
    // Release-store the new data pointer as `data_`, so that it can be
    // acquire-loaded by other threads. This string table becomes the owner of
    // the pointer.
    data = new_data.release();
    data_.store(data, std::memory_order_release);
  }

  return data;
}

// static
template <typename Char>
Address StringTable::Data::TryStringToIndexOrLookupExisting(Isolate* isolate,
                                                            String string,
                                                            String source,
                                                            size_t start) {
  // TODO(leszeks): This method doesn't really belong on StringTable::Data.
  // Ideally it would be a free function in an anonymous namespace, but that
  // causes issues around method and class visibility.

  DisallowGarbageCollection no_gc;

  int length = string.length();
  // The source hash is usable if it is not from a sliced string.
  // For sliced strings we need to recalculate the hash from the given offset
  // with the correct length.
  const bool is_source_hash_usable = start == 0 && length == source.length();

  // First check if the string constains a forwarding index.
  uint32_t raw_hash_field = source.raw_hash_field(kAcquireLoad);
  if (Name::IsForwardingIndex(raw_hash_field) && is_source_hash_usable) {
    const int index = Name::HashBits::decode(raw_hash_field);
    String internalized =
        isolate->string_forwarding_table()->GetForwardString(isolate, index);
    return internalized.ptr();
  }

  uint64_t seed = HashSeed(isolate);

  std::unique_ptr<Char[]> buffer;
  const Char* chars;

  SharedStringAccessGuardIfNeeded access_guard(isolate);
  if (source.IsConsString(isolate)) {
    DCHECK(!source.IsFlat(isolate));
    buffer.reset(new Char[length]);
    String::WriteToFlat(source, buffer.get(), 0, length, isolate, access_guard);
    chars = buffer.get();
  } else {
    chars = source.GetChars<Char>(isolate, no_gc, access_guard) + start;
  }

  if (!Name::IsHashFieldComputed(raw_hash_field) || !is_source_hash_usable) {
    raw_hash_field =
        StringHasher::HashSequentialString<Char>(chars, length, seed);
  }
  // TODO(verwaest): Internalize to one-byte when possible.
  SequentialStringKey<Char> key(raw_hash_field,
                                base::Vector<const Char>(chars, length), seed);

  // String could be an array index.
  if (Name::ContainsCachedArrayIndex(raw_hash_field)) {
    return Smi::FromInt(String::ArrayIndexValueBits::decode(raw_hash_field))
        .ptr();
  }

  if (Name::IsIntegerIndex(raw_hash_field)) {
    // It is an index, but it's not cached.
    return Smi::FromInt(ResultSentinel::kUnsupported).ptr();
  }

  Data* string_table_data =
      isolate->string_table()->data_.load(std::memory_order_acquire);

  InternalIndex entry = string_table_data->FindEntry(isolate, &key, key.hash());
  if (entry.is_not_found()) {
    // A string that's not an array index, and not in the string table,
    // cannot have been used as a property name before.
    return Smi::FromInt(ResultSentinel::kNotFound).ptr();
  }

  String internalized = String::cast(string_table_data->Get(isolate, entry));
  // string can be internalized here, if another thread internalized it.
  // If we found and entry in the string table and string is not internalized,
  // there is no way that it can transition to internalized later on. So a last
  // check here is sufficient.
  if (!string.IsInternalizedString()) {
    SetInternalizedReference(isolate, string, internalized);
  } else {
    DCHECK(FLAG_shared_string_table);
  }
  return internalized.ptr();
}

// static
Address StringTable::TryStringToIndexOrLookupExisting(Isolate* isolate,
                                                      Address raw_string) {
  String string = String::cast(Object(raw_string));
  if (string.IsInternalizedString()) {
    // string could be internalized, if the string table is shared and another
    // thread internalized it.
    DCHECK(FLAG_shared_string_table);
    return raw_string;
  }

  // Valid array indices are >= 0, so they cannot be mixed up with any of
  // the result sentinels, which are negative.
  static_assert(
      !String::ArrayIndexValueBits::is_valid(ResultSentinel::kUnsupported));
  static_assert(
      !String::ArrayIndexValueBits::is_valid(ResultSentinel::kNotFound));

  size_t start = 0;
  String source = string;
  if (source.IsSlicedString()) {
    SlicedString sliced = SlicedString::cast(source);
    start = sliced.offset();
    source = sliced.parent();
  } else if (source.IsConsString() && source.IsFlat()) {
    source = ConsString::cast(source).first();
  }
  if (source.IsThinString()) {
    source = ThinString::cast(source).actual();
    if (string.length() == source.length()) {
      return source.ptr();
    }
  }

  if (source.IsOneByteRepresentation()) {
    return StringTable::Data::TryStringToIndexOrLookupExisting<uint8_t>(
        isolate, string, source, start);
  }
  return StringTable::Data::TryStringToIndexOrLookupExisting<uint16_t>(
      isolate, string, source, start);
}

void StringTable::Print(PtrComprCageBase cage_base) const {
  data_.load(std::memory_order_acquire)->Print(cage_base);
}

size_t StringTable::GetCurrentMemoryUsage() const {
  return sizeof(*this) +
         data_.load(std::memory_order_acquire)->GetCurrentMemoryUsage();
}

void StringTable::IterateElements(RootVisitor* visitor) {
  // This should only happen during garbage collection when background threads
  // are paused, so the load can be relaxed.
  isolate_->heap()->safepoint()->AssertActive();
  data_.load(std::memory_order_relaxed)->IterateElements(visitor);
}

void StringTable::DropOldData() {
  // This should only happen during garbage collection when background threads
  // are paused, so the load can be relaxed.
  isolate_->heap()->safepoint()->AssertActive();
  DCHECK_NE(isolate_->heap()->gc_state(), Heap::NOT_IN_GC);
  data_.load(std::memory_order_relaxed)->DropPreviousData();
}

void StringTable::NotifyElementsRemoved(int count) {
  // This should only happen during garbage collection when background threads
  // are paused, so the load can be relaxed.
  isolate_->heap()->safepoint()->AssertActive();
  DCHECK_NE(isolate_->heap()->gc_state(), Heap::NOT_IN_GC);
  data_.load(std::memory_order_relaxed)->ElementsRemoved(count);
}

class StringForwardingTable::Block {
 public:
  static std::unique_ptr<Block> New(int capacity);
  explicit Block(int capacity);
  int capacity() const { return capacity_; }
  void* operator new(size_t size, int capacity);
  void* operator new(size_t size) = delete;
  void operator delete(void* data);

  void Set(int index, String string, String forward_to) {
    DCHECK_LT(index, capacity());
    Set(IndexOfOriginalString(index), string);
    Set(IndexOfForwardString(index), forward_to);
  }

  String GetOriginalString(Isolate* isolate, int index) const {
    DCHECK_LT(index, capacity());
    return String::cast(Get(isolate, IndexOfOriginalString(index)));
  }

  String GetForwardString(Isolate* isolate, int index) const {
    DCHECK_LT(index, capacity());
    return String::cast(Get(isolate, IndexOfForwardString(index)));
  }

  void IterateElements(RootVisitor* visitor, int up_to_index) {
    OffHeapObjectSlot first_slot = slot(0);
    OffHeapObjectSlot end_slot = slot(IndexOfOriginalString(up_to_index));
    visitor->VisitRootPointers(Root::kStringForwardingTable, nullptr,
                               first_slot, end_slot);
  }
  void UpdateAfterEvacuation(Isolate* isolate);
  void UpdateAfterEvacuation(Isolate* isolate, int up_to_index);

 private:
  static constexpr int kRecordSize = 2;
  static constexpr int kOriginalStringOffset = 0;
  static constexpr int kForwardStringOffset = 1;

  int IndexOfOriginalString(int index) const {
    return index * kRecordSize + kOriginalStringOffset;
  }

  int IndexOfForwardString(int index) const {
    return index * kRecordSize + kForwardStringOffset;
  }

  OffHeapObjectSlot slot(int index) const {
    return OffHeapObjectSlot(&elements_[index]);
  }

  Object Get(PtrComprCageBase cage_base, int internal_index) const {
    return slot(internal_index).Acquire_Load(cage_base);
  }

  void Set(int internal_index, Object object) {
    slot(internal_index).Release_Store(object);
  }

  const int capacity_;
  Tagged_t elements_[1];
};

StringForwardingTable::Block::Block(int capacity) : capacity_(capacity) {}

void* StringForwardingTable::Block::operator new(size_t size, int capacity) {
  // Make sure the size given is the size of the Block structure.
  DCHECK_EQ(size, sizeof(StringForwardingTable::Block));
  // Make sure that the elements_ array is at the end of Block, with no padding,
  // so that subsequent elements can be accessed as offsets from elements_.
  static_assert(offsetof(StringForwardingTable::Block, elements_) ==
                sizeof(StringForwardingTable::Block) - sizeof(Tagged_t) * 1);
  // Make sure that elements_ is aligned when StringTable::Block is aligned.
  static_assert((alignof(StringForwardingTable::Block) +
                 offsetof(StringForwardingTable::Block, elements_)) %
                    kTaggedSize ==
                0);

  const size_t elements_size = capacity * kRecordSize * sizeof(Tagged_t);
  // Storage for the first element is already supplied by elements_, so subtract
  // sizeof(Tagged_t).
  const size_t new_size = size + elements_size - sizeof(Tagged_t);
  DCHECK_LE(alignof(StringForwardingTable::Block), kSystemPointerSize);
  return AlignedAlloc(new_size, kSystemPointerSize);
}

void StringForwardingTable::Block::operator delete(void* block) {
  AlignedFree(block);
}

std::unique_ptr<StringForwardingTable::Block> StringForwardingTable::Block::New(
    int capacity) {
  return std::unique_ptr<Block>(new (capacity) Block(capacity));
}

void StringForwardingTable::Block::UpdateAfterEvacuation(Isolate* isolate) {
  UpdateAfterEvacuation(isolate, capacity_);
}

void StringForwardingTable::Block::UpdateAfterEvacuation(Isolate* isolate,
                                                         int up_to_index) {
  DCHECK(FLAG_always_use_string_forwarding_table);
  for (int index = 0; index < up_to_index; ++index) {
    Object original = Get(isolate, IndexOfOriginalString(index));
    if (!original.IsHeapObject()) continue;
    HeapObject object = HeapObject::cast(original);
    if (Heap::InFromPage(object)) {
      DCHECK(!object.InSharedWritableHeap());
      MapWord map_word = object.map_word(kRelaxedLoad);
      if (map_word.IsForwardingAddress()) {
        HeapObject forwarded_object = map_word.ToForwardingAddress();
        Set(IndexOfOriginalString(index), String::cast(forwarded_object));
      } else {
        Set(IndexOfOriginalString(index), deleted_element());
      }
    } else {
      DCHECK(!object.map_word(kRelaxedLoad).IsForwardingAddress());
    }
  }
}

class StringForwardingTable::BlockVector {
 public:
  using Block = StringForwardingTable::Block;
  using Allocator = std::allocator<Block*>;

  explicit BlockVector(size_t capacity);
  ~BlockVector();
  size_t capacity() const { return capacity_; }

  Block* LoadBlock(size_t index, AcquireLoadTag) {
    DCHECK_LT(index, size());
    return base::AsAtomicPointer::Acquire_Load(&begin_[index]);
  }

  Block* LoadBlock(size_t index) {
    DCHECK_LT(index, size());
    return begin_[index];
  }

  void AddBlock(std::unique_ptr<Block> block) {
    DCHECK_LT(size(), capacity());
    base::AsAtomicPointer::Release_Store(&begin_[size_], block.release());
    size_++;
  }

  static std::unique_ptr<BlockVector> Grow(BlockVector* data, size_t capacity,
                                           const base::Mutex& mutex);

  size_t size() const { return size_; }

 private:
  V8_NO_UNIQUE_ADDRESS Allocator allocator_;
  const size_t capacity_;
  std::atomic<size_t> size_;
  Block** begin_;
};

StringForwardingTable::BlockVector::BlockVector(size_t capacity)
    : allocator_(Allocator()), capacity_(capacity), size_(0) {
  begin_ = allocator_.allocate(capacity);
}

StringForwardingTable::BlockVector::~BlockVector() {
  allocator_.deallocate(begin_, capacity());
}

// static
std::unique_ptr<StringForwardingTable::BlockVector>
StringForwardingTable::BlockVector::Grow(
    StringForwardingTable::BlockVector* data, size_t capacity,
    const base::Mutex& mutex) {
  mutex.AssertHeld();
  std::unique_ptr<BlockVector> new_data =
      std::make_unique<BlockVector>(capacity);
  // Copy pointers to blocks from the old to the new vector.
  for (size_t i = 0; i < data->size(); i++) {
    new_data->begin_[i] = data->LoadBlock(i);
  }
  new_data->size_ = data->size();
  return new_data;
}

StringForwardingTable::StringForwardingTable(Isolate* isolate)
    : isolate_(isolate), next_free_index_(0) {
  InitializeBlockVector();
}

StringForwardingTable::~StringForwardingTable() {
  BlockVector* blocks = blocks_.load(std::memory_order_relaxed);
  for (uint32_t block = 0; block < blocks->size(); block++) {
    delete blocks->LoadBlock(block);
  }
}

void StringForwardingTable::InitializeBlockVector() {
  BlockVector* blocks = block_vector_storage_
                            .emplace_back(std::make_unique<BlockVector>(
                                kInitialBlockVectorCapacity))
                            .get();
  blocks->AddBlock(Block::New(kInitialBlockSize));
  blocks_.store(blocks, std::memory_order_relaxed);
}

StringForwardingTable::BlockVector* StringForwardingTable::EnsureCapacity(
    uint32_t block) {
  BlockVector* blocks = blocks_.load(std::memory_order_acquire);
  if V8_UNLIKELY (block >= blocks->size()) {
    base::MutexGuard table_grow_guard(&grow_mutex_);
    // Reload the vector, as another thread could have grown it.
    blocks = blocks_.load(std::memory_order_relaxed);
    // Check again if we need to grow under lock.
    if (block >= blocks->size()) {
      const uint32_t capacity = CapacityForBlock(block);
      std::unique_ptr<Block> new_block = Block::New(capacity);
      // Grow the vector if the block to insert is greater than the vectors
      // capacity.
      if (block >= blocks->capacity()) {
        std::unique_ptr<BlockVector> new_blocks =
            BlockVector::Grow(blocks, blocks->capacity() * 2, grow_mutex_);
        block_vector_storage_.push_back(std::move(new_blocks));
        blocks = block_vector_storage_.back().get();
        blocks_.store(blocks, std::memory_order_release);
      }
      blocks->AddBlock(std::move(new_block));
    }
  }
  return blocks;
}

int StringForwardingTable::Add(Isolate* isolate, String string,
                               String forward_to) {
  DCHECK_IMPLIES(!FLAG_always_use_string_forwarding_table,
                 string.InSharedHeap());
  DCHECK_IMPLIES(!FLAG_always_use_string_forwarding_table,
                 forward_to.InSharedHeap());
  int index = next_free_index_++;
  uint32_t index_in_block;
  const uint32_t block = BlockForIndex(index, &index_in_block);

  BlockVector* blocks = EnsureCapacity(block);
  Block* data = blocks->LoadBlock(block, kAcquireLoad);
  data->Set(index_in_block, string, forward_to);
  return index;
}

String StringForwardingTable::GetForwardString(Isolate* isolate,
                                               int index) const {
  CHECK_LT(index, Size());
  uint32_t index_in_block;
  const uint32_t block = BlockForIndex(index, &index_in_block);
  Block* data =
      blocks_.load(std::memory_order_acquire)->LoadBlock(block, kAcquireLoad);
  return data->GetForwardString(isolate, index_in_block);
}

// static
Address StringForwardingTable::GetForwardStringAddress(Isolate* isolate,
                                                       int index) {
  return isolate->string_forwarding_table()
      ->GetForwardString(isolate, index)
      .ptr();
}

void StringForwardingTable::IterateElements(RootVisitor* visitor) {
  isolate_->heap()->safepoint()->AssertActive();
  DCHECK_NE(isolate_->heap()->gc_state(), Heap::NOT_IN_GC);

  if (next_free_index_ == 0) return;  // Early exit if table is empty.

  BlockVector* blocks = blocks_.load(std::memory_order_relaxed);
  const uint32_t last_block = static_cast<uint32_t>(blocks->size() - 1);
  for (uint32_t block = 0; block < last_block; ++block) {
    Block* data = blocks->LoadBlock(block);
    data->IterateElements(visitor, data->capacity());
  }
  // Handle last block separately, as it is not filled to capacity.
  const uint32_t max_index = IndexInBlock(next_free_index_ - 1, last_block) + 1;
  Block* data = blocks->LoadBlock(last_block);
  data->IterateElements(visitor, max_index);
}

void StringForwardingTable::Reset() {
  isolate_->heap()->safepoint()->AssertActive();
  DCHECK_NE(isolate_->heap()->gc_state(), Heap::NOT_IN_GC);

  BlockVector* blocks = blocks_.load(std::memory_order_relaxed);
  for (uint32_t block = 0; block < blocks->size(); ++block) {
    delete blocks->LoadBlock(block);
  }

  block_vector_storage_.clear();
  InitializeBlockVector();
  next_free_index_ = 0;
}

void StringForwardingTable::UpdateAfterEvacuation() {
  DCHECK(FLAG_always_use_string_forwarding_table);

  if (next_free_index_ == 0) return;  // Early exit if table is empty.

  BlockVector* blocks = blocks_.load(std::memory_order_relaxed);
  const unsigned int last_block = static_cast<unsigned int>(blocks->size() - 1);
  for (unsigned int block = 0; block < last_block; ++block) {
    Block* data = blocks->LoadBlock(block, kAcquireLoad);
    data->UpdateAfterEvacuation(isolate_);
  }
  // Handle last block separately, as it is not filled to capacity.
  const int max_index = IndexInBlock(next_free_index_ - 1, last_block) + 1;
  blocks->LoadBlock(last_block, kAcquireLoad)
      ->UpdateAfterEvacuation(isolate_, max_index);
}

}  // namespace internal
}  // namespace v8
