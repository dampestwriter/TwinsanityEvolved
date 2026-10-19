// app/launcher/host_tools.cpp -- host detection and the portable tool paths.
#include "host_tools.h"
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
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
    const QString vs = visualStudio();
    if (!vs.isEmpty()) {
        if (description) *description = QStringLiteral("Visual Studio with the C++ Clang tools (%1)").arg(QDir::toNativeSeparators(vs));
        return true;
    }
    if (description) *description = QStringLiteral("Visual Studio 2022 Build Tools with the C++ Clang tools are not installed");
    return false;
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
#elif defined(Q_OS_WIN)
    // winget runs the Visual Studio installer (it asks for administrator approval): the C++ workload with the Clang
    // tools the build uses. Without winget, the download page.
    const QString winget = which("winget");
    if (!winget.isEmpty() && QProcess::startDetached(winget, {"install", "--id", "Microsoft.VisualStudio.2022.BuildTools", "--exact",
            "--accept-package-agreements", "--accept-source-agreements", "--override",
            "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang "
            "--add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.Component.Git --includeRecommended"}))
        return true;
    if (QProcess::startDetached("cmd.exe", {"/c", "start", "", "https://visualstudio.microsoft.com/visual-cpp-build-tools/"})) return true;
    if (error) *error = QStringLiteral("the Visual Studio Build Tools installer could not be started");
    return false;
#else
    if (error) *error = QStringLiteral("no system installer for a compiler on this platform");
    return false;
#endif
}

QString HostTools::visualStudio() const {
#ifdef Q_OS_WIN
    if (vsChecked_) return vsCache_;
    vsChecked_ = true;
    const QString vswhere = QProcessEnvironment::systemEnvironment().value("ProgramFiles(x86)") + "/Microsoft Visual Studio/Installer/vswhere.exe";
    if (!QFileInfo(vswhere).exists()) return QString();
    const QString path = runForOutput(vswhere, {"-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
                                                "-property", "installationPath"}, 30000).section('\n', 0, 0).trimmed();
    if (path.isEmpty() || !QFileInfo(path + "/VC/Tools/Llvm/x64/bin/clang-cl.exe").exists()) return QString();
    vsCache_ = QDir::cleanPath(path);
    return vsCache_;
#else
    return QString();
#endif
}

QProcessEnvironment HostTools::buildEnvironment() const {
    const QProcessEnvironment system = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_WIN
    const QString vs = visualStudio();
    if (vs.isEmpty()) return system;
    // VsDevCmd prints the environment it built through `set`
    QProcess p;
    p.setProgram("cmd.exe");
    p.setNativeArguments(QStringLiteral("/d /c \"\"%1\" -arch=x64 -host_arch=x64 -no_logo >nul && set\"")
                             .arg(QDir::toNativeSeparators(vs + "/Common7/Tools/VsDevCmd.bat")));
    p.start();
    if (!p.waitForFinished(120000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return system;
    QProcessEnvironment dev;
    const QStringList lines = QString::fromLocal8Bit(p.readAllStandardOutput()).split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const int eq = line.indexOf('=');
        if (eq > 0) dev.insert(line.left(eq), line.mid(eq + 1));
    }
    if (!dev.contains("INCLUDE")) return system;
    dev.insert("PATH", QDir::toNativeSeparators(vs + "/VC/Tools/Llvm/x64/bin") + ";" + dev.value("PATH"));
    dev.remove("VCPKG_ROOT");   // Visual Studio's bundled vcpkg; the build uses the launcher's own checkout
    return dev;
#else
    return system;
#endif
}

QString HostTools::git() const {
#ifdef Q_OS_MACOS
    // /usr/bin/git is a shim that offers the Command Line Tools when they are missing; only count it with a compiler.
    if (!hasCompiler()) return QString();
#endif
    const QString onPath = which("git");
    if (!onPath.isEmpty()) return onPath;
#ifdef Q_OS_WIN
    // Visual Studio carries its own Git for Windows
    const QString vs = visualStudio();
    const QString bundled = vs + "/Common7/IDE/CommonExtensions/Microsoft/TeamFoundation/Team Explorer/Git/cmd/git.exe";
    if (!vs.isEmpty() && QFileInfo(bundled).isExecutable()) return bundled;
#endif
    return QString();
}

QString HostTools::toolVersion(const QString& exe) {
    const QRegularExpressionMatch m = QRegularExpression("(\\d+)\\.(\\d+)(?:\\.(\\d+))?").match(runForOutput(exe, {"--version"}, 15000));
    return m.hasMatch() ? m.captured(0) : QString();
}

namespace {
bool atLeast(const QString& version, int major, int minor) {
    const QStringList p = version.split('.');
    if (p.size() < 2) return false;
    return p[0].toInt() > major || (p[0].toInt() == major && p[1].toInt() >= minor);
}
}  // namespace

QString HostTools::systemCmake() const {
    if (cmakeChecked_) return cmakeCache_;
    cmakeChecked_ = true;
    QStringList candidates = {which("cmake")};
#ifdef Q_OS_WIN
    const QString vs = visualStudio();
    if (!vs.isEmpty()) candidates << vs + "/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe";
#elif defined(Q_OS_MACOS)
    candidates << "/opt/homebrew/bin/cmake" << "/usr/local/bin/cmake" << "/Applications/CMake.app/Contents/bin/cmake";
#endif
    for (const QString& c : candidates) {
        if (c.isEmpty() || !QFileInfo(c).isExecutable()) continue;
        if (atLeast(toolVersion(c), 3, 24)) { cmakeCache_ = c; break; }   // the presets and WHOLE_ARCHIVE need 3.24
    }
    return cmakeCache_;
}

QString HostTools::systemNinja() const {
    if (ninjaChecked_) return ninjaCache_;
    ninjaChecked_ = true;
    QStringList candidates = {which("ninja")};
#ifdef Q_OS_WIN
    const QString vs = visualStudio();
    if (!vs.isEmpty()) candidates << vs + "/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe";
#elif defined(Q_OS_MACOS)
    candidates << "/opt/homebrew/bin/ninja" << "/usr/local/bin/ninja";
#endif
    for (const QString& c : candidates) {
        if (c.isEmpty() || !QFileInfo(c).isExecutable()) continue;
        if (atLeast(toolVersion(c), 1, 10)) { ninjaCache_ = c; break; }
    }
    return ninjaCache_;
}

bool HostTools::promptPythonInstall(QString* error) const {
#ifdef Q_OS_WIN
    const QString winget = which("winget");
    if (!winget.isEmpty() && QProcess::startDetached(winget, {"install", "--id", "Python.Python.3.12", "--exact", "--scope", "user",
                                                              "--accept-package-agreements", "--accept-source-agreements"}))
        return true;
    if (QProcess::startDetached("cmd.exe", {"/c", "start", "", "https://www.python.org/downloads/windows/"})) return true;
    if (error) *error = QStringLiteral("the Python installer could not be started");
    return false;
#else
    if (error) *error = QStringLiteral("Python comes with the developer tools of the first step");
    return false;
#endif
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
    if (const DownloadPin* pin = DownloadPins::find("cmake")) {
        const QString p = portableCmakeDir() + "/" + pin->executable;
        if (QFileInfo(p).isExecutable()) return p;
    }
    return systemCmake();
}

QString HostTools::ninja() const {
    if (const DownloadPin* pin = DownloadPins::find("ninja")) {
        const QString p = portableNinjaDir() + "/" + pin->executable;
        if (QFileInfo(p).isExecutable()) return p;
    }
    return systemNinja();
}

}  // namespace ps2r::launcher
