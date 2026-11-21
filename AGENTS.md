# mxlfs - working rules

`CLAUDE.md` imports this file for Claude Code, and `.gemini/settings.json`
names it for Gemini CLI. `README.md` is the overview, `docs/USAGE.md`
describes how mxlfs behaves, and `docs/BUILDING.md` how it is installed,
built, tested and released; this file holds the rules for changing it.
A change to behaviour updates `docs/USAGE.md`, and a change to the build,
the tests, CI or packaging updates `docs/BUILDING.md`, in the same
change.

## Use a git worktree

Before any change to the repository (edit, commit, branch, push, rebase,
`gh pr create`), work in a worktree of its own under `.claude/worktrees/`,
never in the main checkout, which stays read-only. Several sessions work on
the repository at once, and two writers in one tree corrupt each other's
index and branches.

```sh
git fetch origin
git worktree add .claude/worktrees/<topic>-$(openssl rand -hex 4) -b <topic> origin/main
```

After the merge: `git worktree remove <path>`, `git branch -D <topic>`.

## How to work

- State assumptions. If a request has several readings, name them; if
  something is unclear, stop and ask instead of guessing.
- Read the code a change touches, its callers and the helpers next to it
  before writing. If a structure's reason is unclear, find it in the
  comments and `git log`; if it stays unclear, ask.
- Write the least code that solves the problem: no options nobody asked
  for, no abstraction for one use, no handling of states that cannot occur.
- Fail loudly. A change is not done if anything was skipped, and tests did
  not pass if any were skipped. Report what ran, what did not, and why.
- Leave what you touch more correct: delete comments and documentation the
  change makes stale, in the same change.

## The module

mxlfs is a memory filesystem: libfs (`simple_*`) for the namespace, the
page cache for data, no backing store. Features depend on these
invariants; keep them, or change one deliberately with the reason in the
commit:

- File data is unevictable page cache in the largest folios that fit, so
  rings map with PMDs and register with few NIC translation entries.
- There is no writeback and no `page_mkwrite`, so shared writable mappings
  can be long-term pinned for RDMA.
- A view maps its sources' folios in its own fault handler and never
  copies data; switching a source zaps the view's mappings.
- mxlfs reports its own magic, `MXLFS_MAGIC`, not tmpfs's.
- A path walk waits for a missing flow in `->d_manage`, outside the
  directory lock.

Kernel practice:

- Use what the kernel provides before writing code of its own: libfs,
  `filemap_*`, `generic_file_*`, the fs_context parser. Where mxlfs works
  around one, the comment says why, as at `mxlfs_write_end`, which reaches
  `simple_write_end` through `ram_aops` because it is static in 6.18.
  Raising the minimum kernel includes removing workarounds whose reason
  lapsed.
- Only interfaces exported to modules. Never reach an unexported symbol
  through kallsyms or by copying a static kernel function.
- Allocations a user's call causes and keeps are charged to its memory
  cgroup (`GFP_KERNEL_ACCOUNT`). Locks a function expects held are asserted
  (`lockdep_assert_held`). Where several locks nest, the comment states the
  order, as `mxlfs_fold` does.
- `LINUX_VERSION_CODE` appears only in `kernel/compat.h`; the other files
  use the wrappers and macros it defines.
- Kernel coding style (`Documentation/process/coding-style.rst`), as
  `dev/checkpatch.sh` checks it.

## Supported kernels

Linux 6.18 and newer through 7.x; the open release candidate is built as
an early warning. Nothing older is supported or tested.

`kernel/compat.h` holds one wrapper or macro per kernel interface that
changed, each named and commented after the release that changed it
(`mxlfs_filemap_alloc_folio`: 6.19 adds a memory policy argument). A new
difference gets a new wrapper there. Raising the minimum removes the
wrappers for releases below it and updates every place that names the
minimum: each hit of `git grep -nE '6(\.|, )18'` that states it (not
`dev/talos-kernel`, the kernel Talos ships), the kernel regexps in
`kmm/module.yaml`, and the `between` matrix in `ci.yml`.

## Quality gates

CI runs these on every pull request (`.github/workflows/ci.yml`,
`pr-title.yml`); run the ones a change touches before pushing. The
commands are in `docs/BUILDING.md`.

- `dev/checkpatch.sh`: `checkpatch.pl --strict` from a pinned kernel
  commit. Each exception, in the script or `.checkpatch.conf`, states its
  reason.
- `dev/module.sh`: fails on any compiler, sparse or modpost warning
  (`W=1`, sparse pinned in `dev/sparse.sh`) and when sparse did not run.
- The harness builds with `-Werror`; CI passes it in `CFLAGS` (the Harness
  step in `ci.yml`), which `tools/Makefile` does not set, so pass the same
  locally.
- The VM test groups (`vm-tests` in `ci.yml`) on kernels built with
  KASAN, lockdep and `DEBUG_VM`.
- The module against the distribution headers and the kernels in the
  `distro`, `between` and `next-rc` jobs.
- The Debian package under lintian at pedantic level, installed and
  booted by `dev/distro-test.sh`.

A gate is fixed, not bypassed: no new checkpatch exception, `-Wno-` flag
or skipped suite to get a change green without a reason in the commit.

## Tests

- Every change to behaviour comes with a test at the level the repository
  has for it: a `tests/` suite in the VM, a harness command, or both.
- A test must fail without the change and pass with it; show that, by
  reverting the change or forcing the condition. A test that cannot fail
  when the logic breaks is a broken test.
- A timing-dependent result is bounded relative to what the same run
  measures, not tuned to one machine.
- A new suite sources `tests/lib.sh`, checks the kernel log with its
  `kernel_reports` at its end, and joins a `vm-tests` group in `ci.yml`,
  or it never runs.
- A kernel tree in `build/` takes several GB; `build/` may be a symlink to
  one shared between worktrees, which `dev/run.sh` mounts.

## Userspace interface

`kernel/mxlfs_uapi.h` is the ABI: programs built against an older header
keep working with a newer module.

- An ioctl number is never reused or renumbered; a new ioctl takes the
  next free number.
- A struct uses fixed-width types (`__u32`, `__u64`, `__s32`), with
  explicit padding (`pad` in `mxlfs_wanted_fail`) so its layout is the
  same for 32- and 64-bit callers. A pointer travels as a `__u64`
  (`stats_ptr` in `mxlfs_mount_nodes`). A field is never removed, retyped
  or moved; a struct grows only at its end, where the module accepts the
  older size, as `MXLFS_IOC_STATS` does.
- A new request struct has a `flags` field, and the module rejects bits
  it does not know with EINVAL, so a later flag can be probed (see
  `MXLFS_IOC_VIEW_REPLACE_SOURCE`).
- The comment above each ioctl is its reference: arguments, errors, and
  what a caller sees. Change it with the behaviour.
- The header keeps its license and includes only uapi headers.

## Packaging

Every way the module reaches a machine runs
`make -C kernel KDIR=<kernel build tree>` and nothing else; keep that
working. Beyond it:

- DKMS (`dkms.conf`): `BUILD_EXCLUSIVE_KERNEL_MIN` matches the minimum in
  `compat.h`, and `BUILD_EXCLUSIVE_CONFIG` the `CONFIG_TRANSPARENT_HUGEPAGE`
  check in `mxlfs.h`.
- The Debian package installs only the files `debian/rules` lists: a new
  header under `kernel/` goes into that list, a new `.c` file into it and
  into `kernel/Kbuild`.
- KMM (`kmm/`) builds from a release's source tarball, checked against its
  published `.sha256`; a Talos system extension built outside this
  repository builds from the same tarball, pinned by its checksum.

So a released source tarball and its checksum files never change: never
move or recreate a tag, and never replace those assets. The release
workflow refuses to replace an attached tarball.

## Releases

release-please (`.github/release-please-config.json`) maintains the
release pull request; merging it cuts the release. Never edit the version
in `dkms.conf` or in `MODULE_VERSION` in `kernel/super.c` (both marked
`x-release-please-version`), `.github/release-please-manifest.json` or
`CHANGELOG.md` by hand.

## Commits, pull requests and merging

- Conventional Commits, scoped to the area (`fix(kernel): ...`,
  `ci: ...`). Subject at most 72 characters, imperative, lower case after
  the colon, no trailing period; body wrapped at 72. Breaking changes take
  `!` or a `BREAKING CHANGE:` footer. Small, focused commits.
- A message says why the change exists, in terms useful years later to
  someone who never saw the work. The diff shows what. Declarative facts:
  no pronouns, no addressing the reader, no narration of what was tried,
  no filler verbs, no checklists, no `Co-Authored-By` trailers. The same
  applies to pull request descriptions.
- Pull requests are squash-merged, with the title as the subject and the
  branch's commit messages as the body. release-please takes one changelog
  entry per squash, from the title, so the title is a Conventional Commit
  of the type the release needs: `docs`, `style` and `test` produce no
  entry. `!` works in the title. `BREAKING CHANGE:` and `Release-As:` are
  footers, read only at the very end of the squashed message, as at the
  end of a single-commit PR's commit body, which is the squash body; a
  footer anywhere else is inert. A PR of several commits that needs one,
  or that carries more than one release-relevant change, puts them in a
  `BEGIN_COMMIT_OVERRIDE` block in its body, or is split.
- Never force-push `main`. Rewriting a feature branch is fine while nobody
  else works on it.

## Writing

- ASCII only in code, comments, documentation, commit messages and pull
  request descriptions: `-`, `--`, `"`, `'`, `->`, `...`. Generated files
  under `.agents/skills/` are kept as their generator writes them.
- State facts; do not speculate. Do not describe behaviour that the source,
  a run or authoritative documentation does not confirm, and do not write
  what mxlfs "should be used for".
- Comments say why, not what. No task references, review rounds or agent
  session history in code, comments or commits.
- Nothing in this public repository references private repositories,
  internal systems, environments or people's machines.
- A new file under `kernel/` carries an SPDX identifier; other new files
  get none.

## Agent tooling

Skills live in `.agents/skills/<name>/SKILL.md`, the Agent Skills layout;
`.claude/skills` is a symlink to that directory for Claude Code.
Instructions go into this file, not into a harness-specific one.

- graphify: `.agents/skills/graphify/` and the hook in
  `.claude/settings.json` come from `graphify install --project`. The
  skill is graphify's Claude Code variant; other harnesses run the
  `graphify` commands it names directly. A reinstall also appends to
  `CLAUDE.md` and creates `.claude/CLAUDE.md`; revert both, as `CLAUDE.md`
  holds only the import. It also rewrites the hook without `-I` on its
  `python3 -c`; put it back, or the hook imports a `json.py` from the
  working directory. Build the graph with `graphify update .`;
  `.graphifyignore` scopes it. `graphify-out/` is never committed.
- desloppify: `dev/desloppify-skill.sh` writes
  `.agents/skills/desloppify/SKILL.md` from the release it pins by version
  and wheel checksum; to update, change both there and rerun it. Do not
  run `desloppify update-skill` or `desloppify setup`. `.desloppify/` and
  `scorecard.png` are never committed.
