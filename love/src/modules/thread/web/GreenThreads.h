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

#ifndef LOVE_THREAD_WEB_GREENTHREADS_H
#define LOVE_THREAD_WEB_GREENTHREADS_H

#include "common/config.h"

#ifdef LOVE_EMSCRIPTEN

#include "common/runtime.h"
#include "common/Variant.h"

#include <functional>

namespace love
{
namespace thread
{

class LuaThread;
class Channel;

namespace sdl
{
class Thread;
}

/**
 * The browser build has no OS threads, so love.thread Threads run as
 * coroutines on the main thread. Each Thread still gets its own lua_State.
 * Threads are time-sliced with an instruction count hook (Lua 5.1 allows
 * yielding from hooks), and blocking operations like Channel:demand yield
 * the thread until they can complete.
 *
 * When the main thread (or a thread that can't yield, e.g. inside pcall)
 * blocks, other threads are run synchronously until the operation completes.
 **/
namespace web
{

void spawn(LuaThread *t, sdl::Thread *owner);

// Runs threads for up to `budget` seconds, plus per-frame engine tasks.
// Called once per browser frame.
void runFrame(double budget);

// Per-frame callbacks for engine work that runs on a thread on desktop
// (e.g. refilling streaming audio buffers).
int addFrameTask(std::function<void()> task);
void removeFrameTask(int id);
void runFrameTasks();

// Blocking operations. Each pushes its Lua results (or yields) and returns
// the value a lua_CFunction should return.
int demand(lua_State *L, Channel *c, double timeout); // timeout < 0: none
int supply(lua_State *L, Channel *c, const Variant &v, double timeout);
int join(lua_State *L, LuaThread *t);
void waitFor(LuaThread *t); // runs other threads until t is done
int sleep(lua_State *L, double seconds);

// Called when a non-blocking poll (Channel:pop, Thread:isRunning) comes up
// empty on the main thread, so polling loops still let threads progress.
void poll(lua_State *L);

} // web
} // thread
} // love

#endif // LOVE_EMSCRIPTEN

#endif // LOVE_THREAD_WEB_GREENTHREADS_H
