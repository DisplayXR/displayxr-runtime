---
name: ask-gemini
description: |
  Ask Gemini to analyze code and produce a read-only report. Automatically gathers relevant context based on your request.
  Usage: /ask-gemini <your request>
  Examples:
    /ask-gemini review the latest commit and flag potential issues
    /ask-gemini explain the architecture of comp_renderer.c
    /ask-gemini analyze the error handling patterns in oxr_session.c
allowed-tools: Read, Grep, Glob, Bash
---

# Ask Gemini Skill

This skill sends a request to Gemini CLI with automatically gathered context and displays the resulting analysis report.

## Instructions

### Step 1: Parse User Request

Extract the user's request from the skill invocation arguments. The request follows `/ask-gemini `.

### Step 2: Smart Context Gathering

Based on keywords detected in the user's request, gather relevant context:

**Commit-related keywords** ("commit", "latest commit", "last commit", "recent commit"):
- Run: `git show HEAD --stat`
- Run: `git diff HEAD~1`

**Diff/changes keywords** ("diff", "changes", "staged", "modified"):
- Run: `git diff` (for unstaged changes)
- Run: `git diff --staged` (if "staged" is mentioned)

**Branch/history keywords** ("branch", "history", "log", "commits"):
- Run: `git log --oneline -10`
- Run: `git branch -v`

**Specific file mentioned** (detect paths with extensions like `.c`, `.h`, `.cpp`, `.py` or paths containing `/src/`, `/include/`):
- Read the mentioned file(s) using the Read tool
- If file is large, read first 200 lines

**No specific context detected**:
- Run: `git status --short`
- Run: `ls -la` to show working directory structure

### Step 3: Construct and Execute Gemini Prompt

Construct a detailed prompt and execute it using the Gemini CLI with a heredoc to handle special characters:

```bash
gemini -y "$(cat <<'GEMINI_PROMPT_EOF'
You are analyzing code for the CNSDK-OpenXR project, an OpenXR runtime fork of Monado with LeiaSR SDK integration for eye-tracked 3D displays.

IMPORTANT: This is a READ-ONLY analysis. DO NOT suggest code modifications or produce patches. Your task is to analyze and report only.

## User Request
[INSERT_USER_REQUEST_HERE]

## Context
[INSERT_GATHERED_CONTEXT_HERE]

## Response Format
Please provide a detailed analysis addressing the user's request. Structure your response with:

### Summary
Brief overview of your findings.

### Key Findings
Detailed analysis points, organized by relevance.

### Recommendations
If applicable, suggestions for the user to consider (describe conceptually, do not write code).
GEMINI_PROMPT_EOF
)"
```

**Important:** Replace `[INSERT_USER_REQUEST_HERE]` with the actual user request and `[INSERT_GATHERED_CONTEXT_HERE]` with the gathered context from Step 2.

### Step 4: Display the Report

The output from the `gemini -y` command is the report. Display it to the user without modification.

If the Gemini CLI fails or is not installed, inform the user:
- Check that Gemini CLI is installed (`which gemini`)
- Suggest installing it if not available

## Example Execution Flow

For `/ask-gemini review the latest commit and flag potential issues`:

1. **Parse**: User wants to review the latest commit
2. **Context**: Detect "latest commit" → run `git show HEAD --stat` and `git diff HEAD~1`
3. **Construct prompt**: Include the diff output and user request
4. **Execute**: Run `gemini -y "..."` with the constructed prompt
5. **Display**: Show Gemini's analysis report to the user

## Notes

- Always use the `-y` flag with gemini to auto-confirm
- Use heredoc with `'EOF'` (quoted) to prevent variable expansion issues
- Context gathering should be quick - avoid reading too many files
- If context is very large (>10000 chars), truncate with a note
