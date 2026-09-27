/*
 * OmniAI.h, part of the OmniAI VCMI plugin
 *
 * License: GNU General Public License v2.0 or later
 *
 * Adventure-map AI. Inherits CAdventureAI (the GetNewAI factory target is
 * still CGlobalAI) so battles delegate to the engine BattleAI dll, and holds
 * the subsystem owners: the generational-handle registry, the QSBR
 * reclaimer hookup, and the action dispatcher. Turn execution snapshots the
 * visible object set, scores candidates in a TBB arena, then dispatches the
 * winning actions through CCallback.
 */
#pragma once

#include "StdInc.h"
#include "callback/CAdventureAI.h"
#include "eval/CombatEvaluator.h"

struct HeroMoveDetails;

namespace omniai
{
class ActionDispatcher;
class PendingActionQueue;
class HandleRegistry;
class UtilityEvaluator;
class DecisionLog;
struct DifficultyProfile;
}

VCMI_LIB_NAMESPACE_BEGIN
class PathfinderCache;
struct CPathsInfo;
VCMI_LIB_NAMESPACE_END

#include <atomic>
#include <set>
#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include <mutex>
#include <functional>
#include <deque>
#include <condition_variable>
#include <thread>

class OmniAI : public CAdventureAI
{
public:
	OmniAI();
	~OmniAI() override;

	// CAdventureAI
	std::string getBattleAIName() const override;

	// CGameInterface
	void initGameInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CCallback> CB) override;
	void yourTurn(QueryID queryID) override;
	void heroGotLevel(const CGHeroInstance *hero, PrimarySkill pskill,
		std::vector<SecondarySkill> &skills, QueryID queryID) override;
	void commanderGotLevel(const CCommanderInstance * commander,
		std::vector<ui32> skills, QueryID queryID) override;
	void showBlockingDialog(const std::string &text, const std::vector<Component> &components,
		QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept) override;
	void showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel,
		TTeleportExitsList exits, bool impassable, QueryID askID) override;
	void showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down,
		bool removableUnits, QueryID queryID, const MetaString & customTitle) override;
	void showMapObjectSelectDialog(QueryID askID, const Component & icon,
		const MetaString & title, const MetaString & description,
		const std::vector<ObjectInstanceID> & objects) override;
	std::optional<BattleAction> makeSurrenderRetreatDecision(const BattleID & battleID,
		const BattleStateInfoForRetreat & battleState) override;

	// Open-window / exchange queries. Unanswered these block every pack
	// except QueryReply and hang the game (CGameHandler::isBlockedByQueries).
	void showRecruitmentDialog(const CGDwelling *dwelling, const CArmedInstance *dst,
		int level, QueryID queryID) override;
	void showMarketWindow(const IMarket * market, const CGHeroInstance * visitor,
		QueryID queryID) override;
	void showUniversityWindow(const IMarket *market, const CGHeroInstance *visitor,
		QueryID queryID) override;
	void showTavernWindow(const CGObjectInstance * object, const CGHeroInstance * visitor,
		QueryID queryID) override;
	void heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2,
		QueryID queryID) override;

	// IGameEventsReceiver hooks that feed the safety subsystems
	void tileRevealed(const FowTilesType &pos) override;
	void newObject(const CGObjectInstance * obj) override;
	void heroMoved(const TryMoveHero & details, bool verbose = true) override;
	void heroVisit(const CGHeroInstance * visitor, const CGObjectInstance * visitedObj,
		bool start) override;
	void heroVisitsTown(const CGHeroInstance * hero, const CGTownInstance * town) override;
	void objectRemoved(const CGObjectInstance *obj, const PlayerColor & initiator) override;
	void requestSent(const CPackForServer *pack, int requestID) override;
	void requestRealized(PackageApplied *pa) override;
	void playerStartsTurn(PlayerColor player) override;
	void playerEndsTurn(PlayerColor player) override;

	// Battle wrappers: capture predicted ratio + outcome for the learning
	// store, then delegate to CAdventureAI's BattleAI forwarding.
	void battleStart(const BattleID & battleID, const CCreatureSet *army1,
		const CCreatureSet *army2, int3 tile, const CGHeroInstance *hero1,
		const CGHeroInstance *hero2, BattleSide side, bool replayAllowed) override;
	void battleEnd(const BattleID & battleID, const BattleResult *br,
		QueryID queryID) override;

private:
	std::shared_ptr<CCallback> cb;

	std::unique_ptr<omniai::HandleRegistry> registry_;
	std::unique_ptr<omniai::PendingActionQueue> actionQueue_;
	std::unique_ptr<omniai::ActionDispatcher> dispatcher_;
	std::unique_ptr<omniai::UtilityEvaluator> evaluator_;
	std::unique_ptr<PathfinderCache> pathCache_;
	const omniai::DifficultyProfile * profile_ = nullptr;

	// One per AI instance. An AI-only game loads this plugin once per side,
	// so a shared logger would have the two sides overwriting each other.
	std::unique_ptr<omniai::DecisionLog> decisionLog_;
	/// Spectator narration: decision-log lines worth reading, queued from
	/// any thread and sent to the game chat from the turn loop.
	/// Week-one mine plan (September 25th): our heroes paired with the mines
	/// whose approach is still fog, shortest route through the fog first,
	/// one mine per hero. Rebuilt once per round; probeTowardMine follows it.
	void planMineProbes();
	std::map<int32_t, int32_t> minePlan_;   // hero id -> mine id
	int minePlanDay_ = -1, minePlanRound_ = -1, currentRound_ = 0;
	void queueNarration(const std::string & text);
	void flushNarration();
	/// In a watched game only: hold before the next hero action while the
	/// PAUSE file exists in OmniAI's data folder (ten minutes at most).
	void waitWhilePaused(const std::string & where);
	bool narrate_ = false;
	bool chat_ = false;   // narration into the game chat: a watched game with a window only
	std::mutex narrateMutex_;
	std::deque<std::string> narrateQueue_;
	std::string narrateHero_;

	// Objects this player has seen: grown by tileRevealed/newObject,
	// shrunk by objectRemoved. Keyed by ObjectInstanceID number.
	std::mutex seenMutex_;
	std::unordered_set<int32_t> seenObjs_;
	/// Ids objectRemoved reported since the last posture pass (network
	/// thread writes, turn thread drains; guarded by seenMutex_).
	std::vector<int32_t> removedIds_;

	/// One decision round per turn: snapshot objects once, then every hero
	/// scores and walks its own best target.
	void runTurn();

	/// Measurement only, read by nothing that decides: every player's real
	/// gold, income, towns, heroes, army and hero levels from the full game
	/// state, one TRUTH line per player per day. Fog hides the opponent's
	/// economy from the AI, which is right for play and useless for finding
	/// out why its army outgrows ours.
	void logTruth();
	/// Buildings each town had at the last TRUTH pass, so a day's new ones
	/// can be named: town id -> building ids.
	std::map<int32_t, std::set<int32_t>> truthBuilt_;

	/// The role name a hero holds this round, for log lines.
	std::string roleOf(const CGHeroInstance * hero) const;

	/// Score candidates for one hero in the TBB arena and dispatch a
	/// moveHero toward the winner. No-ops when the hero has no MP left.
	void moveBestHero(const CGHeroInstance * hero);

	/// Register all objects visible to this player and acquire handles.
	void snapshotRegistry();

	/// endTurn sent while a query is open gets dropped by the server
	/// (isBlockedByQueries), which would leave the turn hanging forever.
	/// Set when runTurn asks to end; every query answer and realized
	/// receipt re-issues it until playerEndsTurn clears the flag.
	// Read and written from both the network thread and the turn thread.
	std::atomic<bool> endTurnPending_{false};

	// Battles in flight. A hero that walks into a monster is fighting now,
	// and the engine rebuilds bonus system nodes hard for the whole of it.
	// Read as a hint, never waited on: the handlers that end a battle are
	// queued on the same worker pool that runs the turn, so a worker parked
	// on a battle can starve the answer that would finish it. That was
	// tried and it deadlocked; see the note in moveBestHero.
	std::atomic<int> battlesActive_{0};

	// Queries the server has opened and we have not yet answered. While one
	// is open the engine drops every pack except QueryReply
	// (CGameHandler::isBlockedByQueries), so a move or an endTurn sent in
	// that window is refused or times out. Incremented in each show* /
	// dialog callback, decremented when answerQuery replies.
	std::atomic<int> openQueries_{0};

	// steady_clock nanoseconds at which an EndTurn was first held back for a
	// battle, 0 when nothing is being held. Held as a plain integer because
	// atomic<time_point> buys nothing here.
	std::atomic<int64_t> endTurnHeldSince_{0};

	// Everything that talks to the server runs here, never on the thread that
	// delivered the callback. One worker, so ordering is preserved: a query
	// gets answered before the turn that depends on it starts.
	void executeAsync(const char * what, std::function<void()> fn);
	void asyncWorker();

	// Targets a hero has already been sent toward this turn, so the same one
	// is not picked twice and paced back and forth to.
	std::set<int64_t> targetedThisTurn_;
	/// Which collector took each COLLECT target today, so the same hero can
	/// carry on toward it after a walk cut short.
	std::map<int64_t, int32_t> collectedBy_;

	/// Heroes that spent movement this turn without changing tile. A move
	/// that resolves as a blocking visit rather than a step costs the same
	/// movement and moves nobody, and the round loop reads that spend as
	/// progress, so without this the identical order goes out every round
	/// until the day is gone. Cleared at the turn boundary.
	std::set<int32_t> stalledThisTurn_;
	/// Support heroes whose move walk() cut short of a stronger enemy's
	/// reach this turn. The chain that ordered it would pick the same target
	/// every round; these go home or hold instead. Cleared per turn.
	std::set<int32_t> heldThisTurn_;
	/// Last game-week in which the blocked-building diagnostic was printed,
	/// so a stalled town reports once a week instead of every day.
	int lastStallReportWeek_ = -1;
	/// Week each town last printed its build candidates (BUILDSCAN).
	std::map<int32_t, int> buildScanWeek_;
	/// Last game-week in which the revealed-map fraction was reported.
	int lastSeenReportWeek_ = -1;

	// Fog-edge tiles already walked toward this turn. Same oscillation
	// guard as targetedThisTurn_, keyed by tile instead of object id.
	std::set<int3> frontierTriedThisTurn_;

	// Every object one of our heroes has actually stood on this game.
	// The learning store records the same thing but is opt-in and off by
	// default, and the pacing rule has to work without it. Written from
	// the network thread (heroVisit), read from the turn worker.
	std::mutex visitedMutex_;
	std::set<int64_t> visitedObjs_;

	// Object id -> the game-week one of our heroes last took it, for the
	// pickups that re-give every week. Same threading as visitedObjs_.
	std::map<int64_t, int> objectVisitWeek_;

	std::vector<std::thread> workers_;
	std::mutex taskMutex_;
	std::condition_variable taskCv_;
	std::deque<std::pair<const char *, std::function<void()>>> tasks_;
	bool stopping_ = false;
	void maybeEndTurn();
	void answerQuery(QueryID queryID, int selection);

	// Predicted our/enemy army-strength ratio + our side, captured at
	// battleStart and consumed at battleEnd for the learning feedback.
	struct PendingBattle
	{
		double ratio; BattleSide side; bool attacker; double margin;
		std::string who;        // our hero's name and role, "-" without one
		double ours = 0;        // AI value we brought
		double theirs = 0;      // AI value they brought
	};
	// margin is the evaluator's predicted survivor share, signed: positive
	// when it expected us to win, negative when it expected the enemy.
	// -2.0 means no verdict was computable (no hero on our side).
	std::map<BattleID, PendingBattle> pendingBattleRatio_;

	/// Recruit every affordable creature level from a visited dwelling
	/// into dst (Nullkiller's recruitCreatures port), before the dialog
	/// is closed with answer 0.
	/// What NOT having this building costs us, in army value, computed from
	/// the town's own data rather than from a priority order. Income from
	/// CBuilding::produce, dwellings from the faction's creature and the
	/// town's growth, markets from what they unblock, and the tavern from
	/// the hero it can replace.
	double buildingValue(const CGTownInstance * town, const BuildingID & id) const;

	void buildInTown(const CGTownInstance * town);
	void buildInAllTowns();

	/// What a fortification step adds that buildingValue cannot see: the
	/// Citadel and Castle each raise every built dwelling's base growth by
	/// half, valued over the same horizon as income.
	double fortGrowthValue(const CGTownInstance * town, const BuildingID & id) const;

	/// The build planner's saving target: gold held back from recruitment
	/// for a town's next planned step, town id -> gold. Recomputed at every
	/// dawn build pass, dropped the moment the town is threatened.
	std::map<int32_t, int> buildReserve_;
	/// Sum of the reserves above; what recruitment must leave in the purse.
	int buildReserveTotal() const;
	/// Gold beyond the planner's target, all creatures on sale and one hire.
	int surplusGold() const;
	/// Upgrade this army's stacks in place, best upgrade first, paid from
	/// surplus gold only. The army must be in a town that offers them.
	void upgradeArmy(const CArmedInstance * army);
	/// buildReserveTotal() as of the last dawn build pass, for callbacks on
	/// the network thread that must not read the map above.
	std::atomic<int> buildReserveCached_{0};
	/// The hero whose visit event arrived last. The engine sends the visit
	/// before the object's reward dialog, and the dialog does not name who
	/// is standing there.
	std::atomic<int32_t> lastVisitor_{-1};

	/// Buy what every town offers to whichever of our heroes is standing in
	/// it. Runs on the turn thread, before anyone moves, because the server
	/// refuses a recruit aimed at a hero that has already walked out.
	void recruitInAllTowns();

	/// Move a town's garrison into the army of the hero standing in it.
	void collectGarrison(const CGTownInstance * town, const CGHeroInstance * hero);
	/// goldReserve is gold this pass must not touch. Used by the garrison
	/// path to keep the price of a replacement hero intact, because troops
	/// that cannot move are worth less than the hero that moves them.
	void recruitFromDwelling(const CGDwelling * dwelling, const CArmedInstance * dst,
		int goldReserve = 0);

	/// Paint the engine's own view around the hero: fog, walls and how far
	/// the path search actually reached. "Fogged", "walled" and "search
	/// never got there" read identically in the skip list; this splits them.
	void dumpPathGrid(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Grow the world instead of walking another lap of it: head for the
	/// reachable tile that reveals the most unseen ground, or, when the
	/// ground is already known and guards are what seal the pocket, for the
	/// weakest hostile army the pathfinder can reach. Fogged tiles read
	/// BLOCKED to the pathfinder, so targets past the fog edge are
	/// unroutable until a hero physically stands beside them.
	/// Returns true when it dispatched a move.
	bool exploreFrontier(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Would stepping onto this guarded tile start a fight we would win?
	/// Exploration used to step onto them blind, which killed a hero on
	/// day 10 of a match for the sake of twenty tiles of fog.
	bool worthTheGuard(const int3 & tile, const CGHeroInstance * hero) const;

	/// Strongest hostile army on this tile or beside it. A guard owns the
	/// tiles around itself, so what defends an object is rarely on it.
	uint64_t guardStrengthAt(const int3 & tile, const CGHeroInstance * hero) const;

	/// The hostile armed instance that defends this tile, so the evaluator
	/// can estimate a real fight rather than compare two strength numbers.
	const CArmedInstance * strongestHostileAt(const int3 & tile,
		const CGHeroInstance * hero) const;

	/// What a hero fights by stepping onto this tile: a monster guarding it
	/// (its own tile or any neighbour) or an enemy hero or town standing ON
	/// it. Walking beside an enemy hero is not a battle; ending there is a
	/// separate question the reach checks answer.
	const CArmedInstance * strongestHostileOnStep(const int3 & tile,
		const CGHeroInstance * hero) const;

	/// A neutral object whose own army fights the visitor (creature bank,
	/// guarded dwelling, neutral town or garrison, guarded pickup). Not a
	/// monster and not player-owned, so the other checks never saw it.
	bool guardsItself(const CGObjectInstance * obj, const CGHeroInstance * hero) const;

	/// Any of the tile's eight neighbours (or itself) still in fog: a guard
	/// could stand there unseen. walk() stops short of such a step.
	bool bordersFog(const int3 & tile) const;

	/// True when the small-map leash applies to this hero at all (field
	/// hero, map of 48 tiles or less, one town, not attacking).
	bool leashBinds(const CGHeroInstance * hero) const;

	/// The evaluator's verdict on `hero` attacking `target`, dispatching to
	/// the siege path when the target is a town.
	omniai::CombatVerdict combatVerdict(const CGHeroInstance * hero,
		const CArmedInstance * target) const;

	/// The surviving-margin a fight must clear before `hero` commits. Rises
	/// with the army being risked - a model read that is mostly right at 20k
	/// is a coin flip it cannot afford at 200k, so a bigger field force needs
	/// a bigger cushion. Caps at 0.85.
	/// base is the floor before the army-size, fleet-in-being, weekend and
	/// distance terms: 0.60 for a voluntary fight, 0.25 (SIEGE_BASE) for a
	/// siege, which is decisive and priced with walls and towers already.
	double commitMargin(const CGHeroInstance * hero, double base = 0.60) const;

	/// Whether `hero` is expected to beat `target` keeping at least
	/// minSurvivor of its army. The evaluator's win/lose plus a floor.
	bool expectedToWin(const CGHeroInstance * hero,
		const CArmedInstance * target, double minSurvivor) const;

	/// Sum of creature FightValue across the armed instance's slots.
	/// getArmyStrength is AI value, which over-credits ranged/utility
	/// premiums; fight value is the honest unit for "who wins a brawl".
	uint64_t armyFightValue(const CArmedInstance * armed) const;

	/// The army a dwelling would hand over if taken.
	uint64_t recruitsOnOffer(const CGObjectInstance * obj) const;

	/// What the fight for this object buys for the coming weeks, net of what
	/// winning it costs in army. Scaled into the same range as an intrinsic
	/// worth and bounded at both ends.
	double fightValue(const CGObjectInstance * obj, const CGHeroInstance * hero) const;

	/// Bounded proxy for what a guard is sealing off, not just what it
	/// stands on: known objects near the guard that the pathfinder cannot
	/// reach this turn. A guard is frequently the one thing between the
	/// hero and a whole pocket, and beating the guard's own tile used to
	/// score exactly like beating an equally strong guard over an empty
	/// tile. Folded into fightValue.
	double pocketValueBehind(const int3 & guardPos, const CGHeroInstance * hero) const;

	// Our towns an enemy hero is close enough to take, recomputed every
	// round because both sides are moving.
	// town id -> strength of the strongest enemy hero near it
	std::map<int32_t, double> threatenedTowns_;
	/// The threatened towns that would actually fall: the siege model says
	/// the strongest hero near them wins against what stands in them now.
	/// Rebuilt with threatenedTowns_ every round.
	std::set<int32_t> dangerTowns_;

	// Where we last saw an enemy hero - a waypoint toward the pocket they
	// came from. The hunt presses this way instead of the whole-map unseen
	// centroid, because the enemy's side is where the enemy walks from.
	int3 enemyBeacon_;
	bool enemyBeaconValid_ = false;
	/// The first enemy's declared capital (map header posOfMainTown, the
	/// town's anchor). Unlike the beacon, sightings never overwrite it.
	int3 enemyStart_;
	bool enemyStartValid_ = false;

	// A deferred objective: something we cannot do yet but will re-enter the
	// moment its precondition lands. The seed of the registry design - "wait
	// until X" lives on the task, not in a re-discovery pass. The first kind
	// is a route guard too strong to beat today: the field hero goes back to
	// compounding, and the task re-enters when the evaluator says we win it,
	// which makes growth parity work toward a purpose (opening the route to
	// the enemy) instead of an abstract rate.
	struct DeferredTask
	{
		enum class Kind { Guard } kind = Kind::Guard;
		int32_t objId = -1;      // the object to clear, revalidated each round
		int3 pos;                // where it stands
		uint64_t str = 0;        // its strength when deferred, for the log
		int day = -1;            // day it was deferred
	};
	std::vector<DeferredTask> deferredTasks_;

	/// The registry task list: one entry per actionable thing the map has
	/// shown us. Each carries its own precondition - a deferred kind re-enters
	/// the moment its bar clears (a guard the field army now beats, a town
	/// whose garrison we can take), a recurring kind re-arms on its cadence
	/// (the weekly garrison delivery). Assignment hands each ready task to
	/// the hero that reaches it cheapest and is fit for it: the field hero
	/// takes fights, support heroes take deliveries and pickups.
	struct RegTask
	{
		enum class Kind { Guard, Deliver, Capture, Collect, Hold } kind = Kind::Guard;
		int32_t objId = -1;      // guard object id, or the town id
		int3 pos;                // where the target stands
		uint64_t str = 0;        // guard/garrison strength to beat, or pool value
		bool ready = false;      // precondition currently met this round
		int32_t heroId = -1;     // the hero assigned to it, -1 unassigned
	};
	std::vector<RegTask> regTasks_;
	int regenDay_ = -1;          // the day the registry was last rebuilt

	/// A world object we have seen, kept across turns. This is the registry's
	/// raw memory: every mapped object (pile, mine, dwelling, guard, town)
	/// with its position, the strength of whatever guards it, and an estimate
	/// of what it is worth. Refreshed as fog lifts; a record survives the turn
	/// the object drops out of sight, so a guarded prize stays a deferred
	/// task instead of being forgotten.
	struct WorldObj
	{
		enum class Kind { Pile, Mine, Dwelling, Bank, Town, Guard, Portal,
			Pickup, Visit, Other };
		int32_t objId = -1;
		int3 pos;
		Kind kind = Kind::Other;
		uint64_t guardStr = 0;   // strength of the stack guarding it, 0 = free
		double reward = 0;       // what taking it is worth
		int32_t owner = -1;      // player color number, -1 = neutral
		int lastSeen = -1;       // day it was last visible
	};
	std::unordered_map<int32_t, WorldObj> worldObjs_;

	/// The flat per-kind worth table every WorldObj reward is drawn from -
	/// one table, so the route-collection pass (which reads it off the
	/// registry) and refreshWorldModel (which fills the registry) cannot
	/// drift into two different ideas of what a pile is worth.
	double nominalWorth(WorldObj::Kind kind) const;
	/// What a guarded mine or dwelling is worth taking, in army value, on the
	/// horizons buildingValue uses: four weeks of recruits, 28 days of output.
	double guardedPrizeWorth(const CGObjectInstance * obj) const;
	/// How many days an investment is counted to pay: 28 on a 36x36 map,
	/// scaling with the map's linear size (K's map-size note).
	double valueHorizonDays() const;
	/// Share of the army a grind fight must keep: 0.94 on duel-v2's 36x36,
	/// lower on bigger maps, where contact is further off.
	double grindSurvivor() const;
	/// Contact is close: a map of duel-v2's size, or an enemy hero seen in
	/// the last two days that could reach one of our towns within two days.
	/// Recruitment then spends the gold the build planner is saving.
	bool contactSoon() const;

	/// Scan the visible map into worldObjs_ - the world-object records every
	/// task reads from. Idempotent per day.
	void refreshWorldModel();

	/// Rebuild the task list from the world and hand ready tasks to heroes.
	/// Runs once per day so the whole round shares one assignment.
	void refreshTasks();

	/// The task assigned to this hero this round, or nullptr.
	const RegTask * assignedTask(const CGHeroInstance * hero) const;

	/// Execute this hero's assigned registry task. True when it moved.
	bool runRegistryTask(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Run this hero's assigned Collect task, if any. Called late in the
	/// chain so a pickup fills an idle turn instead of preempting a delivery.
	bool runCollectTask(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Register a route-blocking guard we cannot beat yet. Idempotent on the
	/// object id, so a stalled press does not add the same wall twice.
	void deferGuard(const CArmedInstance * guard);

	/// True when pos is a deferred guard's tile or inside its zone of
	/// control. A deferred fight is off-limits for the whole day, to every
	/// move, whichever caller priced what: s17-72x72 deferred a 31885 guard
	/// and lost an 11.7k stack to it later the same turn.
	bool isDeferredTile(const int3 & pos) const;

	/// A deferred route guard is now beatable: the field hero walks onto it
	/// and the fight opens the route it was holding. True when it committed.
	bool runDeferredGuards(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Work out which of our towns an enemy hero is bearing down on.
	void assessThreats();

	/// Balance of power, reported rather than acted on. Steering by it made
	/// the hero march and turn back as the enemy hero crossed the fog line.
	void assessPosture();

	/// Extra worth of an enemy object, because a town is the only thing on
	/// the map that ends the game. Whether it can actually be taken is
	/// decided per object by the danger, guard and reach checks.
	double strategicBonus(const CGObjectInstance * obj) const;

	/// True while our strongest hero outweighs the strongest enemy force we
	/// can see or remember (projected forward). Releases the small-map leash
	/// and gates grinding and hero hunts.
	bool attacking_ = false;
	/// Enemy hero id -> what we last saw of it. Dropped when the hero is
	/// removed from the map.
	/// moved: seen at two different tiles at least once. The idle players
	/// of a six-slot map never move and never threaten anything.
	struct FoeSighting { double str = 0; int day = -1; int3 pos; int reach = 0; bool moved = false; };
	std::map<int32_t, FoeSighting> foeSeen_;

	/// Send this hero home if one of our towns is under threat. Returns true
	/// when it has handled the hero, including the case where the hero is
	/// already standing in the threatened town and should stay there.
	bool defendThreatenedTown(const CGHeroInstance * hero, CPathsInfo * paths);

	/// The strongest enemy hero within threat range of this town - the one
	/// that would actually besiege it.
	const CGHeroInstance * strongestThreatNear(const CGTownInstance * town) const;

	/// Does our town (garrison + our hero + walls) repel this attacker? The
	/// evaluator's verdict on the enemy sieging us.
	/// excluded: a hero of ours to leave out of the defence wherever it
	/// stands (garrison slot, door, inbound), for "does it hold without X".
	bool defenseHolds(const CGHeroInstance * ourHero,
		const CGTownInstance * town, const CGHeroInstance * enemyHero,
		const CGHeroInstance * excluded = nullptr) const;

	/// The evaluator's verdict on enemyHero sieging our town: win means the
	/// attacker takes it, margin is the winner's survivor share.
	omniai::CombatVerdict siegeOnUs(const CGHeroInstance * ourHero,
		const CGTownInstance * town, const CGHeroInstance * enemyHero,
		const CGHeroInstance * excluded = nullptr) const;

	/// Teleport a hero onto a town if it knows Town Portal and has the mana.
	/// The one spell that makes a march instant: a defender reaches a
	/// threatened town this turn, a courier's delivery leg stops costing a
	/// week. Returns true when the cast was sent.
	bool castTownPortalTo(const CGHeroInstance * hero, const CGTownInstance * town);

	/// Leave a town we cannot hold, taking the army with it - a hero that
	/// walks out of a doomed siege keeps compounding; one that stands in it
	/// loses the town and the army together. Retire to another town we own
	/// when one is reachable, else put ground between us and the attacker.
	bool evacuateTown(const CGHeroInstance * hero, const int3 & threatPos,
		CPathsInfo * paths, bool takeGarrison = true);

	/// Compact creature-level histogram of an army, "t1/t2/.../t7" counts.
	/// The tech signal in the growth report: low-tier mass vs high-tier stack.
	std::string armyTiers(const CCreatureSet * army) const;

	/// Is there a stack on offer here we can pay for AND find room for?
	/// The one question that separates a town worth walking to from a shop
	/// the hero emptied yesterday. Both halves matter: a hero carrying
	/// seven different creatures has no slot for an eighth, so the stock
	/// sits there unbought and the shop looks open forever.
	bool dwellingOffersRecruits(const CGDwelling * dwelling, const CArmedInstance * dst);

	/// Is this town holding garrison troops this hero has room to take? A
	/// reason to go home, and the other half of buying into the garrison
	/// while the hero is away.
	bool garrisonWaitingFor(const CGTownInstance * town,
		const CGHeroInstance * hero) const;

	/// Could an enemy hero that would beat us be standing on this tile
	/// tomorrow? Asked about where a move ENDS, because that is what hands
	/// the opponent the first blow.
	bool withinEnemyReach(const int3 & tile, const CGHeroInstance * hero) const;

	/// Turn a path into the steps this turn's movement can actually take.
	/// One copy of the destination-first walk, which had grown three.
	std::vector<int3> stepsToward(CPathsInfo * paths, const CGHeroInstance * hero,
		const int3 & target) const;

	/// Move orders dispatched this game, used by the turn loop to detect a
	/// hero whose move resolved as a blocking visit (no tile change, no
	/// movement spent) that the movement-spent stall check cannot see.
	int movesDispatched_ = 0;

	/// What a trip to this town would add to this hero's army, counting both
	/// stock it can pay for and garrison troops it can carry.
	uint64_t townBusinessValue(const CGTownInstance * town,
		const CGHeroInstance * hero) const;

	/// Send the hero home when a town holds enough to matter. Recruitment as
	/// an appointment rather than something that happens while passing.
	bool runHomeErrand(const CGHeroInstance * hero, CPathsInfo * paths);

	/// A side with no hero left cannot do anything at all, and the only
	/// hire path we had needed a hero to walk into a tavern first. Hire
	/// out of the town itself when the map has taken the last one.
	bool hireHeroIfNone();

	/// Which hero goes out and which stays home, by army size, recomputed
	/// every round because the answer moves.
	void assignHeroRoles();

	/// The home hero's whole job: stand in one of our towns, or walk to the
	/// nearest and stand in that. Returns true when it has handled the hero.
	bool holdTheFort(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Walk a worthwhile load out to the field hero. Refuses while any town
	/// is threatened: the garrison job outranks the courier job.
	bool supplyRun(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Move one hero's army into another's, keeping the weakest stack behind
	/// so the carrier is never left empty on open ground.
	void handOver(const CGHeroInstance * from, const CGHeroInstance * to);
	/// One stack from one army to another: merged into a stack of the same
	/// creature, else into the first free slot. keepOne leaves a single
	/// creature behind (a hero cannot walk with an empty army). False when
	/// the stack has nowhere to go.
	bool moveOneStack(const CArmedInstance * src, const CArmedInstance * dst,
		SlotID slot, bool keepOne = false);
	/// Would handOver(from, to) move any creature? The same rules: a stack
	/// that fits goes over unless it is the carrier's kept fastest, and a
	/// stack that does not fit swaps in when it is worth more than the
	/// receiver's weakest of another type.
	bool wouldHandOver(const CGHeroInstance * from, const CGHeroInstance * to) const;

	/// True when this hero carries a movable artifact (worn or in the pack)
	/// other than its spellbook and war machines - cargo for the field hero.
	bool carriesArtifacts(const CGHeroInstance * hero) const;

	/// Instance id of the hero that roams and fights, -1 when we have none.
	int32_t fieldHeroId_ = -1;

	/// A second field-capable hero for open maps: one hero cannot hold a
	/// taken town and keep pressing at once. The second-strongest fighter
	/// takes assigned captures and holds; -1 while we run thin on heroes.
	int32_t secondFieldId_ = -1;

	/// Towns whose owner just became ours, mapped to the day we took them.
	/// A fresh capture keeps its own recruits as a standing garrison for a
	/// few days instead of feeding a courier, so it builds a defense rather
	/// than being drained back to empty the week the enemy comes back.
	std::map<int32_t, int> freshlyCaptured_;

	/// Leave a holding force on a just-captured town: the weakest stacks go
	/// into the garrison so a lone scout cannot walk the empty town back,
	/// while the hero keeps its fighting strength.
	void leaveHoldingGarrison(const CGHeroInstance * hero, const CGTownInstance * town);

	/// Can this support hero be sent to hold a new post - it holds nothing,
	/// or its town repels the threat on it with the garrison alone.
	bool defenderCanBeSpared(const CGHeroInstance * hero) const;

	/// Is a defender seatable on a taken town - a spareable support hero, or
	/// the gold and a town to hire one. Gates Capture so we do not take a
	/// town we cannot hold the day after.
	bool defenderAvailable() const;

	/// While we are ahead, walk to the nearest visible enemy hero we can
	/// kill near-for-free. Denies the enemy's future growth outright, not
	/// just XP - see the day-8 trace in DEVELOPMENT_LOG.md for the loss
	/// this targets: a weak enemy hero left alone grew 30x in two days.
	bool huntWeakEnemyHero(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Hero development: walk the field hero to the nearest neutral stack it
	/// beats cheaply, for the XP and level that multiply its army.
	bool grindNearestFight(const CGHeroInstance * hero, CPathsInfo * paths);
	/// GRINDSCAN, once a day: every neutral stack the field hero knows, sorted
	/// into what grindNearestFight would take today and what stops the rest.
	/// Logging only.
	void logGrindScan(const CGHeroInstance * hero);
	/// Where to walk when a visible target has no route because the way to
	/// it is still fog: the shortest route that counts unexplored ground as
	/// open, and the last tile on it before the fog that this hero reaches.
	bool fogFrontierToward(const CGHeroInstance * hero, CPathsInfo * paths,
		const int3 & target, int3 & frontier) const;
	/// Island starts: when the enemy can only be reached across water, walk
	/// or sail toward it through the fog, and get a boat when the next step
	/// needs one (Summon Boat, a Shipyard we may use, or build the town's).
	bool seekCrossing(const CGHeroInstance * hero, CPathsInfo * paths);
	/// Optimistic step counts from `target` to every tile of its level:
	/// unexplored ground counts as open, explored ground as the tile says,
	/// water only when `water`. -1 where nothing reaches.
	std::vector<int> crossingField(const int3 & target, bool water) const;
	int boatAskedDay_ = -1;   // the day a boat was last summoned or bought
	int crossingNoteDay_ = -1;   // the day seekCrossing last said why it waits
	/// Where the land route seekCrossing still hopes for first leaves what we
	/// have seen, and the day that was worked out: the tile whose fog decides
	/// whether the crossing is due. deepScout looks there on watery maps.
	int3 crossingLookAt_ = int3(-1, -1, -1);
	int crossingLookDay_ = -1;

	/// Enemy town the field hero has committed to taking, -1 while none is
	/// a reachable win. Kept across turns so the hero marches to it instead
	/// of oscillating between two targets it never reaches.
	int32_t conquestTargetId_ = -1;

	/// Frontier tile the field hero has committed to walking to, and the
	/// move decisions left for it (not game days: see SCOUT_BUDGET). Held
	/// across turns for the same reason conquestTargetId_ is: without it the
	/// home errand gets first refusal every morning and the hero steps
	/// toward the fog and back again.
	int3 scoutTarget_;
	bool scoutTargetSet_ = false;
	int scoutTurnsLeft_ = 0;

	/// Town the field hero is committed to reaching even though the
	/// pathfinder says it cannot: pocketed maps seal towns behind guards,
	/// and the only way to spend the garrison pool is to open the seal.
	/// Set by runHomeErrand when the pool is worth the trip but no path
	/// reaches it; cleared on arrival, capture, or budget.
	int32_t resupplyTownId_ = -1;
	int resupplyTurnsLeft_ = 0;
	/// Town id -> the day a dropped resupply march may be tried again.
	/// Without this runHomeErrand re-arms the same town every round it
	/// declines, so RESUPPLY_BUDGET never expires and the march owns the
	/// field hero for the rest of the game.
	std::map<int32_t, int> resupplyCooldown_;
	/// The same per-resource multipliers handed to the evaluator, kept
	/// here so the turn logic can ask what we are short of without
	/// recomputing the thresholds in two places.
	std::array<double, 7> scarcity_ = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

	/// Which of the 7 resources at least one BUILD_NORMAL building in a
	/// town we own is waiting on right now (canBuildStructure ==
	/// NO_RESOURCES). Identity, not just quantity: mercury reading scarce
	/// while nothing in the build list is made of mercury is not a reason
	/// to walk there. Shared by updateResourceScarcity (spikes the general
	/// scoring pass so a resource PILE prices the same way a mine already
	/// does) and grabScarceMine (the mine-run fallback), so the two do not
	/// carry drifting copies of the same scan.
	std::array<bool, 7> blockedBuildingResources() const;

	/// Walk to a mine producing something we have run out of, ahead of the
	/// scouting mandate. A mine is income rather than a pickup, and the
	/// build queue stopping for want of wood is what loses these games.
	bool grabScarceMine(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Continue a committed resupply march: stand on the reachable tile
	/// nearest the sealed town, stepping into a guarded tile's zone of
	/// control when the guard is one we can beat - the fight that opens
	/// the pocket. Drops when a path opens (the plain errand takes it
	/// then), when the town falls, on budget, or when nothing reachable
	/// is any closer than the hero already is.
	bool resupplyMarch(const CGHeroInstance * hero, CPathsInfo * paths);

	/// The reachable tile that best approaches a destination the
	/// pathfinder cannot reach: closest by map distance, lateral steps
	/// only where they uncover new ground, beatable guarded tiles legal.
	/// False when nothing reachable helps.
	bool scanSeal(const CGHeroInstance * hero, CPathsInfo * paths,
		const int3 & dest, int3 & bestTile,
		const CArmedInstance ** blocker = nullptr);

	/// Press toward a destination the pathfinder cannot reach: closest
	/// reachable tile by map distance, lateral steps only where they
	/// uncover new ground, beatable guarded tiles legal targets. Shared
	/// by resupplyMarch (field hero toward a sealed home town) and
	/// sallyOut (a pocketed hero toward the sealed-out field hero). When
	/// blocker is given it receives the strongest unbeatable guard on the
	/// route, so the caller can defer it until the army clears it.
	bool pressToward(const CGHeroInstance * hero, CPathsInfo * paths,
		const int3 & dest, const std::string & label,
		const CArmedInstance ** blocker = nullptr,
		int3 * pressTile = nullptr, bool ignoreLeash = false);

	/// Walk at the enemy's declared capital while their main, seen today or
	/// yesterday, is farther from it (in days) than we are. Their capital sits
	/// empty most days; conquestMarch takes over once it is in sight.
	bool raidEnemyCapital(const CGHeroInstance * hero, CPathsInfo * paths);
	/// SPEC press when ahead: with the field army clearly above every enemy
	/// hero we know of, walk at the capital the map declares for the enemy
	/// until conquestMarch can see it.
	bool pressCapital(const CGHeroInstance * hero, CPathsInfo * paths);
	/// The only town of its owner that we know of: taking it starts the
	/// owner's seven days without a town, so it is priced as the decisive
	/// prize rather than as one more producer.
	bool lastKnownTownOf(const CGTownInstance * town) const;
	/// Any corrected win, strictly above estimateSiege's floor, is enough
	/// for the decisive town.
	static constexpr double DECISIVE_MARGIN = 0.10;
	bool foeEverSeen_ = false;   // any enemy hero seen this game

	/// Walk toward the nearest unguarded mine near home that is visible but
	/// has no path (its approach is fog), to uncover the route.
	bool probeTowardMine(const CGHeroInstance * hero, CPathsInfo * paths);

	/// While the field hero is sealed off from a town's pool, a hero
	/// inside that pocket loads the garrison and presses the seal toward
	/// the field hero - the pool's own strength takes the choke guard
	/// the field hero cannot, and a carrier that gets out both delivers
	/// and opens the pocket behind it.
	bool sallyOut(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Safety move when every scored target ends in a stronger enemy
	/// hero's reach: the reachable tile that puts the most distance
	/// between us and the nearest threat, ties pulling toward our
	/// nearest town. False when standing still is already the safest
	/// option; the caller then keeps its old fallback.
	bool retreatMove(const CGHeroInstance * hero, CPathsInfo * paths);

	/// A visible enemy hero that beats this one can reach it before our
	/// next turn and it stands outside our towns: walk into a town we reach
	/// today, else retreat out of reach, and end the hero's turn there so
	/// no later chain marches it back. False when not in danger or when no
	/// move improves on standing still.
	bool escapeStrongerHero(const CGHeroInstance * hero, CPathsInfo * paths);

	/// A threatened town holding two of our heroes: the stronger takes the
	/// other's army and the garrison slot. A visitor in front of a garrison
	/// hero is fought outside the walls, alone (isBattleOutsideTown).
	void consolidateTownDefence();

	/// Walk on toward the held frontier tile. Drops the target on arrival,
	/// when the path dies, when the budget runs out, or when standing there
	/// would no longer uncover enough to be worth the day.
	bool scoutMarch(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Average position of everything on this level we have never seen.
	/// False when the level is fully revealed. The bulk of the unknown is
	/// where the enemy is, on any map with an enemy on it.
	bool unseenCentroid(int z, int3 & out) const;

	/// Unseen tiles a hero standing on this tile would uncover.
	int revealFrom(const int3 & p) const;

	/// Whether any open tile this hero can reach would still uncover a
	/// day's worth of fog. The frontier-dry tripwire asks this before
	/// parking the field hero, so a hero that is stuck is not mistaken for
	/// one that has run out of map.
	bool frontierLeft(const CGHeroInstance * hero, CPathsInfo * paths) const;

	/// The other half of the offense lever: a field hero strong enough to
	/// matter but with no hostile town in reach presses the seal toward the
	/// bulk of what we have never seen - on a two-player map that mass is
	/// the enemy's side. Beatable choke guards are legal targets, so the
	/// press fights through instead of idling where exploration ran out,
	/// and forces the capital sighting conquestMarch then needs.
	bool huntTheEnemy(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Sealed-pocket defense: park this hero on a town we own so the enemy
	/// that comes through the seal meets the army, not an empty shell.
	/// Garrisons in place, walks home when out in the field, declines when
	/// no owned town is reachable.
	bool pocketHold(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Press-saturation bookkeeping, shared by huntTheEnemy and sallyOut:
	/// notePress records the day's press endpoint (a blocker or a changed
	/// tile resets the counter; only days spent near the endpoint count,
	/// so travel days are not camping), and sealedPressHold takes over
	/// with pocketHold once the same endpoint has been pressed past the
	/// saturation bound.
	void notePress(const CGHeroInstance * hero, const CArmedInstance * blocker,
		bool moved, const int3 & tile);
	bool sealedPressHold(const CGHeroInstance * hero, CPathsInfo * paths);

	/// Any town we have seen that is not ours, hostile or neutral. False
	/// does not mean conquest is hard, it means there is nothing on the
	/// map to march on at all, and finding one outranks any pickup.
	bool knowsHostileTown() const;

	/// The offense lever: when the field column can beat the weakest
	/// reachable hostile town, walk to its gate instead of roaming. The
	/// target is revalidated every round and dropped the moment it is no
	/// longer ours, reachable, or beatable.
	bool conquestMarch(const CGHeroInstance * hero, CPathsInfo * paths);

	/// What the field column can put in front of a town's gate: the field
	/// hero plus the deliverable strength of couriers that can reach it to
	/// hand over before the siege.
	double conquestStrength(const CGHeroInstance * field) const;

	/// Garrison troops plus any heroes standing in or on the town. Our own
	/// visitor would be on the attacking side, so only a hostile one counts.
	double townDefenseStrength(const CGTownInstance * town) const;

	/// Estimated outcome of `hero` attacking `defender` (a map guard, an
	/// enemy hero, or any armed object). defHero is the defender's hero when
	/// the armed object is a town or an army with a commander, else null.
	/// Returns the evaluator's verdict - win plus the surviving margin.
	omniai::CombatVerdict estimateFight(const CGHeroInstance * hero,
		const CArmedInstance * defender, const CGHeroInstance * defHero,
		int fortLevel) const;

	/// The field hero's stacks plus the stacks deliverable couriers would
	/// hand over, all under the field hero's stats - the army that actually
	/// shows up to a fight the field hero starts.
	omniai::CombatEvaluator::Side fieldArmyProfile(const CGHeroInstance * field) const;

	/// Estimated outcome of `hero` besieging `town`: the defender is the
	/// garrison, a garrison hero, and any hostile hero standing in it, all
	/// behind the fort walls.
	/// withCouriers adds every support hero that can walk to the field hero
	/// (planning a multi-day march); a fight about to happen gets the
	/// hero's own army only.
	omniai::CombatVerdict estimateSiege(const CGHeroInstance * hero,
		const CGTownInstance * town, bool withCouriers = true) const;

	/// A town's own arrow towers' guaranteed per-round damage output, given
	/// its current fortification level. Shared between estimateSiege (are
	/// THEIR walls worth attacking) and defenseHolds (do OUR walls hold) -
	/// both feed the same CombatEvaluator::Side::towerDps bucket.
	double townTowerDps(const CGTownInstance * town) const;

	/// Another of our heroes carries a bigger army and can reach this town
	/// too. Ties break on instance id so exactly one hero is ever the
	/// answer and two equals cannot both stand down.
	bool betterDefenderAvailable(const CGHeroInstance * me,
		const CGTownInstance * town) const;

	/// Hero is the only non-field hero standing in one of our towns.
	bool isSoleHolder(const CGHeroInstance * hero) const;

	/// Instance id of the burner: the cheapest spare hero, which runs the
	/// weekly pickup circuit instead of holding a post or chasing the field
	/// hero. -1 while every hero has a job that needs it.
	int32_t collectorHeroId_ = -1;

	/// The burner's whole job: a least-cost loop over the weekly-reset
	/// pickups within reach (windmills, wheels, gardens, dwelling stock, a
	/// trading post that beats our marketplaces), then home to drop the
	/// takings into a garrison. Refuses guarded or enemy-reached stops.
	bool collectorCircuit(const CGHeroInstance * hero, CPathsInfo * paths);

	/// A support hero holding an unthreatened town that keeps a garrison
	/// without it may run the pickup circuit instead of standing guard.
	bool holderMayCollect(const CGHeroInstance * hero) const;

	/// Support-hero scouting: while no hostile town is known, an idle
	/// non-fighter marches toward the unseen bulk. Finding their capital
	/// is what turns conquestMarch on; a hero that only holds and collects
	/// never does it. One hero claims the job per day.
	bool deepScout(const CGHeroInstance * hero, CPathsInfo * paths);
	int deepScoutDay_ = -1;
	/// Today's scouts: hero id -> the sector it holds (-1: not a sector
	/// scout). One a day, two on a big map still mostly unseen.
	std::map<int32_t, int> deepScouts_;
	/// Big-map scouting by sector: the unseen ground round our main town in
	/// eight 45-degree sectors, 6 to 36 tiles out. The sector with the most
	/// unseen tiles per tile of distance that no other scout holds today
	/// (or `forced`, when this hero already holds one); out is the centroid
	/// of that sector's unseen tiles.
	bool scoutSector(int z, const std::set<int> & taken, int forced,
		int3 & out, int & sector) const;
	/// A map bigger than the knife-fight size with less than half of it
	/// seen: one support hero a day explores ahead of its errands.
	bool scoutingDue() const;
	mutable int seenFractionDay_ = -1;
	mutable double seenFraction_ = 0.0;
	mutable double seenWaterShare_ = 0.0;   // water share of the tiles we have seen

	/// Day the field hero's reserved scoring look last ran. -1 until it has.
	/// The look is a once-a-day first call, not a per-round one - running it
	/// every round starves the housekeeping chains several times over.
	int reservedLookDay_ = -1;

	/// Press-saturation bookkeeping for huntTheEnemy: consecutive days the
	/// press picked the same boundary tile with no guard to wait on. A
	/// saturated seal does not open - the field stack belongs on the town.
	int pressSatDays_ = 0;
	int3 pressSatTile_;
	int32_t pressSatHero_ = -1;

	/// Frontier-exhaustion bookkeeping for the field hero: consecutive days
	/// the fog frontier did not grow while no hostile town was known. The
	/// sealed-pocket press never starts in this shape (chores keep the turn
	/// busy so the scout fallback is never reached), so the press
	/// saturation counter never runs - this is its parallel tripwire.
	int frontierDryDays_ = 0;
	int prevSeenCount_ = -1;

	/// Score every known object, pick the best reachable one, and walk to
	/// it (or explore when nothing is reachable). The universal fallback
	/// for a hero with no housekeeping claim. With minScoreToTake > 0 it is
	/// the gated reserved look instead: it takes the day only when the best
	/// target clears that bar, and returns false - leaving housekeeping its
	/// turn - when nothing reachable is worth pre-empting for.
	bool scoringPassMove(const CGHeroInstance * hero, double minScoreToTake);

	/// Move every stack but the weakest from the hero into a town garrison:
	/// the keep is what the burner walks on, the rest is the week's takings.
	void depositArmy(const CGHeroInstance * hero, const CGTownInstance * town);
	/// Order a hero along a path, stepping it out of the garrison slot
	/// first if that is where it is standing. A garrisoned hero cannot
	/// walk, and the alternative (stepping every garrisoned hero out at
	/// the top of its turn) parks the town holder on the entrance tile,
	/// where it blocks every other hero's way in.
	/// finalFightOk leaves the last step unfenced: callers that priced a
	/// destination fight at their own margin (seal press, swat, break-out)
	/// keep it. Every other walk stops at the first unbeatable hostile.
	/// Returns whether a move was dispatched. Chains return it, so a walk
	/// the leash, a fence or the reach check refuses falls through to the
	/// next option instead of ending the hero's day standing still (16 of 16
	/// K_duel games: 7-69 "leashed" refusals each, the same target re-picked
	/// every round, one field hero idle days 4-11). ignoreLeash is for
	/// retreats and evacuations, which the small-map leash must not refuse.
	bool walk(const CGHeroInstance * hero, const std::vector<int3> & steps,
		bool finalFightOk = false, bool ignoreLeash = false);

	/// The small-map field-hero leash as a test: may this hero end a move on
	/// `end`? Asked by target selection, not only by walk(), so a target the
	/// walk would refuse is never chosen and the day is not lost to it.
	bool leashAllows(const CGHeroInstance * hero, const int3 & end) const;
	/// Walking steps home for the leash, from crossingField (fog open, known
	/// walls closed), rebuilt once a day or when home moves.
	mutable std::vector<int> leashField_;
	mutable int leashFieldDay_ = -1;
	mutable int3 leashFieldHome_;

	/// How many of these steps walk() would actually send before a deferred
	/// tile or an unbeatable hostile stops it. Zero means the walk moves
	/// nobody, and a target reached that way must not be picked.
	size_t safePrefix(const CGHeroInstance * hero, const std::vector<int3> & steps,
		bool finalFightOk = false) const;

	/// Sell surplus gold for whichever building resource is holding the town
	/// up. Needs a marketplace, which is why one is built early.
	void tradeForWhatIsMissing();

	/// Buy the wood, ore or rare shortfall of the best building blocked only
	/// on those, paying from surplus wood and ore, then surplus gold.
	void tradeForBlockedBuilding();

	/// Tell the evaluator what we are short of, so a mine is scored by what
	/// it produces rather than by being a mine.
	void updateResourceScarcity();

	int difficultyIndex() const;
};
