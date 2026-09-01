#pragma once

#include "core/Ops.h"

#include <string>

//////////////////////////////////////////////////////////////////////////////////////////////////
// McpHandler - Loom as an MCP server.
//
// MCP is JSON-RPC 2.0. This class takes a request body and returns a response body; it knows
// nothing about HTTP, Crow, sockets or headers. That is deliberate on two counts: it can be tested
// without a socket, and if a stdio transport is ever wanted (for clients that do not speak HTTP)
// it is a main() around this class and nothing else.
//
// EVERY TOOL GOES THROUGH Ops, exactly like the REST routes. That was the whole point of building
// an operations layer in phase 1 - MCP here is a schema table plus a dispatch switch, not a second
// implementation of the API that slowly drifts from the first one. When a tool and a route
// disagree about what "since" means, agents and humans get different answers to the same question,
// and nobody notices for weeks.
//
// TRANSPORT: Streamable HTTP, the current MCP transport. A POST carrying a JSON-RPC request gets a
// plain application/json response. The spec also permits answering with an SSE stream, which is
// only worth the complexity for servers that push messages of their own - Loom answers questions
// and has nothing to volunteer, so it does not open one. GET on the endpoint is refused for the
// same reason, which the spec explicitly allows.
//
// SESSIONS: none. Loom holds no per-client state, so there is nothing for an Mcp-Session-Id to
// identify. The header is optional and omitting it is valid.
//
// TOOL ERRORS vs PROTOCOL ERRORS - the distinction that matters:
//
//   A malformed request, an unknown method, or bad params is a JSON-RPC error object. The client
//   library handles it and the model never sees it.
//
//   A tool that ran and failed - no such jot, name already taken, edit conflict - is a SUCCESSFUL
//   JSON-RPC response carrying isError:true. That is what puts the failure in front of the model,
//   which can then read it and do something sensible, like re-fetch and retry the edit. Reporting
//   a 409 as a protocol error would hide the one piece of information the agent needed.
//////////////////////////////////////////////////////////////////////////////////////////////////

class McpHandler
{
public:
    McpHandler(Ops& ops, JotStore& store);

    // Returns the JSON-RPC response body, or an EMPTY string when the message was a notification
    // and the protocol requires no reply (the caller should answer HTTP 202 with no body).
    std::string Handle(const std::string& sRequestJson);

private:
    Ops&      mOps;
    JotStore& mStore;
};
