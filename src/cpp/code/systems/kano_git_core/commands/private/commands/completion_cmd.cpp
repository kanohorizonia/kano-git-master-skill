// completion command - emits shell completion scripts

#include "command_registry.hpp"

#include <iostream>
#include <memory>
#include <string>

namespace kano::git::commands {
namespace {

std::string RenderBashScript() {
    return R"BASH(# kano-git bash completion
_kano_git_complete() {
  local cur
  local context
  local -a args
  local out

  cur="${COMP_WORDS[COMP_CWORD]}"
  context=()
  args=()

  local i
  for ((i=1; i<COMP_CWORD; ++i)); do
    context+=("${COMP_WORDS[i]}")
  done

  for token in "${context[@]}"; do
    args+=(--context "$token")
  done

  out="$(kano-git __complete "${args[@]}" --current "$cur" 2>/dev/null)"
  COMPREPLY=($(compgen -W "$out" -- "$cur"))
}

complete -F _kano_git_complete kano-git
complete -F _kano_git_complete kog
)BASH";
}

std::string RenderZshScript() {
    return R"ZSH(#compdef kano-git kog

_kano_git_complete() {
  local cur
  local -a context
  local -a out

  cur="${words[CURRENT]}"
  context=()

  local i
  for ((i=2; i<CURRENT; ++i)); do
    context+=(--context "${words[i]}")
  done

  out=(${(f)"$(kano-git __complete ${context} --current "$cur" 2>/dev/null)"})
  _describe 'kano-git completion' out
}

compdef _kano_git_complete kano-git
compdef _kano_git_complete kog
)ZSH";
}

std::string RenderFishScript() {
    return R"FISH(function __kano_git_complete
    set -l cmd (commandline -opc)
    set -l cur (commandline -ct)

    set -l args
    if test (count $cmd) -gt 1
        for i in (seq 2 (count $cmd))
            set args $args --context $cmd[$i]
        end
    end

    kano-git __complete $args --current "$cur"
end

complete -c kano-git -f -a "(__kano_git_complete)"
complete -c kog -f -a "(__kano_git_complete)"
)FISH";
}

std::string RenderPowerShellScript() {
    return R"POWERSHELL(Register-ArgumentCompleter -CommandName kano-git,kog -ScriptBlock {
    param($wordToComplete, $commandAst, $cursorPosition)

    $elems = $commandAst.CommandElements
    $args = @()
    for ($i = 1; $i -lt $elems.Count - 1; $i++) {
        $args += '--context'
        $args += $elems[$i].Extent.Text
    }
    $args += '--current'
    $args += $wordToComplete

    kano-git __complete @args | ForEach-Object {
        [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_)
    }
})
)POWERSHELL";
}

std::string RenderGitBashEnhancerScript() {
        return R"BASH(# kano-git enhancer for native git completion (bash)
# Usage:
#   source <(kano-git completion git-bash)
# Requires git's own completion (_git) to be already loaded.

_kano_git_config_keys="kano.cache.global-dir kano.cache.local-dir"

_kano_git_enhance_git_config_completion() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    if [[ ${#COMP_WORDS[@]} -lt 2 || "${COMP_WORDS[1]}" != "config" ]]; then
        return 1
    fi

    case "$prev" in
        config|--global|--local|--system)
            COMPREPLY=($(compgen -W "$_kano_git_config_keys" -- "$cur"))
            return 0
            ;;
    esac

    case "$cur" in
        kano.*|kano*|ka*)
            COMPREPLY=($(compgen -W "$_kano_git_config_keys" -- "$cur"))
            return 0
            ;;
    esac

    return 1
}

_kano_git_wrap_existing_git_completion() {
    local old_func
    old_func="$1"

    _kano_git_git_complete_wrapper() {
        if _kano_git_enhance_git_config_completion; then
            return 0
        fi
        "$old_func"
    }

    complete -o bashdefault -o default -F _kano_git_git_complete_wrapper git
}

if declare -F _git >/dev/null 2>&1; then
    _kano_git_wrap_existing_git_completion _git
else
    complete -o bashdefault -o default -F _kano_git_enhance_git_config_completion git
fi
)BASH";
}

} // namespace

void RegisterCompletion(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("completion", "Generate shell completion script");

    auto shell = std::make_shared<std::string>();
    cmd->add_option("shell", *shell, "Target shell: bash|zsh|fish|powershell|git-bash")->required();

    cmd->callback([shell]() {
        if (*shell == "bash") {
            std::cout << RenderBashScript();
            return;
        }
        if (*shell == "zsh") {
            std::cout << RenderZshScript();
            return;
        }
        if (*shell == "fish") {
            std::cout << RenderFishScript();
            return;
        }
        if (*shell == "powershell") {
            std::cout << RenderPowerShellScript();
            return;
        }
        if (*shell == "git-bash") {
            std::cout << RenderGitBashEnhancerScript();
            return;
        }

        std::cerr << "Unsupported shell: " << *shell
                  << " (expected: bash|zsh|fish|powershell|git-bash)\n";
        throw CLI::RuntimeError(2);
    });
}

} // namespace kano::git::commands
