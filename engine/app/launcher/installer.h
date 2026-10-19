// app/launcher/installer.h -- the first run: everything the launcher needs, installed into the app-data folder as a
// checklist of steps (the host compiler, portable CMake + Ninja, the title's source at the latest commit of its
// repository's default branch, the title's
// pinned wasm32 toolchain through its own fetch script, the engine's static media libraries, Python, and on request
// VitaSDK). Every step records its pin in Toolchain/installed.json; a re-run with matching pins does nothing.
#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QVector>
#include <functional>
namespace ps2r::launcher {
class AppData;
class HostTools;
class ProcessRunner;
class TitleManifest;

struct InstallStep {
    enum State { Pending, Running, Done, Failed, Skipped };
    QString id;        // compiler / tools / source / toolchain / media / python / vitasdk
    QString name;      // "Developer tools"
    State state = Pending;
    QString detail;    // the plain-English state ("CMake 3.31.6 and Ninja 1.12.1")
    QString reason;    // the failure, in one sentence
    qint64 bytes = 0;  // downloaded / on disk
    bool optional = false;
    bool canPromptInstall = false;   // the compiler step: the system's own installer can be offered
};

class Installer : public QObject {
    Q_OBJECT
public:
    Installer(AppData* appData, const TitleManifest* manifest, QObject* parent = nullptr);
    ~Installer() override;

    const QVector<InstallStep>& steps() const { return steps_; }
    bool running() const { return running_; }
    bool complete() const;             // every required step Done or Skipped
    bool upToDate() const;             // the pins on disk match the manifest and the pin table -- nothing to run
    QString sourceDir() const;         // Sources/<title id>
    QString sourceCommit() const;      // the commit checked out (from installed.json)
    void checkForSourceUpdate();       // asks the source repository for its latest commit; a newer one re-opens the
                                       // source step (and the steps after it), so the next install action updates
    QString toolchainPinsHash() const; // sha256 of the title's PINS file as installed
    QString ffmpegPrefix() const;      // Toolchain/ffmpeg
    QString vcpkgRoot() const;         // Toolchain/vcpkg (Windows: SDL2 and FFmpeg come from the source's vcpkg.json)
    QString vitaSdkDir() const;        // Toolchain/vitasdk
    bool vitaSdkInstalled() const;
    qint64 totalDownloadedBytes() const { return downloadedBytes_; }
    qint64 totalOnDiskBytes() const;
    QString currentLogPath() const;
    QJsonObject installed() const { return installed_; }

    void start();                      // runs every pending required step in order
    void retry(int stepIndex);         // re-runs one failed step, then continues
    void startVitaSdk();               // the optional step, on request from the Targets page
    void cancel();
    void promptCompilerInstall();      // the compiler step's system installer
    void promptInstall(int stepIndex); // the step's own system installer (the compiler, Python on Windows)

    static qint64 folderBytes(const QString& dir);
    static QString bytesText(qint64 bytes);

signals:
    void stepChanged(int index);
    void progress(const QString& text, int done, int total, bool indeterminate);
    void logLine(const QString& text);
    void finished(bool ok);
    void sourceUpdateAvailable(const QString& commit);

private:
    void runFrom(int index);
    void runStep(int index);
    void finishStep(int index, bool ok, const QString& detail, const QString& reason, qint64 bytes);
    void stepCompiler(int i);
    void stepTools(int i);
    void stepSource(int i);
    void stepToolchain(int i);
    void stepMedia(int i);
    void stepVcpkg(int i);
    QString vcpkgBaseline() const;     // the source's vcpkg.json builtin-baseline
    void stepPython(int i);
    void stepVitaSdk(int i);
    void downloadAndUnpack(const QString& tool, const QString& destDir, std::function<void(bool, QString, qint64)> done);
    void runProcess(const QString& program, const QStringList& args, const QString& workDir, const QString& logStep,
                    std::function<void(bool)> done);
    void makeSourceReadOnly();
    void loadInstalled();
    void saveInstalled();
    bool sourceUrlIsLocal() const;
    QString pinsFileHash() const;

    AppData* appData_;
    const TitleManifest* manifest_;
    HostTools* host_;
    ProcessRunner* proc_ = nullptr;
    std::function<void(bool)> onProcDone_;
    QVector<InstallStep> steps_;
    QJsonObject installed_;
    QElapsedTimer stepTimer_;
    qint64 downloadedBytes_ = 0;
    int current_ = -1;
    bool running_ = false, cancelled_ = false, vitaRequested_ = false;
};

}  // namespace ps2r::launcher
