// app/launcher/installer.cpp -- the first-run checklist, step by step.
#include "installer.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include "app_data.h"
#include "download_pins.h"
#include "host_tools.h"
#include "process_runner.h"
#include "title_manifest.h"
namespace ps2r::launcher {

namespace {
const char* kVdpmUrl = "https://github.com/vitasdk/vdpm";
const QStringList kVitaPackages = {"sdl2", "vitaGL", "ffmpeg"};

QString readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}
}  // namespace

Installer::Installer(AppData* appData, const TitleManifest* manifest, QObject* parent)
    : QObject(parent), appData_(appData), manifest_(manifest), host_(new HostTools(appData)) {
    steps_ = {
        {"compiler", "Developer tools", InstallStep::Pending, "", "", 0, false, true},
        {"tools", "Build tools", InstallStep::Pending, "", "", 0, false, false},
        {"source", "Game source", InstallStep::Pending, "", "", 0, false, false},
        {"toolchain", "Game compiler", InstallStep::Pending, "", "", 0, false, false},
        {"media", "Media libraries", InstallStep::Pending, "", "", 0, false, false},
        {"python", "Python", InstallStep::Pending, "", "", 0, false, false},
        {"vitasdk", "PlayStation Vita tools", InstallStep::Pending, "only when the Vita target is used", "", 0, true, false},
    };
    proc_ = new ProcessRunner(this);
    connect(proc_, &ProcessRunner::line, this, &Installer::logLine);
    connect(proc_, &ProcessRunner::finished, this, [this](bool ok, int) {
        if (onProcDone_) { auto f = onProcDone_; onProcDone_ = nullptr; f(ok); }
    });
    loadInstalled();
    downloadedBytes_ = qint64(installed_.value("downloaded_bytes").toDouble());
    // what is already on disk shows as done before anything runs
    for (InstallStep& s : steps_) {
        if (s.optional) { s.state = vitaSdkInstalled() ? InstallStep::Done : InstallStep::Skipped; continue; }
    }
    if (upToDate()) {
        for (InstallStep& s : steps_) if (!s.optional) s.state = InstallStep::Done;
        steps_[0].detail = installed_.value("compiler").toString();
        steps_[1].detail = QStringLiteral("CMake %1 and Ninja %2").arg(installed_.value("cmake").toString(), installed_.value("ninja").toString());
        steps_[2].detail = QStringLiteral("%1 at %2").arg(manifest_->name(), installed_.value("source_commit").toString().left(7));
        steps_[3].detail = installed_.value("toolchain_detail").toString();
        steps_[4].detail = installed_.value("media_detail").toString();
        steps_[5].detail = installed_.value("python_detail").toString();
        for (InstallStep& s : steps_) s.bytes = installed_.value("bytes_" + s.id).toDouble();
    }
}

Installer::~Installer() { delete host_; }

QString Installer::sourceDir() const { return appData_->sourceDir(manifest_->id()); }
QString Installer::sourceCommit() const { return installed_.value("source_commit").toString(); }
QString Installer::toolchainPinsHash() const { return installed_.value("toolchain_pins").toString(); }
QString Installer::ffmpegPrefix() const { return appData_->toolchainDir() + "/ffmpeg"; }
QString Installer::vitaSdkDir() const { return appData_->toolchainDir() + "/vitasdk"; }
bool Installer::vitaSdkInstalled() const {
    // the toolchain AND the packages the Vita build needs (SDL2, vitaGL, FFmpeg)
    return QFileInfo(vitaSdkDir() + "/bin/arm-vita-eabi-gcc").isExecutable() && QFileInfo::exists(vitaSdkDir() + "/arm-vita-eabi/include/SDL2/SDL.h") &&
           QFileInfo::exists(vitaSdkDir() + "/arm-vita-eabi/lib/libvitaGL.a") && QFileInfo::exists(vitaSdkDir() + "/arm-vita-eabi/lib/libavcodec.a");
}
QString Installer::currentLogPath() const { return proc_ ? proc_->logPath() : QString(); }

bool Installer::complete() const {
    for (const InstallStep& s : steps_) if (!s.optional && s.state != InstallStep::Done) return false;
    return true;
}

QString Installer::pinsFileHash() const {
    return DownloadPins::sha256Of(sourceDir() + "/" + manifest_->toolchainPins());
}

bool Installer::sourceUrlIsLocal() const {
    const QString url = manifest_->sourceUrl();
    return url.startsWith('/') || url.startsWith("file://") || QFileInfo(url).isDir();
}

bool Installer::upToDate() const {
    if (installed_.isEmpty()) return false;
    const DownloadPin* cmake = DownloadPins::find("cmake");
    const DownloadPin* ninja = DownloadPins::find("ninja");
    if (!cmake || !ninja) return false;
    if (installed_.value("cmake").toString() != cmake->version || host_->cmake().isEmpty()) return false;
    if (installed_.value("ninja").toString() != ninja->version || host_->ninja().isEmpty()) return false;
    if (!host_->hasCompiler()) return false;
    if (!QFileInfo(sourceDir() + "/CMakeLists.txt").exists()) return false;
    if (installed_.value("source_commit").toString().isEmpty()) return false;
    if (installed_.value("source_url").toString() != manifest_->sourceUrl()) return false;
    const QString pins = pinsFileHash();
    if (pins.isEmpty() || installed_.value("toolchain_pins").toString() != pins) return false;
    const QString tc = sourceDir() + "/" + manifest_->toolchainFolder();
    if (!QFileInfo(tc + "/wasi-sdk/bin/clang").isExecutable() && !QFileInfo(tc + "/wasi-sdk/bin/clang.exe").isExecutable()) return false;
    if (!manifest_->ffmpegRecipe().isEmpty() && !QFileInfo(ffmpegPrefix() + "/lib/libavcodec.a").exists()) return false;
    if (manifest_->needsPython() && installed_.value("python").toString().isEmpty()) return false;
    return true;
}

void Installer::loadInstalled() {
    const QJsonDocument doc = QJsonDocument::fromJson(readFile(appData_->toolchainDir() + "/installed.json").toUtf8());
    installed_ = doc.isObject() ? doc.object() : QJsonObject();
}

void Installer::saveInstalled() {
    QDir().mkpath(appData_->toolchainDir());
    QSaveFile f(appData_->toolchainDir() + "/installed.json");
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(installed_).toJson(QJsonDocument::Indented));
    f.commit();
}

qint64 Installer::folderBytes(const QString& dir) {
    qint64 total = 0;
    QDirIterator it(dir, QDir::Files | QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); total += it.fileInfo().size(); }
    return total;
}

QString Installer::bytesText(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("—");
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg((bytes + 512) / 1024);
    if (bytes < 1024ll * 1024 * 1024) return QStringLiteral("%1 MB").arg((bytes + 512 * 1024) / (1024 * 1024));
    return QStringLiteral("%1 GB").arg(double(bytes) / (1024.0 * 1024 * 1024), 0, 'f', 1);
}

qint64 Installer::totalOnDiskBytes() const {
    return folderBytes(appData_->toolchainDir()) + folderBytes(appData_->sourcesDir());
}

void Installer::start() {
    if (running_) return;
    QString err;
    appData_->ensureFolders(&err);
    cancelled_ = false;
    running_ = true;
    runFrom(0);
}

void Installer::retry(int stepIndex) {
    if (running_ || stepIndex < 0 || stepIndex >= steps_.size()) return;
    steps_[stepIndex].state = InstallStep::Pending;
    steps_[stepIndex].reason.clear();
    emit stepChanged(stepIndex);
    cancelled_ = false;
    running_ = true;
    runFrom(stepIndex);
}

void Installer::startVitaSdk() {
    if (running_) return;
    vitaRequested_ = true;
    const int i = steps_.size() - 1;
    steps_[i].state = InstallStep::Pending;
    emit stepChanged(i);
    running_ = true;
    runStep(i);
}

void Installer::cancel() {
    cancelled_ = true;
    if (proc_) proc_->cancel();
}

void Installer::promptCompilerInstall() {
    QString err;
    host_->promptCompilerInstall(&err);
    if (!err.isEmpty()) emit logLine(err);
}

void Installer::runFrom(int index) {
    for (int i = index; i < steps_.size(); ++i) {
        InstallStep& s = steps_[i];
        if (s.optional && !vitaRequested_) continue;
        if (s.state == InstallStep::Done) continue;
        runStep(i);
        return;
    }
    running_ = false;
    current_ = -1;
    emit progress(QString(), 0, 0, false);
    emit finished(complete());
}

void Installer::runStep(int i) {
    current_ = i;
    steps_[i].state = InstallStep::Running;
    steps_[i].reason.clear();
    emit stepChanged(i);
    stepTimer_.start();
    const QString id = steps_[i].id;
    if (id == "compiler") stepCompiler(i);
    else if (id == "tools") stepTools(i);
    else if (id == "source") stepSource(i);
    else if (id == "toolchain") stepToolchain(i);
    else if (id == "media") stepMedia(i);
    else if (id == "python") stepPython(i);
    else if (id == "vitasdk") stepVitaSdk(i);
}

void Installer::finishStep(int i, bool ok, const QString& detail, const QString& reason, qint64 bytes) {
    InstallStep& s = steps_[i];
    s.state = ok ? InstallStep::Done : InstallStep::Failed;
    s.detail = detail;
    s.reason = ok ? QString() : reason;
    if (bytes >= 0) s.bytes = bytes;
    installed_["bytes_" + s.id] = double(s.bytes);
    installed_["downloaded_bytes"] = double(downloadedBytes_);
    saveInstalled();
    emit stepChanged(i);
    if (!ok || cancelled_) {
        running_ = false;
        current_ = -1;
        emit progress(QString(), 0, 0, false);
        emit finished(false);
        return;
    }
    if (s.optional) {   // the Vita step runs alone
        running_ = false;
        current_ = -1;
        emit progress(QString(), 0, 0, false);
        emit finished(complete());
        return;
    }
    runFrom(i + 1);
}

void Installer::runProcess(const QString& program, const QStringList& args, const QString& workDir, const QString& logStep,
                           std::function<void(bool)> done) {
    proc_->setWorkingDirectory(workDir);
    proc_->setLogPath(appData_->logPath(logStep));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // the portable tools first on PATH for every child (the title's scripts call cmake / ninja by name)
    QStringList path;
    if (!host_->cmake().isEmpty()) path << QFileInfo(host_->cmake()).absolutePath();
    if (!host_->ninja().isEmpty()) path << QFileInfo(host_->ninja()).absolutePath();
    if (vitaSdkInstalled()) { path << vitaSdkDir() + "/bin"; env.insert("VITASDK", vitaSdkDir()); }
    path << env.value("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    env.insert("PATH", path.join(QDir::listSeparator()));
    env.insert("HOME", AppData::homePath());
    proc_->setEnvironment(env);
    onProcDone_ = std::move(done);
    QString err;
    if (!proc_->start(program, args, &err)) {
        onProcDone_ = nullptr;
        emit logLine(QStringLiteral("could not start %1: %2").arg(program, err));
        done(false);
    }
}

// --- step 1: the host compiler ------------------------------------------------------------------------------------
void Installer::stepCompiler(int i) {
    QString desc;
    if (host_->hasCompiler(&desc)) {
        installed_["compiler"] = desc;
        finishStep(i, true, desc, QString(), 0);
        return;
    }
#ifdef Q_OS_MACOS
    finishStep(i, false, QStringLiteral("not installed"),
               QStringLiteral("The Xcode Command Line Tools are not installed. Press Install to let macOS offer them (about 1 GB, no "
                              "administrator password for the download), then Retry when it has finished."), 0);
#else
    finishStep(i, false, QStringLiteral("not installed"), QStringLiteral("No C++ compiler was found on this machine: %1.").arg(desc), 0);
#endif
}

// --- step 2: portable CMake + Ninja ----------------------------------------------------------------------------------
void Installer::downloadAndUnpack(const QString& tool, const QString& destDir, std::function<void(bool, QString, qint64)> done) {
    const DownloadPin* pin = DownloadPins::find(tool);
    if (!pin) { done(false, QStringLiteral("no pinned %1 for this kind of machine (%2)").arg(tool, DownloadPins::currentHost()), 0); return; }
    const QString exe = destDir + "/" + pin->executable;
    if (QFileInfo(exe).isExecutable()) { done(true, QString(), 0); return; }
    const QString archive = appData_->downloadsDir() + "/" + pin->archive;
    auto unpack = [=] {
        QDir().mkpath(destDir);
        emit progress(QStringLiteral("Unpacking %1 %2").arg(tool, pin->version), 0, 0, true);
        runProcess("tar", {"-xf", archive, "-C", destDir}, destDir, "unpack-" + tool, [=](bool ok) {
            if (!ok || !QFileInfo(exe).exists()) { done(false, QStringLiteral("%1 could not be unpacked (the archive may be damaged; it was deleted so the next try downloads it again)").arg(pin->archive), 0); QFile::remove(archive); return; }
            QFile(exe).setPermissions(QFile(exe).permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup);
            done(true, QString(), QFileInfo(archive).size());
        });
    };
    auto verifyThenUnpack = [=](bool downloaded) {
        const QString got = DownloadPins::sha256Of(archive);
        if (got != pin->sha256) {
            QFile::remove(archive);
            done(false, QStringLiteral("The download of %1 did not match its pinned checksum (got %2…, wanted %3…). It was deleted; "
                                       "Retry downloads it again.").arg(pin->archive, got.left(8), pin->sha256.left(8)), 0);
            return;
        }
        if (downloaded) downloadedBytes_ += QFileInfo(archive).size();
        unpack();
    };
    if (QFileInfo(archive).exists() && DownloadPins::sha256Of(archive) == pin->sha256) { verifyThenUnpack(false); return; }
    QDir().mkpath(appData_->downloadsDir());
    emit progress(QStringLiteral("Downloading %1 %2 (%3)").arg(tool, pin->version, bytesText(pin->approxBytes)), 0, 0, true);
    // curl: on every supported host; -# writes a progress bar whose percentage drives the bar
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(proc_, &ProcessRunner::line, this, [this, tool, pin](const QString& l) {
        static const QRegularExpression pct("(\\d+(?:\\.\\d+)?)%\\s*$");
        const QRegularExpressionMatch m = pct.match(l);
        if (m.hasMatch()) emit progress(QStringLiteral("Downloading %1 %2 (%3)").arg(tool, pin->version, bytesText(pin->approxBytes)), int(m.captured(1).toDouble() * 10), 1000, false);
    });
    runProcess("curl", {"-fL", "--retry", "3", "-#", "-o", archive + ".part", pin->url}, appData_->downloadsDir(), "download-" + tool, [=](bool ok) {
        disconnect(*conn);
        if (!ok) {
            QFile::remove(archive + ".part");
            done(false, QStringLiteral("The download of %1 stopped (%2). Check the network and Retry.").arg(pin->archive, proc_->tail(2).simplified().left(120)), 0);
            return;
        }
        QFile::remove(archive);
        QFile::rename(archive + ".part", archive);
        verifyThenUnpack(true);
    });
}

void Installer::stepTools(int i) {
    const DownloadPin* cmake = DownloadPins::find("cmake");
    const DownloadPin* ninja = DownloadPins::find("ninja");
    if (!cmake || !ninja) { finishStep(i, false, "unsupported", QStringLiteral("No pinned CMake and Ninja exist for this kind of machine (%1).").arg(DownloadPins::currentHost()), 0); return; }
    downloadAndUnpack("cmake", host_->portableCmakeDir(), [=](bool ok, QString reason, qint64 bytes1) {
        if (!ok) { finishStep(i, false, QStringLiteral("CMake could not be installed"), reason, 0); return; }
        downloadAndUnpack("ninja", host_->portableNinjaDir(), [=](bool ok2, QString reason2, qint64 bytes2) {
            if (!ok2) { finishStep(i, false, QStringLiteral("Ninja could not be installed"), reason2, 0); return; }
            installed_["cmake"] = cmake->version;
            installed_["ninja"] = ninja->version;
            const qint64 bytes = folderBytes(host_->portableCmakeDir()) + folderBytes(host_->portableNinjaDir());
            Q_UNUSED(bytes1); Q_UNUSED(bytes2);
            finishStep(i, true, QStringLiteral("CMake %1 and Ninja %2").arg(cmake->version, ninja->version), QString(), bytes);
        });
    });
}

// --- step 3: the title's source at the latest commit of its repository -------------------------------------------
void Installer::stepSource(int i) {
    const QString git = host_->git();
    if (git.isEmpty()) { finishStep(i, false, "no git", QStringLiteral("git was not found; it comes with the developer tools of step 1."), 0); return; }
    const QString dir = sourceDir();
    const QString url = manifest_->sourceUrl();
    if (url.isEmpty()) { finishStep(i, false, "no source", QStringLiteral("The manifest names no source repository."), 0); return; }
    auto finishWithCommit = [=] {
        int code = -1;
        const QString have = HostTools::runForOutput(git, {"-C", dir, "rev-parse", "HEAD"}, 15000, &code);
        if (code != 0 || have.isEmpty()) { finishStep(i, false, "unreadable checkout", QStringLiteral("The source folder exists but is not a usable checkout: %1").arg(dir), 0); return; }
        installed_["source_commit"] = have;
        installed_["source_url"] = url;
        downloadedBytes_ += folderBytes(dir + "/.git");   // what git fetched
        saveInstalled();
        finishStep(i, true, QStringLiteral("%1 at %2").arg(manifest_->name(), have.left(7)), QString(), folderBytes(dir));
    };
    // One repository holds everything the build needs (a title carries its engine and game code as ordinary tracked
    // trees). The source is always the newest commit of the repository's default branch: a first run clones it, a
    // later run fetches that branch's tip and moves the checkout to it (the folder is the launcher's own copy).
    if (QFileInfo(dir + "/.git").exists()) {
#ifndef Q_OS_WIN
        QProcess::execute("chmod", {"-R", "u+w", dir});   // makeSourceReadOnly() left it read-only
#endif
        emit progress(QStringLiteral("Updating the game's source"), 0, 0, true);
        QStringList fetch;
        if (sourceUrlIsLocal()) fetch << "-c" << "protocol.file.allow=always";
        fetch << "-C" << dir << "fetch" << "--quiet" << url << "HEAD";
        runProcess(git, fetch, dir, "source-fetch", [=](bool ok) {
            if (!ok) { finishStep(i, false, "not updated", QStringLiteral("The latest source could not be fetched from %1: %2").arg(url, proc_->tail(3).simplified().left(200)), 0); return; }
            runProcess(git, {"-C", dir, "reset", "--quiet", "--hard", "FETCH_HEAD"}, dir, "source-update", [=](bool moved) {
                if (!moved) { finishStep(i, false, "not updated", QStringLiteral("The source folder could not be moved to the latest commit: %1").arg(proc_->tail(2).simplified().left(160)), 0); return; }
                finishWithCommit();
            });
        });
        return;
    }
    QDir().mkpath(appData_->sourcesDir());
    emit progress(QStringLiteral("Downloading the game's source (%1)").arg(url), 0, 0, true);
    QStringList args;
    if (sourceUrlIsLocal()) args << "-c" << "protocol.file.allow=always";
    args << "clone" << "--quiet" << url << dir;
    runProcess(git, args, appData_->sourcesDir(), "source-clone", [=](bool ok) {
        if (!ok) { finishStep(i, false, "not downloaded", QStringLiteral("The source could not be downloaded from %1: %2").arg(url, proc_->tail(3).simplified().left(200)), 0); return; }
        finishWithCommit();
    });
}

void Installer::checkForSourceUpdate() {
    const QString git = host_->git();
    const QString url = manifest_->sourceUrl();
    const QString have = installed_.value("source_commit").toString();
    if (running_ || git.isEmpty() || url.isEmpty() || have.isEmpty()) return;
    // `git ls-remote <url> HEAD`: one short request for the default branch's tip, off the UI thread; offline or slow
    // simply means no update is offered this time
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [=](int code, QProcess::ExitStatus status) {
        const QString latest = QString::fromUtf8(p->readAllStandardOutput()).section('\t', 0, 0).trimmed();
        p->deleteLater();
        if (status != QProcess::NormalExit || code != 0 || latest.size() < 40 || latest == have || running_) return;
        // the source step and everything built on it run again on the next install action
        for (int s = 0; s < steps_.size(); ++s) {
            if (steps_[s].optional) continue;
            if (steps_[s].id == "source" || steps_[s].id == "toolchain" || steps_[s].id == "media" || steps_[s].id == "python") {
                if (steps_[s].id == "source") steps_[s].detail = QStringLiteral("a newer version is available (%1)").arg(latest.left(7));
                steps_[s].state = InstallStep::Pending;
                emit stepChanged(s);
            }
        }
        emit sourceUpdateAvailable(latest);
    });
    QStringList args;
    if (sourceUrlIsLocal()) args << "-c" << "protocol.file.allow=always";
    args << "ls-remote" << url << "HEAD";
    p->start(git, args);
    QTimer::singleShot(20000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
}

// --- step 4: the title's pinned wasm32 toolchain through its own script -------------------------------------------
void Installer::stepToolchain(int i) {
    const QString dir = sourceDir();
#ifdef Q_OS_WIN
    const QString script = dir + "/" + manifest_->toolchainWindowsScript();
    const QString program = "powershell";
    const QStringList args = {"-ExecutionPolicy", "Bypass", "-File", script};
#else
    const QString script = dir + "/" + manifest_->toolchainScript();
    const QString program = "/bin/bash";
    const QStringList args = {script};
#endif
    if (!QFileInfo(script).exists()) { finishStep(i, false, "no script", QStringLiteral("The source has no %1.").arg(manifest_->toolchainScript()), 0); return; }
    QProcess::execute("chmod", {"-R", "u+w", dir + "/" + manifest_->toolchainFolder()});
    emit progress(QStringLiteral("Downloading the game compiler (checked against the pinned sha256)"), 0, 0, true);
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(proc_, &ProcessRunner::line, this, [this](const QString& l) {
        if (l.contains("downloading")) emit progress(QStringLiteral("Downloading %1").arg(l.section("downloading", 1).trimmed()), 0, 0, true);
        else if (l.contains("verified")) emit progress(QStringLiteral("Verified %1").arg(l.section("verified", 1).trimmed()), 0, 0, true);
    });
    runProcess(program, args, dir, "toolchain", [=](bool ok) {
        disconnect(*conn);
        const QString tc = dir + "/" + manifest_->toolchainFolder();
        if (!ok) {
            QString why = proc_->tail(4).simplified().left(240);
            if (why.contains("MISMATCH")) why = QStringLiteral("a download did not match its pinned sha256 (%1). Nothing was installed from it; Retry downloads it again.").arg(why);
            else if (why.contains("curl") || why.contains("Could not resolve")) why = QStringLiteral("a download stopped (%1). Check the network and Retry.").arg(why);
            finishStep(i, false, "could not be downloaded", QStringLiteral("The game compiler could not be installed: %1").arg(why), 0);
            return;
        }
        const QString pins = pinsFileHash();
        installed_["toolchain_pins"] = pins;
        const QString detail = QStringLiteral("%1, sha256-verified").arg(QDir(tc).entryList(QDir::Dirs | QDir::NoDotAndDotDot).join(", "));
        installed_["toolchain_detail"] = detail;
        const qint64 bytes = folderBytes(tc);
        for (const QFileInfo& fi : QDir(tc).entryInfoList(QDir::Files)) downloadedBytes_ += fi.size();   // the archives the script kept
        makeSourceReadOnly();
        finishStep(i, true, detail, QString(), bytes);
    });
}

void Installer::makeSourceReadOnly() {
#ifndef Q_OS_WIN
    // the files, not the folders: the toolchain folder and the scripts' caches stay writable for the build
    const QString dir = sourceDir();
    QProcess::execute("/bin/sh", {"-c", QStringLiteral("find '%1' -type f -not -path '*/.git/*' -not -path '*/%2/*' -exec chmod a-w {} +")
                                            .arg(dir, manifest_->toolchainFolder())});
#endif
}

// --- step 5: the engine's static media libraries (FFmpeg LGPL; SDL2 is fetched by the build itself) ----------------
void Installer::stepMedia(int i) {
    const QString recipe = manifest_->ffmpegRecipe();
    if (recipe.isEmpty()) { finishStep(i, true, QStringLiteral("none needed"), QString(), 0); return; }
    const QString script = sourceDir() + "/" + recipe;
    if (!QFileInfo(script).exists()) { finishStep(i, false, "no recipe", QStringLiteral("The source has no %1.").arg(recipe), 0); return; }
    const QString prefix = ffmpegPrefix();
    emit progress(QStringLiteral("Building FFmpeg (LGPL, the movie decoder only) — a few minutes"), 0, 0, true);
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(proc_, &ProcessRunner::line, this, [this](const QString& l) {
        if (l.contains("cloning")) emit progress(QStringLiteral("Downloading FFmpeg's source"), 0, 0, true);
        else if (l.contains("configure")) emit progress(QStringLiteral("Configuring FFmpeg (the MPEG decoder only, no GPL parts)"), 0, 0, true);
        else if (l.contains("cached")) emit progress(QStringLiteral("FFmpeg already built"), 0, 0, true);
    });
    runProcess("/bin/bash", {script, prefix}, appData_->toolchainDir(), "media-ffmpeg", [=](bool ok) {
        disconnect(*conn);
        if (!ok || !QFileInfo(prefix + "/lib/libavcodec.a").exists()) {
            finishStep(i, false, "FFmpeg could not be built", QStringLiteral("The movie decoder (FFmpeg) could not be built: %1").arg(proc_->tail(4).simplified().left(240)), 0);
            return;
        }
        downloadedBytes_ += folderBytes(prefix + ".src/.git");   // the shallow clone of FFmpeg's source
        QDir(prefix + ".src").removeRecursively();   // the source tree is not needed once the libraries exist
        const QString tag = readFile(prefix + "/PS2R_FFMPEG_TAG").trimmed();
        const QString detail = QStringLiteral("FFmpeg %1 (LGPL, static); SDL2 is fetched by the build").arg(tag);
        installed_["media_detail"] = detail;
        finishStep(i, true, detail, QString(), folderBytes(prefix));
    });
}

// --- step 6: Python --------------------------------------------------------------------------------------------------
void Installer::stepPython(int i) {
    if (!manifest_->needsPython()) { finishStep(i, true, QStringLiteral("not needed"), QString(), 0); return; }
    const QString py = host_->python3();
    if (py.isEmpty()) {
        finishStep(i, false, "not found", QStringLiteral("No Python 3.8 or newer was found. On macOS it comes with the developer tools of step 1; "
                                                           "on Windows install it from python.org (any 3.8+), then Retry."), 0);
        return;
    }
    const QString v = HostTools::runForOutput(py, {"--version"});
    installed_["python"] = py;
    installed_["python_detail"] = QStringLiteral("%1 found on this machine").arg(v);
    finishStep(i, true, installed_["python_detail"].toString(), QString(), 0);
}

// --- the optional step: VitaSDK into app-data -----------------------------------------------------------------------
void Installer::stepVitaSdk(int i) {
    const QString git = host_->git();
    const QString sdk = vitaSdkDir();
    if (vitaSdkInstalled()) { finishStep(i, true, QStringLiteral("VitaSDK at %1").arg(AppData::shortPath(sdk)), QString(), folderBytes(sdk)); return; }
    if (git.isEmpty()) { finishStep(i, false, "no git", QStringLiteral("git was not found; it comes with the developer tools of step 1."), 0); return; }
    const QString vdpm = appData_->toolchainDir() + "/vdpm";
    auto packages = [=] {
        emit progress(QStringLiteral("Installing the Vita packages (SDL2, vitaGL, FFmpeg)"), 0, 0, true);
        // vdpm is installed inside the SDK by the bootstrap; the clone's copy is the fallback
        runProcess("/bin/bash", {"-c", QStringLiteral("export VITASDK='%1'; export PATH=\"$VITASDK/bin:$PATH\"; cd '%2' && if [ -x \"$VITASDK/bin/vdpm\" ]; then yes 2>/dev/null | \"$VITASDK/bin/vdpm\" install %3; else yes 2>/dev/null | ./vdpm %3; fi").arg(sdk, vdpm, kVitaPackages.join(' '))},
                   vdpm, "vitasdk-packages", [=](bool ok) {
            if (!ok || !vitaSdkInstalled()) { finishStep(i, false, "packages missing", QStringLiteral("The Vita packages could not be installed: %1").arg(proc_->tail(4).simplified().left(240)), 0); return; }
            installed_["vitasdk"] = sdk;
            finishStep(i, true, QStringLiteral("VitaSDK at %1").arg(AppData::shortPath(sdk)), QString(), folderBytes(sdk));
        });
    };
    auto bootstrap = [=] {
        emit progress(QStringLiteral("Downloading the VitaSDK toolchain (about 300 MB)"), 0, 0, true);
        QDir(sdk).removeRecursively();   // the bootstrap wants a new directory
        runProcess("/bin/bash", {"-c", QStringLiteral("export VITASDK='%1'; export PATH=\"$VITASDK/bin:$PATH\"; cd '%2' && ./bootstrap-vitasdk.sh --install-dir \"$VITASDK\"").arg(sdk, vdpm)},
                   vdpm, "vitasdk-bootstrap", [=](bool ok) {
            if (!ok || !QFileInfo(sdk + "/bin/arm-vita-eabi-gcc").isExecutable()) { finishStep(i, false, "not installed", QStringLiteral("The VitaSDK toolchain could not be installed: %1").arg(proc_->tail(4).simplified().left(240)), 0); return; }
            packages();
        });
    };
    if (QFileInfo(sdk + "/bin/arm-vita-eabi-gcc").isExecutable() && QFileInfo(vdpm + "/bootstrap-vitasdk.sh").exists()) { packages(); return; }   // the toolchain is there: the packages only
    if (QFileInfo(vdpm + "/bootstrap-vitasdk.sh").exists()) { bootstrap(); return; }
    emit progress(QStringLiteral("Downloading the VitaSDK package manager"), 0, 0, true);
    runProcess(git, {"clone", "--quiet", "--depth", "1", QString::fromLatin1(kVdpmUrl), vdpm}, appData_->toolchainDir(), "vitasdk-vdpm", [=](bool ok) {
        if (!ok) { finishStep(i, false, "not downloaded", QStringLiteral("The VitaSDK package manager could not be downloaded: %1").arg(proc_->tail(3).simplified().left(200)), 0); return; }
        bootstrap();
    });
}

}  // namespace ps2r::launcher
