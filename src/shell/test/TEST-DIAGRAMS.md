# Test Case Diagrams

Visual diagrams explaining test cases and script behaviors using Mermaid.

## Table of Contents

- [Test Suite Overview](#test-suite-overview)
- [Script Workflows](#script-workflows)
- [Test Scenarios](#test-scenarios)
- [Tools for Viewing](#tools-for-viewing)

## Test Suite Overview

### Test Execution Flow

```mermaid
flowchart TD
    Start([Start Testing]) --> QuickTest{Quick Test?}
    QuickTest -->|Yes| CheckFiles[Check Script Files]
    QuickTest -->|No| FullTest[Full Test Suite]
    
    CheckFiles --> CheckHelp[Check Help Commands]
    CheckHelp --> CheckLib[Check git-helpers.sh]
    CheckLib --> QuickResult{All Pass?}
    QuickResult -->|Yes| QuickSuccess[✓ Quick Test Success]
    QuickResult -->|No| QuickFail[✗ Quick Test Failed]
    
    FullTest --> SetupEnv[Setup Test Environment]
    SetupEnv --> CloneRepo[Clone Test Repository]
    CloneRepo --> RunTests[Run Test Cases]
    
    RunTests --> TestRepo[Test Repository Scripts]
    RunTests --> TestWorkspace[Test Workspace Scripts]
    RunTests --> TestBranch[Test Branch Scripts]
    RunTests --> TestCommit[Test Commit Scripts]
    
    TestRepo --> Summary[Generate Summary]
    TestWorkspace --> Summary
    TestBranch --> Summary
    TestCommit --> Summary
    
    Summary --> Cleanup{Cleanup?}
    Cleanup -->|Yes| RemoveTemp[Remove Test Directory]
    Cleanup -->|No| KeepTemp[Keep Test Directory]
    
    RemoveTemp --> FullResult{All Pass?}
    KeepTemp --> FullResult
    FullResult -->|Yes| FullSuccess[✓ Full Test Success]
    FullResult -->|No| FullFail[✗ Full Test Failed]
    
    QuickSuccess --> End([End])
    QuickFail --> End
    FullSuccess --> End
    FullFail --> End
    
    style QuickSuccess fill:#90EE90
    style FullSuccess fill:#90EE90
    style QuickFail fill:#FFB6C1
    style FullFail fill:#FFB6C1
```

### Test Coverage Map

```mermaid
mindmap
  root((Git Master Skill Tests))
    Repository Management
      update-repo.sh
        Dry-run mode
        Actual update
        Submodule handling
      clone-with-upstream.sh
        Basic clone
        With upstream
        Custom directory
      discover-repos.sh
        List format
        JSON format
        Manifest save
    Workspace Operations
      status-all-repos.sh
        Table format
        JSON format
        Markdown format
        Remote checking
      foreach-repo.sh
        Command execution
        Continue on error
      update-workspace-repos.sh
        Dry-run
        Batch update
        Manifest usage
    Branch Operations
      compare-branches.sh
        Table output
        JSON output
        Markdown output
        Bidirectional
      cherry-pick-batch.sh
        JSON format
        Text format
        Dry-run
        Conflict handling
      rebase-to-upstream-latest.sh
        Dry-run mode
        Stash handling
    Commit Tools
      smart-commit.sh
        Help output
        AI features
```

## Script Workflows

### 1. update-repo.sh Workflow

```mermaid
sequenceDiagram
    participant User
    participant Script as update-repo.sh
    participant Git
    participant Repo as Repository
    
    User->>Script: Execute with path
    Script->>Repo: Check if git repo
    
    alt Has uncommitted changes
        Script->>Git: Create stash
        Git-->>Script: Stash created
    end
    
    Script->>Git: Fetch from remote
    Git-->>Script: Fetch complete
    
    Script->>Git: Get current branch
    Git-->>Script: Branch name
    
    Script->>Git: Rebase onto remote
    
    alt Rebase success
        Git-->>Script: Rebase complete
        Script->>Git: Update submodules
        Git-->>Script: Submodules updated
        
        alt Had stash
            Script->>Git: Pop stash
            Git-->>Script: Stash restored
        end
        
        Script-->>User: ✓ Update complete
    else Rebase failed
        Git-->>Script: Conflicts detected
        Script-->>User: ✗ Rebase failed (stash preserved)
    end
```

### 2. compare-branches.sh Workflow

```mermaid
flowchart TD
    Start([Start]) --> ParseArgs[Parse Arguments]
    ParseArgs --> ValidateRepo{Valid Repo?}
    ValidateRepo -->|No| Error1[Error: Not a git repo]
    ValidateRepo -->|Yes| ValidateBranches{Branches Exist?}
    
    ValidateBranches -->|No| Error2[Error: Branch not found]
    ValidateBranches -->|Yes| CountCommits[Count Commits]
    
    CountCommits --> GetAhead[Get commits ahead]
    CountCommits --> GetBehind{Bidirectional?}
    
    GetBehind -->|Yes| CountBehind[Get commits behind]
    GetBehind -->|No| SkipBehind[Skip behind count]
    
    CountBehind --> GetStats[Get file statistics]
    SkipBehind --> GetStats
    GetAhead --> GetStats
    
    GetStats --> FormatOutput{Output Format?}
    
    FormatOutput -->|Table| FormatTable[Format as Table]
    FormatOutput -->|JSON| FormatJSON[Format as JSON]
    FormatOutput -->|Markdown| FormatMD[Format as Markdown]
    
    FormatTable --> OutputDest{Output to File?}
    FormatJSON --> OutputDest
    FormatMD --> OutputDest
    
    OutputDest -->|Yes| WriteFile[Write to File]
    OutputDest -->|No| WriteStdout[Write to Stdout]
    
    WriteFile --> Success[✓ Success]
    WriteStdout --> Success
    
    Error1 --> End([End])
    Error2 --> End
    Success --> End
    
    style Success fill:#90EE90
    style Error1 fill:#FFB6C1
    style Error2 fill:#FFB6C1
```

### 3. cherry-pick-batch.sh Workflow

```mermaid
stateDiagram-v2
    [*] --> ParseInput: Start
    ParseInput --> DetectFormat: Read input file
    
    DetectFormat --> ParseJSON: JSON format
    DetectFormat --> ParseText: Text format
    
    ParseJSON --> ValidateCommits
    ParseText --> ValidateCommits
    
    ValidateCommits --> CheckDryRun: All commits valid?
    
    state CheckDryRun <<choice>>
    CheckDryRun --> ShowPreview: Dry-run mode
    CheckDryRun --> CherryPickLoop: Execute mode
    
    ShowPreview --> [*]: Exit
    
    CherryPickLoop --> CherryPickCommit: For each commit
    CherryPickCommit --> CheckConflict: Apply commit
    
    state CheckConflict <<choice>>
    CheckConflict --> NextCommit: Success
    CheckConflict --> HandleConflict: Conflict detected
    
    HandleConflict --> WaitUser: Show instructions
    WaitUser --> UserAction: User resolves
    
    state UserAction <<choice>>
    UserAction --> ContinuePick: --continue
    UserAction --> SkipCommit: --skip
    UserAction --> AbortPick: --abort
    
    ContinuePick --> NextCommit
    SkipCommit --> NextCommit
    AbortPick --> [*]: Aborted
    
    NextCommit --> MoreCommits: Check remaining
    
    state MoreCommits <<choice>>
    MoreCommits --> CherryPickLoop: More commits
    MoreCommits --> Summary: All done
    
    Summary --> [*]: Complete
```

### 4. discover-repos.sh Workflow

```mermaid
graph TB
    Start([Start Discovery]) --> SetRoot[Set Root Directory]
    SetRoot --> SetDepth[Set Max Depth]
    SetDepth --> InitList[Initialize Repo List]
    
    InitList --> CheckRoot{Root is Git Repo?}
    CheckRoot -->|Yes| AddRoot[Add Root Repo]
    CheckRoot -->|No| SkipRoot[Skip Root]
    
    AddRoot --> FindSubmodules[Find Submodules]
    SkipRoot --> FindSubmodules
    
    FindSubmodules --> ParseGitmodules[Parse .gitmodules]
    ParseGitmodules --> AddSubmodules[Add Submodules to List]
    
    AddSubmodules --> FindStandalone[Find Standalone Repos]
    FindStandalone --> SearchDirs[Search Directories]
    
    SearchDirs --> CheckDepth{Within Max Depth?}
    CheckDepth -->|No| SkipDir[Skip Directory]
    CheckDepth -->|Yes| CheckExclude{Excluded?}
    
    CheckExclude -->|Yes| SkipDir
    CheckExclude -->|No| CheckGit{Has .git?}
    
    CheckGit -->|Yes| AddStandalone[Add to List]
    CheckGit -->|No| ContinueSearch[Continue Search]
    
    AddStandalone --> MoreDirs{More Directories?}
    ContinueSearch --> MoreDirs
    SkipDir --> MoreDirs
    
    MoreDirs -->|Yes| SearchDirs
    MoreDirs -->|No| CollectMetadata[Collect Metadata]
    
    CollectMetadata --> FormatOutput{Output Format?}
    
    FormatOutput -->|List| OutputList[Output List]
    FormatOutput -->|JSON| OutputJSON[Output JSON]
    
    OutputList --> SaveManifest{Save Manifest?}
    OutputJSON --> SaveManifest
    
    SaveManifest -->|Yes| WriteFile[Write to File]
    SaveManifest -->|No| Done[Done]
    
    WriteFile --> Done
    Done --> End([End])
    
    style Done fill:#90EE90
```

## Test Scenarios

### Test Scenario 1: Update Repository with Stash

```mermaid
sequenceDiagram
    autonumber
    participant Test as Test Script
    participant Repo as Test Repository
    participant Script as update-repo.sh
    participant Git
    
    Test->>Repo: Clone test repository
    Test->>Repo: Make local changes
    Test->>Repo: Don't commit changes
    
    Note over Test,Repo: Repository has uncommitted changes
    
    Test->>Script: Execute update-repo.sh
    Script->>Git: Check for changes
    Git-->>Script: Has uncommitted changes
    
    Script->>Git: Create stash
    Note over Script,Git: Stash: auto-stash-update-repo
    Git-->>Script: Stash created (stash@{0})
    
    Script->>Git: Fetch origin
    Git-->>Script: Fetch complete
    
    Script->>Git: Rebase current branch
    Git-->>Script: Rebase success
    
    Script->>Git: Update submodules
    Git-->>Script: Submodules updated
    
    Script->>Git: Pop stash@{0}
    Git-->>Script: Stash restored
    
    Script-->>Test: ✓ Update complete
    
    Test->>Repo: Verify changes preserved
    Test->>Repo: Verify repository updated
    Test-->>Test: ✓ Test passed
```

### Test Scenario 2: Compare Branches

```mermaid
sequenceDiagram
    autonumber
    participant Test as Test Script
    participant Repo as Test Repository
    participant Script as compare-branches.sh
    
    Test->>Repo: Clone repository
    Test->>Repo: Checkout main branch
    Test->>Repo: Create test-branch
    Test->>Repo: Add commit to test-branch
    
    Note over Test,Repo: Setup: main (base) vs test-branch (1 commit ahead)
    
    Test->>Script: compare-branches.sh main test-branch
    Script->>Repo: Count commits (main..test-branch)
    Repo-->>Script: 1 commit ahead
    
    Script->>Repo: Get commit details
    Repo-->>Script: Hash, author, date, subject
    
    Script->>Repo: Get file statistics
    Repo-->>Script: 1 file changed, 1 insertion
    
    Script->>Script: Format as table
    Script-->>Test: Display comparison
    
    Note over Script,Test: Output shows 1 commit in test-branch
    
    Test->>Script: compare-branches.sh --format json
    Script-->>Test: JSON output
    
    Test->>Script: compare-branches.sh --format markdown
    Script-->>Test: Markdown output
    
    Test->>Script: compare-branches.sh --bidirectional
    Script->>Repo: Count commits (test-branch..main)
    Repo-->>Script: 0 commits behind
    Script-->>Test: Bidirectional comparison
    
    Test-->>Test: ✓ All formats tested
```

### Test Scenario 3: Cherry-Pick Batch

```mermaid
flowchart TD
    Start([Test Start]) --> Setup[Setup Test Repository]
    Setup --> CreateCommits[Create Test Commits]
    
    CreateCommits --> Commit1[Create Commit 1]
    Commit1 --> SaveHash1[Save Hash 1]
    SaveHash1 --> Commit2[Create Commit 2]
    Commit2 --> SaveHash2[Save Hash 2]
    
    SaveHash2 --> CreateBranch[Create Target Branch]
    CreateBranch --> CheckoutTarget[Checkout to HEAD~2]
    
    Note1[Target branch is 2 commits behind]
    CheckoutTarget -.-> Note1
    
    CheckoutTarget --> CreateJSON[Create commits.json]
    CreateJSON --> AddHash1[Add Hash 1 to JSON]
    AddHash1 --> AddHash2[Add Hash 2 to JSON]
    
    AddHash2 --> TestDryRun[Test: Dry-run Mode]
    TestDryRun --> ValidateHashes{Hashes Valid?}
    
    ValidateHashes -->|Yes| ShowPreview[Show Preview]
    ValidateHashes -->|No| ErrorInvalid[Error: Invalid Hash]
    
    ShowPreview --> TestActual[Test: Actual Cherry-Pick]
    TestActual --> ApplyCommit1[Apply Commit 1]
    
    ApplyCommit1 --> Check1{Success?}
    Check1 -->|Yes| ApplyCommit2[Apply Commit 2]
    Check1 -->|No| ErrorConflict[Error: Conflict]
    
    ApplyCommit2 --> Check2{Success?}
    Check2 -->|Yes| VerifyFiles[Verify Files Exist]
    Check2 -->|No| ErrorConflict
    
    VerifyFiles --> CheckFile1{file1.txt exists?}
    CheckFile1 -->|Yes| CheckFile2{file2.txt exists?}
    CheckFile1 -->|No| ErrorMissing[Error: File Missing]
    
    CheckFile2 -->|Yes| TestText[Test: Text Format]
    CheckFile2 -->|No| ErrorMissing
    
    TestText --> CreateTxt[Create commits.txt]
    CreateTxt --> ApplyText[Apply from Text]
    ApplyText --> VerifyText{Success?}
    
    VerifyText -->|Yes| Success[✓ All Tests Passed]
    VerifyText -->|No| ErrorText[Error: Text Format Failed]
    
    ErrorInvalid --> End([Test End])
    ErrorConflict --> End
    ErrorMissing --> End
    ErrorText --> End
    Success --> End
    
    style Success fill:#90EE90
    style ErrorInvalid fill:#FFB6C1
    style ErrorConflict fill:#FFB6C1
    style ErrorMissing fill:#FFB6C1
    style ErrorText fill:#FFB6C1
```

### Test Scenario 4: Workspace Discovery and Update

```mermaid
graph TD
    Start([Test Start]) --> CreateWorkspace[Create Test Workspace]
    CreateWorkspace --> CloneRepo1[Clone Repo 1]
    CloneRepo1 --> CloneRepo2[Clone Repo 2]
    CloneRepo2 --> CloneRepo3[Clone Repo 3]
    
    CloneRepo3 --> TestDiscover[Test: discover-repos.sh]
    TestDiscover --> Discover[Run Discovery]
    Discover --> CheckCount{Found 3 Repos?}
    
    CheckCount -->|Yes| TestJSON[Test: JSON Format]
    CheckCount -->|No| ErrorCount[Error: Wrong Count]
    
    TestJSON --> OutputJSON[Output as JSON]
    OutputJSON --> ValidateJSON{Valid JSON?}
    
    ValidateJSON -->|Yes| TestManifest[Test: Save Manifest]
    ValidateJSON -->|No| ErrorJSON[Error: Invalid JSON]
    
    TestManifest --> SaveFile[Save to manifest.json]
    SaveFile --> CheckFile{File Created?}
    
    CheckFile -->|Yes| TestStatus[Test: status-all-repos.sh]
    CheckFile -->|No| ErrorFile[Error: File Not Created]
    
    TestStatus --> StatusTable[Table Format]
    StatusTable --> StatusJSON[JSON Format]
    StatusJSON --> StatusMD[Markdown Format]
    
    StatusMD --> TestUpdate[Test: update-workspace-repos.sh]
    TestUpdate --> UpdateDryRun[Dry-run Mode]
    UpdateDryRun --> UpdateActual[Actual Update]
    
    UpdateActual --> CheckUpdates{All Updated?}
    CheckUpdates -->|Yes| TestForeach[Test: foreach-repo.sh]
    CheckUpdates -->|No| ErrorUpdate[Error: Update Failed]
    
    TestForeach --> RunCommand[Run git status]
    RunCommand --> CheckOutput{Output for All?}
    
    CheckOutput -->|Yes| Success[✓ All Tests Passed]
    CheckOutput -->|No| ErrorForeach[Error: Command Failed]
    
    ErrorCount --> End([Test End])
    ErrorJSON --> End
    ErrorFile --> End
    ErrorUpdate --> End
    ErrorForeach --> End
    Success --> End
    
    style Success fill:#90EE90
    style ErrorCount fill:#FFB6C1
    style ErrorJSON fill:#FFB6C1
    style ErrorFile fill:#FFB6C1
    style ErrorUpdate fill:#FFB6C1
    style ErrorForeach fill:#FFB6C1
```

## State Diagrams

### Repository State During Update

```mermaid
stateDiagram-v2
    [*] --> Clean: Initial State
    Clean --> Dirty: Make Changes
    Dirty --> Stashed: Create Stash
    Stashed --> Fetching: Fetch Remote
    Fetching --> Rebasing: Rebase Branch
    
    state Rebasing {
        [*] --> Applying: Apply Commits
        Applying --> Conflict: Conflicts Detected
        Applying --> Success: No Conflicts
        Conflict --> Resolved: User Resolves
        Resolved --> Applying
        Success --> [*]
    }
    
    Rebasing --> Updating: Rebase Complete
    Updating --> Restoring: Update Submodules
    Restoring --> Clean: Pop Stash
    
    Rebasing --> Failed: Rebase Failed
    Failed --> Stashed: Preserve Stash
    
    Clean --> [*]: Update Complete
    Stashed --> [*]: Manual Recovery Needed
```

### Test Execution States

```mermaid
stateDiagram-v2
    [*] --> Initializing: Start Test
    Initializing --> SetupEnv: Create Temp Dir
    SetupEnv --> CloneRepo: Clone Test Repo
    
    CloneRepo --> Running: Begin Tests
    
    state Running {
        [*] --> TestRepo: Repository Scripts
        TestRepo --> TestWorkspace: Workspace Scripts
        TestWorkspace --> TestBranch: Branch Scripts
        TestBranch --> TestCommit: Commit Scripts
        TestCommit --> [*]
    }
    
    Running --> Passed: All Tests Pass
    Running --> Failed: Some Tests Fail
    
    Passed --> Cleanup: Generate Summary
    Failed --> Cleanup: Generate Summary
    
    state Cleanup <<choice>>
    Cleanup --> RemoveTemp: --cleanup flag
    Cleanup --> KeepTemp: No cleanup
    
    RemoveTemp --> [*]: Exit 0
    KeepTemp --> [*]: Exit 0/1
```

## Tools for Viewing Diagrams

### Online Tools

1. **Mermaid Live Editor** (Recommended)
   - URL: https://mermaid.live/
   - Features: Real-time preview, export to PNG/SVG
   - Usage: Copy diagram code and paste

2. **GitHub/GitLab**
   - Both support Mermaid in markdown files
   - Renders automatically in README.md

3. **VS Code Extensions**
   - **Markdown Preview Mermaid Support**
   - **Mermaid Markdown Syntax Highlighting**
   - Install and preview in VS Code

### Command Line Tools

1. **Mermaid CLI**
   ```bash
   # Install
   npm install -g @mermaid-js/mermaid-cli
   
   # Generate PNG
   mmdc -i diagram.mmd -o diagram.png
   
   # Generate SVG
   mmdc -i diagram.mmd -o diagram.svg
   ```

2. **Markdown to HTML**
   ```bash
   # Install
   npm install -g markdown-it markdown-it-mermaid
   
   # Convert
   markdown-it TEST-DIAGRAMS.md > output.html
   ```

### Desktop Applications

1. **Typora** (Paid)
   - Native Mermaid support
   - WYSIWYG markdown editor
   - Export to PDF/HTML

2. **Obsidian** (Free)
   - Mermaid plugin available
   - Knowledge base tool
   - Local-first

3. **Draw.io / diagrams.net** (Free)
   - Can import Mermaid
   - Export to many formats
   - Online and desktop versions

### IDE Integration

1. **VS Code**
   ```bash
   # Install extensions
   code --install-extension bierner.markdown-mermaid
   code --install-extension bpruitt-goddard.mermaid-markdown-syntax-highlighting
   ```

2. **IntelliJ IDEA / WebStorm**
   - Built-in Mermaid support in markdown preview
   - No plugin needed

3. **Vim/Neovim**
   - Use `markdown-preview.nvim` plugin
   - Supports Mermaid rendering

## Exporting Diagrams

### To PNG/SVG

```bash
# Using mermaid-cli
mmdc -i TEST-DIAGRAMS.md -o diagrams/

# Using puppeteer
node export-diagrams.js
```

### To PDF

```bash
# Using pandoc
pandoc TEST-DIAGRAMS.md -o TEST-DIAGRAMS.pdf

# Using markdown-pdf
markdown-pdf TEST-DIAGRAMS.md
```

### To HTML

```bash
# Using markdown-it
markdown-it TEST-DIAGRAMS.md > TEST-DIAGRAMS.html

# Using pandoc
pandoc TEST-DIAGRAMS.md -s -o TEST-DIAGRAMS.html
```

## Usage Examples

### View in Browser

```bash
# Open in Mermaid Live Editor
# 1. Copy diagram code
# 2. Go to https://mermaid.live/
# 3. Paste and view

# Or use local HTML
markdown-it TEST-DIAGRAMS.md > output.html
open output.html  # macOS
start output.html # Windows
xdg-open output.html # Linux
```

### Generate Images

```bash
# Install mermaid-cli
npm install -g @mermaid-js/mermaid-cli

# Extract and convert each diagram
# (Manual process or use script)
mmdc -i diagram1.mmd -o diagram1.png
mmdc -i diagram2.mmd -o diagram2.svg
```

### Embed in Documentation

```markdown
# In your markdown file

## Test Flow

```mermaid
flowchart TD
    Start --> End
```

This will render automatically on GitHub/GitLab.
```

## Tips

1. **Use Mermaid Live Editor** for quick viewing and editing
2. **GitHub/GitLab** automatically render Mermaid in markdown
3. **VS Code** with extensions provides best local experience
4. **Export to PNG/SVG** for presentations or documentation
5. **Keep diagrams simple** - complex diagrams are hard to read

## References

- [Mermaid Documentation](https://mermaid.js.org/)
- [Mermaid Live Editor](https://mermaid.live/)
- [GitHub Mermaid Support](https://github.blog/2022-02-14-include-diagrams-markdown-files-mermaid/)
- [Mermaid CLI](https://github.com/mermaid-js/mermaid-cli)
