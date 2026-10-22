// The state of transcript.c's parser, shared with transcript-state.c, which saves and loads it.
// Private to those two files.
#pragma once
#include "hud.h"

#define VEC(T) struct { T *p; int32_t n, cap; }

typedef VEC(Tool) ToolVec;
typedef VEC(Agent) AgentVec;
typedef VEC(Todo) TodoVec;
typedef VEC(us) StrVec;

// Every field but r is saved; a field added here must be added to the layout in transcript-state.c
// as well.
typedef struct {
    omap tools, agents, skills, mcp, mcp_err, completions, usage, task_idx;
    omap agent_ids;  // agentIdToToolUseId: a subagent's id to the index of its entry in agents
    ToolVec toolv;
    AgentVec agentv;
    StrVec skillv, mcpv, errv;
    TodoVec todos;
    bool has_idless;
    double idless[4];
    double settled[4];
    unsigned objects;
    Transcript *r;
} Parser;
