# Installing Callbox

[Docs index](index.md) | [Repository README](../README.md)

Callbox publishes release artifacts for Linux and macOS. The installer detects your platform and picks the matching artifact. It checks the artifact against the release `SHA256SUMS` and validates the archive layout. Then it installs the `callbox` binary and runs it once to verify it. It refuses to install when the checksum is missing, malformed, or wrong.

## One-line install

```sh
curl -LsSf https://github.com/RchGrav/callbox/releases/latest/download/install.sh | sh
```

If the system has `wget` instead of `curl`:

```sh
wget -qO- https://github.com/RchGrav/callbox/releases/latest/download/install.sh | sh
```

The installer needs `curl` or `wget`, `sha256sum` or `shasum`, `tar`, `awk`, `sed`, and `mktemp`.

## Where it installs

- Root installs to `/usr/local/bin`.
- A normal user installs to `/usr/local/bin` when it is writable, otherwise to `$HOME/.local/bin`.
- `CALLBOX_INSTALL_DIR` installs to that directory instead.

Force a system install from a normal shell. This targets `/usr/local/bin` and runs `sudo` only when the current process cannot write there:

```sh
curl -LsSf https://github.com/RchGrav/callbox/releases/latest/download/install.sh | sh -s -- --system
```

`CALLBOX_INSTALL_SYSTEM=1` does the same. `CALLBOX_INSTALL_DIR` cannot be combined with `--system`.

The binary gets mode `0755`. A root or sudo install to `/usr/local/bin` sets ownership to uid 0, gid 0 (`root:root` on Linux, `root:wheel` on macOS) where the platform allows it.

## PATH

If the install directory is not on `PATH`, the installer prints:

```text
run now: <dir>/callbox --help
or export: PATH=<dir>:$PATH
```

For a normal user it also appends a `# callbox installer` block that adds the directory to `PATH` in future shells. The block goes to `$CALLBOX_PROFILE` if set, otherwise `~/.zshrc` (zsh), `~/.bashrc` (bash), or `~/.profile`. It is skipped when `CALLBOX_ENV_FILE` is set, when `CALLBOX_UPDATE_PROFILE` is not `1`, or when the profile already has the marker.

## Script and CI handoff

A child process cannot change its parent shell's `PATH`. In scripts, ask for a handoff file and source it:

```sh
curl -LsSf https://github.com/RchGrav/callbox/releases/latest/download/install.sh |
  CALLBOX_ENV_FILE="$PWD/.callbox-env" sh
. "$PWD/.callbox-env"

"$CALLBOX_BIN" --version
```

The handoff file contains:

```sh
export CALLBOX_BIN='/absolute/path/to/callbox'
export PATH='/install/dir':"$PATH"
```

When stdout is not a terminal, the installer also prints one line on stdout: `CALLBOX_BIN=<absolute path>`. All other output goes to stderr.

A custom directory works the same way:

```sh
curl -LsSf https://github.com/RchGrav/callbox/releases/latest/download/install.sh |
  CALLBOX_INSTALL_DIR="$PWD/.bin" \
  CALLBOX_ENV_FILE="$PWD/.callbox-env" \
  sh
. "$PWD/.callbox-env"
```

## Uninstall

The installer also uninstalls. Add `--uninstall` (or `-u`) with the same `--system` flag or `CALLBOX_INSTALL_DIR` the install used. Every install prints the exact command.

```sh
curl -LsSf https://github.com/RchGrav/callbox/releases/latest/download/install.sh | sh -s -- --uninstall
curl -LsSf https://github.com/RchGrav/callbox/releases/latest/download/install.sh | sh -s -- --uninstall --system
```

This removes the binary and the `# callbox installer` PATH block. The block is removed from the profile chosen by the current `$SHELL` (or `CALLBOX_PROFILE`). Callbox state (your calls, logs, and records) is kept. Remove it by hand if you mean to:

- user calls: `~/.local/state/callbox`
- root-managed calls: `/var/lib/callbox` on Linux, `/var/db/callbox` on macOS

## Pinned version

By default the installer uses the latest GitHub release. Each release's own `install.sh` defaults to that release, so pin a version by fetching it:

```sh
curl -LsSf https://github.com/RchGrav/callbox/releases/download/vX.Y.Z/install.sh | sh
```

Or pass a version to any installer, as an argument or as `CALLBOX_VERSION`. Values with and without the leading `v` are equivalent:

```sh
curl -LsSf https://github.com/RchGrav/callbox/releases/latest/download/install.sh | sh -s -- 0.9.4
```

## Environment reference

| Variable | Effect |
| --- | --- |
| `CALLBOX_VERSION` | Version or tag to install. A version argument overrides it. |
| `CALLBOX_INSTALL_SYSTEM` | `1`, `yes`, `true`, or `on` behaves like `--system`. |
| `CALLBOX_INSTALL_DIR` | Install into this directory. Not allowed with `--system`. |
| `CALLBOX_FLAVOR` | Linux artifact: `gnu-static`, `gnu-dynamic`, or `musl-static`. |
| `CALLBOX_ENV_FILE` | Write the handoff file here. Also skips the profile update. |
| `CALLBOX_UPDATE_PROFILE` | Default `1`. Any other value skips the profile update. |
| `CALLBOX_PROFILE` | Profile file to update or clean, instead of the shell's default. |
| `CALLBOX_INSTALL_DRY_RUN` | `1` prints the detection and plan, then exits without changes. |
| `CALLBOX_INSTALLER_VERSION` | Default version when neither an argument nor `CALLBOX_VERSION` is given. A release's `install.sh` sets it to that release; otherwise `latest`. |
| `CALLBOX_REPO_OWNER`, `CALLBOX_REPO_NAME` | GitHub repository to install from. Default `RchGrav/callbox`. |
| `CALLBOX_GITHUB_BASE`, `CALLBOX_GITHUB_API` | GitHub web and API base URLs. Default `https://github.com` and `https://api.github.com`. |

## Selection logic

The installer detects:

- operating system: Linux or macOS.
- CPU architecture: `amd64`, `arm64`, `armhf`, `mipsel`, or `riscv64`.
- Linux libc: `gnu` or `musl`.

There is no native Windows artifact. Use the installer inside WSL.

Default artifact choices:

| Detected target | Artifact choice |
| --- | --- |
| macOS arm64 | `macos-arm64` |
| macOS x86_64 | `macos-x86_64` |
| Linux amd64 glibc | `linux-amd64-gnu-static` |
| Linux amd64 musl | `linux-amd64-musl-static` |
| Linux arm64 glibc | `linux-arm64-gnu-static` |
| Linux arm64 musl | `linux-arm64-musl-static` |
| Linux armhf glibc | `linux-armhf-gnu-static` |
| Linux armhf musl | `linux-armhf-musl-static` |
| Linux mipsel | `linux-mipsel-musl-static` |
| Linux riscv64 | `linux-riscv64-musl-static` |

The installer stops with an error when the platform is unsupported, or when it cannot detect libc on amd64, arm64, or armhf.

### Linux artifact families

Artifact names give the libc and the link mode separately:

- `linux-*-gnu-static`: built against glibc and linked with `-static`.
- `linux-*-gnu-dynamic`: dynamically linked against the host glibc, so it uses the host's NSS and glibc security updates. Available for amd64, arm64, and armhf.
- `linux-*-musl-static`: statically linked with musl. It has no runtime library dependencies, so it suits minimal containers, rescue systems, and copying one binary between Linux systems.

Static builds (`gnu-static` and `musl-static`) look up accounts in `/etc/passwd` only, because glibc cannot reliably load NSS modules into a static binary. Accounts that exist only in LDAP, SSSD, or NIS are not found. Use `gnu-dynamic` on such hosts.

On glibc hosts the installer defaults to `gnu-static`. Choose another artifact with `CALLBOX_FLAVOR`:

```sh
CALLBOX_FLAVOR=musl-static sh install.sh
CALLBOX_FLAVOR=gnu-dynamic sh install.sh
```

mipsel and riscv64 have only `musl-static`. Any other `CALLBOX_FLAVOR` fails on those.

## Checksum and archive validation

Every release includes a `SHA256SUMS` asset. The installer reads the checksum only from that file. If `SHA256SUMS` is missing, or has no valid 64-hex SHA-256 entry for the selected artifact, installation stops before extraction.

The archive must hold the `callbox` executable at its root. The installer rejects archives that have no root-level `callbox` file, have a symlink in its place, or contain absolute or `..` paths. It does not search the archive for a binary elsewhere, so a malformed archive fails instead of installing the wrong file.

## Manual build

If no release artifact matches your platform, build from source. You need a C11 compiler, GNU make, and bash.

```sh
git clone https://github.com/RchGrav/callbox.git
cd callbox
make
./callbox --version
```

On Linux, `make` links with `STATIC_LDFLAGS=-static` by default. With glibc this gives a GNU static binary with the `/etc/passwd`-only lookup described above. For a dynamic glibc build, run `make STATIC_LDFLAGS=` or `make callbox-dynamic`. For a musl static build, use a musl-targeting compiler and keep static linking. On macOS, `make` links dynamically.

## Continue

[Back to docs index](index.md)
