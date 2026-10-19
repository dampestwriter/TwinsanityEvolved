// app/launcher/host_tools.h -- what this machine already has and where the launcher's own copies go: the host
// compiler (macOS: the Xcode Command Line Tools), git, python3, and the portable CMake / Ninja under
// Toolchain/. Answers paths; never installs system-wide.
#pragma once
#include <QString>
namespace ps2r::launcher {
class AppData;

class HostTools {
public:
    explicit HostTools(const AppData* appData);

    // macOS: the Command Line Tools (or Xcode) are present -- clang and git exist. Windows / Linux: a C++ compiler.
    bool hasCompiler(QString* description = nullptr) const;
    // macOS: asks the system to offer the Command Line Tools (xcode-select --install). Returns false when unsupported.
    bool promptCompilerInstall(QString* error) const;
    QString git() const;       // the git executable or empty
    QString python3() const;   // a python3 of at least 3.8, or empty
    QString cmake() const;     // the portable one under Toolchain/cmake when present, else empty
    QString ninja() const;     // the portable one under Toolchain/ninja when present, else empty
    QString portableCmakeDir() const;
    QString portableNinjaDir() const;
    static QString which(const QString& name);
    static QString runForOutput(const QString& program, const QStringList& args, int timeoutMs = 10000, int* exitCode = nullptr);

private:
    const AppData* appData_;
};

}  // namespace ps2r::launcher
