#pragma once

// LÖVE web: lua-https backend for Emscripten. Requests are made by the
// browser with a synchronous XMLHttpRequest (see web/native/library_love.js),
// so they block like the other backends, and are subject to CORS.

#include "../common/config.h"

#ifdef HTTPS_BACKEND_EMSCRIPTEN

#include "../common/HTTPSClient.h"

class EmscriptenClient : public HTTPSClient
{
public:
	virtual bool valid() const override;
	virtual HTTPSClient::Reply request(const HTTPSClient::Request &req) override;
};

#endif // HTTPS_BACKEND_EMSCRIPTEN
