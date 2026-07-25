# GitLab HTTPS authentication

Use this workflow for GitLab HTTPS remotes that should authenticate through
Git Credential Manager (GCM) and the operating-system credential store.

## Server-side prerequisite

Interactive Cloudflare Access browser authentication is not a Git smart-HTTP
credential mechanism. If Access protects the GitLab web hostname, requests
such as `REPOSITORY.git/info/refs` must either:

- use a narrowly scoped, more-specific Access application that bypasses only
  `*.git/*` paths and relies on GitLab authentication/authorization; or
- use a non-interactive Cloudflare Service Auth design whose headers are
  supplied by a separately approved credential wrapper.

Do not bypass the entire GitLab hostname merely to make clone work. A path
bypass removes Access enforcement and Access request logging for the matched
traffic, so validate that the GitLab web root still requires Access.

KOG configures the Git client only. It does not create or modify Cloudflare
Access applications.

## Safety model

- The GitLab hostname and optional username are explicit arguments.
- `--dry-run` previews installation and Git config without writing.
- User-local installation and global Git config require
  `--confirm-global-write`.
- On macOS and Linux, `--install` downloads a pinned official GCM release from
  `git-ecosystem/git-credential-manager` into a versioned directory under
  `~/.local/share/kog/` and verifies its release SHA-256 before extraction.
- KOG adds the absolute helper path and preserves other credential helpers.
- KOG never creates, reads, stores, or prints a GitLab PAT.
- PATs belong in GCM and the operating-system credential store, not in remotes,
  shell history, repository config, or documentation.

## Preview and apply

```bash
./scripts/kog auth https setup \
  --hostname gitlab.example.com \
  --username git-user \
  --auth-mode pat \
  --install \
  --dry-run

./scripts/kog auth https setup \
  --hostname gitlab.example.com \
  --username git-user \
  --auth-mode pat \
  --install \
  --confirm-global-write
```

Use `--gcm-path /absolute/path/to/git-credential-manager` when GCM is managed
separately. KOG accepts only an executable with a GCM executable name that
successfully answers a non-secret `--version` probe. `pat` is the default auth
mode for a private GitLab instance.
`browser` requires the instance administrator or user to configure a suitable
public OAuth application for GCM. `basic` is available only when the GitLab
instance permits it.

After setup, provision a least-privilege GitLab PAT through the approved secret
workflow. For ordinary Git read/write operations, prefer only
`read_repository` and `write_repository`. Store it through GCM/Keychain; never
embed it in `https://user:token@host/...`.

## Diagnose and validate

```bash
./scripts/kog auth https doctor \
  --hostname gitlab.example.com \
  --auth-mode pat

./scripts/kog auth doctor \
  --url https://gitlab.example.com/group/repository.git

./scripts/kog auth test \
  --url https://gitlab.example.com/group/repository.git

git clone \
  https://gitlab.example.com/group/repository.git \
  /path/to/temporary/checkout
```

The HTTPS doctor requires the configured auth mode to match `--auth-mode`. When
`--username` is provided, it also requires that exact configured username; an
omitted username is not part of readiness.

For a Cloudflare-protected instance, also verify:

- `REPOSITORY.git/info/refs?service=git-upload-pack` reaches GitLab rather than
  redirecting to an Access login page.
- the GitLab web root still redirects unauthenticated browser traffic to
  Cloudflare Access.
- the separate SSH clone endpoint still succeeds if it is offered.

## Rollback

Remove only the scoped values and the exact helper installed for this host:

```bash
git config --global --unset-all \
  credential.https://gitlab.example.com.provider
git config --global --unset-all \
  credential.https://gitlab.example.com.gitLabAuthModes
git config --global --unset-all \
  credential.https://gitlab.example.com.username
git config --global --unset-all \
  credential.helper /absolute/path/to/git-credential-manager
```

Then remove the corresponding versioned directory under
`~/.local/share/kog/git-credential-manager/` if no other host uses it. Delete
or revoke the PAT from GitLab and remove its GCM/Keychain entry separately.
