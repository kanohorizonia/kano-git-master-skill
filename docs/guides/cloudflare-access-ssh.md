# Cloudflare Access SSH

Use this workflow when a Git SSH endpoint is published through Cloudflare
Tunnel and protected by Cloudflare Access. The server-side tunnel route and
Access application must already exist. KOG configures only the client.

## Safety model

- The target hostname is always explicit; deployment-specific hostnames are not
  embedded in KOG.
- `setup` is a preview unless `--confirm-host-write` is supplied.
- KOG owns only a bounded include marker in the main SSH config and a bounded
  block for the selected hostname in its fragment.
- Before the first main-config change, KOG creates
  `~/.ssh/config.kog.bak`.
- No Cloudflare token, Git credential, or SSH private key is stored by KOG.
- Re-running the same setup is idempotent.

## Preview and apply

```bash
./scripts/kog auth cloudflare-ssh setup \
  --hostname gitlab-ssh.example.com \
  --user git \
  --install \
  --dry-run

./scripts/kog auth cloudflare-ssh setup \
  --hostname gitlab-ssh.example.com \
  --user git \
  --install \
  --confirm-host-write
```

`--install` uses Homebrew on macOS and WinGet on Windows when `cloudflared` is
missing. On Linux, install `cloudflared` through the package mechanism approved
for that host, then run setup again. To use an already managed binary, pass
`--cloudflared-path /absolute/path/to/cloudflared`.

The managed SSH host entry is equivalent to:

```sshconfig
Host gitlab-ssh.example.com
  HostName gitlab-ssh.example.com
  User git
  ProxyCommand "/absolute/path/to/cloudflared" access ssh --hostname %h
```

## Diagnose and validate

```bash
./scripts/kog auth cloudflare-ssh doctor \
  --hostname gitlab-ssh.example.com

ssh -T git@gitlab-ssh.example.com

./scripts/kog auth test \
  --url git@gitlab-ssh.example.com:group/repository.git
```

The first live SSH connection may open a browser for Cloudflare Access
authentication. After that session is established, normal SSH-style Git URLs
work through the configured `ProxyCommand`.

For an end-to-end read-only clone check:

```bash
git clone \
  git@gitlab-ssh.example.com:group/repository.git \
  /path/to/temporary/checkout
```

## Rollback

Remove the bounded block for the hostname from
`~/.ssh/config.d/kog-cloudflare-access.conf`. If no managed Cloudflare SSH hosts
remain, remove the KOG Cloudflare Access include block from `~/.ssh/config`.
The original pre-change main configuration is also available at
`~/.ssh/config.kog.bak`.
