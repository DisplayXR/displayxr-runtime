---
name: ask-gemini
description: |
  Ask Gemini to analyze code and produce a read-only report. Automatically gathers relevant context based on your request.
  Usage: /ask-gemini <your request>
  Examples:
    /ask-gemini review the latest commit and flag potential issues
    /ask-gemini explain the architecture of comp_renderer.c
    /ask-gemini analyze the error handling patterns in oxr_session.c
allowed-tools: Task
---

# Ask Gemini Skill

This skill sends a request to Gemini CLI with automatically gathered context and displays the resulting analysis report.

## CRITICAL: Launch Subagent to Save Context

**You MUST use the Task tool with `subagent_type="general-purpose"` to execute this workflow.**

The subagent handles all heavy work (context gathering, prompt construction, gemini execution) in its own context.

### How to Invoke

When this skill is triggered, immediately call:

```
Task(
  subagent_type="general-purpose",
  description="Ask Gemini analysis",
  prompt="[Full workflow prompt below, with USER_REQUEST replaced]"
)
```

---

## Subagent Prompt Template

Pass this complete prompt to the subagent (replace `[USER_REQUEST]` with the user's actual request):

```
Execute the ask-gemini workflow to analyze code and produce a report.

## User Request
[USER_REQUEST]

---

## PHASE 1: GATHER CONTEXT

Based on keywords in the user request, gather relevant context:

### Step 1.1: Detect Keywords and Gather Context

**If request contains "commit", "latest", "last", "recent":**
- Run: `git show HEAD --stat`
- Run: `git diff HEAD~1`

**If request contains "diff", "changes", "staged", "modified":**
- Run: `git diff` (for unstaged)
- Run: `git diff --staged` (if "staged" mentioned)

**If request contains "branch", "history", "log":**
- Run: `git log --oneline -10`
- Run: `git branch -v`

**If request mentions a specific file (contains `.c`, `.h`, `.cpp`, `.py`, or path with `/src/`, `/include/`):**
- Read the mentioned file using the Read tool
- If file is very large (>500 lines), read first 300 lines

**If no specific context detected:**
- Run: `git status --short`
- Run: `ls src/xrt/` to show project structure

Store all gathered context as CONTEXT_OUTPUT.

### Step 1.2: Truncate if Needed

If CONTEXT_OUTPUT exceeds 15000 characters, truncate it and add a note:
"[Context truncated to 15000 chars]"

---

## PHASE 2: CONSTRUCT AND EXECUTE GEMINI PROMPT

### Step 2.1: Build the Prompt

Construct a prompt that includes:
1. Project context (CNSDK-OpenXR / Monado with LeiaSR SDK)
2. READ-ONLY instruction
3. The user's request
4. The gathered context
5. Response format instructions

### Step 2.2: Execute Gemini

Run the following bash command (use the actual values, not placeholders):

```bash
gemini -y "$(cat <<'GEMINI_EOF'
You are analyzing code for the CNSDK-OpenXR project, an OpenXR runtime fork of Monado with LeiaSR SDK integration for eye-tracked 3D displays.

IMPORTANT: This is a READ-ONLY analysis. DO NOT suggest code modifications, patches, or diffs. Your task is to analyze and report only.

## User Request
[INSERT THE ACTUAL USER REQUEST HERE]

## Context
[INSERT THE GATHERED CONTEXT HERE]

## Response Format
Provide a detailed analysis addressing the user's request. Structure your response:

### Summary
Brief overview of your findings.

### Key Findings
Detailed analysis points, organized by relevance.

### Recommendations
If applicable, suggestions for the user to consider (describe conceptually, do not write code).
GEMINI_EOF
)"
```

**CRITICAL:** Replace the placeholder text with actual values before executing.

---

## PHASE 3: REPORT RESULTS

### Step 3.1: Check Execution

If Gemini CLI fails or is not installed:
- Report: "Gemini CLI not available. Install with: npm install -g @anthropic-ai/gemini-cli or check https://github.com/anthropics/gemini-cli"

### Step 3.2: Display Report

Show the complete output from Gemini to the user.

Format your final message as:
```
## Gemini Analysis Report

[Gemini's output here]

---
Context gathered: [brief description of what context was included]
```

STOP.

---

## Example Execution

For request "review the latest commit and flag potential issues":

1. **Detect**: "latest commit" → gather git show HEAD and git diff HEAD~1
2. **Context**: Collect the diff output
3. **Prompt**: Include diff in Gemini prompt with READ-ONLY instruction
4. **Execute**: Run `gemini -y "..."`
5. **Report**: Display Gemini's analysis

---

## Notes

- Always use `-y` flag with gemini to auto-confirm
- Use heredoc with quoted delimiter ('GEMINI_EOF') to prevent variable expansion
- Keep context focused - don't read more files than necessary
- If gemini command times out, use timeout of 120000ms (2 min)
```

---

## Usage Examples

### Review latest commit:
```
/ask-gemini review the latest commit and flag potential issues
```

### Analyze a specific file:
```
/ask-gemini explain the architecture of comp_renderer.c
```

### General code analysis:
```
/ask-gemini analyze the error handling patterns in oxr_session.c
```
