// app/launcher/host_tools.cpp -- host detection and the portable tool paths.
#include "host_tools.h"
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include "app_data.h"
#include "download_pins.h"
namespace ps2r::launcher {

HostTools::HostTools(const AppData* appData) : appData_(appData) {}

QString HostTools::which(const QString& name) { return QStandardPaths::findExecutable(name); }

QString HostTools::runForOutput(const QString& program, const QStringList& args, int timeoutMs, int* exitCode) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(timeoutMs) || !p.waitForFinished(timeoutMs)) {
        if (exitCode) *exitCode = -1;
        return QString();
    }
    if (exitCode) *exitCode = p.exitCode();
    return QString::fromUtf8(p.readAll()).trimmed();
}

bool HostTools::hasCompiler(QString* description) const {
#ifdef Q_OS_MACOS
    // xcode-select -p answers the active developer directory; the clang inside it must exist (a stale path after an
    // uninstall still answers).
    int code = -1;
    const QString dir = runForOutput("/usr/bin/xcode-select", {"-p"}, 10000, &code);
    if (code == 0 && !dir.isEmpty()) {
        for (const QString& clang : {dir + "/usr/bin/clang", dir + "/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang"}) {
            if (QFileInfo(clang).isExecutable()) {
                if (description) {
                    const QString v = runForOutput(clang, {"--version"}).section('\n', 0, 0);
                    *description = dir.contains("CommandLineTools") ? QStringLiteral("Xcode Command Line Tools found (%1)").arg(v)
                                                                      : QStringLiteral("Xcode found at %1 (%2)").arg(dir, v);
                }
                return true;
            }
        }
    }
    if (description) *description = QStringLiteral("the Xcode Command Line Tools are not installed");
    return false;
#elif defined(Q_OS_WIN)
    const QString cl = which("clang-cl");
    if (description) *description = cl.isEmpty() ? QStringLiteral("no clang-cl on this machine") : QStringLiteral("clang-cl at %1").arg(cl);
    return !cl.isEmpty();
#else
    const QString cc = which("c++");
    if (description) *description = cc.isEmpty() ? QStringLiteral("no C++ compiler on this machine") : QStringLiteral("compiler at %1").arg(cc);
    return !cc.isEmpty();
#endif
}

bool HostTools::promptCompilerInstall(QString* error) const {
#ifdef Q_OS_MACOS
    // The system's own dialog offers the Command Line Tools; the launcher waits for the user and re-checks.
    if (QProcess::startDetached("/usr/bin/xcode-select", {"--install"})) return true;
    if (error) *error = QStringLiteral("xcode-select --install could not be started");
    return false;
#else
    if (error) *error = QStringLiteral("no system installer for a compiler on this platform");
    return false;
#endif
}

QString HostTools::git() const {
#ifdef Q_OS_MACOS
    // /usr/bin/git is a shim that offers the Command Line Tools when they are missing; only count it with a compiler.
    if (!hasCompiler()) return QString();
#endif
    return which("git");
}

QString HostTools::python3() const {
    for (const QString& candidate : {which("python3"), QString("/usr/bin/python3"), which("python")}) {
        if (candidate.isEmpty()) continue;
        int code = -1;
        const QString v = runForOutput(candidate, {"-c", "import sys; print(sys.version_info[0], sys.version_info[1])"}, 15000, &code);
        const QStringList parts = v.split(' ');
        if (code == 0 && parts.size() == 2 && parts[0].toInt() == 3 && parts[1].toInt() >= 8) return candidate;
    }
    return QString();
}

QString HostTools::portableCmakeDir() const { return appData_->toolchainDir() + "/cmake"; }
QString HostTools::portableNinjaDir() const { return appData_->toolchainDir() + "/ninja"; }

QString HostTools::cmake() const {
    const DownloadPin* pin = DownloadPins::find("cmake");
    if (!pin) return QString();
    const QString p = portableCmakeDir() + "/" + pin->executable;
    return QFileInfo(p).isExecutable() ? p : QString();
}

QString HostTools::ninja() const {
    const DownloadPin* pin = DownloadPins::find("ninja");
    if (!pin) return QString();
    const QString p = portableNinjaDir() + "/" + pin->executable;
    return QFileInfo(p).isExecutable() ? p : QString();
}

}  // namespace ps2r::launcher
