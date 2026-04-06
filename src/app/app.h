#pragma once

#include <string>
#include <vector>

#include "core/paths.h"
#include "core/store.h"
#include "infrastructure/audit.h"
#include "infrastructure/logger.h"

namespace jkm {

class Application {
public:
    explicit Application(AppPaths paths);

    int Run(const std::vector<std::string>& args);

private:
    int HandleInit(const std::string& operation_id);
    int HandleDeinit(const std::string& operation_id);
    int HandleUninstall(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleInstall(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleRemove(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleRemote(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleVersion(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleDoctor(const std::string& operation_id);
    int HandleStatus(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleList(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleCurrent(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleUse(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleUnuse(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleExec(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleShell(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleLogs(const std::vector<std::string>& args);
    int HandleEnv(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleConfig(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleCache(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleLock(const std::vector<std::string>& args, const std::string& operation_id);
    int HandleSync(const std::vector<std::string>& args, const std::string& operation_id);
    int HandlePlaceholder(const std::string& name, const std::string& operation_id);

    void PrintHelp() const;
    void PrintActiveRuntime(const ActiveRuntime& runtime) const;
    void PrintInstalledRuntime(const InstalledRuntime& runtime) const;
    std::vector<DoctorCheck> BuildDoctorChecks() const;
    std::vector<InstalledRuntime> LoadKnownRuntimes(std::optional<RuntimeType> filter = std::nullopt) const;
    bool EnsureEnvironmentSnapshotCaptured(const std::string& operation_id);
    std::optional<EnvironmentSnapshot> LoadEnvironmentSnapshot() const;

    AppPaths paths_;
    Logger logger_;
    OperationAuditStore audit_store_;
    SettingsStore settings_store_;
    ActiveRuntimeStore active_store_;
    EnvironmentSnapshotStore snapshot_store_;
};

}  // namespace jkm
