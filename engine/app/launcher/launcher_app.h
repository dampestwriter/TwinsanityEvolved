// app/launcher/launcher_app.h -- the launcher's model: the manifest, the app-data folder, the discs, the installer,
// the builds and the running game, wired together; the window shows it and the test driver drives it through the
// same methods the buttons call.
#pragma once
#include <QObject>
#include <QStringList>
#include "disc_scanner.h"
namespace ps2r::launcher {
class AppData;
class BuildRunner;
class GameLibrary;
class GameProcess;
class IniSettings;
class Installer;
class TitleManifest;

class LauncherApp : public QObject {
    Q_OBJECT
public:
    explicit LauncherApp(QObject* parent = nullptr);
    ~LauncherApp() override;

    // Finds the manifest: an explicit path, else PS2R_LAUNCHER_MANIFEST, else titles/*.json beside the application
    // (Contents/Resources/titles on macOS). False with *error when none is found.
    bool loadManifest(const QString& explicitPath, QString* error);
    static QStringList manifestSearchPaths();

    const TitleManifest* manifest() const { return manifest_; }
    AppData* appData() const { return appData_; }
    GameLibrary* library() const { return library_; }
    Installer* installer() const { return installer_; }
    BuildRunner* builder() const { return builder_; }
    GameProcess* game() const { return game_; }
    DiscScanner* scanner() const { return scanner_; }

    // folders (games / mods / memory_cards): the manifest's default unless the user changed it
    QString folder(const QString& id) const;
    void setFolder(const QString& id, const QString& path);
    QString settingsFilePath() const;
    IniSettings* settings() const { return settings_; }
    QString settingValue(const QString& key) const;   // the file's value or the manifest's default
    bool setSettingValue(const QString& key, const QString& value, QString* error = nullptr);
    void seedModsFolder();

    // discs
    void scan();
    // Adds a dropped / chosen file after fingerprinting it; returns false with a plain-English *reason.
    bool addDisc(const QString& path, QString* reason);
    static QString cardSubtitle(const TitleManifest& m, const QString& fingerprintFile, const QString& path);

    // the three actions a card offers; which one applies is nextAction()
    enum Action { Install, Build, Play, Playing, Missing };
    Action nextAction(const QString& isoPath) const;
    QString hostTarget() const;
    void install();
    void build(const QString& targetId);
    bool play(const QString& isoPath, QString* error);
    void stopGame();

    QString version() const;
    QString engineCommit() const;

signals:
    void foldersChanged();
    void settingsChanged(const QString& key);
    void stateChanged();   // anything a card shows

private:
    void seedFolders();
    TitleManifest* manifest_ = nullptr;
    AppData* appData_ = nullptr;
    GameLibrary* library_ = nullptr;
    Installer* installer_ = nullptr;
    BuildRunner* builder_ = nullptr;
    GameProcess* game_ = nullptr;
    DiscScanner* scanner_ = nullptr;
    IniSettings* settings_ = nullptr;
    QString playingIso_;
};

}  // namespace ps2r::launcher
