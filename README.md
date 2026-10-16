# PS5 TabShifter (FW 12.xx – 13.60 Supported)

A standalone on-console payload utility for jailbroken PlayStation 5 consoles (tested across Firmware 12.02, 13.00, 13.40, 13.42, and 13.60) that gives you full control over your PS5 Home Screen layout:

1. **Move apps between `[GAMES]` and `[MEDIA]` tabs:** Keep emulators, stores, and homebrew organized in Media (or vice-versa) while preserving native PS4 (`CUSA`) and PS5 (`PPSA`) category types and `appType` flags so games never get padlocked.
2. **Batch Mode Queueing:** Queue as many tab moves, hides, and pins as you want in one session without getting kicked out of the browser, then click **Apply & Reload PS5 UI** once to apply everything at once.
3. **Pin Icons to Home Screen:** Individual **Pin / Unpin** button next to every app (`pinnedStatus = 1`) to lock your favorite games or homebrew tiles to the Home Screen bar so they never get pushed into the Game Library.
4. **Hide Offline PSN Bloatware:** 1-click toggle to hide or restore `PlayStation Store`, `PlayStation Plus`, `Remote Play`, `Share Play`, and `Live TV` across cold reboots using Sony's native hidden state (`dispLocation = 1`).
5. **Individual System Tile & Welcome Hub Control:** Individually hide or unhide extra system tiles (`Media Gallery`, `Disc Player`, `Music Home`, `Discover`, `Welcome Hub`, and `All Apps`). The `Welcome Hub` (`NPXS40063`) is excluded from the 1-click bloatware sweep by default and preserves its native widget `categoryType` (`67108864`) and `HUBAPP_URI`.
6. **100% On-Console Control:** Automatically installs a native **TabShifter ("TS")** shortcut tile (`TBSH00001`) onto your **Media Tab** that can be opened or uninstalled directly using your DualSense controller.

---

## Why TabShifter?

On PS5 Firmware 12.xx and 13.xx, Sony split the dashboard database into two synchronized files:
* `/system_data/priv/mms/appinfo.db` (`tbl_appinfo` and `tbl_conceptinfo`)
* `/system_data/priv/mms/app.db` (`tbl_contentinfo`, `tbl_conceptmetadata`, and per-user `tbl_iconinfo_*` / `tbl_concepticoninfo_*` tables)

Older database switchers fail on newer firmwares because modifying `app.db` alone causes `SceShellCore` to overwrite changes using stale cache from `appinfo.db` or RAM. TabShifter updates all four database layers simultaneously, applies the proper category bitmasks (`1162` for Games vs `2236` for Media) and native hidden states (`dispLocation = 1`), preserves original `appType` values (`1` for retail/fpkg games, `5` for homebrew), and gracefully reloads `SceShellUI` in ~1.5 seconds without triggering database rebuild prompts.

---

## Installation & Usage

1. Download **`TabShifter.elf`** from the [Latest Release](../../releases/latest).
2. Place `TabShifter.elf` into `/data/pldmgr/payloads/` on your PS5 (or load it via your preferred payload launcher).
3. Run `TabShifter.elf`. It automatically creates untouched `.bak` database backups and registers the **TabShifter ("TS")** app tile onto your **Media Tab**.
4. Open **TabShifter** straight from your PS5 Media Tab (or visit `http://<PS5_IP>:9095/` from any browser on your local network).
5. Queue your desired changes (**Move to Media/Games**, **Pin/Unpin**, **Hide/Unhide**, or **Hide PSN Bloatware**), then click **Apply & Reload PS5 UI** at the top of the page.

---

## How to Uninstall

Because the launcher tile is registered through Sony's system installer (`libSceAppInstUtil`) alongside the database, it can be cleanly removed at any time:
* **From the TabShifter Dashboard:** Click the red **Uninstall Tile & Exit** button at the top of the page.
* **From the PS5 Dashboard:** Press **Options** on the TabShifter tile -> Select **Delete**.

> *Note: If you ever want to revert all tab and visibility changes back to stock, click **Restore DB Backup** inside the TabShifter toolbar.*

---

## Credits & Acknowledgments
* **ps5-payload-dev** for the `ps5-payload-sdk` toolchain.
* **itsPLK** for `ps5-payload-manager` and `libSceAppInstUtil` implementation references.
* **John Törnblom** for `ftpsrv` and native installer patterns.
