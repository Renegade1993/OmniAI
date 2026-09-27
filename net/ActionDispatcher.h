/*
 * ActionDispatcher.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * Sends AI decisions through the VCMI callback layer. The doc blueprint calls
 * for polymorphic CPack dispatch; in 1.7.5 the engine-level entry point is
 * the typed CCallback API (moveHero, trade, castSpell, ...), each of which
 * builds and serializes the matching CPackForServer internally. This class
 * wraps those calls, assigns each dispatched intent a tracking record, and
 * correlates server receipts via the requestSent/requestRealized events the
 * engine raises on the network thread.
 */
#pragma once

#include "../StdInc.h"
#include "constants/EntityIdentifiers.h"
#include "constants/Enumerations.h"
#include "networkPacks/TradeItem.h"

VCMI_LIB_NAMESPACE_BEGIN
class CCallback;
class CGHeroInstance;
class CGTownInstance;
class CGObjectInstance;
struct CPackForServer;
struct PackageApplied;
class int3; // vcmi's int3 is a class; mismatching tag changes the mangled name
VCMI_LIB_NAMESPACE_END

namespace omniai
{

class PendingActionQueue;

/// Coarse classification for receipts, used by the pending queue and for
/// logging. Kept coarse because the engine's packType ids are engine-internal.
enum class ActionKind : uint8_t
{
	Move,
	Trade,
	CastSpell,
	Recruit,
	Build,
	Generic,
};

class ActionDispatcher
{
public:
	ActionDispatcher(std::shared_ptr<CCallback> cb, PendingActionQueue & queue);

	/// Path movement. path is a list of tiles from the pathfinder.
	void moveHero(const CGHeroInstance * hero, const std::vector<int3> & path, bool transit);

	/// Adventure-map spell (dimension door, town portal, etc).
	void castSpell(const CGHeroInstance * hero, SpellID spell, const int3 & pos);

	/// Market trade. mode selects resource-for-resource, creature-for-res, etc.
	void trade(ObjectInstanceID marketId, EMarketMode mode,
		TradeItemSell sell, TradeItemBuy buy, ui32 amount,
		const CGHeroInstance * hero = nullptr);

	/// Hire hero at town/tavern.
	void recruitHero(const CGObjectInstance * townOrTavern, const CGHeroInstance * hero,
		HeroTypeID nextHero = HeroTypeID::NONE);

	void endTurn();

	/// Generic: hand any CPackForServer to the engine. Preferred path when a
	/// caller already built a typed pack; falls back to a no-op with a warning
	/// if the callback cannot accept raw packs on this build.
	void dispatchRaw(const CPackForServer & pack, ActionKind kind);

	/// Event plumbing: call from OmniAI's event overrides.
	void onRequestSent(const CPackForServer * pack, int requestID);
	void onRequestRealized(const PackageApplied & pa);

private:
	std::shared_ptr<CCallback> cb_;
	PendingActionQueue & queue_;

	/// requestID of the most recently issued action on this thread.
	thread_local static int lastRequestID_;
};

}
