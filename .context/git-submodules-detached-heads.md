# Submodule detached HEADs

`git submodule update` / recursive checkout always detaches submodules at the recorded gitlink.

## State (2026-09-26)
- Attached (HEAD was already the tip of this local branch, so no content or gitlink change):
  tree-sitter-{c,cmake,cpp,css,html,javascript,json,python,typescript} -> master,
  tree-sitter-markdown -> split_parser, vendors/tree-sitter -> master, causality -> main,
  causality/vendors/{glfw,glm,vma} -> master, freetype/subprojects/dlg -> main.
- Intentionally detached: pinned releases that are not a branch tip.
  libssh2 @ libssh2-1.11.1, mbedtls @ 3.6.7, mbedtls/framework, freetype @ VER-2-14-3-55.
- `branch =` is recorded in sol's `.gitmodules` and causality's `.gitmodules` for every attached
  submodule (dlg's lives in freetype's upstream repo, untouched), so `git submodule update --remote`
  knows what to track.
- Local-only config (each parent's .git/config): `submodule.<name>.update=merge` for the attached
  ones, so `git submodule update` fast-forwards the checked-out branch instead of detaching.
  Recursive `git checkout` / `pull --recurse-submodules` still detaches; re-attach with:
  `git submodule foreach --recursive 'git symbolic-ref -q HEAD || { b=$(git for-each-ref --points-at HEAD --format="%(refname:short)" refs/heads | head -1); [ -n "$b" ] && git switch -q "$b"; }; true'`
