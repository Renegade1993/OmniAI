/*
 * engine/Engine.cpp, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * The v2 execution engine. See Engine.h for the shape of a day.
 */
#include "../StdInc.h"
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "callback/CCallback.h"
#include "mapObjects/CGHeroInstance.h"
#include "mapObjects/CGObjectInstance.h"
#include "mapObjects/CGTownInstance.h"
#include "mapObjects/CGDwelling.h"
#include "mapObjects/CGResource.h"
#include "mapObjects/CGCreature.h"
#include "mapObjects/MiscObjects.h"
#include "mapping/TerrainTile.h"
#include "pathfinder/CGPathNode.h"
#include "rewardable/Interface.h"
#include "rewardable/Configuration.h"
#include "rewardable/Reward.h"

namespace omniai::v2
{

namespace
{
int chebyshev(const int3 & a, const int3 & b)
{
	return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

std::string fmt0(double v)
{
	std::ostringstream o;
	o << std::fixed << std::setprecision(0) << v;
	return o.str();
}

std::string fmt2(double v)
{
	std::ostringstream o;
	o << std::fixed << std::setprecision(2) << v;
	return o.str();
}
}

Engine::Engine(IEngineHost & host, Settings settings)
	: host_(host), s_(settings)
{
}

double Engine::armyOf(const CGHeroInstance * hero)
{
	return hero ? double(hero->getArmyStrength()) : 0.0;
}

std::string Engine::kindName(Target::Kind kind)
{
	switch(kind)
	{
		case Target::Kind::Visit: return "visit";
		case Target::Kind::Monster: return "fight";
		case Target::Kind::OwnTown: return "town";
		case Target::Kind::Gather: return "gather";
		case Target::Kind::Explore: return "explore";
		case Target::Kind::EnemyHero: return "hunt";
		case Target::Kind::EnemyTown: return "capture";
	}
	return "?";
}

void Engine::makeTurn()
{
	CCallback & cb = host_.game();
	day_ = cb.getDate(Date::DAY);
	blocked_.clear();
	doneToday_.clear();
	noMoves_.clear();
	movesToday_ = 0;

	hireToCap();
	host_.townPhase(true);

	int passes = 0;
	for(int pass = 1; pass <= s_.maxPasses; ++pass)
	{
		passes = pass;
		if(pass > 1)
			host_.townPhase(false);
		pickMainHero();
		// A hero in a garrison slot has no paths, so no job ever reaches it:
		// the town code seats the doorway hero there to free the door for a
		// hire, and nothing stepped it out again (V2M1W2 map 10: the main
		// hero, 55444, sat in Ayer from day 26, while a support hero walked at
		// the occupied door every pass). Out it comes, unless its town is
		// under threat.
		{
			const std::vector<TownThreat> threats = host_.threatenedTowns();
			for(const CGTownInstance * town : cb.getTownsInfo())
			{
				if(!town || !town->getGarrisonHero() || town->getVisitingHero())
					continue;
				bool threatened = false;
				for(const TownThreat & th : threats)
					if(th.townId == town->id.getNum())
						threatened = true;
				if(threatened)
					continue;
				const CGHeroInstance * h = town->getGarrisonHero();
				if(pass == 1)
					host_.logLine("   V2 UNGARRISON " + h->getNameTranslated() + " steps out of " + town->getNameTranslated());
				host_.leaveGarrison(town);
			}
		}
		const std::vector<Target> targets = proposeTargets();
		const std::vector<Option> plan = assign(scoreOptions(targets), targets);
		if(plan.empty())
			break;
		// A pass in which every assigned walk failed still made progress:
		// those jobs are now blocked for the day and the next pass gives the
		// heroes their next-best ones. The day ends only when a pass neither
		// moved anyone nor ruled anything out (V2M1J map 01: heroes ended
		// days with all their movement because one failed walk ended the
		// loop).
		const size_t blockedBefore = blocked_.size();
		if(!execute(plan, targets, pass) && blocked_.size() == blockedBefore)
			break;
	}

	int heroes = 0, idle = 0;
	double mpLeft = 0.0, mpAll = 0.0;
	std::ostringstream left;
	for(const CGHeroInstance * h : cb.getHeroesInfo())
	{
		if(!h)
			continue;
		++heroes;
		const double lim = double(std::max(1, h->movementPointsLimit(true)));
		mpLeft += std::min(lim, double(h->movementPointsRemaining()));
		mpAll += lim;
		if(h->movementPointsRemaining() >= h->movementPointsLimit(true))
			++idle;
		if(h->movementPointsRemaining() > 0.25 * lim)
			left << " " << h->getNameTranslated() << ":" << fmt0(100.0 * h->movementPointsRemaining() / lim) << "%";
	}
	// Movement left unspent at the end of the day, the share of every hero's
	// allowance, and which heroes stopped with more than a quarter left.
	host_.logLine("   V2 d" + std::to_string(day_) + " passes=" + std::to_string(passes)
		+ " moves=" + std::to_string(movesToday_) + " heroes=" + std::to_string(heroes)
		+ " unmoved=" + std::to_string(idle) + " mpLeft=" + fmt0(100.0 * mpLeft / std::max(1.0, mpAll)) + "%"
		+ " main=" + std::to_string(mainHero_) + " dens=" + fmt0(density_)
		+ (left.str().empty() ? "" : " |" + left.str()));
}

int Engine::chooseSecondarySkill(const CGHeroInstance * hero, const std::vector<SecondarySkill> & offered) const
{
	// The legacy choice was random among known skills, so a main hero could
	// end up with Eagle Eye and Navigation while Nullkiller's took Logistics
	// and Offence. The fighting hero values movement and the skills that
	// multiply every stack; the others value movement and income.
	if(offered.empty())
		return 0;
	const bool fighter = hero && hero->id.getNum() == mainHero_;
	auto score = [&](SecondarySkill s) -> double
	{
		double v = 0.0;
		switch(s.toEnum())
		{
			case SecondarySkill::LOGISTICS: v = 10; break;
			case SecondarySkill::OFFENCE: v = fighter ? 9 : 1; break;
			case SecondarySkill::ARMORER: v = fighter ? 9 : 1; break;
			case SecondarySkill::EARTH_MAGIC: v = fighter ? 8 : 2; break;
			case SecondarySkill::NECROMANCY: v = 8; break;
			case SecondarySkill::AIR_MAGIC: v = fighter ? 6 : 2; break;
			case SecondarySkill::TACTICS: v = fighter ? 6 : 0; break;
			case SecondarySkill::ARCHERY: v = fighter ? 6 : 0; break;
			case SecondarySkill::WISDOM: v = fighter ? 6 : 2; break;
			case SecondarySkill::PATHFINDING: v = fighter ? 5 : 8; break;
			case SecondarySkill::LEADERSHIP: v = fighter ? 5 : 1; break;
			case SecondarySkill::RESISTANCE: v = fighter ? 5 : 0; break;
			case SecondarySkill::INTELLIGENCE: v = fighter ? 4 : 1; break;
			case SecondarySkill::DIPLOMACY: v = 4; break;
			case SecondarySkill::WATER_MAGIC: v = fighter ? 4 : 1; break;
			case SecondarySkill::FIRE_MAGIC: v = fighter ? 4 : 1; break;
			case SecondarySkill::LUCK: v = fighter ? 4 : 1; break;
			case SecondarySkill::SORCERY: v = fighter ? 3 : 0; break;
			case SecondarySkill::SCOUTING: v = fighter ? 2 : 6; break;
			case SecondarySkill::ESTATES: v = fighter ? 2 : 6; break;
			case SecondarySkill::BALLISTICS: v = fighter ? 2 : 0; break;
			case SecondarySkill::ARTILLERY: v = fighter ? 2 : 0; break;
			case SecondarySkill::LEARNING: v = 2; break;
			case SecondarySkill::MYSTICISM: v = 1; break;
			case SecondarySkill::SCHOLAR: v = 1; break;
			case SecondarySkill::FIRST_AID: v = 1; break;
			case SecondarySkill::NAVIGATION: v = 0; break;
			case SecondarySkill::EAGLE_EYE: v = 0; break;
			default: v = 1; break;
		}
		// Deepening a skill the hero already has is worth a little more.
		if(hero && hero->getSecSkillLevel(s) > 0)
			v += 2;
		return v;
	};
	int best = 0;
	for(int i = 1; i < int(offered.size()); ++i)
		if(score(offered[i]) > score(offered[best]))
			best = i;
	return best;
}

void Engine::hireToCap()
{
	CCallback & cb = host_.game();
	int towns = 0;
	for(const CGTownInstance * t : cb.getTownsInfo())
		if(t)
			++towns;
	const int cap = std::min(s_.maxHeroes, s_.baseHeroes + std::max(0, towns - 1));
	for(int i = 0; i < 3 && int(cb.getHeroesInfo().size()) < cap; ++i)
		if(!host_.hireOne())
			break;
}

void Engine::pickMainHero()
{
	CCallback & cb = host_.game();
	const CGHeroInstance * current = mainHero_ >= 0 ? cb.getHero(ObjectInstanceID(mainHero_)) : nullptr;
	const CGHeroInstance * best = nullptr;
	for(const CGHeroInstance * h : cb.getHeroesInfo())
		if(h && (!best || h->getTotalStrength() > best->getTotalStrength()))
			best = h;
	// Keep the incumbent unless another hero is plainly stronger, so the role
	// (and the army that flows to it) does not swap on one week's recruits.
	if(current && best && best != current
		&& double(best->getTotalStrength()) < 1.3 * double(current->getTotalStrength()))
		best = current;
	const int32_t id = best ? best->id.getNum() : -1;
	if(id != mainHero_)
	{
		mainHero_ = id;
		if(best)
			host_.logLine("   V2 MAIN " + best->getNameTranslated() + " (army "
				+ fmt0(armyOf(best)) + ", level " + std::to_string(best->level) + ")");
	}
	host_.setMainHero(mainHero_);
}

std::vector<Target> Engine::proposeTargets()
{
	CCallback & cb = host_.game();
	const PlayerColor me = host_.me();
	std::vector<Target> out;
	const CGHeroInstance * main = mainHero_ >= 0 ? cb.getHero(ObjectInstanceID(mainHero_)) : nullptr;
	double knownValue = 0.0;

	for(const CGObjectInstance * obj : cb.getAllVisitableObjs())
	{
		if(!obj)
			continue;
		const bool own = obj->tempOwner == me;
		Target t;
		t.id = obj->id;
		t.tile = obj->visitablePos();
		t.name = obj->getObjectName();

		if(const auto * hero = dynamic_cast<const CGHeroInstance *>(obj))
		{
			if(own || !hero->tempOwner.isValidPlayer())
				continue;
			t.kind = Target::Kind::EnemyHero;
			t.reward = s_.enemyArmyWorth * armyOf(hero) + s_.heroKillBonus;
			out.push_back(t);
			continue;
		}
		if(const auto * town = dynamic_cast<const CGTownInstance *>(obj))
		{
			if(own)
			{
				t.kind = Target::Kind::OwnTown;
				t.reward = double(town->getArmyStrength());   // the garrison pool
			}
			else
				t.kind = Target::Kind::EnemyTown;
			out.push_back(t);
			continue;
		}
		if(obj->ID == Obj::MONSTER)
		{
			t.kind = Target::Kind::Monster;
			out.push_back(t);
			continue;
		}
		// Everything else is worth whatever the valuation says it is to the
		// hero that would take it; scoring asks per hero.
		const bool candidate = dynamic_cast<const CGMine *>(obj) || dynamic_cast<const CGResource *>(obj)
			|| dynamic_cast<const CGDwelling *>(obj) || dynamic_cast<const CGArtifact *>(obj)
			|| dynamic_cast<const Rewardable::Interface *>(obj) || obj->ID == Obj::PRISON;
		if(!candidate)
			continue;
		if(own && !dynamic_cast<const CGDwelling *>(obj))
			continue;
		t.kind = Target::Kind::Visit;
		if(main)
		{
			// Its worth to the main hero now: the onward index reads it, and a
			// weekly object already taken this week is worth nothing to it.
			t.reward = std::max(0.0, visitValue(obj, main));
			if(!discovered_.count(obj->id.getNum()))
			{
				discovered_[obj->id.getNum()] = t.reward;
				discoveredSum_ += t.reward;
			}
		}
		out.push_back(t);
	}

	// Gather: each support hero may walk its army to the main hero.
	if(main)
		for(const CGHeroInstance * h : cb.getHeroesInfo())
		{
			if(!h || h == main)
				continue;
			const double army = armyOf(h);
			if(army < std::max(300.0, 0.08 * armyOf(main)))
				continue;
			// A hand-over that moved nothing (the main hero's seven slots hold
			// other creatures, and none of ours beats its weakest) benches the
			// job for three days; otherwise it wins every pass and the hero
			// stands beside the main hero all day (V2M1S map 16, days 26 to 45:
			// three heroes, 15 passes a day, the same 26636 never handed over).
			const auto failed = gatherFailed_.find(h->id.getNum());
			if(failed != gatherFailed_.end() && day_ - failed->second < 3)
				continue;
			Target t;
			t.kind = Target::Kind::Gather;
			t.id = main->id;
			t.tile = main->visitablePos();
			t.onlyHero = h->id.getNum();
			t.reward = s_.gatherWorth * army;
			t.name = "army to " + main->getNameTranslated();
			out.push_back(t);
		}

	proposeExplore(out, knownValue);
	return out;
}

void Engine::proposeExplore(std::vector<Target> & out, double knownValue)
{
	// Frontier tiles: seen, open, with unseen ground beside them. Each is
	// worth the unseen tiles inside a sight radius of it, counted with a
	// summed-area table so the whole frontier costs one pass over the map.
	// The worth of an unseen tile scales with what the seen ground has turned
	// out to hold.
	CCallback & cb = host_.game();
	const int3 size = cb.getMapSize();
	const int W = size.x, H = size.y;
	visLevels_.assign(size.z, std::vector<uint8_t>(W * H, 0));
	int visible = 0;
	for(int z = 0; z < size.z; ++z)
		for(int y = 0; y < H; ++y)
			for(int x = 0; x < W; ++x)
				if(cb.isVisible(int3(x, y, z)))
				{
					visLevels_[z][y * W + x] = 1;
					++visible;
				}
	const auto & visLevels = visLevels_;
	// What the seen ground held when it was first seen, per seen tile. The
	// value still lying on it fell as heroes collected it, so exploring lost
	// to the leftovers nearby: by day 28 of the M1 gate (V2M1T) our heroes had
	// seen 0.78 of the tiles Nullkiller's had, by day 56 0.71.
	const double density = std::clamp(0.5 * discoveredSum_ / std::max(1, visible), 5.0, 150.0);
	density_ = density;
	const int R = 5;
	for(int z = 0; z < size.z; ++z)
	{
		const std::vector<uint8_t> & vis = visLevels[z];
		auto seen = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && vis[y * W + x]; };
		std::vector<int> fog((W + 1) * (H + 1), 0);
		auto at = [&](int x, int y) -> int & { return fog[y * (W + 1) + x]; };
		for(int y = 0; y < H; ++y)
			for(int x = 0; x < W; ++x)
				at(x + 1, y + 1) = (vis[y * W + x] ? 0 : 1)
					+ at(x, y + 1) + at(x + 1, y) - at(x, y);
		auto unseenAround = [&](int cx, int cy)
		{
			const int x0 = std::max(0, cx - R), y0 = std::max(0, cy - R);
			const int x1 = std::min(W, cx + R + 1), y1 = std::min(H, cy + R + 1);
			return at(x1, y1) - at(x0, y1) - at(x1, y0) + at(x0, y0);
		};
		// Coarse grid of frontier picks, one per 6x6 cell, so the target list
		// stays short and two heroes do not chase the same strip.
		const int CELL = 6;
		for(int gy = 0; gy < H; gy += CELL)
			for(int gx = 0; gx < W; gx += CELL)
			{
				int bestGain = 0;
				int3 bestTile;
				for(int y = gy; y < std::min(H, gy + CELL); ++y)
					for(int x = gx; x < std::min(W, gx + CELL); ++x)
					{
						const int3 p(x, y, z);
						if(!vis[y * W + x])
							continue;
						const TerrainTile * tile = cb.getTile(p, false);
						if(!tile || tile->blocked() || tile->isWater())
							continue;
						bool edge = false;
						for(int dy = -1; dy <= 1 && !edge; ++dy)
							for(int dx = -1; dx <= 1 && !edge; ++dx)
							{
								const int qx = x + dx, qy = y + dy;
								if(qx >= 0 && qy >= 0 && qx < W && qy < H && !seen(qx, qy))
									edge = true;
							}
						if(!edge)
							continue;
						const int gain = unseenAround(x, y);
						if(gain > bestGain)
						{
							bestGain = gain;
							bestTile = p;
						}
					}
				if(bestGain < 8)
					continue;
				Target t;
				t.kind = Target::Kind::Explore;
				t.id = ObjectInstanceID();
				t.tile = bestTile;
				t.reward = density * double(bestGain);
				t.name = "unseen ground (" + std::to_string(bestGain) + " tiles)";
				out.push_back(t);
			}
	}

	// A two-way monolith or a subterranean gate whose other end nobody of ours
	// has seen. The channel reads impassable until then, so no path runs
	// through it and the ground beyond is in no frontier: stepping in is the
	// only way to find it. Worth a full sight radius of unseen ground. In
	// V2M1AG and AI (60 games) Nullkiller's heroes made 48 one-day jumps of 30
	// tiles or more on the three maps with monoliths, ours 1.
	const int sightArea = (2 * R + 1) * (2 * R + 1);
	for(const CGObjectInstance * obj : cb.getAllVisitableObjs())
	{
		if(!obj || (obj->ID != Obj::MONOLITH_TWO_WAY && obj->ID != Obj::SUBTERRANEAN_GATE))
			continue;
		const auto * port = dynamic_cast<const CGTeleport *>(obj);
		if(!port || !port->isEntrance() || !cb.isVisible(obj->visitablePos()))
			continue;
		bool otherEndSeen = false;
		for(const ObjectInstanceID & exit : cb.getTeleportChannelExits(port->channel, host_.me()))
			if(exit != obj->id)
				otherEndSeen = true;
		if(otherEndSeen)
			continue;
		Target t;
		t.kind = Target::Kind::Explore;
		t.id = obj->id;
		t.tile = obj->visitablePos();
		t.reward = density * double(sightArea);
		t.name = "unseen ground through " + obj->getObjectName();
		out.push_back(t);
	}
}

double Engine::xpScale(const CGHeroInstance * hero) const
{
	return hero && hero->id.getNum() == mainHero_ ? s_.xpScaleMain : s_.xpScaleSupport;
}

double Engine::levelWorth(const CGHeroInstance * hero) const
{
	// One level, or one primary skill point, is about 5 percent more damage
	// dealt or 2.5 percent less taken for the rest of the game, on an army
	// that grows (about threefold over the next weeks, measured on both
	// sides): priced at 5 percent of three times today's army for the main
	// hero (a quarter of that for the others), with a floor so an early level
	// is not worth nothing.
	return xpScale(hero) * 0.05 * std::max(4000.0, 3.0 * armyOf(hero));
}

double Engine::xpWorth(const CGHeroInstance * hero, double xp)
{
	// The legacy price is (xp / experience to the next level) x 5 percent of
	// the army; the share of a level is recovered from it and priced at
	// levelWorth.
	if(!hero || xp <= 0.0)
		return 0.0;
	const double legacy = host_.xpValue(hero, xp);
	const double levelShare = legacy / (0.05 * std::max(1.0, armyOf(hero)));
	return levelShare * levelWorth(hero);
}

double Engine::visitValue(const CGObjectInstance * obj, const CGHeroInstance * hero)
{
	if(!obj || !hero)
		return 0.0;
	const PlayerColor me = host_.me();
	auto price = [](int res) -> double
	{
		if(res == GameResID(EGameResID::GOLD).getNum())
			return 1.0;
		if(res == GameResID(EGameResID::WOOD).getNum() || res == GameResID(EGameResID::ORE).getNum())
			return 100.0;
		return 200.0;
	};
	auto need = [&](int res) -> double
	{
		const double cap = res == GameResID(EGameResID::GOLD).getNum() ? s_.goldNeedCap : s_.needCap;
		return std::clamp(host_.resourceNeed(res), 1.0, cap);
	};
	constexpr double APG = 1.2;
	constexpr double RATE = 0.03;
	const double annuity = (1.0 - std::pow(1.0 + RATE, -host_.horizonDays())) / RATE;

	// A mine or a pile: what it yields, priced up by how badly the resource
	// is needed now (a building waiting on ore makes ore the scarce thing).
	if(const auto * mine = dynamic_cast<const CGMine *>(obj))
	{
		if(mine->tempOwner == me)
			return 0.0;
		const int res = mine->producedResource.getNum();
		const double deny = mine->tempOwner.isValidPlayer() ? 2.0 : 1.0;
		return double(mine->producedQuantity) * price(res) * APG * annuity * need(res) * deny;
	}
	if(const auto * pile = dynamic_cast<const CGResource *>(obj))
	{
		const int res = pile->resourceID().getNum();
		return double(pile->getAmount()) * price(res) * APG * need(res);
	}

	// A visit this hero (or this week, this side) has already made gives
	// nothing more; the legacy price does not check that.
	if(!dynamic_cast<const CGDwelling *>(obj) && obj->wasVisited(hero))
		return 0.0;
	double value = host_.worth(obj, hero);
	// The legacy price counts experience at one times and primary skills not
	// at all; both make the hero that fights stronger for the rest of the game.
	if(const auto * rw = dynamic_cast<const Rewardable::Interface *>(obj))
	{
		double extra = 0.0;
		for(ui32 index : rw->getAvailableRewards(hero, Rewardable::EEventType::EVENT_FIRST_VISIT))
		{
			if(index >= rw->configuration.info.size())
				continue;
			const auto & reward = rw->configuration.info[index].reward;
			double e = xpWorth(hero, double(reward.heroExperience)) - host_.xpValue(hero, double(reward.heroExperience));
			for(si32 points : reward.primary)
				e += double(std::max(0, points)) * levelWorth(hero);
			extra = std::max(extra, e);
		}
		value += extra;
	}
	return value;
}

double Engine::onwardValue(const CGHeroInstance * hero, const int3 & tile) const
{
	// What lies just past a visit, seen from the hero: visits within the
	// radius of it and further from the hero than it is, nearer ones counting
	// more. Days on several visits covered 7.3 tiles to single-target days'
	// 12.5 to 16 (V2M1Y, AC, AD): heroes hopped about a cluster, nearest first.
	if(!hero || s_.onwardShare <= 0.0)
		return 0.0;
	const int3 from = hero->visitablePos();
	const int reach = chebyshev(from, tile);
	const int R = s_.onwardRadius;
	double sum = 0.0;
	for(int cy = (tile.y - R) / 8 - 1; cy <= (tile.y + R) / 8; ++cy)
		for(int cx = (tile.x - R) / 8 - 1; cx <= (tile.x + R) / 8; ++cx)
		{
			const auto cell = onward_.find({cx, cy, tile.z});
			if(cell == onward_.end())
				continue;
			for(const auto & [p, v] : cell->second)
			{
				if(p == tile)
					continue;
				const int d = chebyshev(tile, p);
				if(d > R || chebyshev(from, p) <= reach)
					continue;
				sum += v * (1.0 - double(d) / double(R + 1));
			}
		}
	return s_.onwardShare * sum;
}

double Engine::gateValue(const CPathsInfo * paths, const int3 & guard)
{
	// A guard that stands in a passage is worth the ground it shuts off. From
	// the guard's tile, count the open or unseen tiles the hero cannot reach
	// today (the far side), flooding outward through such tiles and stopping
	// at ground the hero can already reach. A roadside stack with reachable
	// ground all round opens nothing.
	if(!paths || visLevels_.empty() || guard.z < 0 || guard.z >= int(visLevels_.size()))
		return 0.0;
	CCallback & cb = host_.game();
	const int3 size = cb.getMapSize();
	const int W = size.x, H = size.y;
	const std::vector<uint8_t> & vis = visLevels_[guard.z];
	auto reachable = [&](const int3 & p)
	{
		const CGPathNode * n = paths->getPathInfo(p);
		return n && n->reachable();
	};
	auto open = [&](const int3 & p)
	{
		if(!vis[p.y * W + p.x])
			return true;   // unseen: counted as open ground
		const TerrainTile * t = cb.getTile(p, false);
		return t && !t->blocked() && !t->isWater();
	};
	std::vector<int3> frontier;
	std::set<int3> seenSet;
	seenSet.insert(guard);
	for(int dy = -1; dy <= 1; ++dy)
		for(int dx = -1; dx <= 1; ++dx)
		{
			const int3 q(guard.x + dx, guard.y + dy, guard.z);
			if((dx || dy) && q.x >= 0 && q.y >= 0 && q.x < W && q.y < H && !reachable(q) && open(q))
			{
				frontier.push_back(q);
				seenSet.insert(q);
			}
		}
	int count = 0;
	constexpr int LIMIT = 600;
	for(size_t i = 0; i < frontier.size() && count < LIMIT; ++i)
	{
		const int3 p = frontier[i];
		++count;
		for(int dy = -1; dy <= 1; ++dy)
			for(int dx = -1; dx <= 1; ++dx)
			{
				const int3 q(p.x + dx, p.y + dy, p.z);
				if((!dx && !dy) || q.x < 0 || q.y < 0 || q.x >= W || q.y >= H || seenSet.count(q))
					continue;
				seenSet.insert(q);
				if(!reachable(q) && open(q))
					frontier.push_back(q);
			}
	}
	return 0.5 * density_ * double(count);
}

bool Engine::surroundingsSeen(const int3 & tile)
{
	// A guard hidden in the fog is not priced; a job is only taken once the
	// ground around it is in sight. Walking closer (or exploring) shows it.
	CCallback & cb = host_.game();
	for(int dy = -1; dy <= 1; ++dy)
		for(int dx = -1; dx <= 1; ++dx)
		{
			const int3 q(tile.x + dx, tile.y + dy, tile.z);
			if(cb.isInTheMap(q) && !cb.isVisible(q))
				return false;
		}
	return true;
}

bool Engine::dangerousFor(const CGHeroInstance * hero, const int3 & tile,
	const std::vector<FoeInfo> & foes) const
{
	const double ours = armyOf(hero);
	for(const FoeInfo & f : foes)
	{
		if(f.ageDays > 1 || f.strength <= ours || f.pos.z != tile.z)
			continue;
		if(chebyshev(f.pos, tile) <= f.reach * (1 + f.ageDays))
			return true;
	}
	return false;
}

bool Engine::scoreOne(const CGHeroInstance * hero, const CPathsInfo * paths, const Target & t,
	Option & out)
{
	if(!hero || !paths)
		return false;
	if(t.onlyHero >= 0 && t.onlyHero != hero->id.getNum())
		return false;
	if(blocked_.count({hero->id.getNum(), t.tile}))
		return false;
	// A visit the hero already made and that did not use the object up is
	// not repeated for a week (a refused dialog, a reward it cannot take).
	if(t.kind == Target::Kind::Visit)
	{
		auto v = visited_.find({hero->id.getNum(), t.tile});
		if(v != visited_.end() && day_ - v->second < 7)
			return false;
	}
	const bool isMain = hero->id.getNum() == mainHero_;
	const CGPathNode * node = paths->getPathInfo(t.tile);
	if(!node || !node->reachable())
		return false;
	const double maxMp = double(std::max(1, hero->movementPointsLimit(true)));
	const double days = double(node->turns)
		+ std::clamp(1.0 - double(node->moveRemains) / maxMp, 0.0, 1.0);
	if(days > s_.horizonDays)
		return false;

	double reward = 0.0, loss = 0.0;
	const double army = armyOf(hero);
	auto priceFight = [&](const CArmedInstance * foe) -> bool
	{
		if(!foe)
			return true;
		const FightEstimate e = host_.estimate(hero, foe);
		if(!e.win || e.kept < 1.0 - s_.lossBudget)
			return false;
		loss += (1.0 - e.kept) * army;
		return true;
	};

	CCallback & cb = host_.game();
	const CGObjectInstance * obj = t.id.hasValue() ? cb.getObj(t.id, false) : nullptr;
	switch(t.kind)
	{
		case Target::Kind::Visit:
		{
			if(!obj)
				return false;
			if(!surroundingsSeen(t.tile))
				return false;
			reward = visitValue(obj, hero);
			if(reward <= 0.0)
				return false;
			if(!priceFight(host_.guardAt(t.tile, hero)))
				return false;
			reward += onwardValue(hero, t.tile);
			break;
		}
		case Target::Kind::Monster:
		{
			const auto * armed = dynamic_cast<const CArmedInstance *>(obj);
			if(!armed)
				return false;
			double xp = 0.0;
			for(const auto & slot : armed->Slots())
				if(slot.second && slot.second->getCreature())
					xp += double(slot.second->getCount()) * double(slot.second->getCreature()->getMaxHealth());
			reward = xpWorth(hero, xp) + host_.pocketBehind(t.tile, hero) + gateValue(paths, t.tile);
			if(reward <= 0.0)
				return false;
			if(!priceFight(armed))
				return false;
			break;
		}
		case Target::Kind::OwnTown:
		{
			// The main hero collects a garrison worth the walk; a support hero
			// fetches one to carry to the main hero when the main hero is not
			// coming for it itself (Gather takes it the rest of the way).
			if(hero->visitablePos() == t.tile)
				return false;
			// Another of ours in the doorway: the walk ends beside it, if it
			// ends today; one arriving later finds the doorway as it is then
			// (most doorway heroes are a fresh hire on its way out).
			if(const auto * town = dynamic_cast<const CGTownInstance *>(obj))
				if(town->getVisitingHero() && town->getVisitingHero() != hero && node->turns == 0)
					return false;
			// A hero already walking to this town keeps the job at half the
			// pool it needed to start: the pool moves with every purchase and
			// every pickup, and a walk dropped halfway is a walk wasted (V2M1AC
			// and AD: 93 courier and garrison walks dropped as the pool dipped).
			const auto going = lastTile_.find(hero->id.getNum());
			const double keep = going != lastTile_.end() && going->second == t.tile ? 0.5 : 1.0;
			const CGHeroInstance * main = mainHero_ >= 0 ? cb.getHero(ObjectInstanceID(mainHero_)) : nullptr;
			const double mainArmy = main ? armyOf(main) : army;
			if(isMain)
			{
				if(t.reward < keep * std::max(800.0, 0.25 * army))
					return false;
				reward = s_.garrisonWorth * t.reward;
			}
			else
			{
				if(t.reward < keep * std::max(1500.0, 0.05 * mainArmy))
					return false;
				reward = s_.courierWorth * t.reward;
			}
			break;
		}
		case Target::Kind::Gather:
		{
			reward = t.reward;
			break;
		}
		case Target::Kind::Explore:
		{
			reward = t.reward;
			// A monolith or gate (the only explore targets that are objects) is
			// often guarded, and the guard is priced like a visit's: V2M1AK map
			// 07, two heroes walked to one behind Power Liches (58266) every day.
			if(obj && !priceFight(host_.guardAt(t.tile, hero)))
				return false;
			break;
		}
		case Target::Kind::EnemyHero:
		{
			const auto * foe = dynamic_cast<const CArmedInstance *>(obj);
			if(!foe)
				return false;
			const FightEstimate e = host_.estimate(hero, foe);
			if(!e.win || e.kept < 0.6)
				return false;
			loss = (1.0 - e.kept) * army;
			reward = t.reward;
			break;
		}
		case Target::Kind::EnemyTown:
		{
			if(!obj)
				return false;
			reward = host_.worth(obj, hero);
			if(reward <= 0.0)
				return false;
			// Holding no town, a town is the game itself.
			if(cb.getTownsInfo().empty())
				reward = std::max(reward, 10.0 * armyOf(hero)) + 100000.0;
			const auto * town = dynamic_cast<const CArmedInstance *>(obj);
			if(town && town->getArmyStrength() > 0)
			{
				const FightEstimate e = host_.estimate(hero, town);
				if(!e.win || e.kept < 1.0 - s_.lossBudget)
					return false;
				loss = (1.0 - e.kept) * army;
			}
			if(!priceFight(host_.guardAt(t.tile, hero) == town ? nullptr : host_.guardAt(t.tile, hero)))
				return false;
			break;
		}
	}

	const double net = reward - loss;
	if(net <= 0.0)
		return false;
	double score = std::pow(net, s_.valueExponent) / (days + 0.25);
	auto last = lastTile_.find(hero->id.getNum());
	if(last != lastTile_.end() && last->second == t.tile)
		score *= s_.momentum;
	out.hero = hero->id.getNum();
	out.reward = reward;
	out.loss = loss;
	out.days = days;
	out.score = score;
	return true;
}

std::vector<Option> Engine::scoreOptions(const std::vector<Target> & targets)
{
	CCallback & cb = host_.game();
	const std::vector<FoeInfo> foes = host_.foes();
	std::set<int3> ourTowns;
	for(const CGTownInstance * t : cb.getTownsInfo())
		if(t)
			ourTowns.insert(t->visitablePos());

	// The visits on offer, by 8x8 cell, each at its worth to the main hero
	// now, for onwardValue().
	onward_.clear();
	for(const Target & t : targets)
		if(t.kind == Target::Kind::Visit && t.reward > 0.0)
			onward_[{t.tile.x / 8, t.tile.y / 8, t.tile.z}].push_back({t.tile, t.reward});

	std::vector<Option> options;
	// The main hero standing in a threatened town stays there: its army, the
	// garrison it collected on arrival and the walls fight together. Walking
	// out to the next prize is how map 05's only town fell on day 28 (V2M1D).
	const std::vector<TownThreat> threats = host_.threatenedTowns();
	if(mainHero_ >= 0 && !doneToday_.count(mainHero_))
		if(const CGHeroInstance * main = cb.getHero(ObjectInstanceID(mainHero_)))
			for(const TownThreat & threat : threats)
				if(const auto * town = dynamic_cast<const CGTownInstance *>(cb.getObj(ObjectInstanceID(threat.townId), false)))
					if(town->visitablePos() == main->visitablePos())
					{
						doneToday_.insert(mainHero_);
						host_.logLine("   V2 HOLD " + main->getNameTranslated() + " stays in " + town->getNameTranslated()
							+ " (threat " + fmt0(threat.threat) + ")");
					}
	// Once a day, the main hero's view of the monsters in reach: what each
	// fight is predicted to cost and to pay, so a log shows why it fought or
	// did not.
	if(!monsterLogDay_.count(day_) && mainHero_ >= 0)
	{
		monsterLogDay_.insert(day_);
		const CGHeroInstance * main = cb.getHero(ObjectInstanceID(mainHero_));
		const auto shared = main ? host_.paths(main) : nullptr;
		std::ostringstream o;
		int n = 0;
		for(const Target & t : targets)
		{
			if((t.kind != Target::Kind::Monster && t.kind != Target::Kind::EnemyTown) || !shared || n >= 10)
				continue;
			const CGPathNode * node = shared->getPathInfo(t.tile);
			if(!node || !node->reachable() || node->turns > 2)
				continue;
			const auto * armed = dynamic_cast<const CArmedInstance *>(cb.getObj(t.id, false));
			if(!armed)
				continue;
			const FightEstimate e = host_.estimate(main, armed);
			double xp = 0.0;
			for(const auto & slot : armed->Slots())
				if(slot.second && slot.second->getCreature())
					xp += double(slot.second->getCount()) * double(slot.second->getCreature()->getMaxHealth());
			o << " " << (t.kind == Target::Kind::EnemyTown ? "TOWN " : "") << t.name << "(" << fmt0(double(armed->getArmyStrength())) << ")"
			  << (e.win ? " win " : " lose ") << fmt2(e.kept) << " xp " << fmt0(xp)
			  << " r " << fmt0(xpWorth(main, xp) + host_.pocketBehind(t.tile, main))
			  << " gate " << fmt0(t.kind == Target::Kind::Monster ? gateValue(shared.get(), t.tile) : 0.0)
			  << " t" << int(node->turns) << ";";
			++n;
		}
		if(n)
			host_.logLine("   V2 MONSTERS d" + std::to_string(day_) + " " + main->getNameTranslated()
				+ " (" + fmt0(armyOf(main)) + "):" + o.str());
	}
	for(const CGHeroInstance * hero : cb.getHeroesInfo())
	{
		if(!hero || hero->movementPointsRemaining() <= 0 || doneToday_.count(hero->id.getNum()))
			continue;
		const auto shared = host_.paths(hero);
		const CPathsInfo * paths = shared.get();
		if(!paths)
			continue;
		for(size_t i = 0; i < targets.size(); ++i)
		{
			const Target & t = targets[i];
			// Without a town the game is lost in seven days: any town is worth
			// the risk, so the danger rule does not hold heroes back from one.
			const bool townless = ourTowns.empty() && t.kind == Target::Kind::EnemyTown;
			if(!ourTowns.count(t.tile) && t.kind != Target::Kind::EnemyHero && !townless
				&& dangerousFor(hero, t.tile, foes))
				continue;
			Option o;
			o.target = i;
			if(scoreOne(hero, paths, t, o))
				options.push_back(o);
		}
	}

	// A hero with nothing worth doing goes to the nearest town of ours: it
	// adds to the town's defence, and it is where the next courier job starts.
	// Idle heroes were 4 to 7 percent of all hero-days (V2M1F, V2M1I).
	{
		std::set<int32_t> withJob;
		for(const Option & o : options)
			withJob.insert(o.hero);
		for(const CGHeroInstance * hero : cb.getHeroesInfo())
		{
			if(!hero || withJob.count(hero->id.getNum()) || hero->movementPointsRemaining() <= 0
				|| doneToday_.count(hero->id.getNum()) || ourTowns.count(hero->visitablePos()))
				continue;
			const auto shared = host_.paths(hero);
			if(!shared)
				continue;
			double bestDays = 1e9;
			size_t bestTarget = targets.size();
			for(size_t i = 0; i < targets.size(); ++i)
			{
				if(targets[i].kind != Target::Kind::OwnTown || blocked_.count({hero->id.getNum(), targets[i].tile}))
					continue;
				// A gate another of ours already stands in cannot be entered: the
				// walk ends beside it, and the hero tried again every pass (V2M1S
				// map 07 day 42: Elleshar 12 passes, then Wystan 18, one step each).
				bool gateTaken = false;
				for(const CGHeroInstance * other : cb.getHeroesInfo())
					if(other && other != hero && other->visitablePos() == targets[i].tile)
						gateTaken = true;
				if(gateTaken)
					continue;
				const CGPathNode * node = shared->getPathInfo(targets[i].tile);
				if(!node || !node->reachable())
					continue;
				const double d = double(node->turns) + std::clamp(1.0 - double(node->moveRemains)
					/ double(std::max(1, hero->movementPointsLimit(true))), 0.0, 1.0);
				if(d < bestDays)
				{
					bestDays = d;
					bestTarget = i;
				}
			}
			if(bestTarget < targets.size())
			{
				Option o;
				o.hero = hero->id.getNum();
				o.target = bestTarget;
				o.days = bestDays;
				o.score = 1e-6;
				options.push_back(o);
			}
		}
	}

	// A garrison or courier job pulls a hero off a job it has not reached
	// only at twice that job's score. Town jobs jump as the town code buys into the
	// garrison every pass and as the week's growth lands, and a hero dropped
	// its walk for one: jobs given up for a town job, courier walks dropped as
	// a pool dipped, and the recall below flipping between towns were 38% of
	// the hero-days lost to dropped jobs in V2M1AC and AD (switch_trace.py).
	{
		std::map<int32_t, double> current;   // hero -> score of the unreached job it is walking to
		for(const Option & o : options)
		{
			const auto lt = lastTile_.find(o.hero);
			if(lt == lastTile_.end() || targets[o.target].tile != lt->second)
				continue;
			const CGHeroInstance * h = cb.getHero(ObjectInstanceID(o.hero));
			if(!h || (lt->second.z == h->visitablePos().z && chebyshev(lt->second, h->visitablePos()) <= 2))
				continue;
			current[o.hero] = std::max(current[o.hero], o.score);
		}
		options.erase(std::remove_if(options.begin(), options.end(), [&](const Option & o)
		{
			const auto c = current.find(o.hero);
			return c != current.end() && targets[o.target].kind == Target::Kind::OwnTown
				&& targets[o.target].tile != lastTile_[o.hero] && o.score < s_.townSwitchMargin * c->second;
		}), options.end());
	}

	// A town under threat calls the main hero home, whatever else is on offer.
	// It stays called to the same town while that town is still threatened:
	// the threats are re-read every pass, every recall scored the same, and
	// the main hero was sent from one town to another and back (V2M1AD map
	// 04, days 60-61: Kanan, then Silverspire 78 tiles away; 36 recalls
	// dropped halfway cost 41 hero-days). Among new threats the nearest town
	// wins.
	const CGHeroInstance * main = mainHero_ >= 0 ? cb.getHero(ObjectInstanceID(mainHero_)) : nullptr;
	bool recallStands = false;
	for(const TownThreat & threat : threats)
		if(threat.townId == recallTown_)
			recallStands = true;
	if(!recallStands)
		recallTown_ = -1;
	if(main && main->movementPointsRemaining() > 0 && !doneToday_.count(mainHero_))
		for(const TownThreat & threat : threats)
		{
			if(recallStands && threat.townId != recallTown_)
				continue;
			const auto * town = dynamic_cast<const CGTownInstance *>(cb.getObj(ObjectInstanceID(threat.townId), false));
			if(!town)
				continue;
			for(size_t i = 0; i < targets.size(); ++i)
				if(targets[i].kind == Target::Kind::OwnTown && targets[i].tile == town->visitablePos()
					&& main->visitablePos() != town->visitablePos())
				{
					const auto shared = host_.paths(main);
					const CGPathNode * node = shared ? shared->getPathInfo(town->visitablePos()) : nullptr;
					if(!node || !node->reachable())
						continue;
					Option o;
					o.hero = main->id.getNum();
					o.target = i;
					o.reward = host_.worth(town, main);
					o.days = double(node->turns) + 0.5;
					o.score = 1e12 / (1.0 + o.days);   // defending outranks every gain; the nearest town first
					options.push_back(o);
				}
		}
	return options;
}

std::vector<Option> Engine::assign(std::vector<Option> options, const std::vector<Target> & targets)
{
	// Greedy assignment: best pair first, each hero once, each target once
	// (a hero's own Gather job aside, which no one else can take anyway).
	std::sort(options.begin(), options.end(),
		[](const Option & a, const Option & b) { return a.score > b.score; });
	std::set<int32_t> heroesTaken;
	std::set<int3> tilesTaken;
	std::vector<Option> plan;
	for(const Option & o : options)
	{
		if(heroesTaken.count(o.hero))
			continue;
		const Target & t = targets[o.target];
		const bool exclusive = t.kind != Target::Kind::Gather;
		if(exclusive && tilesTaken.count(t.tile))
			continue;
		// Two explorers do not head for the same patch of fog.
		if(t.kind == Target::Kind::Explore)
		{
			bool near = false;
			for(const Option & p : plan)
				if(targets[p.target].kind == Target::Kind::Explore
					&& targets[p.target].tile.z == t.tile.z
					&& chebyshev(targets[p.target].tile, t.tile) <= 12)
					near = true;
			if(near)
				continue;
		}
		heroesTaken.insert(o.hero);
		if(exclusive)
			tilesTaken.insert(t.tile);
		plan.push_back(o);
	}
	return plan;
}

bool Engine::execute(const std::vector<Option> & plan, const std::vector<Target> & targets, int pass)
{
	CCallback & cb = host_.game();
	// Army joins the main hero before the main hero walks away from it.
	std::vector<Option> order = plan;
	std::stable_sort(order.begin(), order.end(), [&](const Option & a, const Option & b)
	{
		const bool ga = targets[a.target].kind == Target::Kind::Gather;
		const bool gb = targets[b.target].kind == Target::Kind::Gather;
		return ga && !gb;
	});

	bool anyMoved = false;
	for(const Option & o : order)
	{
		const Target & t = targets[o.target];
		const CGHeroInstance * hero = cb.getHero(ObjectInstanceID(o.hero));
		if(!hero || hero->movementPointsRemaining() <= 0)
			continue;
		const auto shared = host_.paths(hero);
		std::vector<int3> steps = host_.routeSteps(shared.get(), hero, t.tile);

		// A hero leaving a job it has not reached, more than two tiles off:
		// why the old one lost (V2M1AA: half the jobs more than a day away were
		// dropped, 80% of them for no other hero).
		{
			const auto lt = lastTile_.find(o.hero);
			if(lt != lastTile_.end() && lt->second != t.tile && lt->second != hero->visitablePos()
				&& (lt->second.z != hero->visitablePos().z || chebyshev(lt->second, hero->visitablePos()) > 2))
			{
				std::string why = "gone";
				for(const Target & old : targets)
					if(old.tile == lt->second && (old.onlyHero < 0 || old.onlyHero == o.hero))
					{
						Option was;
						if(!scoreOne(hero, shared.get(), old, was))
							why = "unscorable (" + std::string(kindName(old.kind)) + ")";
						else
							why = "outscored " + fmt0(was.score) + " < " + fmt0(o.score) + " (" + kindName(old.kind)
								+ " d=" + fmt2(was.days) + ")";
						break;
					}
				host_.logLine("   V2 SWITCH " + hero->getNameTranslated() + " from " + lt->second.toString()
					+ " to " + t.tile.toString() + ": " + why);
			}
		}

		// The job priced its own fight at the destination. Any other guard a
		// step would fight has to pass the same loss budget, or the walk stops
		// short of it.
		const bool pricedEnd = t.kind == Target::Kind::Visit || t.kind == Target::Kind::Monster
			|| t.kind == Target::Kind::EnemyHero || t.kind == Target::Kind::EnemyTown
			|| (t.kind == Target::Kind::Explore && t.id.hasValue());
		for(size_t i = 0; i < steps.size(); ++i)
		{
			const int3 tile = hero->convertToVisitablePos(steps[i]);
			// A step beside unseen ground can walk into a guard nobody could
			// see when the route was planned (the server fights it mid-walk).
			// Stop short: the walk so far shows that ground, and the next pass
			// plans the rest with it in sight.
			if(!surroundingsSeen(tile))
			{
				steps.resize(i);
				break;
			}
			if(pricedEnd && tile == t.tile)
				break;
			const CArmedInstance * foe = host_.guardOnStep(tile, hero);
			if(!foe)
				continue;
			const FightEstimate e = host_.estimate(hero, foe);
			if(e.win && e.kept >= 1.0 - s_.lossBudget)
				continue;
			host_.logLine("   V2 STOP " + hero->getNameTranslated() + " short of " + tile.toString()
				+ ": guard " + foe->getObjectName() + " (" + fmt0(double(foe->getArmyStrength()))
				+ ") at step " + std::to_string(i + 1) + " of " + std::to_string(steps.size()));
			steps.resize(i);
			break;
		}
		if(steps.empty())
		{
			blocked_.insert({o.hero, t.tile});
			continue;
		}

		const int3 before = hero->visitablePos();
		const int mpBefore = hero->movementPointsRemaining();
		host_.logLine("   V2 p" + std::to_string(pass) + " " + hero->getNameTranslated()
			+ (o.hero == mainHero_ ? "*" : "") + " " + kindName(t.kind) + " " + t.name
			+ " at " + t.tile.toString() + " r=" + fmt0(o.reward) + " loss=" + fmt0(o.loss)
			+ " d=" + fmt2(o.days) + " s=" + fmt0(o.score) + " steps=" + std::to_string(steps.size()));
		host_.move(hero, steps);

		hero = cb.getHero(ObjectInstanceID(o.hero));   // a lost fight removes it
		if(!hero)
		{
			host_.logLine("   V2 LOST hero " + std::to_string(o.hero) + " going to " + t.tile.toString()
				+ " (" + kindName(t.kind) + "), last seen at " + before.toString());
			anyMoved = true;
			++movesToday_;
			continue;
		}
		const bool moved = hero->visitablePos() != before || hero->movementPointsRemaining() < mpBefore;
		if(!moved)
		{
			// A walk the server did not carry out. Logged with what may explain
			// it; after three in a day the hero is left alone until tomorrow
			// (V2M1L map 03: one hero sent 40 walks a day for a week, none taken).
			blocked_.insert({o.hero, t.tile});
			const int n = ++noMoves_[o.hero];
			host_.logLine("   V2 NOMOVE " + hero->getNameTranslated() + " at " + before.toString()
				+ " mp " + std::to_string(mpBefore) + " first step " + hero->convertToVisitablePos(steps.front()).toString()
				+ " queries " + std::to_string(host_.openQueryCount()) + " (" + std::to_string(n) + " today)");
			if(n >= 3)
				doneToday_.insert(o.hero);
			continue;
		}
		anyMoved = true;
		++movesToday_;
		lastTile_[o.hero] = t.tile;
		if(t.kind == Target::Kind::OwnTown && o.score >= 1e10)
			recallTown_ = t.id.getNum();   // a threat recall (see scoreOptions)
		if(hero->visitablePos() == t.tile && t.kind == Target::Kind::Visit)
			visited_[{o.hero, t.tile}] = day_;
		if(hero->visitablePos() == t.tile && t.kind == Target::Kind::OwnTown && o.hero != mainHero_ && o.reward > 0.0)
			if(const auto * town = dynamic_cast<const CGTownInstance *>(cb.getObj(t.id, false)))
			{
				host_.logLine("   V2 COURIER " + hero->getNameTranslated() + " takes the garrison of "
					+ town->getNameTranslated() + " (" + fmt0(double(town->getArmyStrength())) + ")");
				host_.takeGarrison(town, hero);
			}

		if(t.kind == Target::Kind::Gather)
		{
			const CGHeroInstance * main = cb.getHero(t.id);
			if(main && main->visitablePos().z == hero->visitablePos().z
				&& chebyshev(main->visitablePos(), hero->visitablePos()) <= 1)
			{
				const double carried = armyOf(hero);
				host_.logLine("   V2 GATHER " + hero->getNameTranslated() + " hands "
					+ fmt0(carried) + " to " + main->getNameTranslated());
				host_.giveArmy(hero, main);
				const CGHeroInstance * after = cb.getHero(ObjectInstanceID(o.hero));
				if(after && armyOf(after) > 0.9 * carried)
				{
					host_.logLine("   V2 GATHER refused: " + after->getNameTranslated() + " still carries "
						+ fmt0(armyOf(after)) + ", no gather for three days");
					gatherFailed_[o.hero] = day_;
				}
			}
		}
	}
	return anyMoved;
}

}
