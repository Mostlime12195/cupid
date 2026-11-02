/**
 * Web entry point for LÖVE.
 *
 * Replaces src/love.cpp: instead of resuming the boot coroutine in a blocking
 * loop, it is resumed once per browser animation frame. boot.lua yields after
 * every iteration of the love.run main loop, so one resume == one frame.
 **/

#include "common/version.h"
#include "common/runtime.h"
#include "common/Variant.h"
#include "modules/love/love.h"
#include "modules/thread/web/GreenThreads.h"

#include <SDL3/SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

// Implemented in library_love.js.
extern void loveweb_on_quit(int code, int restart);
}

namespace
{

lua_State *L = nullptr;
int stackpos = 0;
bool paused = false;
std::vector<std::string> args;
love::Variant restartvalue;

int love_preload(lua_State *L, lua_CFunction f, const char *name)
{
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "preload");
	lua_pushcfunction(L, f);
	lua_setfield(L, -2, name);
	lua_pop(L, 2);
	return 0;
}

void start()
{
	L = luaL_newstate();
	luaL_openlibs(L);

	love_preload(L, luaopen_love_jitsetup, "love.jitsetup");
	lua_getglobal(L, "require");
	lua_pushstring(L, "love.jitsetup");
	lua_call(L, 1, 0);

	love_preload(L, luaopen_love, "love");

	lua_newtable(L);
	lua_pushstring(L, args.empty() ? "love" : args[0].c_str());
	lua_rawseti(L, -2, -2);
	lua_pushstring(L, "embedded boot.lua");
	lua_rawseti(L, -2, -1);
	for (size_t i = 1; i < args.size(); i++)
	{
		lua_pushstring(L, args[i].c_str());
		lua_rawseti(L, -2, (int) i);
	}
	lua_setglobal(L, "arg");

	lua_getglobal(L, "require");
	lua_pushstring(L, "love");
	lua_call(L, 1, 1);
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "_exe");
	love::luax_pushvariant(L, restartvalue);
	lua_setfield(L, -2, "restart");
	restartvalue = love::Variant();
	lua_pop(L, 1);

	lua_getglobal(L, "require");
	lua_pushstring(L, "love.boot");
	lua_call(L, 1, 1);

	// Same setup as love.cpp: the boot function is resumed on the main state.
	lua_newthread(L);
	lua_pushvalue(L, -2);
	stackpos = lua_gettop(L);
}

void finish()
{
	int retval = 0;
	bool restart = false;
	int retidx = stackpos;

	if (!lua_isnoneornil(L, retidx))
	{
		if (lua_type(L, retidx) == LUA_TSTRING && strcmp(lua_tostring(L, retidx), "restart") == 0)
			restart = true;
		if (lua_isnumber(L, retidx))
			retval = (int) lua_tonumber(L, retidx);
		if (retidx < lua_gettop(L))
			restartvalue = love::luax_checkvariant(L, retidx + 1, false);
	}

	lua_close(L);
	L = nullptr;

	if (restart)
	{
		start();
		return;
	}

	emscripten_cancel_main_loop();
	SDL_Quit();
	loveweb_on_quit(retval, 0);
}

void frame()
{
	if (L == nullptr || paused)
		return;

	double start = emscripten_get_now();

	int nres = 0;
	int status = love::luax_resume(L, 0, &nres);

	if (status == LUA_YIELD)
	{
		lua_pop(L, lua_gettop(L) - stackpos);

		// love.thread Threads (and streaming audio) get whatever is left of
		// a 60 Hz frame after the game's own update and draw.
		double elapsed = (emscripten_get_now() - start) / 1000.0;
		double budget = std::clamp(0.014 - elapsed, 0.002, 0.012);
		love::thread::web::runFrame(budget);
		return;
	}

	if (status != 0)
	{
		const char *msg = lua_tostring(L, -1);
		emscripten_log(EM_LOG_ERROR, "love: fatal error: %s", msg ? msg : "(unknown)");
		lua_close(L);
		L = nullptr;
		emscripten_cancel_main_loop();
		loveweb_on_quit(1, 0);
		return;
	}

	finish();
}

} // anonymous namespace

extern "C" {

// Called from JS when the player wants the game to stop (e.g. editor "Stop").
EMSCRIPTEN_KEEPALIVE void loveweb_quit()
{
	if (L == nullptr)
		return;
	SDL_Event e = {};
	e.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&e);
}

// Pause the main loop entirely (tab hidden, editor pause button).
EMSCRIPTEN_KEEPALIVE void loveweb_set_paused(int p)
{
	paused = p != 0;
}

EMSCRIPTEN_KEEPALIVE void loveweb_push_event(const char *)
{
}

} // extern "C"

int main(int argc, char **argv)
{
	for (int i = 0; i < argc; i++)
		args.emplace_back(argv[i]);

	start();
	// 0 fps = requestAnimationFrame; don't simulate an infinite loop so main
	// returns and the JS side can continue.
	emscripten_set_main_loop(frame, 0, false);
	return 0;
}
