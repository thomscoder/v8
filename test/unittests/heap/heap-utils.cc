// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/unittests/heap/heap-utils.h"

#include "src/heap/incremental-marking.h"
#include "src/heap/mark-compact.h"
#include "src/heap/safepoint.h"

namespace v8 {
namespace internal {

void HeapInternalsBase::SimulateIncrementalMarking(Heap* heap,
                                                   bool force_completion) {
  constexpr double kStepSizeInMs = 100;
  CHECK(FLAG_incremental_marking);
  i::IncrementalMarking* marking = heap->incremental_marking();
  i::MarkCompactCollector* collector = heap->mark_compact_collector();
  if (collector->sweeping_in_progress()) {
    SafepointScope scope(heap);
    collector->EnsureSweepingCompleted(
        MarkCompactCollector::SweepingForcedFinalizationMode::kV8Only);
  }
  CHECK(marking->IsMarking() || marking->IsStopped() || marking->IsComplete());
  if (marking->IsStopped()) {
    heap->StartIncrementalMarking(i::Heap::kNoGCFlags,
                                  i::GarbageCollectionReason::kTesting);
  }
  CHECK(marking->IsMarking() || marking->IsComplete());
  if (!force_completion) return;

  while (!marking->IsComplete()) {
    marking->Step(kStepSizeInMs,
                  i::IncrementalMarking::CompletionAction::kGCViaTask,
                  i::StepOrigin::kV8);
  }
  CHECK(marking->IsComplete());
}

}  // namespace internal
}  // namespace v8
