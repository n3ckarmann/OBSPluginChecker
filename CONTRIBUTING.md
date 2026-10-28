# How bugs and releases are handled

**English** · [Deutsch](#deutsch)

1. **Every bug gets an issue.** As soon as a bug is found, whether by a user or while testing, it is filed as an issue in the [public repository](https://github.com/n3ckarmann/OBSPluginChecker/issues): what is affected, the cause once known, a workaround if there is one, and the planned fix.
2. **Bugs are fixed in a new version.** A released version is never changed afterwards. The fix raises the version in `buildspec.json` and ships as the next release.
3. **Every release has release notes.** `release-notes/vX.Y.Z.md` (English and German) lists the changes and links the issues it fixes. The build refuses to draft a release without that file.
4. **The fix closes the issue.** The commit that brings the fix to the public repository says `Fixes #N`, and the issue gets a short comment naming the version that contains the fix.
5. **Test before release.** Each release is tested in a separate test OBS before it goes out. Release builds come only from GitHub Actions.

Found a bug? Please [open an issue](https://github.com/n3ckarmann/OBSPluginChecker/issues/new). Never paste stream keys, `service.json` or other settings into an issue or a log.

## Deutsch

1. **Jeder Fehler wird ein Issue.** Sobald ein Fehler auffällt, ob von Nutzern gemeldet oder beim Testen gefunden, wird er als Issue im [öffentlichen Repository](https://github.com/n3ckarmann/OBSPluginChecker/issues) angelegt: was betroffen ist, die Ursache, sobald sie bekannt ist, ein Workaround, falls es einen gibt, und der geplante Fix.
2. **Fehler werden in einer neuen Version behoben.** Eine veröffentlichte Version wird nachträglich nicht geändert. Der Fix erhöht die Version in `buildspec.json` und kommt mit dem nächsten Release.
3. **Jedes Release hat Release Notes.** `release-notes/vX.Y.Z.md` (Englisch und Deutsch) nennt die Änderungen und verlinkt die behobenen Issues. Ohne diese Datei legt der Build keinen Release-Entwurf an.
4. **Der Fix schließt das Issue.** Der Commit, der den Fix ins öffentliche Repository bringt, enthält `Fixes #N`, und das Issue bekommt einen kurzen Kommentar mit der Version, die den Fix enthält.
5. **Vor dem Release wird getestet.** Jedes Release wird vor der Veröffentlichung in einer separaten Test-OBS geprüft. Release-Builds entstehen nur in GitHub Actions.

Einen Fehler gefunden? Bitte [ein Issue anlegen](https://github.com/n3ckarmann/OBSPluginChecker/issues/new). Niemals Streamkeys, `service.json` oder andere Einstellungen in ein Issue oder ein Log kopieren.
