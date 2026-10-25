// PortShell::Get(): the first loaded mod's shell.bin, read once. See port_shell_assets.h.
#include "port_shell_assets.h"

#include <fstream>
#include <memory>

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_shell_file.h"
#include "port_strings.h"

namespace PortShell {
const Assets* Get() {
  static bool sTried = false;
  static std::unique_ptr<Assets> sAssets;
  if (sTried) {
    return sAssets.get();
  }
  sTried = true;
  const std::string path = PortMods::ShellPath();
  if (path.empty()) {
    return nullptr;
  }
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  const std::vector<uint8_t> data = port::ReadAll(in);
  auto assets = std::make_unique<Assets>();
  std::string error;
  if (!in && data.empty()) {
    error = "cannot read the file";
  } else if (!Parse(data.data(), data.size(), *assets, error)) {
    // error set
  } else {
    sAssets = std::move(assets);
    return sAssets.get();
  }
  PortLog::Write("shell: %s: %s\n", path.c_str(), error.c_str());
  return nullptr;
}
}  // namespace PortShell
