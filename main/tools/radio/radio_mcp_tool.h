#ifndef RADIO_MCP_TOOL_H
#define RADIO_MCP_TOOL_H

#include "mcp_server.h"

// Registers the internet-radio MCP tools so the voice agent can play, stop,
// and report the status of a built-in VOV live station. Requires network
// access; call once during startup.
class RadioMcpTool {
public:
    static void Initialize();

private:
    static ToolResult HandlePlayStation(const PropertyList& properties);
    static ToolResult HandleStop(const PropertyList& properties);
    static ToolResult HandleGetStatus(const PropertyList& properties);
};

#endif  // RADIO_MCP_TOOL_H
