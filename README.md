# PS5 TabShifter (Universal FW 5.00 – 13.60)

A lightweight on-console utility and web interface for jailbroken PlayStation 5 consoles (FW 5.00 through 13.60) that gives you complete control over your PS5 Home Screen layout.

---

### Features

* **Move Apps Between Games and Media:** Keep emulators, homebrew, and utilities in the Media tab (or vice versa). Preserves native PS4 (CUSA), PS5 (PPSA), and FPKG entitlements alongside category types (0, 61440, 65536) so games never get locked.
* **Universal Firmware Engine:** Dual-schema database engine supporting both legacy field_list arrays (FW 5.00–10.xx) and modern flat JSON (FW 11.00–13.60). Updates all database layers simultaneously with proper Prospero concept bitmasks (1162 for Games, 2236 for Media).
* **Multi-User Profile Sync:** Automatically detects and updates all local user profiles on the console, keeping home screen arrangements uniform across all accounts.
* **Persistent Rest Mode Bloatware Cleaner:** Hides stock PSN bloatware (PlayStation Store, PS Plus, Remote Play, Share Play, Live TV) and stops them from returning after waking from Rest Mode or cold reboots.
* **Pin Icons to Home Screen:** Pin or unpin any app to lock your favorite titles onto the primary home bar so they never get pushed into the Game Library.
* **No-Scroll-Jump Web UI:** Viewport stays locked in place while queueing changes. Includes instant search, quick filter pills (All, Games, Media, Hidden, PS5, PS4, Homebrew), and a 1-click **Smart Tidy** button.
* **One-Click Padlock Repair:** Built-in recovery tool to instantly repair concept bitmasks across your entire library if another tool caused padlock icons on your games.
* **In-App Updater & Local `.elf` Upload:** Automatically checks GitHub Releases for new updates, displays release notes, and lets you install `TabShifter.elf` directly through the Web UI (**Upload .elf**) without FTP.
* **100% On-Console Operation & Clean Uninstall:** Automatically installs the TabShifter ("TS") launcher tile on your Media tab (`TBSH00001`). Supports native Home Screen **Options -> Delete** and a Web UI **Uninstall** button that permanently removes the tile without touching your app layout or backups.

---

### Why TabShifter?

On modern PS5 firmwares, Sony splits dashboard data across two synchronized databases:

* `/system_data/priv/mms/appinfo.db`
* `/system_data/priv/mms/app.db`

Modifying only one database causes the system shell to restore stale metadata from cache, corrupt concept bitmasks (causing game padlocks), or revert changes after entering Rest Mode. 

TabShifter updates all database layers simultaneously, applies the correct concept bitmasks (1162 / 2236), and safely reloads SceShellUI in ~1.5 seconds without causing database rebuild prompts.

---

### Installation & Usage

1. Download `TabShifter.elf` from the Latest Release.
2. Place `TabShifter.elf` into `/data/pldmgr/payloads/` on your PS5 (or send it via your favorite payload loader).
3. Run the payload. On first launch, it creates safety database backups and adds the **TabShifter ("TS")** tile to your Media tab.
4. Launch TabShifter from your **Media** tab (or open `http://<PS5_IP>:9095/` in any browser on your network).
5. Queue your changes, then click **Apply & Reload PS5 UI** at the top.

#### Upgrading from v1.3.0 to v1.4.0
> **Note:** Clicking **Update Now** inside **v1.3.0** will fail with **`Error`** because GitHub's release asset downloads (`github.com` / `release-assets.githubusercontent.com`) do not send CORS headers for browser `fetch()`. To upgrade from v1.3.0 to v1.4.0 once:
> * Replace `TabShifter.elf` via Payload Manager or FTP, **or**
> * While `v1.3.0` is open on your PS5, push the downloaded `v1.4.0` binary from a PC/terminal without FTP:
>   ```bash
>   curl -X POST --data-binary @TabShifter.elf http://<PS5_IP>:9095/api/update-payload
>   ```
> * Re-launch the payload on your PS5. From **v1.4.0** onward, you can use the **Download .elf** / **Upload .elf** buttons directly in the Web UI.

---

### Database Recovery & Safety

TabShifter creates safety backups on first initialization:

* `/system_data/priv/mms/app.db.tabshifter.bak`
* `/system_data/priv/mms/appinfo.db.tabshifter.bak`

If you ever need to restore your layout:

* **Fix Padlocks:** Click the orange **Fix Padlocks** button in the Web UI to restore concept bitmasks if game icons are locked.
* **Restore DB Backup:** Click **Restore DB Backup** in the Web UI to revert original tab layout and visibility in-place while keeping PS4 FPKGs and apps installed after the initial backup intact.

---

### Credits & Acknowledgments

* **ps5-payload-dev** for the ps5-payload-sdk toolchain.
* **itsPLK** for ps5-payload-manager and libSceAppInstUtil references.
* **John Törnblom** for ftpsrv and packaging patterns.
