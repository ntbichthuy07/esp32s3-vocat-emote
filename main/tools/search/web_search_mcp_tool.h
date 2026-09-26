#ifndef WEB_SEARCH_MCP_TOOL_H
#define WEB_SEARCH_MCP_TOOL_H

#include "mcp_server.h"

// Registers the web search MCP tool, backed directly by the Tavily search API (no LAN service
// dependency). Works on any board with internet access; call once during startup.
class WebSearchMcpTool {
public:
    static void Initialize();

private:
    static ToolResult HandleSearch(const PropertyList& properties);
};

#endif  // WEB_SEARCH_MCP_TOOL_H
