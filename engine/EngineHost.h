/*
 * engine/EngineHost.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * What the v2 engine may ask of the plugin shell. The engine never touches
 * the shell's members directly: everything it reads or does goes through this
 * interface, so either side can be replaced without the other noticing.
 * Megaplan-OmniAI-2026-10-04.md, section 4.8.
 */
#pragma once

#include "../StdInc.h"
#include "int3.h"
#include "constants/EntityIdentifiers.h"
#include <memory>
#include <string>
#include <vector>

class CCallback;

VCMI_LIB_NAMESPACE_BEGIN
class CGHeroInstance;
class CGObjectInstance;
class CArmedInstance;
class CGTownInstance;
struct CPathsInfo;
VCMI_LIB_NAMESPACE_END

namespace omniai::v2
{

/// The shell's combat model on one fight: does the hero win, and what share of
/// its army is expected to remain afterwards (0..1, calibrated against logged
/// fights).
struct FightEstimate
{
	bool win = false;
	double kept = 0.0;
};

/// An enemy hero the shell has seen recently: where, how strong (army value),
/// how far it walks in a day (tiles), and how many days ago it was seen.
struct FoeInfo
{
	int32_t id = -1;
	int3 pos;
	double strength = 0.0;
	int reach = 0;
	int ageDays = 0;
};

/// A town of ours that an enemy army can reach, with that army's strength.
struct TownThreat
{
	int32_t townId = -1;
	double threat = 0.0;
};

class IEngineHost
{
public:
	virtual ~IEngineHost() = default;

	// The game and the log.
	virtual CCallback & game() = 0;
	virtual PlayerColor me() const = 0;
	virtual void logLine(const std::string & text) = 0;
	virtual void logDetail(const std::string & text) = 0;

	// Movement and paths.
	virtual std::shared_ptr<const CPathsInfo> paths(const CGHeroInstance * hero) = 0;
	/// This turn's part of the route to `target`, as move anchors, in order.
	virtual std::vector<int3> routeSteps(const CPathsInfo * paths, const CGHeroInstance * hero,
		const int3 & target) = 0;
	/// Moves the hero along `steps` (anchors). Steps out of a garrison first.
	virtual bool move(const CGHeroInstance * hero, const std::vector<int3> & steps) = 0;
	/// Server queries (dialogs) still waiting for our answer; a move sent while
	/// one is open is refused.
	virtual int openQueryCount() const = 0;

	// Fights.
	virtual FightEstimate estimate(const CGHeroInstance * hero, const CArmedInstance * target) = 0;
	/// The strongest hostile that guards `tile` (on it or around it).
	virtual const CArmedInstance * guardAt(const int3 & tile, const CGHeroInstance * hero) = 0;
	/// The hostile a step onto `tile` would fight (monsters around it, owners on it).
	virtual const CArmedInstance * guardOnStep(const int3 & tile, const CGHeroInstance * hero) = 0;

	// Valuation, in army value. The legacy numbers to begin with.
	virtual double worth(const CGObjectInstance * obj, const CGHeroInstance * hero) = 0;
	virtual double pocketBehind(const int3 & guardPos, const CGHeroInstance * hero) = 0;
	virtual double xpValue(const CGHeroInstance * hero, double xp) = 0;
	virtual double horizonDays() = 0;
	/// How badly a resource is needed: 1 when there is enough, more when short
	/// or when a building waits on it.
	virtual double resourceNeed(int resource) = 0;

	// Threats.
	virtual std::vector<FoeInfo> foes() = 0;
	virtual std::vector<TownThreat> threatenedTowns() = 0;

	// Army handling.
	virtual void giveArmy(const CGHeroInstance * from, const CGHeroInstance * to) = 0;
	/// Moves a town's garrison onto a hero standing in it.
	virtual void takeGarrison(const CGTownInstance * town, const CGHeroInstance * hero) = 0;
	virtual void setMainHero(int32_t heroId) = 0;

	// Towns: building, recruiting and trading, and one hero hire.
	virtual void townPhase(bool firstOfDay) = 0;
	virtual bool hireOne() = 0;
};

}
