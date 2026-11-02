#include "EmscriptenClient.h"

#ifdef HTTPS_BACKEND_EMSCRIPTEN

#include <cstdlib>
#include <stdexcept>
#include <vector>

extern "C"
{
// Implemented in web/native/library_love.js. Returns the HTTP status, 0 for a
// network error (the body is then an error message, like the NSURL backend),
// or -1 if the URL can't be requested at all (the body is the error message).
// The body and header buffers are malloc'd and owned by the caller.
int loveweb_https_request(const char *url, const char *method,
                          const char *const *headers, int nheaders,
                          const char *body, int bodylen,
                          char **outbody, int *outbodylen, char **outheaders);
}

bool EmscriptenClient::valid() const
{
	return true;
}

HTTPSClient::Reply EmscriptenClient::request(const HTTPSClient::Request &req)
{
	std::vector<const char *> headers;
	for (const auto &header : req.headers)
	{
		headers.push_back(header.first.c_str());
		headers.push_back(header.second.c_str());
	}

	char *body = nullptr;
	int bodylen = 0;
	char *rawheaders = nullptr;

	int status = loveweb_https_request(req.url.c_str(), req.method.c_str(),
		headers.data(), (int) req.headers.size(),
		req.postdata.data(), (int) req.postdata.size(),
		&body, &bodylen, &rawheaders);

	std::string bodystr = body != nullptr ? std::string(body, bodylen) : std::string();
	std::string headerstr = rawheaders != nullptr ? std::string(rawheaders) : std::string();
	free(body);
	free(rawheaders);

	if (status < 0)
		throw std::runtime_error(bodystr);

	Reply reply;
	reply.responseCode = status;
	reply.body = std::move(bodystr);

	// XMLHttpRequest.getAllResponseHeaders(): "name: value\r\n" per header.
	size_t pos = 0;
	while (pos < headerstr.size())
	{
		size_t end = headerstr.find("\r\n", pos);
		if (end == std::string::npos)
			end = headerstr.size();
		size_t colon = headerstr.find(':', pos);
		if (colon != std::string::npos && colon < end)
		{
			size_t valuestart = headerstr.find_first_not_of(' ', colon + 1);
			if (valuestart == std::string::npos || valuestart > end)
				valuestart = end;
			reply.headers[headerstr.substr(pos, colon - pos)] = headerstr.substr(valuestart, end - valuestart);
		}
		pos = end + 2;
	}

	return reply;
}

#endif // HTTPS_BACKEND_EMSCRIPTEN
