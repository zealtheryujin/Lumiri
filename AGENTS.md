# Workspace maintenance

Maintain the existing sources in place. The user wants a single current distribution: `Lumiri/` and `Lumiri.zip`.

Do not create version-named release folders, archives, source copies, or automatic backups unless the user explicitly requests them. Update the existing distribution with `repack.ps1` after building the affected component. Internal application version metadata may still be updated.

Preserve user configuration, custom game entries, and Git metadata. Keep project-owned source code free of comments; retain third-party license notices. Keep build dependencies and tests needed for future development.
