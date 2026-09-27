/*
 * QSBRReclaimer.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * Quiescent State-Based Reclamation. Threads that retire AI-owned objects do
 * not free them immediately; they push them into a lock-free limbo queue.
 * When every participating thread announces a quiescent state (task_arena
 * exit, turn boundary), all limbo objects are destroyed safely.
 *
 * The engine-side counterpart (deferring CMap::removeObject inside VCMI
 * itself) requires patching lib/mapping/CMap.cpp; see
 * docs/engine-qsbr-patch.md. This plugin-side reclaimer governs the shadow
 * objects OmniAI allocates: cloned bonus nodes, shadow stacks, snapshots.
 */
#pragma once

#include "../StdInc.h"

namespace omniai
{

class QSBRReclaimer
{
public:
	static QSBRReclaimer & instance()
	{
		static QSBRReclaimer inst;
		return inst;
	}

	/// Retire an AI-owned object. Memory stays mapped until the next purge.
	/// Called from any thread; the queue is lock-free.
	void retire(std::shared_ptr<void> obj)
	{
		if(obj)
			limbo_.push(std::move(obj));
		++retiredCount_;
	}

	/// Convenience overload preserving the object's real type.
	template<typename T>
	void retire(std::shared_ptr<T> obj)
	{
		retire(std::shared_ptr<void>(obj));
	}

	/// Announce that this thread left every read critical section.
	/// The last thread to arrive performs the sweep.
	void declareQuiescentState()
	{
		uint64_t interval = interval_.load(std::memory_order_acquire);
		uint64_t arrived = arrivedThisInterval_.fetch_add(1, std::memory_order_acq_rel) + 1;
		if(arrived >= participantCount_.load(std::memory_order_acquire))
		{
			// Everyone quiescent: advance interval and purge the limbo bag.
			arrivedThisInterval_.store(0, std::memory_order_release);
			interval_.fetch_add(1, std::memory_order_acq_rel);
			purge();
		}
	}

	/// Register a TBB arena size so the reclaimer knows how many workers must
	/// declare a quiescent state before memory may be swept.
	void setParticipants(uint32_t n)
	{
		participantCount_.store(n == 0 ? 1 : n, std::memory_order_release);
	}

	/// Force a sweep regardless of participation (turn boundary, teardown).
	void purge()
	{
		std::shared_ptr<void> obj;
		while(limbo_.try_pop(obj))
		{
			obj.reset();
			++purgedCount_;
		}
	}

	size_t pending() const { return retiredCount_.load() - purgedCount_.load(); }

private:
	QSBRReclaimer() = default;

	tbb::concurrent_queue<std::shared_ptr<void>> limbo_;
	std::atomic<uint64_t> interval_{0};
	std::atomic<uint64_t> arrivedThisInterval_{0};
	std::atomic<uint32_t> participantCount_{1};
	std::atomic<uint64_t> retiredCount_{0};
	std::atomic<uint64_t> purgedCount_{0};
};

/// RAII guard: declares a quiescent state when a parallel block exits.
struct QuiescentGuard
{
	~QuiescentGuard() { QSBRReclaimer::instance().declareQuiescentState(); }
};

}
