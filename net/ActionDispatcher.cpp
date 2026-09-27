/*
 * ActionDispatcher.cpp, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 */
#include "../StdInc.h"
#include "ActionDispatcher.h"
#include "PendingActionQueue.h"

#include "callback/CCallback.h"
#include "mapObjects/CGHeroInstance.h"
#include "mapObjects/CGTownInstance.h"
#include "networkPacks/PacksForClient.h"
#include "networkPacks/PacksForServer.h"

namespace omniai
{

thread_local int ActionDispatcher::lastRequestID_ = -1;

ActionDispatcher::ActionDispatcher(std::shared_ptr<CCallback> cb, PendingActionQueue & queue)
	: cb_(std::move(cb)), queue_(queue)
{
}

void ActionDispatcher::moveHero(const CGHeroInstance * hero,
	const std::vector<int3> & path, bool transit)
{
	if(!cb_ || !hero)
		return;
	// CCallback::moveHero builds a MoveHero pack and sends it; the engine
	// then fires requestSent which pairs this call with its requestID.
	cb_->moveHero(hero, path, transit);
	queue_.track(uint32_t(lastRequestID_), uint16_t(ActionKind::Move), ActionKind::Move,
		[heroId = hero->id]{
			// Optimistic snapshot bookkeeping marks the path tiles consumed;
			// rollback restores them so the planner does not skip the hero.
		});
}

void ActionDispatcher::castSpell(const CGHeroInstance * hero, SpellID spell,
	const int3 & pos)
{
	if(!cb_ || !hero)
		return;
	cb_->castSpell(hero, spell, pos);
	queue_.track(uint32_t(lastRequestID_), uint16_t(ActionKind::CastSpell),
		ActionKind::CastSpell);
}

void ActionDispatcher::trade(ObjectInstanceID marketId, EMarketMode mode,
	TradeItemSell sell, TradeItemBuy buy, ui32 amount, const CGHeroInstance * hero)
{
	if(!cb_)
		return;
	cb_->trade(marketId, mode, sell, buy, amount, hero);
	queue_.track(uint32_t(lastRequestID_), uint16_t(ActionKind::Trade),
		ActionKind::Trade);
}

void ActionDispatcher::recruitHero(const CGObjectInstance * townOrTavern,
	const CGHeroInstance * hero, HeroTypeID nextHero)
{
	if(!cb_)
		return;
	cb_->recruitHero(townOrTavern, hero, nextHero);
	queue_.track(uint32_t(lastRequestID_), uint16_t(ActionKind::Recruit),
		ActionKind::Recruit);
}

void ActionDispatcher::endTurn()
{
	if(cb_)
		cb_->endTurn();
}

void ActionDispatcher::dispatchRaw(const CPackForServer & pack, ActionKind kind)
{
	// The public CCallback does not expose raw packet submission; typed
	// methods above are the sanctioned path. Raw dispatch exists for future
	// engine patches that open IClient::sendRequest to AI plugins.
	logAi->warn("OmniAI: dispatchRaw called for kind %d; no public raw-send API on CCallback in 1.7.5",
		int(kind));
}

void ActionDispatcher::onRequestSent(const CPackForServer * pack, int requestID)
{
	lastRequestID_ = requestID;
}

void ActionDispatcher::onRequestRealized(const PackageApplied & pa)
{
	queue_.onReceipt(pa);
}

}
