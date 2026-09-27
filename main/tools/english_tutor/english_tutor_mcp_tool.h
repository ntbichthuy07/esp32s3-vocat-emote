#ifndef ENGLISH_TUTOR_MCP_TOOL_H
#define ENGLISH_TUTOR_MCP_TOOL_H

#include "mcp_server.h"

// Registers the self.tutor.* MCP tools: start a practice session (weakness-biased topic pick,
// due-review preview), log a mistake or vocabulary note, quiz/record spaced-repetition
// reviews, actively teach a curated vocabulary word, close a session, and check progress --
// backed by a Google Sheet (see english_tutor_service.h / apps_script.gs). Works on any board
// with network access; call once during startup.
class EnglishTutorMcpTool {
public:
    static void Initialize();

private:
    static ToolResult HandleStartSession(const PropertyList& properties);
    static ToolResult HandleLogNote(const PropertyList& properties);
    static ToolResult HandleGetDueReview(const PropertyList& properties);
    static ToolResult HandleRecordReview(const PropertyList& properties);
    static ToolResult HandleGetVocabWord(const PropertyList& properties);
    static ToolResult HandleEndSession(const PropertyList& properties);
    static ToolResult HandleGetProgress(const PropertyList& properties);
};

#endif  // ENGLISH_TUTOR_MCP_TOOL_H
