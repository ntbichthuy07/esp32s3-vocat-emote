#ifndef FINANCE_MCP_TOOL_H
#define FINANCE_MCP_TOOL_H

#include "mcp_server.h"

// Registers the personal-finance MCP tools (record a transaction, read back a month's total) so
// the voice agent can act as a simple expense tracker backed by a Google Sheet (see
// finance_service.h / apps_script.gs). Works on any board with network access; call once during
// startup.
class FinanceMcpTool {
public:
    static void Initialize();

private:
    static ToolResult HandleAddTransaction(const PropertyList& properties);
    static ToolResult HandleUpdateTransaction(const PropertyList& properties);
    static ToolResult HandleGetSummary(const PropertyList& properties);
    static ToolResult HandleGetCategorySummary(const PropertyList& properties);
    static ToolResult HandleListTransactions(const PropertyList& properties);
    static ToolResult HandleDeleteTransaction(const PropertyList& properties);
    static ToolResult HandleSetBudget(const PropertyList& properties);
    static ToolResult HandleGetBudget(const PropertyList& properties);
    static ToolResult HandleComparePeriods(const PropertyList& properties);
    static ToolResult HandleAddSavingsGoal(const PropertyList& properties);
    static ToolResult HandleGetSavingsGoal(const PropertyList& properties);
    static ToolResult HandleListSavingsGoals(const PropertyList& properties);
    static ToolResult HandleDeleteSavingsGoal(const PropertyList& properties);
};

#endif  // FINANCE_MCP_TOOL_H
