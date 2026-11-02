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
#include <emscripten/eventloop.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

// Implemented in library_love.js.
extern void loveweb_on_quit(int code, int restart);
extern void loveweb_text_input_state(int active, int x, int y, int w, int h, int winw, int winh);
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

// ---------------------------------------------------------------------------
// Text input from the browser (IME, emoji pickers, on-screen keyboards).
//
// library_love.js keeps a hidden text field focused while SDL text input is
// active and forwards what it receives as SDL text events. SDL keeps the
// pointer we give it in the queued event, so the strings live here until the
// event has been taken off the queue.

std::vector<char *> eventStrings;

SDL_Window *getWindow()
{
	SDL_Window *window = SDL_GL_GetCurrentWindow();
	return window != nullptr ? window : SDL_GetKeyboardFocus();
}

void releaseEventStrings()
{
	if (eventStrings.empty())
		return;

	SDL_Event queued[128];
	int n = SDL_PeepEvents(queued, 128, SDL_PEEKEVENT, SDL_EVENT_TEXT_EDITING, SDL_EVENT_TEXT_INPUT);
	if (n >= 128)
		return; // Try again next frame.

	auto stillQueued = [&](const char *str)
	{
		for (int i = 0; i < n; i++)
		{
			const char *t = queued[i].type == SDL_EVENT_TEXT_INPUT ? queued[i].text.text : queued[i].edit.text;
			if (t == str)
				return true;
		}
		return false;
	};

	auto it = std::remove_if(eventStrings.begin(), eventStrings.end(), [&](char *str)
	{
		if (n > 0 && stillQueued(str))
			return false;
		SDL_free(str);
		return true;
	});
	eventStrings.erase(it, eventStrings.end());
}

void pushTextEvent(Uint32 type, const char *text, int start, int length)
{
	SDL_Window *window = getWindow();
	if (window == nullptr || !SDL_TextInputActive(window) || text == nullptr)
		return;

	char *copy = SDL_strdup(text);
	if (copy == nullptr)
		return;

	SDL_Event e = {};
	e.type = type;
	if (type == SDL_EVENT_TEXT_INPUT)
	{
		e.text.windowID = SDL_GetWindowID(window);
		e.text.text = copy;
	}
	else
	{
		e.edit.windowID = SDL_GetWindowID(window);
		e.edit.text = copy;
		e.edit.start = start;
		e.edit.length = length;
	}

	if (SDL_PushEvent(&e))
		eventStrings.push_back(copy);
	else
		SDL_free(copy);
}

// Tells the page when text input starts or stops, or its area changes.
void syncTextInputState()
{
	static int last[7] = {-1};

	int state[7] = {};
	SDL_Window *window = L != nullptr ? getWindow() : nullptr;
	if (window != nullptr && SDL_TextInputActive(window))
	{
		SDL_Rect rect = {};
		int cursor = 0;
		state[0] = 1;
		if (SDL_GetTextInputArea(window, &rect, &cursor))
		{
			state[1] = rect.x + cursor;
			state[2] = rect.y;
			state[3] = rect.w;
			state[4] = rect.h;
		}
		SDL_GetWindowSize(window, &state[5], &state[6]);
	}

	if (memcmp(state, last, sizeof(state)) != 0)
	{
		memcpy(last, state, sizeof(state));
		loveweb_text_input_state(state[0], state[1], state[2], state[3], state[4], state[5], state[6]);
	}
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
	syncTextInputState();
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
		releaseEventStrings();
		syncTextInputState();

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
		syncTextInputState();
		loveweb_on_quit(1, 0);
		return;
	}

	finish();
}

// requestAnimationFrame stops in background tabs but timers keep running
// (and aren't throttled while a tab plays audio), so streaming audio is also
// refilled from an interval.
void audioPump(void *)
{
	if (L != nullptr)
		love::thread::web::runFrameTasks();
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

// Resizes a resizable game window to fill the page, like a maximized desktop
// window. Returns 0 (and does nothing) if the game's window isn't resizable.
EMSCRIPTEN_KEEPALIVE int loveweb_fit_window(int w, int h)
{
	SDL_Window *window = SDL_GL_GetCurrentWindow();
	if (window == nullptr || (SDL_GetWindowFlags(window) & SDL_WINDOW_RESIZABLE) == 0)
		return 0;

	int curw = 0, curh = 0;
	SDL_GetWindowSize(window, &curw, &curh);
	if (curw != w || curh != h)
		SDL_SetWindowSize(window, w, h);
	return 1;
}

// Committed text from the page's hidden text field (love.textinput).
EMSCRIPTEN_KEEPALIVE void loveweb_text_input(const char *text)
{
	if (text != nullptr && *text != '\0')
		pushTextEvent(SDL_EVENT_TEXT_INPUT, text, 0, 0);
}

// IME composition in progress (love.textedited). start is in characters.
EMSCRIPTEN_KEEPALIVE void loveweb_text_editing(const char *text, int start, int length)
{
	pushTextEvent(SDL_EVENT_TEXT_EDITING, text, start, length);
}

// Editing keys that some on-screen keyboards only report as text field edits
// (no usable keydown): 0 = backspace, 1 = delete, 2 = return.
EMSCRIPTEN_KEEPALIVE void loveweb_text_key(int which)
{
	SDL_Window *window = getWindow();
	if (window == nullptr)
		return;

	static const SDL_Scancode scancodes[] = { SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_DELETE, SDL_SCANCODE_RETURN };
	static const SDL_Keycode keycodes[] = { SDLK_BACKSPACE, SDLK_DELETE, SDLK_RETURN };
	if (which < 0 || which > 2)
		return;

	for (int down = 1; down >= 0; down--)
	{
		SDL_Event e = {};
		e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		e.key.windowID = SDL_GetWindowID(window);
		e.key.scancode = scancodes[which];
		e.key.key = keycodes[which];
		e.key.mod = SDL_GetModState();
		e.key.down = down != 0;
		SDL_PushEvent(&e);
	}
}

} // extern "C"

int main(int argc, char **argv)
{
	for (int i = 0; i < argc; i++)
		args.emplace_back(argv[i]);

	start();
	// 0 fps = requestAnimationFrame; don't simulate an infinite loop so main
	// returns and the JS side can continue.
	emscripten_set_interval(audioPump, 50.0, nullptr);
	emscripten_set_main_loop(frame, 0, false);
	return 0;
}
