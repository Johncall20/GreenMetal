# Contributing

Thank you for looking. This project has had one author and one test machine, so almost any outside input moves it
forward. Issues and pull requests are welcome in English or Portuguese.

## How a change gets in

Nobody but the maintainer can write to this repository, so every change arrives as a pull request and is merged only
after the maintainer has read it.

1. Look at the [open issues](../../issues), especially the ones labelled `help wanted`. Comment on one before you
   start anything large, so two people do not do the same work.
2. Fork the repository and make your change on a branch in your fork.
3. Open a pull request against `main`. The template asks what you tested and on which card.
4. The maintainer reviews it, may ask for changes, and merges it. A pull request that cannot be tested on the
   reference machine may wait until someone with the right card confirms it.

No code yet? A [card report](../../issues/new/choose) is a contribution too.

**Em português:** só o mantenedor escreve neste repositório. Toda mudança chega como pull request a partir de um fork e
só entra depois que ele revisar e aprovar. Veja as [issues abertas](../../issues), comente na que quiser pegar, faça a
mudança num branch do seu fork e abra o pull request contra `main`. Pode escrever em português.

## The most useful thing you can do

Try it on a card that is not a GTX 960 and say where it stopped. A report is useful even when nothing works.
Include:

- the card and its PCI ID (`ioreg -l | grep -i "device-id"` or System Information);
- macOS version and build, and your boot-args;
- which Web Driver package is installed, and the versions of the Lilu, WhateverGreen and AMFIPass kexts;
- the versions of the four kexts from this project (`kmutil showloaded | grep maxwell`);
- what you ran, what you expected and what happened;
- for a panic, the panic report. `keepsyms=1 debug=0x100` in boot-args makes it readable.

`libnvrm387` logs every request it sends to the resource manager when run with `NVRM387_LOG=1`. For anything below
Metal, that log is usually the fastest way to see what the card refused.

## Before you run anything

Have a way back that does not depend on the screen:

1. a USB stick with a copy of your working EFI, tested;
2. a bootable clone of the system volume;
3. `ssh` enabled on the test machine, and a second computer to use it from.

`kext/deploy/RECUPERACAO.txt` and `kext/troca/LEIA-ME.txt` (both in Portuguese) describe the rollback paths the author
uses. The scripts in `kext/` were written for one machine and assume the tree is at `~/maxwell_metal`. Read a script
before you run it with `sudo`.

## What must stay out of the repository, issues and pull requests

- **No NVIDIA or Apple binaries**, and nothing extracted from them: no kexts, no firmware, no disassembly listings, no
  decompiler output. Describing an interface in your own words, as a struct layout or a table of offsets, is fine.
  Pasting someone else's code is not.
- **No serial numbers or machine identifiers.** `ioreg` dumps, panic reports and EDID blobs contain them. Trim before
  you paste.
- **No passwords, keys or tokens**, including in scripts.

The `.gitignore` is a whitelist for this reason: only source and text files in the listed directories can be added.
If your change needs a new file type or directory, add it there deliberately.

## Code

- Match the file you are editing. Most of the kext code follows the style of the NullMoth sources it was ported from.
- Files that came from NullMoth keep their copyright header and their licence. Say in the pull request which upstream
  version your change was made against.
- Do not send the Mesa patch, or parts of it, to Mesa from this repository. Mesa has its own contribution rules,
  including for AI-assisted code. See [NOTICE.md](NOTICE.md).
- If an AI assistant wrote a meaningful part of your change, say so in the pull request.
- State what you tested and on which card. "Builds, not run" is a perfectly acceptable thing to say.

## Licence of contributions

By contributing you agree that your contribution is under the licence of the directory it lands in, as mapped in
[LICENSE](LICENSE).
