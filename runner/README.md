# Self-hosted GitHub Actions runner

This Docker Compose service registers a repository runner named and labeled
`ubuntu`. It uses the official Actions runner release pinned in `Dockerfile`.
Its container is limited to logical CPUs 0 through 7 and an eight-CPU quota.
On the current `ubuntu-local` host, those CPU IDs are the first thread of each
of its eight physical cores.

The workflow sends pushes to `main` to this runner. Pull requests remain on
GitHub-hosted runners because this repository is public and fork code must not
run on the home server.

## First-time registration

From this directory's repository root, with `GITSCOUT_PAT` available in the
environment:

```sh
GH_TOKEN="$GITSCOUT_PAT" gh api \
  repos/gitscout-bot/noshare-cover/actions/runners/registration-token \
  --method POST --jq .token \
  | docker compose -f runner/compose.yaml run --rm --no-deps --interactive runner configure

docker compose -f runner/compose.yaml up -d --build
```

The registration token is streamed to the one-time configuration container;
the PAT is not stored in the runner or Compose configuration. Runner
registration and work files are kept in named Docker volumes.

After each job, a runner hook restores ownership of the workspace. This is
needed because the Arch job container runs its package setup as root while the
Nix job checks out as the unprivileged runner account.
