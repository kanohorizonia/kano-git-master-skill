# Requirements Document

## Introduction

本文件定義 kano-git TUI 指令輸入增強功能的需求。目前的 TUI 實作僅支援單鍵快捷鍵操作，缺乏直觀的指令輸入介面和即時補完提示，導致使用者學習曲線陡峭且操作體驗不佳。本功能將引入類似 vim 命令模式的指令輸入系統，提供自動補完、即時提示和清晰的指令說明，大幅改善 TUI 的可用性。

## Glossary

- **TUI**: Text User Interface，基於 FTXUI 框架實作的文字使用者介面
- **Command_Input_System**: 指令輸入系統，負責接收、解析和執行使用者輸入的指令
- **Autocomplete_Engine**: 自動補完引擎，根據當前輸入提供候選指令和參數
- **Command_Registry**: 指令註冊系統，維護所有可用指令的元資料（定義於 command_registry.hpp/cpp）
- **Command_Palette**: 指令面板，顯示可用指令列表和說明的 UI 元件
- **Input_Buffer**: 輸入緩衝區，儲存使用者當前輸入的指令文字
- **Candidate_List**: 候選清單，顯示符合當前輸入的指令或參數建議
- **Command_Mode**: 指令模式，類似 vim 的命令模式，按下特定鍵（如 ':'）進入
- **Normal_Mode**: 正常模式，TUI 的預設操作模式，使用單鍵快捷鍵
- **Command_Metadata**: 指令元資料，包含指令名稱、描述、參數定義等資訊
- **FTXUI**: 用於建構 TUI 的 C++ 框架

## Requirements

### Requirement 1: Command Mode Entry

**User Story:** 作為 TUI 使用者，我希望能夠進入指令輸入模式，以便輸入完整的指令而非僅使用單鍵快捷鍵

#### Acceptance Criteria

1. WHEN the user presses ':' in Normal_Mode, THE TUI SHALL enter Command_Mode
2. WHEN Command_Mode is active, THE TUI SHALL display an Input_Buffer at the bottom of the screen
3. WHEN Command_Mode is active, THE TUI SHALL display a ':' prompt prefix before the Input_Buffer
4. WHEN the user presses Escape in Command_Mode, THE TUI SHALL return to Normal_Mode and clear the Input_Buffer
5. WHEN the user presses Enter in Command_Mode with non-empty Input_Buffer, THE TUI SHALL attempt to execute the command
6. WHEN the user presses Enter in Command_Mode with empty Input_Buffer, THE TUI SHALL return to Normal_Mode

### Requirement 2: Command Input and Editing

**User Story:** 作為 TUI 使用者，我希望能夠編輯輸入的指令文字，以便修正錯誤或調整指令

#### Acceptance Criteria

1. WHEN the user types a character in Command_Mode, THE TUI SHALL append the character to the Input_Buffer
2. WHEN the user presses Backspace in Command_Mode, THE TUI SHALL remove the last character from the Input_Buffer
3. WHEN the user presses Ctrl+U in Command_Mode, THE TUI SHALL clear the entire Input_Buffer
4. WHEN the user presses Left Arrow in Command_Mode, THE TUI SHALL move the cursor left within the Input_Buffer
5. WHEN the user presses Right Arrow in Command_Mode, THE TUI SHALL move the cursor right within the Input_Buffer
6. WHEN the user presses Home in Command_Mode, THE TUI SHALL move the cursor to the beginning of the Input_Buffer
7. WHEN the user presses End in Command_Mode, THE TUI SHALL move the cursor to the end of the Input_Buffer

### Requirement 3: Autocomplete Candidate Generation

**User Story:** 作為 TUI 使用者，我希望看到符合當前輸入的指令建議，以便快速找到需要的指令

#### Acceptance Criteria

1. WHEN Command_Mode is active and Input_Buffer is not empty, THE Autocomplete_Engine SHALL generate a Candidate_List based on the Input_Buffer content
2. WHEN the Input_Buffer contains only a partial command name, THE Autocomplete_Engine SHALL return matching command names from Command_Registry
3. WHEN the Input_Buffer contains a complete command name followed by space, THE Autocomplete_Engine SHALL return available subcommands or options for that command
4. WHEN the Input_Buffer contains a command with partial option name starting with '-', THE Autocomplete_Engine SHALL return matching option names
5. WHEN no candidates match the Input_Buffer, THE Autocomplete_Engine SHALL return an empty Candidate_List
6. THE Autocomplete_Engine SHALL sort candidates alphabetically for consistent presentation
7. THE Autocomplete_Engine SHALL limit the Candidate_List to a maximum of 10 items to avoid UI clutter

### Requirement 4: Autocomplete UI Display

**User Story:** 作為 TUI 使用者，我希望看到清晰的自動補完建議顯示，以便理解每個候選指令的用途

#### Acceptance Criteria

1. WHEN the Candidate_List is not empty, THE TUI SHALL display the Candidate_List above the Input_Buffer
2. WHEN displaying a candidate, THE TUI SHALL show both the command name and its description
3. WHEN the Candidate_List contains more than one item, THE TUI SHALL highlight the currently selected candidate
4. WHEN a candidate represents a command, THE TUI SHALL display its short description from Command_Metadata
5. WHEN a candidate represents an option, THE TUI SHALL display the option flag and its description
6. THE TUI SHALL display the Candidate_List in a bordered panel to visually separate it from other UI elements

### Requirement 5: Autocomplete Navigation and Selection

**User Story:** 作為 TUI 使用者，我希望能夠瀏覽和選擇自動補完建議，以便快速輸入指令

#### Acceptance Criteria

1. WHEN the Candidate_List is displayed, THE TUI SHALL select the first candidate by default
2. WHEN the user presses Tab in Command_Mode, THE TUI SHALL move selection to the next candidate in the Candidate_List
3. WHEN the user presses Shift+Tab in Command_Mode, THE TUI SHALL move selection to the previous candidate in the Candidate_List
4. WHEN the user presses Down Arrow in Command_Mode and Candidate_List is displayed, THE TUI SHALL move selection to the next candidate
5. WHEN the user presses Up Arrow in Command_Mode and Candidate_List is displayed, THE TUI SHALL move selection to the previous candidate
6. WHEN selection reaches the end of the Candidate_List and user navigates forward, THE TUI SHALL wrap to the first candidate
7. WHEN selection reaches the beginning of the Candidate_List and user navigates backward, THE TUI SHALL wrap to the last candidate
8. WHEN the user presses Tab with a single candidate in the Candidate_List, THE TUI SHALL auto-complete the Input_Buffer with the selected candidate
9. WHEN the user presses Enter with a highlighted candidate, THE TUI SHALL auto-complete the Input_Buffer with the selected candidate and add a trailing space

### Requirement 6: Command Metadata Integration

**User Story:** 作為 TUI 使用者，我希望看到準確的指令資訊，以便了解每個指令的功能和用法

#### Acceptance Criteria

1. THE Command_Input_System SHALL retrieve Command_Metadata from the existing Command_Registry
2. THE Command_Input_System SHALL NOT maintain a separate static command list to avoid metadata drift
3. WHEN a command is registered via RegisterAll, THE Command_Metadata SHALL be automatically available to the Autocomplete_Engine
4. THE Command_Metadata SHALL include command name, description, subcommands, and options
5. THE Command_Metadata SHALL include option flags (both long and short forms), descriptions, and value requirements
6. WHEN Command_Registry is updated with new commands, THE Autocomplete_Engine SHALL reflect the changes without code modification

### Requirement 7: Command Execution

**User Story:** 作為 TUI 使用者，我希望能夠執行輸入的指令，以便完成 git 操作

#### Acceptance Criteria

1. WHEN the user presses Enter in Command_Mode with a valid command in Input_Buffer, THE TUI SHALL execute the command
2. WHEN a command is executed successfully, THE TUI SHALL return to Normal_Mode and display a success message in the footer
3. WHEN a command execution fails, THE TUI SHALL display an error message in the footer and remain in Command_Mode
4. WHEN a command requires confirmation (e.g., push, commit), THE TUI SHALL display a confirmation dialog before execution
5. WHEN the user enters an unknown command, THE TUI SHALL display "Unknown command: <command>" in the footer
6. THE TUI SHALL support executing the following command categories: refresh, commit, push, fetch, cherry-pick, rebase, history, filter

### Requirement 8: Command Palette Display

**User Story:** 作為 TUI 使用者，我希望能夠瀏覽所有可用指令，以便發現和學習 TUI 功能

#### Acceptance Criteria

1. WHEN the user presses Ctrl+P in Normal_Mode, THE TUI SHALL display the Command_Palette
2. THE Command_Palette SHALL list all available commands with their descriptions
3. THE Command_Palette SHALL group commands by category (repository operations, history, navigation, system)
4. WHEN the user selects a command from the Command_Palette, THE TUI SHALL enter Command_Mode with the command name pre-filled in the Input_Buffer
5. WHEN the user presses Escape in the Command_Palette, THE TUI SHALL close the Command_Palette and return to Normal_Mode
6. THE Command_Palette SHALL support fuzzy search filtering by command name or description

### Requirement 9: Backward Compatibility

**User Story:** 作為現有 TUI 使用者，我希望原有的單鍵快捷鍵仍然可用，以便保持現有的工作流程

#### Acceptance Criteria

1. WHEN the user presses a single-key shortcut in Normal_Mode, THE TUI SHALL execute the corresponding action as before
2. THE TUI SHALL maintain support for existing shortcuts: r (refresh), d (dirty-only), f (fetch), c/C (commit), p/P (push), Enter (history), q (quit)
3. WHEN Command_Mode is active, THE TUI SHALL NOT trigger single-key shortcuts for regular character input
4. THE TUI SHALL display both single-key shortcuts and command input instructions in the footer help text

### Requirement 10: Help and Documentation

**User Story:** 作為 TUI 使用者，我希望能夠查看幫助資訊，以便了解如何使用指令輸入功能

#### Acceptance Criteria

1. WHEN the user types "help" in Command_Mode and presses Enter, THE TUI SHALL display a help panel
2. THE help panel SHALL list all available commands with their syntax and descriptions
3. THE help panel SHALL explain how to enter Command_Mode and use autocomplete
4. THE help panel SHALL list keyboard shortcuts for navigation and selection
5. WHEN the user presses '?' in Normal_Mode, THE TUI SHALL display the help panel
6. WHEN the user presses Escape or 'q' in the help panel, THE TUI SHALL close the help panel and return to Normal_Mode

### Requirement 11: Error Handling and User Feedback

**User Story:** 作為 TUI 使用者，我希望收到清晰的錯誤訊息和操作回饋，以便了解系統狀態和問題原因

#### Acceptance Criteria

1. WHEN a command fails to parse, THE TUI SHALL display "Invalid command syntax: <details>" in the footer
2. WHEN a command execution encounters an error, THE TUI SHALL display the error message from the underlying git operation
3. WHEN the Autocomplete_Engine fails to generate candidates, THE TUI SHALL log the error and continue without crashing
4. WHEN Command_Metadata retrieval fails, THE TUI SHALL display "Command metadata unavailable" and disable autocomplete
5. THE TUI SHALL clear error messages when the user starts typing a new command
6. THE TUI SHALL display operation progress for long-running commands (e.g., "Fetching...", "Pushing...")

### Requirement 12: Performance and Responsiveness

**User Story:** 作為 TUI 使用者，我希望指令輸入和自動補完反應迅速，以便流暢地操作

#### Acceptance Criteria

1. WHEN the user types a character in Command_Mode, THE TUI SHALL update the Input_Buffer display within 50ms
2. WHEN the Input_Buffer changes, THE Autocomplete_Engine SHALL generate the Candidate_List within 100ms
3. WHEN the Candidate_List is updated, THE TUI SHALL refresh the display within 50ms
4. THE Autocomplete_Engine SHALL use efficient string matching algorithms to minimize computation time
5. THE TUI SHALL NOT block user input while generating autocomplete candidates
6. WHEN Command_Metadata contains more than 100 commands, THE Autocomplete_Engine SHALL maintain sub-100ms response time for candidate generation
