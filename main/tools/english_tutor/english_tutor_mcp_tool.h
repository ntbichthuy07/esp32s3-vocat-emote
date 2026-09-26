#ifndef ENGLISH_TUTOR_MCP_TOOL_H
#define ENGLISH_TUTOR_MCP_TOOL_H

#include "mcp_server.h"

// Registers the self.tutor.* MCP tools (MVP): start a practice session, log a corrected
// mistake, change level, close a session, and check progress -- backed by a Google Sheet (see
// english_tutor_service.h / apps_script.gs). Works on any board with network access; call once
// during startup.
class EnglishTutorMcpTool {
public:
    static void Initialize();

private:
    static ToolResult HandleStartSession(const PropertyList& properties);
    static ToolResult HandleLogMistake(const PropertyList& properties);
    static ToolResult HandleSetLevel(const PropertyList& properties);
    static ToolResult HandleEndSession(const PropertyList& properties);
    static ToolResult HandleGetProgress(const PropertyList& properties);
};

#endif  // ENGLISH_TUTOR_MCP_TOOL_H
