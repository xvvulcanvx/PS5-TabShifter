PS5 TabShifter (Universal FW 5.00 – 13.60 Supported)

A lightweight, standalone on-console daemon and web interface for jailbroken PlayStation 5 consoles (FW 5.00 through 13.60) that provides complete control over your PS5 Home Screen layout.
Key Features

    Move Apps Between [GAMES] and [MEDIA] Tabs

        Keep emulators, utilities, and homebrew organized in the Media tab (or vice versa).

        Preserves native PS4 (CUSA), PS5 (PPSA), and FPKG entitlement data alongside category types (0, 61440, 65536) so commercial titles never get padlocked.

    Universal Firmware Engine (Dual-Schema Support)

        Dynamically handles both legacy field_list array JSON (FW 5.00–10.xx) and modern flat JSON (FW 11.00–13.60).

        Simultaneously updates all four database layers across app.db and appinfo.db, enforcing proper Prospero concept bitmasks (1162 for Games, 2236 for Media).

    Multi-User Profile Synchronization

        Automatically discovers and iterates through all local user profiles (tbl_iconinfo_*, tbl_concepticoninfo_*), keeping tab changes and hidden states uniform across every account on the console.

    Persistent Rest Mode & Cold Reboot Bloatware Suppression

        Hides offline stock bloatware (PlayStation Store, PlayStation Plus, Remote Play, Share Play, Live TV) using Sony's native hidden states.

        Sets emsPushedFlag = 0, injects {"deleted":true,"pushed":false} into notification flags, and clears entries from tbl_group_* so hidden tiles never resurface after waking from Rest Mode or cold boots.

    Pin Icons to Home Screen

        Individually pin or unpin any app (pinnedStatus = 1) to anchor your favorite titles to the primary horizontal home bar without risking them being rotated out into the Game Library.

    Web UI Search, Filtering & Queueing

        Instant Search & Filter Pills: Filter your library instantly by All, Games, Media, Hidden, PS5, PS4, or Homebrew.

        In-Place DOM Queueing: No annoying scroll-jumping—the viewport stays locked right where you are as you queue up multiple edits.

        1-Click Smart Tidy: Automatically sweeps all installed homebrew to the Media tab and hides stock PSN bloatware in a single tap.

        One-Click Padlock Repair: Built-in recovery tool to instantly repair concept bitmasks across your entire library if another tool broke your entitlements.

    In-App Self-Updater

        Automatically queries GitHub Releases for newer builds, presents an expandable release notes changelog, and applies atomic binary replacements to /data/ without needing manual file transfer.

    100% On-Console Operation

        Automatically generates procedural "TS" icon assets and registers the TBSH00001 shortcut tile directly on your Media tab.

        Silent background startup checks ensure zero black-screen reload latency when re-running the payload.

Why TabShifter?

Starting around FW 11.40 and carrying into FW 12.xx/13.xx, Sony split dashboard state management across two synchronized databases:

    /system_data/priv/mms/appinfo.db (tbl_appinfo and tbl_conceptinfo)

    /system_data/priv/mms/app.db (tbl_contentinfo, tbl_conceptmetadata, and user-specific tbl_iconinfo_* / tbl_concepticoninfo_* tables)

Legacy homebrew database editors typically only alter app.db or assume a single schema. This causes SceShellCore to restore stale metadata from cache, corrupt concept bitmasks (which triggers padlock icons on retail games), or drop custom arrangements upon waking from Rest Mode.

TabShifter writes across all presentation and concept metadata tables simultaneously, respects individual firmware schema nuances, and gracefully triggers a hot reload of SceShellUI in ~1.5 seconds without causing database panics or triggering system rebuild prompts.
Installation & Usage

    Download TabShifter.elf from the Latest Release.

    Copy TabShifter.elf to /data/pldmgr/payloads/ on your PS5 (or inject it via your preferred network payload loader/Payload Manager).

    Execute the payload. On first launch, it creates untouched .bak safety snapshots of your system databases and places the TabShifter ("TS") launcher tile onto your Media tab.

    Launch TabShifter directly from the Media tab on your TV screen, or open http://<PS5_IP>:9095/ in any browser on your local network (phone, tablet, or PC).

    Queue your desired changes (move tabs, pin titles, hide apps, or hit Smart Tidy), then click Apply & Reload PS5 UI to update your home screen.

Database Recovery & Safety

TabShifter creates safety backups on first initialization:

    /system_data/priv/mms/app.db.tabshifter.bak

    /system_data/priv/mms/appinfo.db.tabshifter.bak

If you ever wish to roll back changes:

    Click Restore DB Backup directly from the top navigation bar inside the TabShifter Web UI. This wipes active SQLite WAL/SHM journals and reinstates your pre-modification database baseline.

    Alternatively, click Fix Padlocks if your game tiles are displaying license locks after running other database modification tools.

Credits & Acknowledgments

    ps5-payload-dev for the ps5-payload-sdk toolchain.

    itsPLK for ps5-payload-manager and libSceAppInstUtil implementation references.

    John Törnblom for ftpsrv and native packaging utilities.
