# PS5 TabShifter (FW 13.60 Supported)

A standalone on-console payload utility for jailbroken PlayStation 5 consoles running Firmware 13.60 that allows you to:
1. **Move apps between `[GAMES]` and `[MEDIA]` tabs** (keep emulators, stores, and homebrew organized in Media, or vice-versa).
2. **Hide useless offline PSN bloatware** (PlayStation Store, PlayStation Plus, Remote Play, Share Play, Welcome Hub, Live TV).
3. **Instantly reload `SceShellUI`** in ~1.5 seconds without a system reboot.
4. **100% on-console control:** Installs a native Home Screen tile (TBSH00001) with a custom "TS" icon that can be opened or uninstalled directly using your DualSense controller.

---

## Why TabShifter?

On PS5 Firmware 13.60, Sony split the dashboard database into two synchronized databases:
* `/system_data/priv/mms/appinfo.db` (`tbl_appinfo` and `tbl_conceptinfo`)
* `/system_data/priv/mms/app.db` (`tbl_contentinfo`, `tbl_conceptmetadata`, and per-user `tbl_iconinfo_*` / `tbl_concepticoninfo_*` tables)

Older database switchers fail on 13.60 because modifying `app.db` alone causes `SceShellCore` to overwrite changes using stale cache from `appinfo.db` or RAM. TabShifter updates all four database layers simultaneously alongside `param.json` manifests, applies the proper 13.60 category bitmasks (1162 for Games vs 2236 for Media), and gracefully restarts `SceShellUI`.

---

## Installation & Usage

1. Download **`TabShifter.elf`** from the [Latest Release](../../releases/latest).
2. Place `TabShifter.elf` into `/data/pldmgr/payloads/` on your PS5 (or load it via your preferred payload launcher).
3. Run `TabShifter.elf`. It will automatically register the native **TabShifter ("TS")** app tile to your **Media Tab**.
4. Open **TabShifter** straight from your PS5 Home Screen to move apps or toggle bloatware with your controller.

---

## How to Uninstall

Because the tile is registered natively through Sony's system installer (`libSceAppInstUtil`), it can be cleanly uninstalled at any time:
* **From the PS5 Dashboard:** Press **Options** on the TabShifter tile -> Select **Delete**.
* **From the TabShifter Dashboard:** Click the red **🗑️ Uninstall Tile & Exit** button.

---

## Credits & Acknowledgments
* **ps5-payload-dev** for the `ps5-payload-sdk` toolchain.
* **itsPLK** for `ps5-payload-manager` and `libSceAppInstUtil` implementation references.
* **John Törnblom** for `ftpsrv` and native installer patterns.
