/**
 * Copyright (c) 2006-2026 LOVE Development Team
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 **/

#include "GreenThreads.h"

#ifdef LOVE_EMSCRIPTEN

#include "thread/LuaThread.h"
#include "thread/Channel.h"
#include "thread/sdl/Thread.h"
#include "common/Exception.h"

#include <emscripten.h>

#include <algorithm>
#include <map>
#include <vector>

extern "C" {
// Needed to tell whether a coroutine is inside a C call (pcall, metamethod)
// and therefore can't yield.
#include "lstate.h"
}

namespace love
{
namespace thread
{
namespace web
{

namespace
{

// How often the count hook checks the clock, in VM instructions.
const int HOOK_INSTRUCTIONS = 1000;

// Minimum slice a runnable thread gets when several share a frame.
const double MIN_SLICE = 0.001;

// Slice used for each pass while the main thread is blocked.
const double BLOCKED_SLICE = 0.002;

// Extra time threads get when the main thread polls without result.
const double POLL_SLICE = 0.0005;

enum class WaitKind
{
	NONE,
	DEMAND,
	SUPPLY,
	SLEEP,
	JOIN,
};

struct Wait
{
	WaitKind kind = WaitKind::NONE;
	StrongRef<Channel> channel;
	uint64 id = 0;
	double deadline = -1.0; // absolute time; < 0 means no timeout
	StrongRef<LuaThread> target;
};

struct Green
{
	LuaThread *thread = nullptr; // retained by sdl::Thread::start
	sdl::Thread *owner = nullptr;
	lua_State *L = nullptr;      // the thread's own Lua state
	lua_State *co = nullptr;     // coroutine running the thread's code
	int coref = LUA_NOREF;
	int nargs = 0;               // values pushed onto co for the next resume
	bool started = false;
	bool busy = false;           // being resumed (somewhere on the C stack)
	bool done = false;
	double sliceEnd = 0.0;
	Wait wait;
};

std::vector<Green *> threads;
Green *current = nullptr;
size_t roundRobin = 0;
int depth = 0;

std::map<int, std::function<void()>> frameTasks;
int nextTaskID = 1;

double now()
{
	return emscripten_get_now() / 1000.0;
}

bool isYieldable(lua_State *L)
{
	if (current == nullptr)
		return false;

	// Code inside pcall runs in a child coroutine that forwards its yields
	// to the coroutine that called pcall (see lbaselib.c).
	for (;;)
	{
		if (L->nCcalls > L->baseCcalls)
			return false;
		if (L == current->co)
			return true;
		if (L->pcallparent == nullptr)
			return false;
		L = L->pcallparent;
	}
}

void countHook(lua_State *L, lua_Debug *)
{
	if (isYieldable(L) && now() >= current->sliceEnd)
		lua_yield(L, 0);
}

std::string traceback(Green &g)
{
	lua_State *L = g.L;
	lua_getglobal(L, "debug");
	lua_getfield(L, -1, "traceback");
	lua_remove(L, -2);
	lua_rawgeti(L, LUA_REGISTRYINDEX, g.coref);
	lua_xmove(g.co, L, 1); // error message

	std::string err;
	if (lua_pcall(L, 2, 1, 0) == 0 && lua_isstring(L, -1))
		err = lua_tostring(L, -1);
	else
		err = "Unknown error";
	lua_pop(L, 1);
	return err;
}

void finish(Green &g, const std::string *err)
{
	g.done = true;
	g.wait = Wait();

	if (g.L != nullptr)
	{
		g.thread->webFinish(g.L, err);
		g.L = nullptr;
		g.co = nullptr;
	}

	g.owner->webFinished();
	// Retained in sdl::Thread::start. May delete the thread (and its owner).
	g.thread->release();
}

// If the thread's wait can complete, pushes its results onto the coroutine.
bool completeWait(Green &g)
{
	Wait &w = g.wait;
	bool timedout = w.deadline >= 0.0 && now() >= w.deadline;

	switch (w.kind)
	{
	case WaitKind::NONE:
		return true;
	case WaitKind::DEMAND:
	{
		Variant v;
		if (w.channel->pop(&v))
			luax_pushvariant(g.co, v);
		else if (timedout)
			lua_pushnil(g.co);
		else
			return false;
		g.nargs = 1;
		break;
	}
	case WaitKind::SUPPLY:
		if (w.channel->hasRead(w.id))
			lua_pushboolean(g.co, 1);
		else if (timedout)
			lua_pushboolean(g.co, 0);
		else
			return false;
		g.nargs = 1;
		break;
	case WaitKind::SLEEP:
		if (!timedout)
			return false;
		g.nargs = 0;
		break;
	case WaitKind::JOIN:
		if (w.target->isRunning())
			return false;
		g.nargs = 0;
		break;
	}

	w = Wait();
	return true;
}

// Returns true if the thread was resumed (made progress).
bool step(Green &g, double sliceEnd)
{
	if (g.done || g.busy)
		return false;

	if (!g.started)
	{
		g.started = true;
		if (!g.thread->webPrepare(g.L, g.co, g.coref, g.nargs))
		{
			finish(g, nullptr);
			return true;
		}
		lua_sethook(g.co, countHook, LUA_MASKCOUNT, HOOK_INSTRUCTIONS);
	}
	else if (!completeWait(g))
		return false;

	Green *prev = current;
	current = &g;
	g.busy = true;
	g.sliceEnd = sliceEnd;

	int status = lua_resume(g.co, g.nargs);

	g.busy = false;
	g.nargs = 0;
	current = prev;

	if (status == LUA_YIELD)
		return true;

	if (status == 0)
		finish(g, nullptr);
	else
	{
		std::string err = traceback(g);
		finish(g, &err);
	}
	return true;
}

bool hasTimedWaits()
{
	for (Green *g : threads)
	{
		if (!g->done && g->wait.kind != WaitKind::NONE && g->wait.deadline >= 0.0)
			return true;
	}
	return false;
}

void cleanup()
{
	if (depth > 0)
		return;

	auto it = std::remove_if(threads.begin(), threads.end(), [](Green *g) {
		if (!g->done)
			return false;
		delete g;
		return true;
	});
	threads.erase(it, threads.end());
}

/**
 * Runs threads until cond() is true or `timeout` seconds pass (timeout < 0:
 * forever). Throws if nothing could ever make cond() true.
 **/
bool runUntil(const std::function<bool()> &cond, double timeout)
{
	double start = now();
	bool result = false;
	depth++;

	try
	{
		for (;;)
		{
			if (cond())
			{
				result = true;
				break;
			}

			if (timeout >= 0.0 && now() - start >= timeout)
				break;

			// Note: cond() may consume a value (Channel::pop), so it must only
			// be checked once per pass.
			bool progress = false;
			for (size_t i = 0; i < threads.size(); i++)
			{
				if (step(*threads[i], now() + BLOCKED_SLICE))
					progress = true;
			}

			if (!progress && timeout < 0.0 && !hasTimedWaits())
				throw love::Exception("Deadlock: this would wait forever, because no thread can make progress.");
		}
	}
	catch (...)
	{
		depth--;
		cleanup();
		throw;
	}

	depth--;
	cleanup();
	return result;
}

// Runs threads for up to `seconds`, stopping early if all of them are
// blocked on something the caller has to provide.
void runFor(double seconds)
{
	if (threads.empty())
		return;

	double end = now() + seconds;
	depth++;
	for (;;)
	{
		bool progress = false;
		for (size_t i = 0; i < threads.size() && now() < end; i++)
		{
			if (step(*threads[i], end))
				progress = true;
		}
		if (now() >= end || (!progress && !hasTimedWaits()))
			break;
	}
	depth--;
	cleanup();
}

int pushVariantOrNil(lua_State *L, bool ok, const Variant &v)
{
	if (ok)
		luax_pushvariant(L, v);
	else
		lua_pushnil(L);
	return 1;
}

} // anonymous namespace

void spawn(LuaThread *t, sdl::Thread *owner)
{
	Green *g = new Green();
	g->thread = t;
	g->owner = owner;
	threads.push_back(g);
}

void runFrameTasks()
{
	for (auto &task : frameTasks)
		task.second();
}

void runFrame(double budget)
{
	runFrameTasks();

	if (threads.empty())
		return;

	double end = now() + budget;
	depth++;

	for (;;)
	{
		size_t n = threads.size();
		size_t runnable = 0;
		for (Green *g : threads)
			runnable += (!g->done && !g->busy) ? 1 : 0;

		bool progress = false;
		for (size_t k = 0; k < n && now() < end; k++)
		{
			Green *g = threads[(roundRobin + k) % n];
			double slice = std::max(MIN_SLICE, (end - now()) / std::max<size_t>(runnable, 1));
			if (step(*g, std::min(end, now() + slice)))
				progress = true;
		}
		roundRobin++;

		if (!progress || now() >= end)
			break;
	}

	depth--;
	cleanup();
}

int addFrameTask(std::function<void()> task)
{
	int id = nextTaskID++;
	frameTasks[id] = std::move(task);
	return id;
}

void removeFrameTask(int id)
{
	frameTasks.erase(id);
}

int demand(lua_State *L, Channel *c, double timeout)
{
	Variant v;
	if (c->pop(&v))
		return pushVariantOrNil(L, true, v);

	if (isYieldable(L))
	{
		Wait &w = current->wait;
		w.kind = WaitKind::DEMAND;
		w.channel.set(c);
		w.deadline = timeout >= 0.0 ? now() + timeout : -1.0;
		return lua_yield(L, 0);
	}

	bool ok = false;
	luax_catchexcept(L, [&]() { ok = runUntil([&]() { return c->pop(&v); }, timeout); });
	return pushVariantOrNil(L, ok, v);
}

int supply(lua_State *L, Channel *c, const Variant &v, double timeout)
{
	uint64 id = c->push(v);
	if (c->hasRead(id))
	{
		lua_pushboolean(L, 1);
		return 1;
	}

	if (isYieldable(L))
	{
		Wait &w = current->wait;
		w.kind = WaitKind::SUPPLY;
		w.channel.set(c);
		w.id = id;
		w.deadline = timeout >= 0.0 ? now() + timeout : -1.0;
		return lua_yield(L, 0);
	}

	bool ok = false;
	luax_catchexcept(L, [&]() { ok = runUntil([&]() { return c->hasRead(id); }, timeout); });
	lua_pushboolean(L, ok ? 1 : 0);
	return 1;
}

int join(lua_State *L, LuaThread *t)
{
	if (!t->isRunning())
		return 0;

	if (current != nullptr && current->thread == t)
		return luaL_error(L, "A thread cannot wait for itself.");

	if (isYieldable(L))
	{
		Wait &w = current->wait;
		w.kind = WaitKind::JOIN;
		w.target.set(t);
		return lua_yield(L, 0);
	}

	luax_catchexcept(L, [&]() { waitFor(t); });
	return 0;
}

void waitFor(LuaThread *t)
{
	runUntil([&]() { return !t->isRunning(); }, -1.0);
}

int sleep(lua_State *L, double seconds)
{
	if (isYieldable(L))
	{
		Wait &w = current->wait;
		w.kind = WaitKind::SLEEP;
		w.deadline = now() + std::max(seconds, 0.0);
		return lua_yield(L, 0);
	}

	// Blocking the browser's main thread is pointless, but a game sleeping
	// while it waits for a thread should let that thread work meanwhile.
	if (current == nullptr)
		runFor(seconds);
	return 0;
}

void poll(lua_State *L)
{
	if (current == nullptr && !threads.empty())
		runFor(POLL_SLICE);
	LOVE_UNUSED(L);
}

} // web
} // thread
} // love

#endif // LOVE_EMSCRIPTEN
