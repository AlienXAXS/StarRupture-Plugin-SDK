#pragma once

// The request side of lazy icon loading, free of game and ImGui types so it
// can be tested on its own. item_icons.h builds on it.
//
// The render callback asks for an icon by name; the game thread serves the asks
// in small batches and stores the handle. One mutex guards the table. It is
// never held while an icon loads, because a load blocks for several
// milliseconds and the render callback reads the table.

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace IconRequests
{
	using Handle = void*;

	// How long one tick may spend loading icons, in milliseconds. It always
	// loads at least one.
	constexpr double kDefaultBudgetMs = 8.0;

	// How many times one icon may fail to load before it is given up. A load
	// fails while its texture is still streaming in, which clears within a few
	// frames, so this only stops an icon that never loads.
	constexpr int kMaxAttempts = 600;

	class Table
	{
	public:
		// The names that have an icon to load. Anything else is never queued.
		void SetKnown(std::unordered_set<std::string> names)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_known = std::move(names);
		}

		// True for a name that has an icon, loaded, loading or still to load.
		bool Has(const std::string& name) const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_known.find(name) == m_known.end())
				return false;
			const auto it = m_entries.find(name);
			return it == m_entries.end() || it->second.state != State::GaveUp;
		}

		// The handle if it is loaded. Otherwise queues the name (once) and
		// returns null.
		Handle Get(const std::string& name)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const auto it = m_entries.find(name);
			if (it != m_entries.end())
				return it->second.handle;
			if (m_known.find(name) == m_known.end())
				return nullptr;
			m_entries.emplace(name, Entry{});
			m_queue.push_back(name);
			return nullptr;
		}

		// Moves the next queued name out for loading. False when none is queued.
		bool Take(std::string& name)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_queue.empty())
				return false;
			name = std::move(m_queue.front());
			m_queue.pop_front();
			m_entries[name].state = State::Loading;
			return true;
		}

		// Gives up on a name for good, for a failure that waiting will not fix.
		// Call it while the name is being loaded; Finish then leaves it alone.
		// True the first time, so the caller can log once.
		bool GiveUp(const std::string& name)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const auto it = m_entries.find(name);
			if (it == m_entries.end() || it->second.state == State::GaveUp)
				return false;
			it->second.state = State::GaveUp;
			return true;
		}

		// Records the result of loading a taken name. A null handle is a failure:
		// the name goes to the back of the queue, until it has failed
		// kMaxAttempts times. True when this call gave the name up.
		bool Finish(const std::string& name, Handle handle)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const auto it = m_entries.find(name);
			if (it == m_entries.end() || it->second.state == State::GaveUp)
				return false;
			Entry& entry = it->second;
			if (handle)
			{
				entry.handle = handle;
				entry.state  = State::Loaded;
			}
			else if (++entry.attempts >= kMaxAttempts)
			{
				entry.state = State::GaveUp;
				return true;
			}
			else
			{
				entry.state = State::Queued;
				m_queue.push_back(name);
			}
			return false;
		}

		size_t Pending() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_queue.size();
		}

		// Forgets everything and returns the loaded handles for the caller to free.
		std::vector<Handle> Clear()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			std::vector<Handle> loaded;
			for (const auto& [name, entry] : m_entries)
				if (entry.handle)
					loaded.push_back(entry.handle);
			m_entries.clear();
			m_queue.clear();
			m_known.clear();
			return loaded;
		}

	private:
		enum class State { Queued, Loading, Loaded, GaveUp };

		struct Entry
		{
			Handle handle   = nullptr;
			int    attempts = 0;
			State  state    = State::Queued;
		};

		mutable std::mutex                     m_mutex;
		std::unordered_set<std::string>        m_known;
		std::unordered_map<std::string, Entry> m_entries;
		std::deque<std::string>                m_queue;
	};

	// Loads queued icons until the budget is spent, trying each queued name at
	// most once. load(name) returns the handle or null; nowMs() is a clock in
	// milliseconds. gaveUp(name) is called when a failed load uses up the name's
	// attempts. Returns how many loads it tried. Call on the game thread.
	template <class LoadFn, class NowFn, class GaveUpFn>
	int Serve(Table& table, LoadFn&& load, NowFn&& nowMs, double budgetMs, GaveUpFn&& gaveUp)
	{
		const double start = nowMs();
		size_t left = table.Pending();
		int tried = 0;
		std::string name;
		while (left-- > 0 && (tried == 0 || nowMs() - start < budgetMs) && table.Take(name))
		{
			if (table.Finish(name, load(name)))
				gaveUp(name);
			++tried;
		}
		return tried;
	}

	template <class LoadFn, class NowFn>
	int Serve(Table& table, LoadFn&& load, NowFn&& nowMs, double budgetMs)
	{
		return Serve(table, load, nowMs, budgetMs, [](const std::string&) {});
	}
}
