# OmniAI as a DMB catalog mod

Dead Man's Boots (DMB) loads AI plugins that come as mods listed in its catalog (the contract is
`docs/modders/DMB_Addons.md` on DMB's `dmb` branch). `OmniAI/` here is that mod:

- `mod.json` declares the plugin: `"aiPlugin"` with library `OmniAI`, name `OmniAI`, kind
  `adventure`, and `builtFor`, the exact DMB version the library was built against.
- `ai/OmniAI.dll` is the plugin itself. It is build output, not kept in git: it must be built
  in-tree against the DMB source named in `builtFor` (DMB builds OmniAI in-tree when this
  repository is checked out as its `AI/OmniAI`), because a C++ AI shares classes with the engine.
  DMB ships `tbb12.dll`, so the mod does not carry it.
- `Mods/Learning` is the same learning toggle as in the stock VCMI installer.

In-tree builds define `OMNIAI_IN_TREE`, so without `OMNIAI_DIR` OmniAI keeps its files in the
engine's own user folder (`VCMIDirs::get().userDataPath() / "OmniAI"`); DMB's client also sets
`OMNIAI_DIR` to that folder itself. The stock VCMI build and its installer (`installer/`) are
unchanged.

To list it: zip `OmniAI/` with the built `ai/OmniAI.dll`, publish the zip, and the catalog's
maintainers pin the `ai` folder's hash (`codeSha256`) and add the entry.
