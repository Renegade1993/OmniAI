/*
 * StdInc.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * Precompiled header. Pulls in the VCMI global header and exposes the lib
 * namespace so plugin sources can use VCMI types unqualified.
 */
#pragma once

// The vcmi facade target publishes the repo root as an include dir in both
// in-tree and standalone mode, so "Global.h" resolves either way.
#include "Global.h"

VCMI_LIB_USING_NAMESPACE

#include <tbb/task_arena.h>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_vector.h>
#include <tbb/concurrent_hash_map.h>
#include <tbb/scalable_allocator.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>
