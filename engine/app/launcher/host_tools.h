// app/launcher/host_tools.h -- what this machine already has and where the launcher's own copies go: the host
// compiler (macOS: the Xcode Command Line Tools), git, python3, and the portable CMake / Ninja under
// Toolchain/. Answers paths; never installs system-wide.
#pragma once
#include <QProcessEnvironment>
#include <QString>
namespace ps2r::launcher {
class AppData;

class HostTools {
public:
    explicit HostTools(const AppData* appData);

    // macOS: the Command Line Tools (or Xcode) are present -- clang and git exist. Windows / Linux: a C++ compiler.
    bool hasCompiler(QString* description = nullptr) const;
    // macOS: asks the system to offer the Command Line Tools (xcode-select --install). Windows: winget installs the
    // Visual Studio 2022 Build Tools with the C++ Clang tools. Returns false when unsupported.
    bool promptCompilerInstall(QString* error) const;
    // Windows: the Visual Studio 2022 (or Build Tools) installation that carries the C++ Clang tools, else empty.
    QString visualStudio() const;
    // The environment the game's build runs in. Windows: that installation's x64 developer environment (clang-cl,
    // the SDK, Ninja on PATH) without its own VCPKG_ROOT; elsewhere the system's.
    QProcessEnvironment buildEnvironment() const;
    QString git() const;       // the git executable or empty
    QString python3() const;   // a python3 of at least 3.8, or empty
    // CMake / Ninja: the launcher's own copy under Toolchain/ when present, else one already on this machine that is new
    // enough (CMake 3.24+, Ninja 1.10+: PATH, Visual Studio's bundled copies, Homebrew), else empty
    QString cmake() const;
    QString ninja() const;
    static QString toolVersion(const QString& exe);   // "3.30.0" from `<exe> --version`, or empty
    // Windows: winget installs Python 3.12. Returns false when unsupported.
    bool promptPythonInstall(QString* error) const;
    QString portableCmakeDir() const;
    QString portableNinjaDir() const;
    static QString which(const QString& name);
    static QString runForOutput(const QString& program, const QStringList& args, int timeoutMs = 10000, int* exitCode = nullptr);

private:
    QString systemCmake() const;
    QString systemNinja() const;
    const AppData* appData_;
    [[maybe_unused]] mutable QString vsCache_;      // Windows only (visualStudio())
    [[maybe_unused]] mutable bool vsChecked_ = false;
    mutable QString cmakeCache_, ninjaCache_;
    mutable bool cmakeChecked_ = false, ninjaChecked_ = false;
};

}  // namespace ps2r::launcher
