/*
 * PendingActionQueue.cpp, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 */
#include "../StdInc.h"
#include "PendingActionQueue.h"

#include "networkPacks/PacksForClient.h"

namespace omniai
{

static const char * kindName(ActionKind kind)
{
	switch(kind)
	{
		case ActionKind::Move:      return "Move";
		case ActionKind::Trade:     return "Trade";
		case ActionKind::CastSpell: return "CastSpell";
		case ActionKind::Recruit:   return "Recruit";
		case ActionKind::Build:     return "Build";
		default:                    return "Generic";
	}
}

void PendingActionQueue::track(uint32_t requestID, uint16_t packType,
	ActionKind kind, std::function<void()> rollback)
{
	std::lock_guard lock(mutex_);
	if(pending_.count(requestID))
		logAi->warn("OmniAI: requestID %u reused while still pending; dropping stale entry", requestID);
	pending_[requestID] = PendingAction{
		requestID, packType, kind,
		std::chrono::steady_clock::now(), std::move(rollback)};
}

bool PendingActionQueue::onReceipt(const PackageApplied & pa)
{
	PendingAction action;
	{
		std::lock_guard lock(mutex_);
		auto it = pending_.find(pa.requestID);
		if(it == pending_.end())
			return false;
		action = std::move(it->second);
		pending_.erase(it);
	}

	// pa.packType is the engine's CTypeList id; that registry is per-module
	// static and unreachable from a plugin dll, so correlation stays by
	// requestID only.

	if(!pa.result)
	{
		logAi->warn("OmniAI: action %u (%s) rejected by server; rolling back",
			pa.requestID, kindName(action.kind));
		if(action.rollback)
			action.rollback();
	}
	return true;
}

bool PendingActionQueue::resolveOldest(ActionKind kind)
{
	std::lock_guard lock(mutex_);
	auto best = pending_.end();
	for(auto it = pending_.begin(); it != pending_.end(); ++it)
		if(it->second.kind == kind
			&& (best == pending_.end() || it->second.sentAt < best->second.sentAt))
			best = it;
	if(best == pending_.end())
		return false;
	pending_.erase(best);
	return true;
}

void PendingActionQueue::reset()
{
	std::vector<PendingAction> toUndo;
	{
		std::lock_guard lock(mutex_);
		for(auto & [id, a] : pending_)
			toUndo.push_back(std::move(a));
		pending_.clear();
	}
	for(auto & a : toUndo)
		if(a.rollback)
			a.rollback();
}

size_t PendingActionQueue::sweepExpired(std::chrono::milliseconds timeout)
{
	const auto now = std::chrono::steady_clock::now();
	std::vector<PendingAction> expired;
	{
		std::lock_guard lock(mutex_);
		for(auto it = pending_.begin(); it != pending_.end();)
		{
			if(now - it->second.sentAt > timeout)
			{
				expired.push_back(std::move(it->second));
				it = pending_.erase(it);
			}
			else
				++it;
		}
	}
	for(auto & a : expired)
	{
		logAi->warn("OmniAI: action %u (%s) timed out without receipt; rolling back",
			a.requestID, kindName(a.kind));
		if(a.rollback)
			a.rollback();
	}
	return expired.size();
}

size_t PendingActionQueue::pendingCount() const
{
	std::lock_guard lock(mutex_);
	return pending_.size();
}

}
