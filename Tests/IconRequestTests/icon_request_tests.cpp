// Icon request tests. The table and the serving loop in include/icon_requests.h
// take no game types, so a fake clock and a fake loader stand in for the game.
#include <icon_requests.h>

#include <cstdio>
#include <initializer_list>
#include <string>
#include <unordered_set>
#include <vector>

using namespace IconRequests;

static int g_checks = 0;
static int g_failed = 0;

static void Check(bool ok, const char* what)
{
	++g_checks;
	if (!ok)
	{
		++g_failed;
		printf("    FAIL %s\n", what);
	}
}

namespace
{
	Handle H(int n) { return reinterpret_cast<Handle>(static_cast<size_t>(n)); }

	std::unordered_set<std::string> Known(std::initializer_list<const char*> names)
	{
		return std::unordered_set<std::string>(names.begin(), names.end());
	}

	// A clock that moves only when the loader runs.
	struct FakeClock
	{
		double now = 0.0;
		double operator()() const { return now; }
	};
}

int main()
{
	printf("icon requests\n");

	{
		Table t;
		t.SetKnown(Known({ "a", "b" }));
		Check(t.Get("a") == nullptr, "an unloaded icon gives null");
		t.Get("a");
		t.Get("a");
		Check(t.Pending() == 1, "asking again for the same icon queues it once");
		t.Get("b");
		Check(t.Pending() == 2, "a second icon queues too");
		Check(t.Has("a") && t.Has("b"), "a queued icon counts as having one");
	}

	{
		Table t;
		Check(t.Get("a") == nullptr && t.Pending() == 0, "nothing queues before the item list is known");
		t.SetKnown(Known({ "a" }));
		Check(t.Get("nope") == nullptr && t.Pending() == 0, "an unknown name never queues");
		Check(!t.Has("nope"), "an unknown name has no icon");
		Check(t.Has("a"), "a known name has one");
		t.Get("a");
		Check(t.Pending() == 1, "a known name queues once the list is known");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b", "c", "d", "e" }));
		for (const char* n : { "a", "b", "c", "d", "e" })
			t.Get(n);

		FakeClock clock;
		const auto load = [&](const std::string&) { clock.now += 5.0; return H(1); };

		Check(Serve(t, load, clock, 8.0) == 2, "the budget stops the loads once it is spent");
		Check(t.Pending() == 3, "the rest wait for the next tick");
		Check(Serve(t, load, clock, 8.0) == 2, "the next tick serves the next ones");
		Check(Serve(t, load, clock, 8.0) == 1, "the last one is served alone");
		Check(Serve(t, load, clock, 8.0) == 0, "an empty queue serves nothing");
		Check(t.Get("a") == H(1), "a loaded icon is returned");
		Check(t.Pending() == 0, "a loaded icon is not queued again");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b" }));
		t.Get("a");
		t.Get("b");
		FakeClock clock;
		const auto slow = [&](const std::string&) { clock.now += 100.0; return H(1); };
		Check(Serve(t, slow, clock, 8.0) == 1, "one load is served even when it alone exceeds the budget");
		const auto instant = [&](const std::string&) { return H(2); };
		Check(Serve(t, instant, clock, 0.0) == 1, "a budget of zero still serves one");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b", "c" }));
		for (const char* n : { "a", "b", "c" })
			t.Get(n);
		FakeClock clock;
		int calls = 0;
		const auto fail = [&](const std::string&) { ++calls; return Handle(nullptr); };
		Check(Serve(t, fail, clock, 8.0) == 3 && calls == 3, "each queued icon is tried once per tick");
		Check(t.Pending() == 3, "failed icons stay queued for a later tick");
		Check(t.Get("a") == nullptr && t.Pending() == 3, "asking for a queued icon does not queue it again");
	}

	{
		Table t;
		t.SetKnown(Known({ "a" }));
		t.Get("a");
		FakeClock clock;
		int calls = 0;
		const auto lateLoad = [&](const std::string&) { return ++calls < 40 ? Handle(nullptr) : H(7); };
		for (int i = 0; i < 100; ++i)
			Serve(t, lateLoad, clock, 8.0);
		Check(calls == 40, "a failing icon is retried until it loads, then left alone");
		Check(t.Get("a") == H(7), "the retried icon ends up loaded");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b" }));
		t.Get("a");
		FakeClock clock;
		int calls = 0;
		const auto never = [&](const std::string&) { ++calls; return Handle(nullptr); };
		for (int i = 0; i < kMaxAttempts + 50; ++i)
			Serve(t, never, clock, 8.0);
		Check(calls == kMaxAttempts, "a failing icon is tried kMaxAttempts times and no more");
		Check(t.Pending() == 0, "an icon given up on leaves the queue");
		Check(!t.Has("a"), "an icon given up on counts as having none");
		Check(t.Get("a") == nullptr && t.Pending() == 0, "asking for it again does not queue it");
		Check(t.Has("b"), "other icons are not affected");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b" }));
		t.Get("a");
		t.Get("b");
		FakeClock clock;
		int calls = 0;
		std::vector<std::string> logged;
		const auto noSlot = [&](const std::string& n)
		{
			++calls;
			if (n == "a")
				t.GiveUp(n);
			return Handle(nullptr);
		};
		const auto note = [&](const std::string& n) { logged.push_back(n); };
		Serve(t, noSlot, clock, 8.0, note);
		Check(calls == 2 && t.Pending() == 1, "an icon with no free slot is not queued again");
		Check(!t.Has("a") && t.Has("b"), "only the icon without a slot counts as having none");
		for (int i = 0; i < 20; ++i)
			Serve(t, noSlot, clock, 8.0, note);
		Check(calls == 22, "an icon with no free slot is tried once");
		Check(!t.GiveUp("a"), "giving up twice reports once");
		Check(logged.empty(), "a terminal give-up is not also counted as running out of attempts");
	}

	{
		Table t;
		t.SetKnown(Known({ "a" }));
		t.Get("a");
		FakeClock clock;
		std::vector<std::string> logged;
		const auto never = [](const std::string&) { return Handle(nullptr); };
		for (int i = 0; i < kMaxAttempts + 5; ++i)
			Serve(t, never, clock, 8.0, [&](const std::string& n) { logged.push_back(n); });
		Check(logged.size() == 1 && logged[0] == "a", "running out of attempts is reported once");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b", "c" }));
		t.Get("a");
		t.Get("b");
		t.Get("c");
		FakeClock clock;
		int n = 0;
		const auto load = [&](const std::string&) { return H(++n); };
		Serve(t, load, clock, 0.0);
		Serve(t, load, clock, 0.0);
		Check(t.Pending() == 1, "two of three are loaded");
		const std::vector<Handle> freed = t.Clear();
		Check(freed.size() == 2, "clearing hands back the loaded handles to free");
		Check(t.Pending() == 0, "clearing drops the queue");
		Check(!t.Has("a") && t.Get("a") == nullptr && t.Pending() == 0, "clearing forgets the item list");
		t.SetKnown(Known({ "a" }));
		Check(t.Get("a") == nullptr && t.Pending() == 1, "after a clear the table works again");
	}

	{
		// The loader runs without the table locked: the render callback asks
		// for icons while a load is in flight, and a lock held across the load
		// would stall it (and deadlock here).
		Table t;
		t.SetKnown(Known({ "a", "b" }));
		t.Get("a");
		FakeClock clock;
		bool asked = false;
		const auto load = [&](const std::string&)
		{
			t.Get("b");
			asked = t.Has("a") && t.Pending() == 1;
			return H(1);
		};
		Serve(t, load, clock, 8.0);
		Check(asked, "the table can be used while an icon loads");
	}

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
