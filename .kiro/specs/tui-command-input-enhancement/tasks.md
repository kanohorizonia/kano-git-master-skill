# Implementation Plan: TUI Command Input Enhancement

## Overview

This plan implements a vim-style command input system with intelligent autocomplete for the kano-git TUI. The implementation follows a bottom-up approach, building core components first (metadata extraction, autocomplete engine), then integrating them into the TUI, and finally adding UI components and polish.

The design uses C++ with FTXUI for UI rendering and CLI11 for command metadata. All components will be tested with both unit tests and property-based tests using Catch2.

## Tasks

- [ ] 1. Set up testing infrastructure
  - Create test directory structure (unit/, property/, integration/)
  - Configure Catch2 for property-based testing with generators
  - Add test targets to CMake build system
  - Create test data generators (random_tui_state, random_command_string, etc.)
  - _Requirements: 12.1, 12.2, 12.3_

- [ ] 2. Implement MetadataCache component
  - [ ] 2.1 Create CommandMetadata and OptionMetadata data structures
    - Define structs in new header file `metadata_cache.hpp`
    - Include fields: name, description, subcommands, options, allow_extras
    - _Requirements: 6.4, 6.5_

  - [ ] 2.2 Implement metadata extraction from CLI11 app tree
    - Extract logic from existing `meta_cmd.cpp`
    - Implement `ExtractMetadata()`, `ExtractCommand()`, `ExtractOptions()`
    - Handle nested subcommands and option parsing
    - _Requirements: 6.1, 6.2_

  - [ ] 2.3 Implement query interface for metadata
    - Implement `GetAllCommands()`, `GetCommand()`, `GetSubcommands()`, `GetOptions()`
    - Add caching mechanism with `Refresh()` method
    - _Requirements: 6.3, 6.6_

  - [ ]* 2.4 Write unit tests for MetadataCache
    - Test metadata extraction from sample CLI11 app
    - Test query methods with various command structures
    - Test cache refresh behavior
    - _Requirements: 6.1, 6.2, 6.3_

  - [ ]* 2.5 Write property test for metadata synchronization
    - **Property 12: Metadata Synchronization**
    - **Validates: Requirements 6.6**
    - Verify commands registered in CLI11 are automatically available
    - _Requirements: 6.6_

- [ ] 3. Implement AutocompleteEngine component
  - [ ] 3.1 Create InputContext and CandidateItem data structures
    - Define structs for parsing input and representing candidates
    - Include CompletionPhase enum (Command, Subcommand, Option, OptionValue)
    - _Requirements: 3.1, 3.2, 3.3, 3.4_

  - [ ] 3.2 Implement input parsing logic
    - Implement `ParseInput()` to tokenize and determine completion phase
    - Handle edge cases: empty input, trailing spaces, partial tokens
    - _Requirements: 3.1, 3.2, 3.3_

  - [ ] 3.3 Implement command name completion
    - Implement `CompleteCommand()` with prefix matching
    - Use case-insensitive comparison
    - _Requirements: 3.2_

  - [ ] 3.4 Implement subcommand completion
    - Implement `CompleteSubcommand()` for commands with subcommands
    - Query metadata for available subcommands
    - _Requirements: 3.3_

  - [ ] 3.5 Implement option completion
    - Implement `CompleteOption()` for options starting with '-'
    - Support both long (--option) and short (-o) forms
    - _Requirements: 3.4_

  - [ ] 3.6 Implement candidate filtering and sorting
    - Implement `FilterAndSort()` with alphabetical sorting
    - Limit results to maximum 10 candidates
    - _Requirements: 3.6, 3.7_

  - [ ]* 3.7 Write unit tests for AutocompleteEngine
    - Test command completion with various prefixes
    - Test subcommand completion for multi-level commands
    - Test option completion with long and short forms
    - Test empty candidate list for non-matching input
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5_

  - [ ]* 3.8 Write property test for autocomplete candidate matching
    - **Property 4: Autocomplete Candidate Matching**
    - **Validates: Requirements 3.1, 3.2, 6.1, 6.3**
    - Verify all candidates match input prefix
    - _Requirements: 3.1, 3.2_

  - [ ]* 3.9 Write property test for context-aware completion
    - **Property 5: Context-Aware Completion**
    - **Validates: Requirements 3.3, 3.4, 6.4, 6.5**
    - Verify subcommands/options belong to specified command
    - _Requirements: 3.3, 3.4_

  - [ ]* 3.10 Write property test for candidate list constraints
    - **Property 6: Candidate List Constraints**
    - **Validates: Requirements 3.6, 3.7**
    - Verify alphabetical sorting and max 10 items
    - _Requirements: 3.6, 3.7_

  - [ ]* 3.11 Write property test for empty candidate list
    - **Property 7: Empty Candidate List for Non-Matching Input**
    - **Validates: Requirements 3.5**
    - Verify empty list for non-matching input
    - _Requirements: 3.5_

- [ ] 4. Checkpoint - Core components complete
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 5. Implement CommandModeState component
  - [ ] 5.1 Create InputBuffer data structure
    - Implement buffer with text and cursor position
    - Implement Insert(), Backspace(), Delete(), Clear(), MoveCursor()
    - Maintain invariant: 0 ≤ cursor_pos ≤ buffer.length()
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7_

  - [ ] 5.2 Implement CandidateSelection data structure
    - Implement circular navigation with SelectNext(), SelectPrev()
    - Implement GetSelected() with bounds checking
    - _Requirements: 5.2, 5.3, 5.4, 5.5, 5.6, 5.7_

  - [ ] 5.3 Implement CommandModeState class
    - Add fields: buffer, cursor_pos, candidates, selected_candidate, show_candidates
    - Implement input handling methods (OnCharacter, OnBackspace, OnDelete, OnClearBuffer)
    - Implement cursor navigation methods (OnCursorLeft, OnCursorRight, OnHome, OnEnd)
    - Implement candidate navigation methods (OnNextCandidate, OnPrevCandidate, OnAcceptCandidate)
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7_

  - [ ] 5.4 Implement UpdateCandidates() method
    - Trigger autocomplete engine on buffer changes
    - Update candidates list and reset selection
    - _Requirements: 3.1, 4.1_

  - [ ]* 5.5 Write unit tests for InputBuffer
    - Test character insertion at various cursor positions
    - Test backspace and delete at boundaries
    - Test cursor movement and bounds checking
    - _Requirements: 2.1, 2.2, 2.4, 2.5, 2.6, 2.7_

  - [ ]* 5.6 Write property test for input buffer editing invariants
    - **Property 2: Input Buffer Editing Preserves Invariants**
    - **Validates: Requirements 2.1, 2.2, 2.4, 2.5, 2.6, 2.7**
    - Verify cursor position always within valid range
    - _Requirements: 2.1, 2.2, 2.4, 2.5, 2.6, 2.7_

  - [ ]* 5.7 Write property test for buffer clear idempotence
    - **Property 3: Buffer Clear is Idempotent**
    - **Validates: Requirements 2.3**
    - Verify Ctrl+U clears buffer and repeated presses maintain empty state
    - _Requirements: 2.3_

  - [ ]* 5.8 Write unit tests for CandidateSelection
    - Test circular navigation forward and backward
    - Test wrapping at boundaries
    - Test empty candidate list handling
    - _Requirements: 5.2, 5.3, 5.4, 5.5, 5.6, 5.7_

  - [ ]* 5.9 Write property test for candidate selection wrapping
    - **Property 9: Candidate Selection Wrapping**
    - **Validates: Requirements 5.6, 5.7**
    - Verify circular navigation wraps correctly
    - _Requirements: 5.6, 5.7_

  - [ ]* 5.10 Write property test for candidate navigation consistency
    - **Property 10: Candidate Navigation Consistency**
    - **Validates: Requirements 5.2, 5.3, 5.4, 5.5**
    - Verify Tab/Down and Shift+Tab/Up produce equivalent results
    - _Requirements: 5.2, 5.3, 5.4, 5.5_

- [ ] 6. Implement CommandExecutor component
  - [ ] 6.1 Create ExecutionResult data structure
    - Define struct with success, message, needs_confirmation, confirmed_action
    - _Requirements: 7.1, 7.2, 7.3, 7.4_

  - [ ] 6.2 Implement command line parsing
    - Implement `ParseCommandLine()` to tokenize input
    - Handle quoted strings and escaped characters
    - _Requirements: 7.1, 11.1_

  - [ ] 6.3 Implement command execution logic
    - Implement `Execute()` method using CLI11 parsing
    - Find command in app tree and invoke parser
    - Handle unknown commands and parse errors
    - _Requirements: 7.1, 7.5, 11.1_

  - [ ] 6.4 Implement confirmation logic for destructive commands
    - Implement `NeedsConfirmation()` for push, commit --force, etc.
    - Implement `BuildConfirmationMessage()` for user prompts
    - _Requirements: 7.4_

  - [ ]* 6.5 Write unit tests for CommandExecutor
    - Test valid command execution
    - Test unknown command handling
    - Test parse error handling
    - Test confirmation logic for destructive commands
    - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5_

  - [ ]* 6.6 Write property test for command execution success flow
    - **Property 13: Command Execution Success Flow**
    - **Validates: Requirements 7.1, 7.2**
    - Verify successful execution returns to Normal_Mode
    - _Requirements: 7.1, 7.2_

  - [ ]* 6.7 Write property test for command execution failure flow
    - **Property 14: Command Execution Failure Flow**
    - **Validates: Requirements 7.3**
    - Verify failed execution stays in Command_Mode
    - _Requirements: 7.3_

  - [ ]* 6.8 Write property test for unknown command error format
    - **Property 15: Unknown Command Error Format**
    - **Validates: Requirements 7.5**
    - Verify error message format for unknown commands
    - _Requirements: 7.5_

  - [ ]* 6.9 Write property test for confirmation dialog
    - **Property 16: Confirmation Dialog for Destructive Commands**
    - **Validates: Requirements 7.4**
    - Verify destructive commands show confirmation before execution
    - _Requirements: 7.4_

- [ ] 7. Checkpoint - Command execution ready
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 8. Implement TuiState and mode management
  - [ ] 8.1 Create TuiMode enum and TuiState struct
    - Define modes: Normal, Command, CommandPalette, Help, Confirm
    - Add fields: mode, command_state, palette_state, confirm_state, footer_message
    - _Requirements: 1.1, 1.4, 1.6_

  - [ ] 8.2 Implement mode transition logic
    - Implement HandleEvent() dispatcher based on current mode
    - Implement mode-specific handlers (HandleNormalMode, HandleCommandMode, etc.)
    - _Requirements: 1.1, 1.4, 1.6_

  - [ ] 8.3 Implement Command_Mode entry and exit
    - Enter Command_Mode on ':' key in Normal_Mode
    - Exit on Escape or Enter with empty buffer
    - Clear input buffer on exit
    - _Requirements: 1.1, 1.4, 1.6_

  - [ ]* 8.4 Write unit tests for mode transitions
    - Test entering Command_Mode from Normal_Mode
    - Test exiting Command_Mode via Escape
    - Test exiting Command_Mode via Enter with empty buffer
    - _Requirements: 1.1, 1.4, 1.6_

  - [ ]* 8.5 Write property test for mode transition correctness
    - **Property 1: Mode Transition Correctness**
    - **Validates: Requirements 1.1, 1.4, 1.6**
    - Verify round-trip Normal → Command → Normal clears buffer
    - _Requirements: 1.1, 1.4, 1.6_

  - [ ]* 8.6 Write property test for mode isolation
    - **Property 22: Mode Isolation for Shortcuts**
    - **Validates: Requirements 9.3**
    - Verify single-key shortcuts don't trigger in Command_Mode
    - _Requirements: 9.3_

- [ ] 9. Integrate command input into existing TUI
  - [ ] 9.1 Modify RunFtxuiDashboard to include TuiState
    - Add TuiState instance to existing state variables
    - Initialize MetadataCache and AutocompleteEngine at startup
    - _Requirements: 6.1, 6.2_

  - [ ] 9.2 Route keyboard events through TuiState
    - Modify event handler to call TuiState::HandleEvent()
    - Preserve existing single-key shortcut handling in Normal_Mode
    - _Requirements: 1.1, 9.1, 9.2_

  - [ ] 9.3 Implement HandleCommandMode event routing
    - Handle Escape, Enter, Tab, Shift+Tab, Arrow keys
    - Handle character input, Backspace, Delete, Ctrl+U
    - Handle cursor movement (Left, Right, Home, End)
    - Trigger autocomplete on buffer changes
    - _Requirements: 1.4, 1.5, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 5.2, 5.3, 5.4, 5.5_

  - [ ] 9.4 Implement command execution on Enter
    - Call CommandExecutor::Execute() with buffer content
    - Handle success: return to Normal_Mode, show success message
    - Handle failure: stay in Command_Mode, show error message
    - Handle confirmation: show confirmation dialog
    - _Requirements: 1.5, 7.1, 7.2, 7.3, 7.4_

  - [ ]* 9.5 Write integration tests for event routing
    - Test complete flow: enter Command_Mode, type command, execute
    - Test autocomplete trigger on typing
    - Test candidate navigation and selection
    - _Requirements: 1.1, 1.4, 1.5, 2.1, 5.2, 5.3_

- [ ] 10. Implement UI rendering components
  - [ ] 10.1 Implement RenderInputLine() for command input display
    - Show ':' prompt prefix
    - Show input buffer with cursor (block character █)
    - Handle cursor position within text
    - Add border for visual separation
    - _Requirements: 1.2, 1.3_

  - [ ] 10.2 Implement RenderCandidateList() for autocomplete display
    - Show candidate name and description for each item
    - Highlight selected candidate with inverted colors
    - Add border for visual separation
    - Show only when candidates available
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6_

  - [ ] 10.3 Integrate command input UI into main TUI layout
    - Position candidate list above input line
    - Position input line above footer
    - Show only when in Command_Mode
    - _Requirements: 1.2, 4.1_

  - [ ] 10.4 Update footer to show command mode help text
    - Show "Press ':' for command mode" in Normal_Mode
    - Show "Tab: next | Esc: cancel" in Command_Mode
    - Preserve existing help text for single-key shortcuts
    - _Requirements: 9.4_

  - [ ]* 10.5 Write property test for candidate display completeness
    - **Property 8: Candidate Display Completeness**
    - **Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 5.1**
    - Verify all candidates displayed with name and description
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6_

  - [ ]* 10.6 Write property test for candidate acceptance
    - **Property 11: Candidate Acceptance Updates Buffer**
    - **Validates: Requirements 5.9**
    - Verify Enter updates buffer with selected candidate
    - _Requirements: 5.9_

- [ ] 11. Checkpoint - Basic command input working
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 12. Implement CommandPaletteState component
  - [ ] 12.1 Create PaletteItem data structure
    - Define struct with name, description, category
    - _Requirements: 8.2, 8.3_

  - [ ] 12.2 Implement CommandPaletteState class
    - Add fields: all_commands, filtered_commands, search_query, selected_index
    - Populate all_commands from MetadataCache at initialization
    - Group commands by category (Repository, History, Navigation, System)
    - _Requirements: 8.1, 8.2, 8.3_

  - [ ] 12.3 Implement fuzzy search filtering
    - Implement UpdateFilter() with substring matching (case-insensitive)
    - Match against both command name and description
    - _Requirements: 8.6_

  - [ ] 12.4 Implement palette rendering
    - Implement Render() to display grouped command list
    - Show search input at top
    - Highlight selected command
    - Show category headers
    - _Requirements: 8.1, 8.2, 8.3_

  - [ ] 12.5 Implement palette event handling
    - Handle Ctrl+P to open palette from Normal_Mode
    - Handle Escape to close palette
    - Handle Enter to select command and enter Command_Mode
    - Handle Up/Down for navigation
    - Handle character input for search
    - _Requirements: 8.1, 8.4, 8.5_

  - [ ]* 12.6 Write unit tests for CommandPaletteState
    - Test fuzzy search filtering
    - Test command selection
    - Test category grouping
    - _Requirements: 8.1, 8.2, 8.3, 8.6_

  - [ ]* 12.7 Write property test for palette completeness
    - **Property 17: Command Palette Completeness**
    - **Validates: Requirements 8.1, 8.2, 8.3**
    - Verify all registered commands appear in palette
    - _Requirements: 8.1, 8.2, 8.3_

  - [ ]* 12.8 Write property test for palette selection
    - **Property 18: Command Palette Selection Pre-fills Buffer**
    - **Validates: Requirements 8.4**
    - Verify selected command pre-fills input buffer
    - _Requirements: 8.4_

  - [ ]* 12.9 Write property test for fuzzy search
    - **Property 19: Command Palette Fuzzy Search**
    - **Validates: Requirements 8.6**
    - Verify search matches name and description substrings
    - _Requirements: 8.6_

  - [ ]* 12.10 Write property test for palette dismissal
    - **Property 20: Command Palette Dismissal**
    - **Validates: Requirements 8.5**
    - Verify Escape closes palette and returns to Normal_Mode
    - _Requirements: 8.5_

- [ ] 13. Implement Help Panel
  - [ ] 13.1 Create help content structure
    - Define help text sections: Command Mode, Single-Key Shortcuts, Other
    - Include keyboard shortcuts and descriptions
    - _Requirements: 10.2, 10.3, 10.4_

  - [ ] 13.2 Implement help panel rendering
    - Render help content in bordered panel
    - Show "Press Esc or q to close" footer
    - _Requirements: 10.2, 10.3, 10.4_

  - [ ] 13.3 Implement help panel event handling
    - Handle '?' in Normal_Mode to open help
    - Handle ":help" command to open help
    - Handle Escape and 'q' to close help
    - _Requirements: 10.1, 10.5, 10.6_

  - [ ]* 13.4 Write property test for help panel completeness
    - **Property 23: Help Panel Completeness**
    - **Validates: Requirements 10.2, 10.3, 10.4**
    - Verify help panel lists all commands and shortcuts
    - _Requirements: 10.2, 10.3, 10.4_

  - [ ]* 13.5 Write property test for help panel activation
    - **Property 24: Help Panel Activation and Dismissal**
    - **Validates: Requirements 10.5, 10.6**
    - Verify '?' opens help and Escape/q closes it
    - _Requirements: 10.5, 10.6_

- [ ] 14. Implement error handling and user feedback
  - [ ] 14.1 Implement parse error handling
    - Catch CLI11::ParseError and format as "Invalid command syntax: <details>"
    - Display in footer with error styling
    - _Requirements: 11.1_

  - [ ] 14.2 Implement git error propagation
    - Catch git operation errors and display in footer
    - Preserve original error message from git
    - _Requirements: 11.2_

  - [ ] 14.3 Implement autocomplete error resilience
    - Wrap autocomplete calls in try-catch
    - Log errors to stderr, continue without crashing
    - Display "Command metadata unavailable" if metadata fails
    - _Requirements: 11.3, 11.4_

  - [ ] 14.4 Implement error message lifecycle
    - Clear error message when user starts typing new command
    - Clear footer_is_error flag on new input
    - _Requirements: 11.5_

  - [ ] 14.5 Implement progress feedback for long operations
    - Display "Fetching...", "Pushing...", etc. during execution
    - Update footer message during long-running commands
    - _Requirements: 11.6_

  - [ ]* 14.6 Write property test for parse error format
    - **Property 25: Parse Error Message Format**
    - **Validates: Requirements 11.1**
    - Verify parse errors use correct format
    - _Requirements: 11.1_

  - [ ]* 14.7 Write property test for git error propagation
    - **Property 26: Git Error Propagation**
    - **Validates: Requirements 11.2**
    - Verify git errors displayed in footer
    - _Requirements: 11.2_

  - [ ]* 14.8 Write property test for autocomplete error resilience
    - **Property 27: Autocomplete Error Resilience**
    - **Validates: Requirements 11.3, 11.4**
    - Verify TUI continues functioning after autocomplete failure
    - _Requirements: 11.3, 11.4_

  - [ ]* 14.9 Write property test for error message lifecycle
    - **Property 28: Error Message Lifecycle**
    - **Validates: Requirements 11.5**
    - Verify error messages clear on new input
    - _Requirements: 11.5_

  - [ ]* 14.10 Write property test for progress feedback
    - **Property 29: Progress Feedback for Long Operations**
    - **Validates: Requirements 11.6**
    - Verify progress messages displayed during long operations
    - _Requirements: 11.6_

- [ ] 15. Checkpoint - Error handling complete
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 16. Implement backward compatibility
  - [ ] 16.1 Verify single-key shortcuts work in Normal_Mode
    - Test r (refresh), d (dirty-only), f (fetch), c/C (commit), p/P (push), Enter (history), q (quit)
    - Ensure no regression in existing functionality
    - _Requirements: 9.1, 9.2_

  - [ ] 16.2 Ensure single-key shortcuts don't trigger in Command_Mode
    - Verify characters are added to buffer instead of triggering actions
    - Already implemented in HandleCommandMode, verify with tests
    - _Requirements: 9.3_

  - [ ]* 16.3 Write property test for backward compatibility
    - **Property 21: Backward Compatibility with Single-Key Shortcuts**
    - **Validates: Requirements 9.1, 9.2**
    - Verify all single-key shortcuts work as before
    - _Requirements: 9.1, 9.2_

  - [ ]* 16.4 Write integration test for backward compatibility
    - Test complete workflows using only single-key shortcuts
    - Verify no behavior changes from before feature implementation
    - _Requirements: 9.1, 9.2, 9.3_

- [ ] 17. Implement performance optimizations
  - [ ] 17.1 Add metadata caching
    - Cache extracted metadata in MetadataCache
    - Only refresh on explicit Refresh() call
    - _Requirements: 12.6_

  - [ ] 17.2 Optimize autocomplete candidate generation
    - Use prefix matching (not substring) for O(n) complexity
    - Early exit when max candidates (10) reached
    - Pre-lowercase command names for fast comparison
    - _Requirements: 12.4_

  - [ ] 17.3 Implement non-blocking autocomplete
    - Ensure autocomplete doesn't block user input
    - Use FTXUI's event loop for async behavior
    - _Requirements: 12.5_

  - [ ]* 17.4 Write property test for input responsiveness
    - **Property 30: Input Responsiveness**
    - **Validates: Requirements 12.1, 12.2, 12.3**
    - Verify input updates within 50ms, autocomplete within 100ms
    - _Requirements: 12.1, 12.2, 12.3_

  - [ ]* 17.5 Write property test for non-blocking autocomplete
    - **Property 31: Non-Blocking Autocomplete**
    - **Validates: Requirements 12.5**
    - Verify user input continues during autocomplete generation
    - _Requirements: 12.5_

  - [ ]* 17.6 Write property test for autocomplete scalability
    - **Property 32: Autocomplete Scalability**
    - **Validates: Requirements 12.6**
    - Verify sub-100ms response with 100+ commands
    - _Requirements: 12.6_

- [ ] 18. Implement command category support
  - [ ] 18.1 Verify all command categories are supported
    - Test refresh, commit, push, fetch, cherry-pick, rebase, history, filter commands
    - Ensure all can be executed via Command_Mode
    - _Requirements: 7.6_

  - [ ]* 18.2 Write property test for command category support
    - **Property 33: Command Category Support**
    - **Validates: Requirements 7.6**
    - Verify all command categories executable via Command_Mode
    - _Requirements: 7.6_

- [ ] 19. Final integration and polish
  - [ ] 19.1 Update footer help text
    - Show both single-key shortcuts and command input instructions
    - Ensure help text is visible in all modes
    - _Requirements: 9.4_

  - [ ] 19.2 Add visual polish to UI components
    - Ensure consistent border styles
    - Verify cursor visibility (block character █)
    - Test with various terminal sizes
    - _Requirements: 1.2, 1.3, 4.6_

  - [ ] 19.3 Test complete user workflows
    - Test: enter command mode, type command, autocomplete, execute
    - Test: open command palette, search, select command
    - Test: view help panel, navigate, close
    - Test: error handling and recovery
    - _Requirements: All_

  - [ ]* 19.4 Write property test for footer help display
    - **Property 34: Footer Help Display**
    - **Validates: Requirements 9.4**
    - Verify footer shows both shortcuts and command input instructions
    - _Requirements: 9.4_

- [ ] 20. Final checkpoint - Feature complete
  - Ensure all tests pass, ask the user if questions arise.
  - Verify all 78 acceptance criteria are met
  - Verify all 34 correctness properties have passing tests
  - Run performance benchmarks to verify targets met
  - Test on reference hardware and various terminal emulators

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation
- Property tests validate universal correctness properties
- Unit tests validate specific examples and edge cases
- Implementation uses C++ with FTXUI, CLI11, and Catch2
- All components integrate with existing kano-git TUI infrastructure
