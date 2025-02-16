// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/v8-initialization.h"
#include "src/base/strings.h"
#include "src/heap/factory.h"
#include "src/heap/heap-inl.h"
#include "src/heap/parked-scope.h"
#include "src/objects/objects-inl.h"
#include "test/cctest/cctest.h"
#include "test/cctest/heap/heap-utils.h"

namespace v8 {
namespace internal {
namespace test_shared_strings {

struct V8_NODISCARD IsolateWrapper {
  explicit IsolateWrapper(v8::Isolate* isolate) : isolate(isolate) {}
  ~IsolateWrapper() { isolate->Dispose(); }
  v8::Isolate* const isolate;
};

class MultiClientIsolateTest {
 public:
  MultiClientIsolateTest() {
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
        v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = allocator.get();
    shared_isolate_ =
        reinterpret_cast<v8::Isolate*>(Isolate::NewShared(create_params));
  }

  ~MultiClientIsolateTest() { Isolate::Delete(i_shared_isolate()); }

  v8::Isolate* shared_isolate() const { return shared_isolate_; }

  Isolate* i_shared_isolate() const {
    return reinterpret_cast<Isolate*>(shared_isolate_);
  }

  v8::Isolate* NewClientIsolate() {
    CHECK_NOT_NULL(shared_isolate_);
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
        v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = allocator.get();
    create_params.experimental_attach_to_shared_isolate = shared_isolate_;
    return v8::Isolate::New(create_params);
  }

 private:
  v8::Isolate* shared_isolate_;
};

UNINITIALIZED_TEST(InPlaceInternalizableStringsAreShared) {
  if (FLAG_single_generation) return;
  if (!ReadOnlyHeap::IsReadOnlySpaceShared()) return;
  if (!COMPRESS_POINTERS_IN_SHARED_CAGE_BOOL) return;

  FLAG_shared_string_table = true;

  MultiClientIsolateTest test;
  IsolateWrapper isolate1_wrapper(test.NewClientIsolate());
  v8::Isolate* isolate1 = isolate1_wrapper.isolate;
  Isolate* i_isolate1 = reinterpret_cast<Isolate*>(isolate1);
  Factory* factory1 = i_isolate1->factory();

  HandleScope handle_scope(i_isolate1);

  const char raw_one_byte[] = "foo";
  base::uc16 raw_two_byte[] = {2001, 2002, 2003};
  base::Vector<const base::uc16> two_byte(raw_two_byte, 3);

  // Old generation 1- and 2-byte seq strings are in-place internalizable.
  Handle<String> old_one_byte_seq =
      factory1->NewStringFromAsciiChecked(raw_one_byte, AllocationType::kOld);
  CHECK(old_one_byte_seq->InSharedHeap());
  Handle<String> old_two_byte_seq =
      factory1->NewStringFromTwoByte(two_byte, AllocationType::kOld)
          .ToHandleChecked();
  CHECK(old_two_byte_seq->InSharedHeap());

  // Young generation are not internalizable and not shared when sharing the
  // string table.
  Handle<String> young_one_byte_seq =
      factory1->NewStringFromAsciiChecked(raw_one_byte, AllocationType::kYoung);
  CHECK(!young_one_byte_seq->InSharedHeap());
  Handle<String> young_two_byte_seq =
      factory1->NewStringFromTwoByte(two_byte, AllocationType::kYoung)
          .ToHandleChecked();
  CHECK(!young_two_byte_seq->InSharedHeap());

  // Internalized strings are shared.
  Handle<String> one_byte_intern = factory1->NewOneByteInternalizedString(
      base::OneByteVector(raw_one_byte), 1);
  CHECK(one_byte_intern->InSharedHeap());
  Handle<String> two_byte_intern =
      factory1->NewTwoByteInternalizedString(two_byte, 1);
  CHECK(two_byte_intern->InSharedHeap());
}

UNINITIALIZED_TEST(InPlaceInternalization) {
  if (!ReadOnlyHeap::IsReadOnlySpaceShared()) return;
  if (!COMPRESS_POINTERS_IN_SHARED_CAGE_BOOL) return;

  FLAG_shared_string_table = true;

  MultiClientIsolateTest test;
  IsolateWrapper isolate1_wrapper(test.NewClientIsolate());
  IsolateWrapper isolate2_wrapper(test.NewClientIsolate());
  v8::Isolate* isolate1 = isolate1_wrapper.isolate;
  v8::Isolate* isolate2 = isolate2_wrapper.isolate;
  Isolate* i_isolate1 = reinterpret_cast<Isolate*>(isolate1);
  Factory* factory1 = i_isolate1->factory();
  Isolate* i_isolate2 = reinterpret_cast<Isolate*>(isolate2);
  Factory* factory2 = i_isolate2->factory();

  HandleScope scope1(i_isolate1);
  HandleScope scope2(i_isolate2);

  const char raw_one_byte[] = "foo";
  base::uc16 raw_two_byte[] = {2001, 2002, 2003};
  base::Vector<const base::uc16> two_byte(raw_two_byte, 3);

  // Allocate two in-place internalizable strings in isolate1 then intern
  // them.
  Handle<String> old_one_byte_seq1 =
      factory1->NewStringFromAsciiChecked(raw_one_byte, AllocationType::kOld);
  Handle<String> old_two_byte_seq1 =
      factory1->NewStringFromTwoByte(two_byte, AllocationType::kOld)
          .ToHandleChecked();
  Handle<String> one_byte_intern1 =
      factory1->InternalizeString(old_one_byte_seq1);
  Handle<String> two_byte_intern1 =
      factory1->InternalizeString(old_two_byte_seq1);
  CHECK(old_one_byte_seq1->InSharedHeap());
  CHECK(old_two_byte_seq1->InSharedHeap());
  CHECK(one_byte_intern1->InSharedHeap());
  CHECK(two_byte_intern1->InSharedHeap());
  CHECK(old_one_byte_seq1.equals(one_byte_intern1));
  CHECK(old_two_byte_seq1.equals(two_byte_intern1));
  CHECK_EQ(*old_one_byte_seq1, *one_byte_intern1);
  CHECK_EQ(*old_two_byte_seq1, *two_byte_intern1);

  // Allocate two in-place internalizable strings with the same contents in
  // isolate2 then intern them. They should be the same as the interned strings
  // from isolate1.
  Handle<String> old_one_byte_seq2 =
      factory2->NewStringFromAsciiChecked(raw_one_byte, AllocationType::kOld);
  Handle<String> old_two_byte_seq2 =
      factory2->NewStringFromTwoByte(two_byte, AllocationType::kOld)
          .ToHandleChecked();
  Handle<String> one_byte_intern2 =
      factory2->InternalizeString(old_one_byte_seq2);
  Handle<String> two_byte_intern2 =
      factory2->InternalizeString(old_two_byte_seq2);
  CHECK(old_one_byte_seq2->InSharedHeap());
  CHECK(old_two_byte_seq2->InSharedHeap());
  CHECK(one_byte_intern2->InSharedHeap());
  CHECK(two_byte_intern2->InSharedHeap());
  CHECK(!old_one_byte_seq2.equals(one_byte_intern2));
  CHECK(!old_two_byte_seq2.equals(two_byte_intern2));
  CHECK_NE(*old_one_byte_seq2, *one_byte_intern2);
  CHECK_NE(*old_two_byte_seq2, *two_byte_intern2);
  CHECK_EQ(*one_byte_intern1, *one_byte_intern2);
  CHECK_EQ(*two_byte_intern1, *two_byte_intern2);
}

UNINITIALIZED_TEST(YoungInternalization) {
  if (FLAG_single_generation) return;
  if (!ReadOnlyHeap::IsReadOnlySpaceShared()) return;
  if (!COMPRESS_POINTERS_IN_SHARED_CAGE_BOOL) return;

  FLAG_shared_string_table = true;

  MultiClientIsolateTest test;
  IsolateWrapper isolate1_wrapper(test.NewClientIsolate());
  IsolateWrapper isolate2_wrapper(test.NewClientIsolate());
  v8::Isolate* isolate1 = isolate1_wrapper.isolate;
  v8::Isolate* isolate2 = isolate2_wrapper.isolate;
  Isolate* i_isolate1 = reinterpret_cast<Isolate*>(isolate1);
  Factory* factory1 = i_isolate1->factory();
  Isolate* i_isolate2 = reinterpret_cast<Isolate*>(isolate2);
  Factory* factory2 = i_isolate2->factory();

  HandleScope scope1(i_isolate1);
  HandleScope scope2(i_isolate2);

  const char raw_one_byte[] = "foo";
  base::uc16 raw_two_byte[] = {2001, 2002, 2003};
  base::Vector<const base::uc16> two_byte(raw_two_byte, 3);

  // Allocate two young strings in isolate1 then intern them. Young strings
  // aren't in-place internalizable and are copied when internalized.
  Handle<String> young_one_byte_seq1 =
      factory1->NewStringFromAsciiChecked(raw_one_byte, AllocationType::kYoung);
  Handle<String> young_two_byte_seq1 =
      factory1->NewStringFromTwoByte(two_byte, AllocationType::kYoung)
          .ToHandleChecked();
  Handle<String> one_byte_intern1 =
      factory1->InternalizeString(young_one_byte_seq1);
  Handle<String> two_byte_intern1 =
      factory1->InternalizeString(young_two_byte_seq1);
  CHECK(!young_one_byte_seq1->InSharedHeap());
  CHECK(!young_two_byte_seq1->InSharedHeap());
  CHECK(one_byte_intern1->InSharedHeap());
  CHECK(two_byte_intern1->InSharedHeap());
  CHECK(!young_one_byte_seq1.equals(one_byte_intern1));
  CHECK(!young_two_byte_seq1.equals(two_byte_intern1));
  CHECK_NE(*young_one_byte_seq1, *one_byte_intern1);
  CHECK_NE(*young_two_byte_seq1, *two_byte_intern1);

  // Allocate two young strings with the same contents in isolate2 then intern
  // them. They should be the same as the interned strings from isolate1.
  Handle<String> young_one_byte_seq2 =
      factory2->NewStringFromAsciiChecked(raw_one_byte, AllocationType::kYoung);
  Handle<String> young_two_byte_seq2 =
      factory2->NewStringFromTwoByte(two_byte, AllocationType::kYoung)
          .ToHandleChecked();
  Handle<String> one_byte_intern2 =
      factory2->InternalizeString(young_one_byte_seq2);
  Handle<String> two_byte_intern2 =
      factory2->InternalizeString(young_two_byte_seq2);
  CHECK(!young_one_byte_seq2.equals(one_byte_intern2));
  CHECK(!young_two_byte_seq2.equals(two_byte_intern2));
  CHECK_NE(*young_one_byte_seq2, *one_byte_intern2);
  CHECK_NE(*young_two_byte_seq2, *two_byte_intern2);
  CHECK_EQ(*one_byte_intern1, *one_byte_intern2);
  CHECK_EQ(*two_byte_intern1, *two_byte_intern2);
}

class ConcurrentStringThreadBase : public v8::base::Thread {
 public:
  ConcurrentStringThreadBase(const char* name, MultiClientIsolateTest* test,
                             Handle<FixedArray> shared_strings,
                             ParkingSemaphore* sema_ready,
                             ParkingSemaphore* sema_execute_start,
                             ParkingSemaphore* sema_execute_complete)
      : v8::base::Thread(base::Thread::Options(name)),
        test_(test),
        shared_strings_(shared_strings),
        sema_ready_(sema_ready),
        sema_execute_start_(sema_execute_start),
        sema_execute_complete_(sema_execute_complete) {}

  virtual void Setup() {}
  virtual void RunForString(Handle<String> string) = 0;
  virtual void Teardown() {}
  void Run() override {
    IsolateWrapper isolate_wrapper(test_->NewClientIsolate());
    isolate = isolate_wrapper.isolate;
    i_isolate = reinterpret_cast<Isolate*>(isolate);

    Setup();

    sema_ready_->Signal();
    sema_execute_start_->ParkedWait(i_isolate->main_thread_local_isolate());

    {
      HandleScope scope(i_isolate);
      for (int i = 0; i < shared_strings_->length(); i++) {
        Handle<String> input_string(String::cast(shared_strings_->get(i)),
                                    i_isolate);
        RunForString(input_string);
      }
    }

    sema_execute_complete_->Signal();

    Teardown();

    isolate = nullptr;
    i_isolate = nullptr;
  }

  void ParkedJoin(const ParkedScope& scope) {
    USE(scope);
    Join();
  }

 protected:
  using base::Thread::Join;

  v8::Isolate* isolate;
  Isolate* i_isolate;
  MultiClientIsolateTest* test_;
  Handle<FixedArray> shared_strings_;
  ParkingSemaphore* sema_ready_;
  ParkingSemaphore* sema_execute_start_;
  ParkingSemaphore* sema_execute_complete_;
};

enum TestHitOrMiss { kTestMiss, kTestHit };

class ConcurrentInternalizationThread final
    : public ConcurrentStringThreadBase {
 public:
  ConcurrentInternalizationThread(MultiClientIsolateTest* test,
                                  Handle<FixedArray> shared_strings,
                                  TestHitOrMiss hit_or_miss,
                                  ParkingSemaphore* sema_ready,
                                  ParkingSemaphore* sema_execute_start,
                                  ParkingSemaphore* sema_execute_complete)
      : ConcurrentStringThreadBase("ConcurrentInternalizationThread", test,
                                   shared_strings, sema_ready,
                                   sema_execute_start, sema_execute_complete),
        hit_or_miss_(hit_or_miss) {}

  void Setup() override { factory = i_isolate->factory(); }

  void RunForString(Handle<String> input_string) override {
    CHECK(input_string->IsShared());
    Handle<String> interned = factory->InternalizeString(input_string);
    CHECK(interned->IsShared());
    CHECK(interned->IsInternalizedString());
    if (hit_or_miss_ == kTestMiss) {
      CHECK_EQ(*input_string, *interned);
    } else {
      CHECK(input_string->HasForwardingIndex());
      CHECK(String::Equals(i_isolate, input_string, interned));
    }
  }

 private:
  TestHitOrMiss hit_or_miss_;
  Factory* factory;
};

namespace {

Handle<FixedArray> CreateSharedOneByteStrings(Isolate* isolate,
                                              Factory* factory, int count,
                                              bool internalize) {
  Handle<FixedArray> shared_strings =
      factory->NewFixedArray(count, AllocationType::kSharedOld);
  for (int i = 0; i < count; i++) {
    char* ascii = new char[i + 3];
    // Don't make single character strings, which will end up deduplicating to
    // an RO string and mess up the string table hit test.
    for (int j = 0; j < i + 2; j++) ascii[j] = 'a';
    ascii[i + 2] = '\0';
    if (internalize) {
      // When testing concurrent string table hits, pre-internalize a string of
      // the same contents so all subsequent internalizations are hits.
      factory->InternalizeString(factory->NewStringFromAsciiChecked(ascii));
    }
    Handle<String> string = String::Share(
        isolate,
        factory->NewStringFromAsciiChecked(ascii, AllocationType::kOld));
    CHECK(string->IsShared());
    string->EnsureHash();
    shared_strings->set(i, *string);
    delete[] ascii;
  }
  return shared_strings;
}

void TestConcurrentInternalization(TestHitOrMiss hit_or_miss) {
  if (!ReadOnlyHeap::IsReadOnlySpaceShared()) return;
  if (!COMPRESS_POINTERS_IN_SHARED_CAGE_BOOL) return;

  FLAG_shared_string_table = true;

  MultiClientIsolateTest test;

  constexpr int kThreads = 4;
  constexpr int kStrings = 4096;

  IsolateWrapper isolate_wrapper(test.NewClientIsolate());
  v8::Isolate* isolate = isolate_wrapper.isolate;
  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);
  Factory* factory = i_isolate->factory();

  HandleScope scope(i_isolate);

  Handle<FixedArray> shared_strings = CreateSharedOneByteStrings(
      i_isolate, factory, kStrings, hit_or_miss == kTestHit);

  ParkingSemaphore sema_ready(0);
  ParkingSemaphore sema_execute_start(0);
  ParkingSemaphore sema_execute_complete(0);
  std::vector<std::unique_ptr<ConcurrentInternalizationThread>> threads;
  for (int i = 0; i < kThreads; i++) {
    auto thread = std::make_unique<ConcurrentInternalizationThread>(
        &test, shared_strings, hit_or_miss, &sema_ready, &sema_execute_start,
        &sema_execute_complete);
    CHECK(thread->Start());
    threads.push_back(std::move(thread));
  }

  LocalIsolate* local_isolate = i_isolate->main_thread_local_isolate();
  for (int i = 0; i < kThreads; i++) {
    sema_ready.ParkedWait(local_isolate);
  }
  for (int i = 0; i < kThreads; i++) {
    sema_execute_start.Signal();
  }
  for (int i = 0; i < kThreads; i++) {
    sema_execute_complete.ParkedWait(local_isolate);
  }

  ParkedScope parked(local_isolate);
  for (auto& thread : threads) {
    thread->ParkedJoin(parked);
  }
}
}  // namespace

UNINITIALIZED_TEST(ConcurrentInternalizationMiss) {
  TestConcurrentInternalization(kTestMiss);
}

UNINITIALIZED_TEST(ConcurrentInternalizationHit) {
  TestConcurrentInternalization(kTestHit);
}

class ConcurrentStringTableLookupThread final
    : public ConcurrentStringThreadBase {
 public:
  ConcurrentStringTableLookupThread(MultiClientIsolateTest* test,
                                    Handle<FixedArray> shared_strings,
                                    ParkingSemaphore* sema_ready,
                                    ParkingSemaphore* sema_execute_start,
                                    ParkingSemaphore* sema_execute_complete)
      : ConcurrentStringThreadBase("ConcurrentStringTableLookup", test,
                                   shared_strings, sema_ready,
                                   sema_execute_start, sema_execute_complete) {}

  void RunForString(Handle<String> input_string) override {
    CHECK(input_string->IsShared());
    Object result = Object(StringTable::TryStringToIndexOrLookupExisting(
        i_isolate, input_string->ptr()));
    if (result.IsString()) {
      String internalized = String::cast(result);
      CHECK(internalized.IsInternalizedString());
      CHECK_IMPLIES(input_string->IsInternalizedString(),
                    *input_string == internalized);
    } else {
      CHECK_EQ(Smi::cast(result).value(), ResultSentinel::kNotFound);
    }
  }
};

UNINITIALIZED_TEST(ConcurrentStringTableLookup) {
  if (!ReadOnlyHeap::IsReadOnlySpaceShared()) return;
  if (!COMPRESS_POINTERS_IN_SHARED_CAGE_BOOL) return;

  FLAG_shared_string_table = true;

  MultiClientIsolateTest test;

  constexpr int kTotalThreads = 4;
  constexpr int kInternalizationThreads = 1;
  constexpr int kStrings = 4096;

  IsolateWrapper isolate_wrapper(test.NewClientIsolate());
  v8::Isolate* isolate = isolate_wrapper.isolate;
  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);
  Factory* factory = i_isolate->factory();

  HandleScope scope(i_isolate);

  Handle<FixedArray> shared_strings =
      CreateSharedOneByteStrings(i_isolate, factory, kStrings, false);

  ParkingSemaphore sema_ready(0);
  ParkingSemaphore sema_execute_start(0);
  ParkingSemaphore sema_execute_complete(0);
  std::vector<std::unique_ptr<ConcurrentStringThreadBase>> threads;
  for (int i = 0; i < kInternalizationThreads; i++) {
    auto thread = std::make_unique<ConcurrentInternalizationThread>(
        &test, shared_strings, kTestMiss, &sema_ready, &sema_execute_start,
        &sema_execute_complete);
    CHECK(thread->Start());
    threads.push_back(std::move(thread));
  }
  for (int i = 0; i < kTotalThreads - kInternalizationThreads; i++) {
    auto thread = std::make_unique<ConcurrentStringTableLookupThread>(
        &test, shared_strings, &sema_ready, &sema_execute_start,
        &sema_execute_complete);
    CHECK(thread->Start());
    threads.push_back(std::move(thread));
  }

  LocalIsolate* local_isolate = i_isolate->main_thread_local_isolate();
  for (int i = 0; i < kTotalThreads; i++) {
    sema_ready.ParkedWait(local_isolate);
  }
  for (int i = 0; i < kTotalThreads; i++) {
    sema_execute_start.Signal();
  }
  for (int i = 0; i < kTotalThreads; i++) {
    sema_execute_complete.ParkedWait(local_isolate);
  }

  ParkedScope parked(local_isolate);
  for (auto& thread : threads) {
    thread->ParkedJoin(parked);
  }
}

namespace {
void CheckSharedStringIsEqualCopy(Handle<String> shared,
                                  Handle<String> original) {
  CHECK(shared->IsShared());
  CHECK(shared->Equals(*original));
  CHECK_NE(*shared, *original);
}

Handle<String> ShareAndVerify(Isolate* isolate, Handle<String> string) {
  Handle<String> shared = String::Share(isolate, string);
  CHECK(shared->IsShared());
#ifdef VERIFY_HEAP
  shared->ObjectVerify(isolate);
  string->ObjectVerify(isolate);
#endif  // VERIFY_HEAP
  return shared;
}
}  // namespace

UNINITIALIZED_TEST(StringShare) {
  if (!ReadOnlyHeap::IsReadOnlySpaceShared()) return;
  if (!COMPRESS_POINTERS_IN_SHARED_CAGE_BOOL) return;

  FLAG_shared_string_table = true;

  MultiClientIsolateTest test;
  IsolateWrapper isolate_wrapper(test.NewClientIsolate());
  v8::Isolate* isolate = isolate_wrapper.isolate;
  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);
  Factory* factory = i_isolate->factory();

  HandleScope scope(i_isolate);

  // A longer string so that concatenated to itself, the result is >
  // ConsString::kMinLength.
  const char raw_one_byte[] =
      "Lorem ipsum dolor sit amet, consectetur adipiscing elit";
  base::uc16 raw_two_byte[] = {2001, 2002, 2003};
  base::Vector<const base::uc16> two_byte(raw_two_byte, 3);

  {
    // Old-generation sequential strings are shared in-place.
    Handle<String> one_byte_seq =
        factory->NewStringFromAsciiChecked(raw_one_byte, AllocationType::kOld);
    Handle<String> two_byte_seq =
        factory->NewStringFromTwoByte(two_byte, AllocationType::kOld)
            .ToHandleChecked();
    CHECK(!one_byte_seq->IsShared());
    CHECK(!two_byte_seq->IsShared());
    Handle<String> shared_one_byte = ShareAndVerify(i_isolate, one_byte_seq);
    Handle<String> shared_two_byte = ShareAndVerify(i_isolate, two_byte_seq);
    CHECK_EQ(*one_byte_seq, *shared_one_byte);
    CHECK_EQ(*two_byte_seq, *shared_two_byte);
  }

  {
    // Internalized strings are always shared.
    Handle<String> one_byte_seq =
        factory->NewStringFromAsciiChecked(raw_one_byte, AllocationType::kOld);
    Handle<String> two_byte_seq =
        factory->NewStringFromTwoByte(two_byte, AllocationType::kOld)
            .ToHandleChecked();
    CHECK(!one_byte_seq->IsShared());
    CHECK(!two_byte_seq->IsShared());
    Handle<String> one_byte_intern = factory->InternalizeString(one_byte_seq);
    Handle<String> two_byte_intern = factory->InternalizeString(two_byte_seq);
    CHECK(one_byte_intern->IsShared());
    CHECK(two_byte_intern->IsShared());
    Handle<String> shared_one_byte_intern =
        ShareAndVerify(i_isolate, one_byte_intern);
    Handle<String> shared_two_byte_intern =
        ShareAndVerify(i_isolate, two_byte_intern);
    CHECK_EQ(*one_byte_intern, *shared_one_byte_intern);
    CHECK_EQ(*two_byte_intern, *shared_two_byte_intern);
  }

  // All other strings are flattened then copied if the flatten didn't already
  // create a new copy.

  if (!FLAG_single_generation) {
    // Young strings
    Handle<String> young_one_byte_seq = factory->NewStringFromAsciiChecked(
        raw_one_byte, AllocationType::kYoung);
    Handle<String> young_two_byte_seq =
        factory->NewStringFromTwoByte(two_byte, AllocationType::kYoung)
            .ToHandleChecked();
    CHECK(Heap::InYoungGeneration(*young_one_byte_seq));
    CHECK(Heap::InYoungGeneration(*young_two_byte_seq));
    CHECK(!young_one_byte_seq->IsShared());
    CHECK(!young_two_byte_seq->IsShared());
    Handle<String> shared_one_byte =
        ShareAndVerify(i_isolate, young_one_byte_seq);
    Handle<String> shared_two_byte =
        ShareAndVerify(i_isolate, young_two_byte_seq);
    CheckSharedStringIsEqualCopy(shared_one_byte, young_one_byte_seq);
    CheckSharedStringIsEqualCopy(shared_two_byte, young_two_byte_seq);
  }

  if (!FLAG_always_use_string_forwarding_table) {
    // Thin strings
    Handle<String> one_byte_seq1 =
        factory->NewStringFromAsciiChecked(raw_one_byte);
    Handle<String> one_byte_seq2 =
        factory->NewStringFromAsciiChecked(raw_one_byte);
    CHECK(!one_byte_seq1->IsShared());
    CHECK(!one_byte_seq2->IsShared());
    factory->InternalizeString(one_byte_seq1);
    factory->InternalizeString(one_byte_seq2);
    CHECK(StringShape(*one_byte_seq2).IsThin());
    Handle<String> shared = ShareAndVerify(i_isolate, one_byte_seq2);
    CheckSharedStringIsEqualCopy(shared, one_byte_seq2);
  }

  {
    // Cons strings
    Handle<String> one_byte_seq1 =
        factory->NewStringFromAsciiChecked(raw_one_byte);
    Handle<String> one_byte_seq2 =
        factory->NewStringFromAsciiChecked(raw_one_byte);
    CHECK(!one_byte_seq1->IsShared());
    CHECK(!one_byte_seq2->IsShared());
    Handle<String> cons =
        factory->NewConsString(one_byte_seq1, one_byte_seq2).ToHandleChecked();
    CHECK(!cons->IsShared());
    CHECK(cons->IsConsString());
    Handle<String> shared = ShareAndVerify(i_isolate, cons);
    CheckSharedStringIsEqualCopy(shared, cons);
  }

  {
    // Sliced strings
    Handle<String> one_byte_seq =
        factory->NewStringFromAsciiChecked(raw_one_byte);
    CHECK(!one_byte_seq->IsShared());
    Handle<String> sliced =
        factory->NewSubString(one_byte_seq, 1, one_byte_seq->length());
    CHECK(!sliced->IsShared());
    CHECK(sliced->IsSlicedString());
    Handle<String> shared = ShareAndVerify(i_isolate, sliced);
    CheckSharedStringIsEqualCopy(shared, sliced);
  }
}

UNINITIALIZED_TEST(PromotionMarkCompact) {
  if (FLAG_single_generation) return;
  if (!ReadOnlyHeap::IsReadOnlySpaceShared()) return;
  if (!COMPRESS_POINTERS_IN_SHARED_CAGE_BOOL) return;

  FLAG_stress_concurrent_allocation = false;  // For SealCurrentObjects.
  FLAG_shared_string_table = true;

  MultiClientIsolateTest test;
  IsolateWrapper isolate_wrapper(test.NewClientIsolate());
  v8::Isolate* isolate = isolate_wrapper.isolate;
  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);
  Factory* factory = i_isolate->factory();
  Heap* heap = i_isolate->heap();
  // Heap* shared_heap = test.i_shared_isolate()->heap();

  const char raw_one_byte[] = "foo";

  {
    HandleScope scope(i_isolate);

    // heap::SealCurrentObjects(heap);
    // heap::SealCurrentObjects(shared_heap);

    Handle<String> one_byte_seq = factory->NewStringFromAsciiChecked(
        raw_one_byte, AllocationType::kYoung);

    CHECK(String::IsInPlaceInternalizable(*one_byte_seq));
    CHECK(heap->InSpace(*one_byte_seq, NEW_SPACE));

    for (int i = 0; i < 2; i++) {
      heap->CollectAllGarbage(Heap::kNoGCFlags,
                              GarbageCollectionReason::kTesting);
    }

    // In-place-internalizable strings are promoted into the shared heap when
    // sharing.
    CHECK(!heap->Contains(*one_byte_seq));
    CHECK(heap->SharedHeapContains(*one_byte_seq));
  }
}

UNINITIALIZED_TEST(PromotionScavenge) {
  if (FLAG_single_generation) return;
  if (!ReadOnlyHeap::IsReadOnlySpaceShared()) return;
  if (!COMPRESS_POINTERS_IN_SHARED_CAGE_BOOL) return;

  FLAG_stress_concurrent_allocation = false;  // For SealCurrentObjects.
  FLAG_shared_string_table = true;

  MultiClientIsolateTest test;
  IsolateWrapper isolate_wrapper(test.NewClientIsolate());
  v8::Isolate* isolate = isolate_wrapper.isolate;
  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);
  Factory* factory = i_isolate->factory();
  Heap* heap = i_isolate->heap();
  // Heap* shared_heap = test.i_shared_isolate()->heap();

  const char raw_one_byte[] = "foo";

  {
    HandleScope scope(i_isolate);

    // heap::SealCurrentObjects(heap);
    // heap::SealCurrentObjects(shared_heap);

    Handle<String> one_byte_seq = factory->NewStringFromAsciiChecked(
        raw_one_byte, AllocationType::kYoung);

    CHECK(String::IsInPlaceInternalizable(*one_byte_seq));
    CHECK(heap->InSpace(*one_byte_seq, NEW_SPACE));

    for (int i = 0; i < 2; i++) {
      heap->CollectGarbage(NEW_SPACE, GarbageCollectionReason::kTesting);
    }

    // In-place-internalizable strings are promoted into the shared heap when
    // sharing.
    CHECK(!heap->Contains(*one_byte_seq));
    CHECK(heap->SharedHeapContains(*one_byte_seq));
  }
}

UNINITIALIZED_TEST(SharedStringsTransitionDuringGC) {
  if (!ReadOnlyHeap::IsReadOnlySpaceShared()) return;
  if (!COMPRESS_POINTERS_IN_SHARED_CAGE_BOOL) return;

  FLAG_shared_string_table = true;

  MultiClientIsolateTest test;

  constexpr int kStrings = 4096;

  IsolateWrapper isolate_wrapper(test.NewClientIsolate());
  v8::Isolate* isolate = isolate_wrapper.isolate;
  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);
  Factory* factory = i_isolate->factory();

  HandleScope scope(i_isolate);

  // Run two times to test that everything is reset correctly during GC.
  for (int run = 0; run < 2; run++) {
    Handle<FixedArray> shared_strings =
        CreateSharedOneByteStrings(i_isolate, factory, kStrings, run == 0);

    // Check strings are in the forwarding table after internalization.
    for (int i = 0; i < shared_strings->length(); i++) {
      Handle<String> input_string(String::cast(shared_strings->get(i)),
                                  i_isolate);
      Handle<String> interned = factory->InternalizeString(input_string);
      CHECK(input_string->IsShared());
      CHECK(!input_string->IsThinString());
      CHECK(input_string->HasForwardingIndex());
      CHECK(String::Equals(i_isolate, input_string, interned));
    }

    // Trigger garbage collection on the shared isolate.
    i_isolate->heap()->CollectSharedGarbage(GarbageCollectionReason::kTesting);

    // Check that GC cleared the forwarding table.
    CHECK_EQ(i_isolate->string_forwarding_table()->Size(), 0);

    // Check all strings are transitioned to ThinStrings
    for (int i = 0; i < shared_strings->length(); i++) {
      Handle<String> input_string(String::cast(shared_strings->get(i)),
                                  i_isolate);
      CHECK(input_string->IsThinString());
    }
  }
}

}  // namespace test_shared_strings
}  // namespace internal
}  // namespace v8
