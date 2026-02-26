// completion command - emits shell completion scripts

#include "command_registry.hpp"

#include <iostream>
#include <string>

namespace kano::git::commands {
namespace {

std::string RenderBashScript() {
    return R"BASH(# kano-git bash completion
_kano_git_complete() {
  local cur
  local context
  local out

  cur="${COMP_WORDS[COMP_CWORD]}"
  context=()

  local i
  for ((i=1; i<COMP_CWORD; ++i)); do
    context+=("${COMP_WORDS[i]}")
  done

  out="$(kano-git __complete ${context[@]/#/--context } --current "$cur" 2>/dev/null)"
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

} // namespace

void RegisterCompletion(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("completion", "Generate shell completion script");

    auto* shell = new std::string{};
    cmd->add_option("shell", *shell, "Target shell: bash|zsh|fish|powershell")->required();

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

        std::cerr << "Unsupported shell: " << *shell
                  << " (expected: bash|zsh|fish|powershell)\n";
        std::exit(2);
    });
}

} // namespace kano::git::commands
