/*
 * test_main.cpp, part of the OmniAI VCMI plugin
 *
 * Standalone unit canaries for the memory and net subsystems. Compiled as a
 * console exe linked against the VCMI import lib; no engine needed.
 * Covers: generational-handle staleness, QSBR deferred reclamation, pending
 * action receipt correlation and rejection rollback.
 */
#include "../StdInc.h"
#include "../memory/GenerationalHandle.h"
#include "../memory/QSBRReclaimer.h"
#include "../net/PendingActionQueue.h"
#include "../eval/ShadowDAG.h"
#include "../eval/UtilityEvaluator.h"
#include "../eval/DifficultyMatrix.h"
#include "../memory/LearningStore.h"

#include "networkPacks/PacksForClient.h"
#include "bonuses/CBonusSystemNode.h"
#include "bonuses/Bonus.h"
#include "bonuses/BonusEnum.h"
#include "bonuses/BonusSelector.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

static int failures = 0;
#define CHECK(cond, name) do { \
		if(cond) { std::printf("PASS %s\n", name); } \
		else { std::printf("FAIL %s\n", name); ++failures; } \
	} while(0)

using namespace omniai;

static void testHandlePackRoundtrip()
{
	GenerationalHandle h(1234, 77);
	GenerationalHandle u = GenerationalHandle::unpack(h.pack());
	CHECK(u == h, "handle pack/unpack roundtrip");
	CHECK(h.valid(), "handle valid when generation != 0");
	CHECK(!GenerationalHandle().valid(), "default handle invalid");
}

static void testStaleGeneration()
{
	HandleRegistry reg;
	auto h1 = reg.acquire(500);
	CHECK(reg.validate(h1), "fresh handle validates");
	CHECK(reg.resolve(h1) == 500, "resolve returns object id");

	reg.retire(500);
	CHECK(!reg.validate(h1), "retired handle fails validation (stale)");
	CHECK(reg.resolve(h1) == -1, "stale resolve returns -1");

	auto h2 = reg.acquire(501);
	CHECK(h2.index == h1.index + 1, "new object gets next slot");
	CHECK(h2.generation == 1, "new slot starts at generation 1");
}

static void testGenerationBumpsOnReuse()
{
	HandleRegistry reg;
	auto h1 = reg.acquire(900);
	reg.retire(900);
	auto h2 = reg.acquire(900); // same object id re-registered (e.g. reload)
	CHECK(h2.index == h1.index, "reacquire reuses the slot");
	CHECK(h2.generation == h1.generation + 1, "generation bumped");
	CHECK(!reg.validate(h1), "old handle still stale after reacquire");
	CHECK(reg.validate(h2), "new handle validates");
}

static void testQSBRDeferredReclaim()
{
	auto & r = QSBRReclaimer::instance();
	r.setParticipants(2);
	r.purge(); // drain leftovers from earlier tests

	auto flag = std::make_shared<int>(1);
	{
		std::weak_ptr<int> w = flag;
		r.retire(flag);
		CHECK(!w.expired(), "limbo keeps object alive after retire");
		CHECK(r.pending() >= 1, "pending counts retired object");
	}

	// One participant quiesces: not enough for a sweep (needs 2)
	r.declareQuiescentState();
	// Second participant quiesces: sweep runs
	r.declareQuiescentState();
	CHECK(r.pending() == 0, "purge runs after all participants quiesce");
}

static void testQSBRDirectPurge()
{
	auto & r = QSBRReclaimer::instance();
	r.retire(std::make_shared<int>(7));
	r.retire(std::make_shared<int>(8));
	size_t before = r.pending();
	r.purge();
	CHECK(before >= 2 && r.pending() == 0, "direct purge drains limbo");
}

static void testReceiptCorrelation()
{
	PendingActionQueue q;
	bool rolled = false;
	q.track(42, 100, ActionKind::Move, [&]{ rolled = true; });
	CHECK(q.pendingCount() == 1, "pending after track");

	PackageApplied applied(PlayerColor(1), 42, 100, true);
	bool matched = q.onReceipt(applied);
	CHECK(matched, "receipt correlates by requestID");
	CHECK(q.pendingCount() == 0, "pending cleared on match");
	CHECK(!rolled, "accepted receipt does not roll back");
}

static void testRejectedActionRollsBack()
{
	PendingActionQueue q;
	bool rolled = false;
	q.track(7, 200, ActionKind::Trade, [&]{ rolled = true; });

	PackageApplied rejected(PlayerColor(1), 7, 200, false);
	bool matched = q.onReceipt(rejected);
	CHECK(matched, "rejected receipt still correlates");
	CHECK(rolled, "rejection runs the rollback");
	CHECK(q.pendingCount() == 0, "pending cleared after rollback");
}

static void testExpirySweep()
{
	PendingActionQueue q;
	bool rolled = false;
	q.track(9, 300, ActionKind::Build, [&]{ rolled = true; });
	// Backdate is not exposed; sweep with zero timeout expires everything.
	size_t swept = q.sweepExpired(std::chrono::milliseconds(0));
	CHECK(swept == 1, "sweepExpired removes stale action");
	CHECK(rolled, "expired action rolls back");
}

static void testShadowDAGSeesSourceBonuses()
{
	// Regression: attach direction was inverted (source attached TO root),
	// which left the shadow root empty and artifactDelta() always zero.
	CBonusSystemNode artifact(BonusNodeType::ARTIFACT, false);
	artifact.addNewBonus(std::make_shared<Bonus>(
		BonusDuration::PERMANENT, BonusType::STACK_HEALTH,
		BonusSource::ARTIFACT, 10, BonusSourceID(ArtifactID(1))));

	ShadowDAG dag;
	const int64_t baseline = dag.valOf(Selector::type()(BonusType::STACK_HEALTH), 0);
	dag.attachSource(artifact);
	const int64_t withArt = dag.valOf(Selector::type()(BonusType::STACK_HEALTH), 0);
	dag.detachSource(artifact);

	CHECK(withArt - baseline == 10, "shadow DAG sees attached source bonuses");
	CHECK(dag.valOf(Selector::type()(BonusType::STACK_HEALTH), 0) == baseline,
		"detach restores baseline");
}

static void testEvaluatorShadowIsLive()
{
	// Regression: shadowOwner_/shadow_ were never assigned, so the DAG path
	// silently returned 0 in production regardless of the attach fix.
	omniai::HandleRegistry reg;
	omniai::UtilityEvaluator eval(reg, omniai::profileFor(1));

	CBonusSystemNode artifact(BonusNodeType::ARTIFACT, false);
	artifact.addNewBonus(std::make_shared<Bonus>(
		BonusDuration::PERMANENT, BonusType::STACK_HEALTH,
		BonusSource::ARTIFACT, 10, BonusSourceID(ArtifactID(1))));

	// bonusNodeDelta goes through the owned ShadowEvaluator - the hero arg
	// is unused in the current implementation, so a null hero is safe here.
	CHECK(eval.bonusNodeDelta(artifact, *static_cast<CGHeroInstance*>(nullptr)) != 0.0,
		"evaluator shadow evaluator is instantiated");
}

static void testLearningStoreDisabledDefaults()
{
	// With the flag off the store must be a pure no-op: every read returns
	// the neutral value and every record is dropped.
	auto & learn = LearningStore::instance();
	learn.init(nullptr); // no flag, no env -> disabled
	CHECK(!learn.enabled(), "learning disabled without flag");
	CHECK(learn.worthAdjust(42) == 0.0, "disabled store zero worth adjust");
	CHECK(learn.dangerThreshold() == 1.1, "disabled store default danger");
	CHECK(!learn.isDepleted(7), "disabled store nothing depleted");
	learn.recordVisit(42, true);
	learn.recordBattle(1.5, false);
	learn.markDepleted(7);
	learn.flush(); // must not write when disabled
	CHECK(learn.worthAdjust(42) == 0.0, "disabled store ignores records");
}

static void testLearningStoreFollowsDir()
{
	// OmniAI.cpp hands the store its omniDir(), which follows OMNIAI_DIR, so
	// an isolated install keeps memory.json beside its decision logs and
	// never writes it to Documents (DMB Dev's DMB-ISOLATION-1 marker).
	namespace fs = std::filesystem;
	const fs::path dir = fs::current_path() / "omniai_test_learning";
	std::error_code ec;
	fs::remove_all(dir, ec);
	_putenv_s("OMNIAI_LEARNING", "1");
	auto & learn = LearningStore::instance();
	learn.setDir(dir.string());
	learn.init(nullptr);
	CHECK(learn.enabled(), "learning on through OMNIAI_LEARNING");
	CHECK(learn.storePath() == (dir / "memory.json").string(), "memory file in the folder set");
	learn.recordVisit(42, true);
	learn.flush();
	CHECK(fs::exists(dir / "memory.json", ec), "flush writes into the folder set");
	// Back to what the other tests expect: learning off, no folder set.
	_putenv_s("OMNIAI_LEARNING", "");
	learn.setDir(std::string());
	learn.init(nullptr);
	CHECK(!learn.enabled(), "learning off once OMNIAI_LEARNING is cleared");
	CHECK(learn.storePath().empty(), "no memory file while learning is off");
	fs::remove_all(dir, ec);
}

int main()
{
	testHandlePackRoundtrip();
	testStaleGeneration();
	testGenerationBumpsOnReuse();
	testQSBRDeferredReclaim();
	testQSBRDirectPurge();
	testReceiptCorrelation();
	testRejectedActionRollsBack();
	testExpirySweep();
	testShadowDAGSeesSourceBonuses();
	testEvaluatorShadowIsLive();
	testLearningStoreDisabledDefaults();
	testLearningStoreFollowsDir();

	std::printf("%s (%d failure%s)\n",
		failures ? "FAILURES" : "ALL PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
