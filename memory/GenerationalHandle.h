/*
 * GenerationalHandle.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * 64-bit generational handle (slot map) over VCMI ObjectInstanceID values.
 * Guards the AI against the ABA problem: a freed array slot reused by a new
 * object must not validate against a stale handle. The registry is owned by
 * the AI and bumped on objectRemoved / objectPropertyChanged events, so a
 * worker holding an old handle fails validation instead of reading a
 * recycled slot.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace omniai
{

/// Packed 64-bit handle: low 32 bits = slot index (ObjectInstanceID), high 32
/// bits = generation counter. Fits a single atomic load and std::vector of
/// these is a pure-integer snapshot that stays in L1/L2 cache.
struct GenerationalHandle
{
	uint32_t index = 0;
	uint32_t generation = 0;

	constexpr GenerationalHandle() = default;
	constexpr GenerationalHandle(uint32_t idx, uint32_t gen)
		: index(idx), generation(gen) {}

	constexpr uint64_t pack() const
	{
		return (uint64_t(generation) << 32) | uint64_t(index);
	}
	static constexpr GenerationalHandle unpack(uint64_t raw)
	{
		return { uint32_t(raw & 0xFFFFFFFFu), uint32_t(raw >> 32) };
	}

	constexpr bool valid() const { return generation != 0; }
	constexpr bool operator==(const GenerationalHandle & o) const
	{
		return index == o.index && generation == o.generation;
	}
	constexpr bool operator!=(const GenerationalHandle & o) const { return !(*this == o); }
};

static_assert(sizeof(GenerationalHandle) == 8, "handle must stay 64-bit");

/// Thread-safe registry mapping engine object IDs to generational handles.
/// Register every object the AI plans to evaluate; on each removal event call
/// retire() so the slot's generation increments and stale lookups fail.
class HandleRegistry
{
public:
	GenerationalHandle acquire(uint32_t objectId)
	{
		std::unique_lock lock(mutex_);
		auto it = slotOf_.find(objectId);
		uint32_t idx;
		if(it != slotOf_.end())
			idx = it->second;
		else
		{
			idx = static_cast<uint32_t>(generations_.size());
			generations_.push_back(1);
			slotOf_[objectId] = idx;
			liveId_.push_back(objectId);
		}
		return { idx, generations_[idx] };
	}

	/// Mark the object as removed: bumps generation so old handles go stale.
	void retire(uint32_t objectId)
	{
		std::unique_lock lock(mutex_);
		auto it = slotOf_.find(objectId);
		if(it == slotOf_.end())
			return;
		generations_[it->second]++; // stale handles now mismatch
	}

	/// True if the handle still maps to the same generation it was issued under.
	bool validate(GenerationalHandle h) const
	{
		std::shared_lock lock(mutex_);
		return h.valid()
			&& h.index < generations_.size()
			&& generations_[h.index] == h.generation;
	}

	/// Resolve a handle back to the engine object id, or -1 if stale.
	int64_t resolve(GenerationalHandle h) const
	{
		std::shared_lock lock(mutex_);
		if(!h.valid() || h.index >= generations_.size() || generations_[h.index] != h.generation)
			return -1;
		return liveId_[h.index];
	}

	/// Cheap O(1)-per-entry snapshot: just the packed handles, cache friendly.
	std::vector<uint64_t> snapshotPacked() const
	{
		std::shared_lock lock(mutex_);
		std::vector<uint64_t> out;
		out.reserve(generations_.size());
		for(size_t i = 0; i < generations_.size(); ++i)
			out.push_back(GenerationalHandle(uint32_t(i), generations_[i]).pack());
		return out;
	}

	void clear()
	{
		std::unique_lock lock(mutex_);
		generations_.clear();
		liveId_.clear();
		slotOf_.clear();
	}

private:
	mutable std::shared_mutex mutex_;
	std::vector<uint32_t> generations_;   // slot -> current generation
	std::vector<uint32_t> liveId_;        // slot -> engine ObjectInstanceID
	std::unordered_map<uint32_t, uint32_t> slotOf_; // ObjectInstanceID -> slot
};

}
