#ifndef NEWS_MCP_TOOL_H
#define NEWS_MCP_TOOL_H

#include "mcp_server.h"

// Registers the VnExpress news MCP tool so the voice agent can fetch and
// read out the latest Vietnamese headlines. Works on any board with network
// access; call once during startup.
class NewsMcpTool {
public:
    static void Initialize();

private:
    static ToolResult HandleGetLatestNews(const PropertyList& properties);
};

#endif  // NEWS_MCP_TOOL_H
