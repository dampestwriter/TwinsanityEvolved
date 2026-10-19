// app/launcher/launcher_app.cpp -- the model wired together.
#include "launcher_app.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcessEnvironment>
#include "app_data.h"
#include "build_runner.h"
#include "game_library.h"
#include "game_process.h"
#include "ini_settings.h"
#include "installer.h"
#include "iso_fingerprint.h"
#include "title_manifest.h"
namespace ps2r::launcher {

namespace {
bool copyFolder(const QString& from, const QString& to) {
    QDir src(from);
    if (!src.exists()) return false;
    QDir().mkpath(to);
    for (const QFileInfo& fi : src.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString dest = to + "/" + fi.fileName();
        if (fi.isDir()) copyFolder(fi.absoluteFilePath(), dest);
        else if (!QFileInfo::exists(dest)) QFile::copy(fi.absoluteFilePath(), dest);
    }
    return true;
}
}  // namespace

LauncherApp::LauncherApp(QObject* parent) : QObject(parent) {}

LauncherApp::~LauncherApp() {
    delete settings_;
    delete manifest_;
    delete appData_;
}

QStringList LauncherApp::manifestSearchPaths() {
    QStringList dirs;
    const QString env = QProcessEnvironment::systemEnvironment().value("PS2R_LAUNCHER_MANIFEST");
    if (!env.isEmpty()) dirs << env;
    const QString app = QCoreApplication::applicationDirPath();
    dirs << app + "/../Resources/titles" << app + "/titles" << app + "/../titles" << app + "/../../titles";
    return dirs;
}

bool LauncherApp::loadManifest(const QString& explicitPath, QString* error) {
    QStringList candidates;
    if (!explicitPath.isEmpty()) candidates << explicitPath;
    for (const QString& p : manifestSearchPaths()) {
        const QFileInfo fi(p);
        if (fi.isFile()) candidates << p;
        else if (fi.isDir()) for (const QString& f : QDir(p).entryList({"*.json"}, QDir::Files, QDir::Name)) candidates << p + "/" + f;
    }
    QString firstError;
    for (const QString& c : candidates) {
        TitleManifest* m = new TitleManifest();
        QString err;
        if (m->load(c, &err)) {
            manifest_ = m;
            break;
        }
        if (firstError.isEmpty()) firstError = err;
        delete m;
    }
    if (!manifest_) {
        if (error) *error = candidates.isEmpty() ? QStringLiteral("No title manifest was found beside the application (titles/*.json).") : firstError;
        return false;
    }
    appData_ = new AppData(manifest_->appDataFolder());
    QString err;
    appData_->ensureFolders(&err);
    library_ = new GameLibrary(appData_, this);
    installer_ = new Installer(appData_, manifest_, this);
    builder_ = new BuildRunner(appData_, manifest_, installer_, this);
    game_ = new GameProcess(manifest_, this);
    scanner_ = new DiscScanner(this);
    settings_ = new IniSettings(settingsFilePath());
    seedFolders();
    connect(scanner_, &DiscScanner::found, this, [this](const DiscFound& d) { library_->add(d.path, d.fingerprintFile, d.sizeBytes); });
    connect(installer_, &Installer::stepChanged, this, [this](int) { emit stateChanged(); });
    connect(installer_, &Installer::finished, this, [this](bool) { emit stateChanged(); });
    connect(builder_, &BuildRunner::finished, this, [this](const QString&, bool, const BuildRecord&) { emit stateChanged(); });
    connect(game_, &GameProcess::finished, this, [this](bool, qint64 seconds) {
        if (!playingIso_.isEmpty()) library_->recordPlayEnd(playingIso_, seconds);
        playingIso_.clear();
        emit stateChanged();
    });
    connect(library_, &GameLibrary::changed, this, &LauncherApp::stateChanged);
    return true;
}

void LauncherApp::seedFolders() {
    for (const FolderSpec& f : manifest_->folders()) QDir().mkpath(folder(f.id));
}

QString LauncherApp::folder(const QString& id) const {
    const QString chosen = appData_->value("folder." + id).toString();
    if (!chosen.isEmpty()) return chosen;
    for (const FolderSpec& f : manifest_->folders()) {
        if (f.id == id) return appData_->expand(f.defaultPath, appData_->value("folder.games").toString());
    }
    if (id == "games") return appData_->defaultGamesDir();
    if (id == "mods") return appData_->defaultModsDir();
    return QString();
}

void LauncherApp::setFolder(const QString& id, const QString& path) {
    appData_->setValue("folder." + id, path);
    appData_->save();
    QDir().mkpath(path);
    emit foldersChanged();
}

QString LauncherApp::settingsFilePath() const {
    const QString f = manifest_->settingsFile();
    return appData_->expand(f.isEmpty() ? QStringLiteral("${app_data}/ps2r.ini") : f);
}

QString LauncherApp::settingValue(const QString& key) const {
    QString v = settings_->get(key);
    if (!v.isEmpty()) return v;
    for (const SettingsRowSpec& r : manifest_->allSettingsRows()) if (r.key == key) return r.defaultValue;
    return QString();
}

bool LauncherApp::setSettingValue(const QString& key, const QString& value, QString* error) {
    if (!settings_->set(key, value, error)) return false;
    emit settingsChanged(key);
    return true;
}

void LauncherApp::seedModsFolder() {
    for (const FolderSpec& f : manifest_->folders()) {
        if (f.id != "mods" || f.seedFromSource.isEmpty()) continue;
        const QString src = installer_->sourceDir() + "/" + f.seedFromSource;
        if (QDir(src).exists()) copyFolder(src, folder("mods"));
    }
}

void LauncherApp::scan() {
    QStringList folders;
    for (const QString& f : manifest_->scanFolders()) folders << appData_->expand(f, folder("games"));
    folders << folder("games");
    folders.removeDuplicates();
    QStringList files;
    for (const DiscFingerprint& fp : manifest_->allFingerprints()) files << fp.file;
    scanner_->start(folders, manifest_->scanRemovable(), manifest_->scanMaxDepth(), manifest_->discExtensions(), files);
}

bool LauncherApp::addDisc(const QString& path, QString* reason) {
    QStringList files;
    for (const DiscFingerprint& fp : manifest_->allFingerprints()) files << fp.file;
    QString matched, err;
    switch (IsoFingerprint::identify(path, files, &matched, &err)) {
    case IsoFingerprint::Match:
        library_->add(path, matched, QFileInfo(path).size());
        return true;
    case IsoFingerprint::NotTheGame:
        if (reason) *reason = QStringLiteral("This is not a %1 disc image. \"%2\" has no %3 on it. Choose a disc image of the game you own.")
                                  .arg(manifest_->name(), path, files.join(" or "));
        return false;
    case IsoFingerprint::NotAnIso:
        if (reason) *reason = QStringLiteral("\"%1\" cannot be read as a disc image (%2). It may be a damaged download or a different format "
                                             "(a .bin with a .cue is not supported yet).").arg(path, err);
        return false;
    case IsoFingerprint::Unreadable:
    default:
        if (reason) *reason = QStringLiteral("\"%1\" could not be opened (%2).").arg(path, err);
        return false;
    }
}

QString LauncherApp::cardSubtitle(const TitleManifest& m, const QString& fingerprintFile, const QString& path) {
    QString region, id;
    for (const DiscFingerprint& fp : m.allFingerprints()) {
        if (fp.file.compare(fingerprintFile, Qt::CaseInsensitive) == 0) { region = fp.region; id = fp.id; break; }
    }
    QStringList parts;
    if (!region.isEmpty()) parts << region;
    if (!id.isEmpty()) parts << id;
    parts << QFileInfo(path).fileName();
    return parts.join(QStringLiteral(" · "));
}

QString LauncherApp::hostTarget() const { return BuildRunner::hostTargetId(); }

LauncherApp::Action LauncherApp::nextAction(const QString& isoPath) const {
    const GameEntry* g = library_->game(isoPath);
    if (!g || g->missing) return Missing;
    if (game_->running() && game_->isoPath() == isoPath) return Playing;
    if (!installer_->complete()) return Install;
    if (!builder_->upToDate(hostTarget())) return Build;
    return Play;
}

void LauncherApp::install() { installer_->start(); }

void LauncherApp::build(const QString& targetId) {
    seedModsFolder();
    builder_->start(targetId);
}

bool LauncherApp::play(const QString& isoPath, QString* error) {
    if (game_->running()) { if (error) *error = QStringLiteral("a game is already running"); return false; }
    const BuildRecord r = builder_->record(hostTarget());
    if (!r.ok) { if (error) *error = QStringLiteral("no build for this machine yet"); return false; }
    seedModsFolder();
    QStringList extra;
    const QString mc = folder("memory_cards");
    if (!game_->start(r.binaryPath, isoPath, settingsFilePath(), mc, extra, appData_->logPath("play"), error)) return false;
    playingIso_ = isoPath;
    library_->recordPlayStart(isoPath);
    emit stateChanged();
    return true;
}

void LauncherApp::stopGame() { game_->stop(); }

QString LauncherApp::version() const {
#ifdef PS2R_LAUNCHER_VERSION
    return QStringLiteral(PS2R_LAUNCHER_VERSION);
#else
    return QStringLiteral("0.1");
#endif
}

QString LauncherApp::engineCommit() const {
    // the engine commit the launcher was built from, stamped by the packaging into Resources/BUILD-ID when present
    for (const QString& p : {QCoreApplication::applicationDirPath() + "/../Resources/BUILD-ID", QCoreApplication::applicationDirPath() + "/BUILD-ID"}) {
        QFile f(p);
        if (f.open(QIODevice::ReadOnly)) {
            for (const QString& l : QString::fromUtf8(f.readAll()).split('\n')) if (l.startsWith("engine")) return l.section(' ', 1).simplified().left(12);
        }
    }
    return QString();
}

}  // namespace ps2r::launcher
