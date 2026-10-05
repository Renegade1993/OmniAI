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
	movesToday_ = 0;

	host_.townPhase(true);
	hireToCap();

	int passes = 0;
	for(int pass = 1; pass <= s_.maxPasses; ++pass)
	{
		passes = pass;
		if(pass > 1)
			host_.townPhase(false);
		pickMainHero();
		const std::vector<Target> targets = proposeTargets();
		const std::vector<Option> plan = assign(scoreOptions(targets), targets);
		if(plan.empty() || !execute(plan, targets, pass))
			break;
	}

	int heroes = 0, idle = 0;
	for(const CGHeroInstance * h : cb.getHeroesInfo())
	{
		if(!h)
			continue;
		++heroes;
		if(h->movementPointsRemaining() >= h->movementPointsLimit(true))
			++idle;
	}
	host_.logLine("   V2 d" + std::to_string(day_) + " passes=" + std::to_string(passes)
		+ " moves=" + std::to_string(movesToday_) + " heroes=" + std::to_string(heroes)
		+ " unmoved=" + std::to_string(idle) + " main=" + std::to_string(mainHero_));
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
			knownValue += std::max(0.0, host_.worth(obj, main));
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
	std::vector<std::vector<uint8_t>> visLevels(size.z, std::vector<uint8_t>(W * H, 0));
	int visible = 0;
	for(int z = 0; z < size.z; ++z)
		for(int y = 0; y < H; ++y)
			for(int x = 0; x < W; ++x)
				if(cb.isVisible(int3(x, y, z)))
				{
					visLevels[z][y * W + x] = 1;
					++visible;
				}
	const double density = std::clamp(0.5 * knownValue / std::max(1, visible), 5.0, 60.0);
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
			reward = host_.worth(obj, hero);
			if(reward <= 0.0)
				return false;
			if(!priceFight(host_.guardAt(t.tile, hero)))
				return false;
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
			reward = host_.xpValue(hero, xp) + host_.pocketBehind(t.tile, hero);
			if(reward <= 0.0)
				return false;
			if(!priceFight(armed))
				return false;
			break;
		}
		case Target::Kind::OwnTown:
		{
			// Only the main hero collects the garrison, and only a pool worth
			// the walk.
			if(!isMain || hero->visitablePos() == t.tile)
				return false;
			if(t.reward < std::max(800.0, 0.12 * army))
				return false;
			reward = s_.garrisonWorth * t.reward;
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
	double score = net / (days + 0.25);
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

	std::vector<Option> options;
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
			if(!ourTowns.count(t.tile) && t.kind != Target::Kind::EnemyHero
				&& dangerousFor(hero, t.tile, foes))
				continue;
			Option o;
			o.target = i;
			if(scoreOne(hero, paths, t, o))
				options.push_back(o);
		}
	}

	// A town under threat calls the main hero home, whatever else is on offer.
	const CGHeroInstance * main = mainHero_ >= 0 ? cb.getHero(ObjectInstanceID(mainHero_)) : nullptr;
	if(main && main->movementPointsRemaining() > 0)
		for(const TownThreat & threat : host_.threatenedTowns())
		{
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
					o.score = 1e12;   // defending outranks every gain
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

		// The job priced its own fight at the destination. Any other guard a
		// step would fight has to pass the same loss budget, or the walk stops
		// short of it.
		const bool pricedEnd = t.kind == Target::Kind::Visit || t.kind == Target::Kind::Monster
			|| t.kind == Target::Kind::EnemyHero || t.kind == Target::Kind::EnemyTown;
		for(size_t i = 0; i < steps.size(); ++i)
		{
			const int3 tile = hero->convertToVisitablePos(steps[i]);
			if(pricedEnd && tile == t.tile)
				break;
			const CArmedInstance * foe = host_.guardOnStep(tile, hero);
			if(!foe)
				continue;
			const FightEstimate e = host_.estimate(hero, foe);
			if(e.win && e.kept >= 1.0 - s_.lossBudget)
				continue;
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
			anyMoved = true;
			++movesToday_;
			continue;
		}
		const bool moved = hero->visitablePos() != before || hero->movementPointsRemaining() < mpBefore;
		if(!moved)
		{
			blocked_.insert({o.hero, t.tile});
			continue;
		}
		anyMoved = true;
		++movesToday_;
		lastTile_[o.hero] = t.tile;

		if(t.kind == Target::Kind::Gather)
		{
			const CGHeroInstance * main = cb.getHero(t.id);
			if(main && main->visitablePos().z == hero->visitablePos().z
				&& chebyshev(main->visitablePos(), hero->visitablePos()) <= 1)
			{
				host_.logLine("   V2 GATHER " + hero->getNameTranslated() + " hands "
					+ fmt0(armyOf(hero)) + " to " + main->getNameTranslated());
				host_.giveArmy(hero, main);
			}
		}
	}
	return anyMoved;
}

}
