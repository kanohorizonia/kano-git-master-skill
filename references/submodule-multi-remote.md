# Version-controlled submodule remotes

KOG extends standard `.gitmodules` entries with named `kog-remote-*` keys. Git ignores unknown keys, so ordinary `git submodule` commands remain compatible while KOG can reproduce fork, mirror, SSH, and HTTPS remotes on another machine.

```bash
kog submodule remote-set deps/example upstream git@github.com:org/example.git
kog submodule remote-set deps/example upstream-http https://github.com/org/example.git
kog submodule sync-urls
```

The resulting entry is version controlled:

```ini
[submodule "deps/example"]
    path = deps/example
    url = https://github.com/org/example.git
    kog-remote-upstream = git@github.com:org/example.git
    kog-remote-upstream-http = https://github.com/org/example.git
```

Remote names are caller-defined safe slugs. `remote-set --dry-run` previews changes. `sync-urls --dry-run` shows the standard origin sync and every named remote without mutation. Initialized submodule repositories receive `remote add` or `remote set-url`; missing submodules retain the version-controlled declaration until initialized.

Use the general native commands inside the submodule repository for network actions, for example `kog fetch --remote upstream` or `kog push --remote origin`. Authentication and protocol fallback remain explicit: configure SSH and HTTPS as separate named remotes, probe them with `kog auth test`, and select the admitted remote. KOG does not silently turn a failed push into a credential-bearing request to another endpoint.
