// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_CPPGC_GLOBALS_H_
#define V8_HEAP_CPPGC_GLOBALS_H_

#include <stddef.h>
#include <stdint.h>

#include "include/cppgc/internal/gc-info.h"
#include "src/base/build_config.h"

namespace cppgc {
namespace internal {

using Address = uint8_t*;
using ConstAddress = const uint8_t*;

constexpr size_t kKB = 1024;
constexpr size_t kMB = kKB * 1024;
constexpr size_t kGB = kMB * 1024;

// AccessMode used for choosing between atomic and non-atomic accesses.
enum class AccessMode : uint8_t { kNonAtomic, kAtomic };

// See 6.7.6 (http://eel.is/c++draft/basic.align) for alignment restrictions. We
// do not fully support all alignment restrictions (following
// alignof(std​::​max_­align_­t)) but limit to alignof(double).
//
// This means that any scalar type with stricter alignment requirements (in
// practice: long double) cannot be used unrestricted in garbage-collected
// objects.
#if defined(V8_TARGET_ARCH_64_BIT)
constexpr size_t kAllocationGranularity = 8;
#else   // !V8_TARGET_ARCH_64_BIT
constexpr size_t kAllocationGranularity = 4;
#endif  // !V8_TARGET_ARCH_64_BIT
constexpr size_t kAllocationMask = kAllocationGranularity - 1;

constexpr size_t kPageSizeLog2 = 17;
constexpr size_t kPageSize = 1 << kPageSizeLog2;
constexpr size_t kPageOffsetMask = kPageSize - 1;
constexpr size_t kPageBaseMask = ~kPageOffsetMask;

#if defined(__aarch64__) && defined(V8_OS_MACOS)
#if !defined(V8_TARGET_ARCH_ARM64)
#error "V8_TARGET_ARCH_ARM64 and __aarch64__ must match"
#endif  // !defined(V8_TARGET_ARCH_ARM64)
// No guard pages on ARM64 macOS. This target has 16 kiB pages, meaning that
// the guard pages do not protect anything, since there is no inaccessible
// region surrounding the allocation.
//
// However, with a 4k guard page size (as below), we avoid putting any data
// inside the "guard pages" region. Effectively, this wastes 2 * 4kiB of memory
// for each 128kiB page, since this is memory we pay for (since accounting as at
// the OS page level), but never use.
//
// The layout of pages is broadly:
// | guard page | header | payload | guard page |
// <---  4k --->                    <---  4k --->
// <------------------ 128k -------------------->
//
// Since this is aligned on an OS page boundary (16k), the guard pages are part
// of the first and last OS page, respectively. So they are really private dirty
// memory which we never use.
constexpr size_t kGuardPageSize = 0;
#else
// Guard pages are always put into memory. Whether they are actually protected
// depends on the allocator provided to the garbage collector.
constexpr size_t kGuardPageSize = 4096;
#endif

constexpr size_t kLargeObjectSizeThreshold = kPageSize / 2;

constexpr GCInfoIndex kFreeListGCInfoIndex = 0;
constexpr size_t kFreeListEntrySize = 2 * sizeof(uintptr_t);

constexpr size_t kCagedHeapReservationSize = static_cast<size_t>(4) * kGB;
constexpr size_t kCagedHeapReservationAlignment = kCagedHeapReservationSize;
// TODO(v8:12231): To reduce OOM probability, instead of the fixed-size
// reservation consider to use a moving needle implementation or simply
// calibrating this 2GB/2GB split.
constexpr size_t kCagedHeapNormalPageReservationSize =
    kCagedHeapReservationSize / 2;

}  // namespace internal
}  // namespace cppgc

#endif  // V8_HEAP_CPPGC_GLOBALS_H_
