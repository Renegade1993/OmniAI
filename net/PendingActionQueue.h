/*
 * PendingActionQueue.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * Tracks dispatched-but-unconfirmed actions. The engine acknowledges every
 * CPackForServer with a PackageApplied receipt (requestID, packType, result);
 * a rejected action (result == false) means the server discarded it and the
 * AI's optimistic state snapshot must roll back before the next evaluation
 * round. This queue owns the pending set, correlates receipts, and runs the
 * rollback callbacks.
 */
#pragma once

#include "../StdInc.h"
#include "ActionDispatcher.h"

VCMI_LIB_NAMESPACE_BEGIN
struct PackageApplied;
VCMI_LIB_NAMESPACE_END

namespace omniai
{

struct PendingAction
{
	uint32_t requestID = 0;
	uint16_t packType = 0;
	ActionKind kind = ActionKind::Generic;
	std::chrono::steady_clock::time_point sentAt;
	/// Reversal for optimistic bookkeeping, e.g. "unmark tile visited" or
	/// "restore hero move points in snapshot". Empty means nothing to undo.
	std::function<void()> rollback;
};

class PendingActionQueue
{
public:
	/// Register the action after dispatch. Called from requestSent, where the
	/// engine supplies the requestID that PackageApplied will echo back.
	void track(uint32_t requestID, uint16_t packType, ActionKind kind,
		std::function<void()> rollback = {});

	/// Route a server receipt. Returns true when the receipt matched a
	/// tracked action. On rejection, runs that action's rollback.
	bool onReceipt(const PackageApplied & pa);

	/// Roll back and drop everything, e.g. after a load or turn boundary.
	void reset();

	/// Drop the oldest pending action of a kind without running its
	/// rollback: the engine's own outcome pack just proved it realized
	/// (e.g. heroMoved confirms a MoveHero whose PackageApplied never
	/// came). Returns false when nothing of that kind was pending.
	bool resolveOldest(ActionKind kind);

	/// Roll back actions older than timeout that never got a receipt.
	size_t sweepExpired(std::chrono::milliseconds timeout);

	size_t pendingCount() const;

private:
	mutable std::mutex mutex_;
	std::unordered_map<uint32_t, PendingAction> pending_;
};

}
