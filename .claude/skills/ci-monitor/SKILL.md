---
name: ci-monitor
description: Automate git commit, push, GitHub Actions build monitoring, and auto-fix common build errors. Use /ci-monitor to commit current changes and monitor the build, or /ci-monitor "message" to specify commit message. This skill launches a subagent to save context.
allowed-tools: Read, Grep, Glob, Bash, Task, Edit, Write
---

# CI-Monitor Skill

This skill automates the complete build workflow for Monado fork with LeiaSR SDK: commit → push → monitor → diagnose/fix errors.

## Architecture

```
Local: commit → push
GitHub Actions: build-windows.yml → CMake/Ninja → Upload artifact → Slack notification
                      ↓ (on failure)
Local: analyze logs → apply fix → commit fix → push → re-monitor (up to 3 attempts)
```

**Build Configuration:**
- **Platform:** Windows (windows-2022 runner)
- **Build System:** CMake + Ninja Multi-Config
- **Dependencies:** vcpkg, Vulkan SDK, LeiaSR SDK
- **Artifact:** SRMonado package in `_package/`

## CRITICAL: Launch Subagent to Save Context

**You MUST use the Task tool with `subagent_type="general-purpose"` to execute this workflow.**

The subagent handles all heavy work (git, build monitoring, log parsing, fixes) in its own context.

### How to Invoke

When this skill is triggered, immediately call:

```
Task(
  subagent_type="general-purpose",
  description="Build monitor workflow",
  prompt="[Full workflow prompt below, with USER_MESSAGE replaced]"
)
```

### Gathering Files to Commit

Before launching the subagent, you MUST determine which files to include in the commit:

1. **Review your conversation history** for every file you modified via Edit or Write tools during this session. Collect these paths into a list.
2. **Cross-reference with `git status --short`** to confirm each file is actually dirty (modified/untracked). Drop any that are clean.
3. **Build the `[FILES_TO_COMMIT]` value:**
   - If you found session-modified files: use a newline-separated list of paths (e.g., `src/xrt/compositor/main/comp_renderer.c\nsrc/xrt/drivers/leiasr/leiasr.cpp`)
   - If you have no tracked session files (e.g., user invoked `/ci-monitor` directly without prior edits): use the literal string `AUTO`
4. **Substitute `[FILES_TO_COMMIT]`** in the subagent prompt template below.

---

## Subagent Prompt Template

Pass this complete prompt to the subagent (replace `[USER_MESSAGE]` with the user's commit message or "auto-generate", and `[FILES_TO_COMMIT]` with the file list or "AUTO"):

```
Execute the LeiaSR-OpenXR ci-monitor workflow. You have access to Edit and Write tools to fix build errors.

## Configuration
- Repository: LeiaSR-OpenXR (Monado fork with Leia SDK)
- Workflow: build-windows.yml
- Artifact Name: SRMonado
- Build: CMake + Ninja Multi-Config (Release)
- Max Fix Attempts: 3

## Commit Message
[USER_MESSAGE]

## Files to Commit
[FILES_TO_COMMIT]

If the above is a list of file paths, stage ONLY those files in Phase 1.
If the above is "AUTO" or empty/missing, snapshot the dirty files at invocation time (see Phase 1 instructions).
NEVER use `git add -A` or `git add .` under any circumstances.

---

## PHASE 1: COMMIT AND PUSH

### Step 1.1: Pre-flight Check
Run: `git status`
- If no changes to commit, report "Nothing to commit" and STOP.
- Otherwise, continue to Step 1.2.

### Step 1.2: Stage Changes (Selective)

**If the "Files to Commit" section above contains a file list (not "AUTO"):**
- Stage ONLY those specific files:
  ```bash
  git add path/to/file1 path/to/file2 ...
  ```

**If "Files to Commit" is "AUTO" or empty/missing (fallback):**
- Snapshot the currently dirty files RIGHT NOW to prevent drift during build monitoring:
  ```bash
  git status --short | awk '{print $NF}'
  ```
- Store this list, then stage ONLY those files:
  ```bash
  git add <each file from snapshot>
  ```

**NEVER use `git add -A` or `git add .`** — this prevents unrelated dirty files from being swept into the commit.

### Step 1.3: Generate Commit Message (if needed)
If commit message is "auto-generate":
- Run: `git diff --cached --stat`
- Examine the changes and create a descriptive message summarizing what changed

### Step 1.4: Create Commit
Run:
```bash
git commit -m "$(cat <<'EOF'
[YOUR COMMIT MESSAGE HERE]

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```
**CRITICAL:** Extract and store the commit hash from the output (e.g., `[branch abc1234]`).
Run: `git rev-parse HEAD`
Store this as COMMIT_SHA - you will need it to verify the correct build is monitored.

### Step 1.5: Push to Remote
Run: `git push origin HEAD`
- Note the branch name from output
- If push fails, report the error and STOP

---

## PHASE 2: MONITOR BUILD

### Step 2.1: Wait for Workflow to Register
Run: `sleep 10`
(GitHub Actions needs time to register the new push)

### Step 2.2: Get Run ID for OUR Commit (CRITICAL)
**You MUST verify the run is for the exact commit you pushed, not a previous run.**

Run this command to find runs and check their commit SHA:
```bash
gh run list --limit 5 --json databaseId,status,headBranch,headSha,displayTitle
```

**Verification loop:**
1. Look for a run where `headSha` starts with your COMMIT_SHA (first 7+ chars match)
2. If no matching run found, wait 10 seconds and retry (up to 6 retries = 1 minute)
3. If still no matching run after retries, report error and STOP

Example verification:
- Your COMMIT_SHA: `f2286b75b...`
- Run headSha must start with: `f2286b75b`

Once you find the matching run, store its `databaseId` as RUN_ID.

### Step 2.3: Watch Build
Run: `gh run watch RUN_ID --interval 15` (use timeout 600000ms = 10 min)

### Step 2.4: Check Result
Run: `gh run view RUN_ID --json status,conclusion`
- If conclusion is "success": Go to PHASE 4 (Report Success)
- If conclusion is "failure": Go to PHASE 3 (Diagnose and Fix)
- If status is still "in_progress": The watch command may have timed out, check again

---

## PHASE 3: DIAGNOSE AND FIX (Loop up to 3 times)

Track: fix_attempt = 1
Track: fix_files_modified = [] (append every file path you modify with Edit/Write during this phase)

### Step 3.1: Get Error Logs
Run: `gh run view RUN_ID --log-failed | tail -200`
Save the output for analysis.

### Step 3.2: Identify Error Type
Parse the logs and look for these patterns (in order of priority):

**Pattern A: Missing include file**
```
fatal error C1083: Cannot open include file: 'XYZ.h'
```
→ Go to Fix A

**Pattern B: Undeclared identifier**
```
error C2065: 'XYZ': undeclared identifier
```
→ Go to Fix B

**Pattern C: Not a member of struct/class**
```
error C2039: 'member_name': is not a member of 'StructName'
```
→ Go to Fix C

**Pattern D: Unresolved external symbol (linker)**
```
error LNK2019: unresolved external symbol "function_name"
```
→ Go to Fix D

**Pattern E: Redefinition error**
```
error C2084: function 'X' already has a body
error C2011: 'X': 'struct' type redefinition
error C2371: 'X': redefinition; different basic types
```
→ Go to Fix E

**Pattern F: Implicit function declaration (C)**
```
warning: implicit declaration of function 'XYZ'
```
→ Go to Fix F

**No match found**: Report error with logs and STOP (manual intervention needed)

---

### FIX A: Missing Include File

1. Extract the missing filename from error (e.g., 'xrt_session_target.h')
2. Search for the file:
   ```bash
   find src/xrt -name "FILENAME" 2>/dev/null
   ```
3. If file exists:
   - Check the include path in the failing file
   - Use Edit tool to fix the include path to match where the file actually is
4. If file doesn't exist:
   - Check if it should be generated or if it's a typo
   - Look for similar files that might be the intended include
5. Go to Step 3.5

### FIX B: Undeclared Identifier

1. Extract the identifier name from error
2. Search for where it's defined:
   ```bash
   grep -rn "IDENTIFIER" src/xrt/include/ --include="*.h"
   grep -rn "#define IDENTIFIER" src/xrt/ --include="*.h"
   grep -rn "enum.*IDENTIFIER" src/xrt/ --include="*.h"
   ```
3. If found in a header:
   - Use Edit tool to add the missing #include to the failing file
4. If it's a new identifier that should be defined:
   - Check surrounding code for context
   - Add the definition or declaration where appropriate
5. Go to Step 3.5

### FIX C: Not a Member of Struct

1. Extract struct/class name and member name from error
2. Find the struct definition:
   ```bash
   grep -rn "struct StructName" src/xrt/include/ --include="*.h" -A 50
   ```
3. Check what members actually exist
4. Use Edit tool to:
   - Fix the member name to the correct one, OR
   - Add the missing member to the struct (if this is a new field you added)
5. Go to Step 3.5

### FIX D: Unresolved External Symbol (Linker Error)

1. Extract the function name from error
2. Check if function is declared but not implemented:
   ```bash
   grep -rn "function_name" src/xrt/ --include="*.c" --include="*.cpp"
   ```
3. If implementation is missing:
   - Find where it should be implemented (look at similar functions)
   - Use Write or Edit tool to add the implementation
4. If it's a library linking issue:
   - Check CMakeLists.txt for missing target_link_libraries
   - Add the missing library
5. Go to Step 3.5

### FIX E: Redefinition Error

1. Find all definitions:
   ```bash
   grep -rn "SYMBOL_NAME" src/xrt/ --include="*.h" --include="*.c"
   ```
2. Identify the issue type:

   **For C2371 "different basic types":** This usually means a function is called before it's declared.
   In C, undeclared functions are assumed to return `int`, causing a conflict when the actual
   definition is found. Fix by adding a forward declaration before the first call:
   ```c
   // Forward declaration
   static void function_name(parameters);
   ```

   **For C2084/C2011 true duplicates:**
   - Remove the duplicate definition, OR
   - Add include guards if missing, OR
   - Use #pragma once

3. Go to Step 3.5

### FIX F: Implicit Function Declaration

1. Extract function name
2. Find where it's declared:
   ```bash
   grep -rn "function_name" src/xrt/include/ --include="*.h"
   ```
3. Use Edit tool to add the missing #include to the source file
4. Go to Step 3.5

---

### Step 3.5: Commit the Fix (Selective Staging)

Stage ONLY the files you modified during this fix attempt (from your `fix_files_modified` list):
```bash
git add path/to/fixed_file1 path/to/fixed_file2 ...
```
**NEVER use `git add -A` or `git add .`** — only stage files you directly edited with Edit/Write tools.

Then commit:
```bash
git commit -m "$(cat <<'EOF'
Fix: [brief description of what was fixed]

Auto-fix attempt fix_attempt/3

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

### Step 3.6: Push the Fix
Run: `git push origin HEAD`

### Step 3.7: Re-monitor
- Increment fix_attempt
- If fix_attempt > 3: Go to PHASE 5 (Report Fix Failure)
- Otherwise:
  1. Get the new COMMIT_SHA: `git rev-parse HEAD`
  2. Return to Step 2.1 (wait for new workflow)
  3. **CRITICAL:** Use the NEW commit SHA to verify the correct run in Step 2.2

---

## PHASE 4: REPORT SUCCESS

Run: `gh run view RUN_ID --json conclusion,databaseId,url,updatedAt`

Report:
```
Build completed successfully!
- Committed: '[message]' ([N] files changed)
- Pushed to: [branch]
- Build: SUCCEEDED (run #RUN_ID)
- URL: [workflow URL]
- Artifact: Available at Actions → SRMonado
```

If there were fix attempts, add:
```
- Auto-fixes applied: [N] (see commit history)
```

STOP.

---

## PHASE 5: REPORT FIX FAILURE

Report:
```
Build FAILED after [N] fix attempts

Original error: [first error encountered]
Fixes attempted:
1. [description of fix 1]
2. [description of fix 2]
3. [description of fix 3]

Current error: [remaining error from logs]
Build URL: [workflow URL]

Manual intervention required.
```

STOP.

---

## Key Files Reference

When looking for fixes, check these locations:

**Headers (struct definitions, declarations):**
- src/xrt/include/xrt/*.h - Core interfaces
- src/xrt/compositor/main/comp_*.h - Compositor headers

**Implementations:**
- src/xrt/compositor/main/*.c - Compositor code
- src/xrt/compositor/multi/*.c - Multi-client compositor
- src/xrt/state_trackers/oxr/*.c - OpenXR bindings
- src/xrt/drivers/leiasr/*.cpp - Leia SR driver

**Build configuration:**
- src/xrt/CMakeLists.txt - Main build file
- src/xrt/compositor/CMakeLists.txt - Compositor build
- src/xrt/drivers/CMakeLists.txt - Drivers build
```

---

## GitHub Actions Workflow Details

The workflow (`build-windows.yml`) does:

1. **Checkout** - Clones repo with submodules and LFS
2. **Download SR SDK** - Gets LeiaSR SDK from internal releases
3. **Setup vcpkg** - Installs dependencies via vcpkg
4. **Install Vulkan SDK** - Version 1.3.283.0
5. **CMake Generate** - Configures with Ninja Multi-Config
6. **Build** - `cmake --build build --config Release --target install`
7. **Upload Artifact** - Uploads `_package/` as SRMonado artifact
8. **Slack Notification** - Posts success/failure to Slack

### Triggers
- Push to `main` branch
- Push to branches ending in `with-ci-build`
- All pull requests
- Tags starting with `v`

---

## Usage Examples

### With commit message:
```
/ci-monitor "Implement XR_EXT_session_target extension"
```

### Without message (auto-generate):
```
/ci-monitor
```

### Just monitor current build (no commit):
```
/ci-monitor --watch-only
```
For watch-only mode, skip PHASE 1 and start at PHASE 2, using the latest run for the current branch.
