/*
 * engine/Engine.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * The v2 execution engine (Megaplan-OmniAI-2026-10-04.md, section 4.8). One
 * game day is a loop of passes. Each pass looks at the whole board again,
 * proposes every job any hero could do, scores every hero-job pair in one
 * unit (army value per day of the hero's time, after the army the job is
 * expected to cost), gives each hero its best job that no better pair has
 * taken, and moves every hero as far as today's movement allows. The day ends
 * when a pass moves nobody.
 *
 * The parts are kept apart on purpose: target proposal, scoring, assignment
 * and execution only talk through the Target and Option records below, so
 * any one of them can be replaced without touching the others.
 */
#pragma once

#include "EngineHost.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace omniai::v2
{

/// Tunable numbers. Defaults are the first estimates; the strategy layer
/// (milestone M4) will set them per game.
struct Settings
{
	/// Highest share of a hero's army a voluntary fight may cost.
	double lossBudget = 0.35;
	/// Score multiplier for a hero's previous target, so near-equal jobs do
	/// not swap back and forth between passes.
	double momentum = 1.4;
	/// Days beyond which a job is not considered.
	double horizonDays = 14.0;
	/// Share of a support hero's army that counts as gained when it reaches
	/// the main hero.
	double gatherWorth = 0.6;
	/// Share of a town's garrison that counts as gained when the main hero
	/// collects it.
	double garrisonWorth = 0.8;
	/// The same for a support hero that fetches a garrison to carry it to the
	/// main hero (it still has to walk it over).
	double courierWorth = 0.5;
	/// Army value of a hero kill on top of the army destroyed.
	double heroKillBonus = 2500.0;
	/// Share of the enemy army destroyed that counts as gained.
	double enemyArmyWorth = 0.5;
	/// Experience and primary skills are worth this many times the legacy
	/// price (5 percent of the army per level) to the main hero, and to the
	/// others.
	double xpScaleMain = 1.0;
	double xpScaleSupport = 0.25;
	/// Caps on how much a short resource multiplies a mine's or pile's worth.
	double needCap = 4.0;
	double goldNeedCap = 3.0;
	/// Scores are net value to this power, per day of the hero's time. Below
	/// 1 it favours many near jobs over one far, valuable one (Nullkiller
	/// takes the nearest job that pays at all); 1 ranks by value per day.
	double valueExponent = 0.6;
	/// Passes per day, a bound.
	int maxPasses = 40;
	/// Heroes kept: the first `baseHeroes`, one more per town above the
	/// first, never more than `maxHeroes`.
	int baseHeroes = 3;
	int maxHeroes = 5;
};

struct Target
{
	enum class Kind { Visit, Monster, OwnTown, Gather, Explore, EnemyHero, EnemyTown };

	Kind kind = Kind::Visit;
	ObjectInstanceID id;   // the object, hero or town; NONE for Explore
	int3 tile;             // where the hero has to arrive
	int32_t onlyHero = -1; // a job only one hero may take (Gather), else -1
	double reward = 0.0;   // hero-independent reward, if any
	std::string name;
};

struct Option
{
	int32_t hero = -1;
	size_t target = 0;
	double reward = 0.0;
	double loss = 0.0;
	double days = 0.0;
	double score = 0.0;
};

class Engine
{
public:
	explicit Engine(IEngineHost & host, Settings settings = Settings());

	/// Plays one day. Called from the shell's turn, after its dawn logging.
	void makeTurn();

private:
	IEngineHost & host_;
	Settings s_;

	int day_ = 0;
	int32_t mainHero_ = -1;
	std::map<int32_t, int3> lastTile_;              // hero -> tile of its last job (momentum)
	std::set<std::pair<int32_t, int3>> blocked_;     // hero/tile jobs that failed today
	std::set<int32_t> doneToday_;                    // heroes that cannot act again today
	std::map<std::pair<int32_t, int3>, int> visited_; // hero/tile visits made, by day
	int movesToday_ = 0;
	std::set<int> monsterLogDay_;

	// Stages of one pass.
	void hireToCap();
	void pickMainHero();
	std::vector<Target> proposeTargets();
	void proposeExplore(std::vector<Target> & out, double knownValue);
	std::vector<Option> scoreOptions(const std::vector<Target> & targets);
	bool scoreOne(const CGHeroInstance * hero, const CPathsInfo * paths, const Target & t,
		Option & out);
	std::vector<Option> assign(std::vector<Option> options, const std::vector<Target> & targets);
	bool execute(const std::vector<Option> & plan, const std::vector<Target> & targets, int pass);

	// Helpers.
	double visitValue(const CGObjectInstance * obj, const CGHeroInstance * hero);
	double levelWorth(const CGHeroInstance * hero) const;
	double xpWorth(const CGHeroInstance * hero, double xp);
	double xpScale(const CGHeroInstance * hero) const;
	bool surroundingsSeen(const int3 & tile);
	bool dangerousFor(const CGHeroInstance * hero, const int3 & tile,
		const std::vector<FoeInfo> & foes) const;
	static double armyOf(const CGHeroInstance * hero);
	static std::string kindName(Target::Kind kind);
};

}
