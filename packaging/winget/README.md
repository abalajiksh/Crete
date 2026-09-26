# winget

crête's winget package is `abksh.crete`. winget hosts nothing itself: a package
is three YAML manifests in a pull request to GitHub's
[`microsoft/winget-pkgs`](https://github.com/microsoft/winget-pkgs), pointing
at the Windows zip on the Codeberg release. Its CI downloads that zip, installs
it in a sandbox and scans it, so **the release must be published first.**

`scripts/winget_manifest.py` writes the manifests from that zip:

```bash
scripts/winget_manifest.py v0.20.0 crete-0.20.0-windows-x86_64.zip out/
# -> out/manifests/a/abksh/crete/0.20.0/abksh.crete{,.installer,.locale.en-US}.yaml
```

The zip is installed as two portable commands, `crete` and `crete-gui`, with
`ArchiveBinariesDependOnPath: true`: winget puts the extracted folder on PATH
instead of symlinking the exes, because `crete-gui` loads `fonts\` from beside
its own executable and a symlinked launch would look beside the link.

## First submission (by hand, once)

A new package is reviewed by a winget moderator, usually within a few days.

1. Download the release zip from Codeberg (not a Jenkins artifact: the hash
   must be of the published bytes) and run the script above.
2. On a Windows machine, check it installs from the local manifests:
   ```powershell
   winget settings --enable LocalManifestFiles     # once, as admin
   winget validate --manifest out\manifests\a\abksh\crete\0.20.0
   winget install --manifest out\manifests\a\abksh\crete\0.20.0
   crete --version; crete-gui                      # fonts must be Archivo, not ImGui's default
   winget uninstall abksh.crete
   ```
3. Fork `microsoft/winget-pkgs` on GitHub, copy `out/manifests/...` into the
   same path in the fork, commit, and open a pull request titled
   `New package: abksh.crete version 0.20.0`. Fill in its checklist.

## Later versions

Once the package exists, updates can be automated from the release job with
[Komac](https://github.com/russellbanks/Komac) (runs on Linux, so on `bee`):
`komac update abksh.crete --version X.Y.Z --urls <zip url> --submit`, with a
GitHub token (classic, `public_repo` scope) as a Jenkins credential. Not wired
up yet.
