#include "clang/Index/IndexDataStore.h"
#include "clang/Index/IndexUnitReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"

#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::sys;
using namespace clang::index;

static cl::opt<std::string> IndexStore(cl::Positional, cl::Required,
                                       cl::desc("<indexstore>"));

// Helper function to use consistent output. Uses `stdout` to ensure the output
// is greppable, or redirectable to file (separate from API/system errors).
static void logMissingFile(StringRef unitName, StringRef key, StringRef path) {
  outs() << unitName << ": " << key << ": " << path << "\n";
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  std::string storeError{};
  auto store = IndexDataStore::create(IndexStore, storeError);
  if (not store) {
    errs() << "error: failed to open indexstore " << IndexStore << " -- "
           << storeError << "\n";
    return EXIT_FAILURE;
  }

  std::vector<std::string> unitNames{};
  store->foreachUnitName(false, [&](StringRef unitName) {
    unitNames.push_back(unitName.str());
    return true;
  });

  auto exitStatus = EXIT_SUCCESS;
  for (const auto &unitName : unitNames) {
    std::string readerError;
    auto reader = IndexUnitReader::createWithUnitFilename(unitName, IndexStore,
                                                          readerError);
    if (not reader) {
      exitStatus = EXIT_FAILURE;
      errs() << "error: failed to read unit file " << unitName << " -- "
             << readerError << "\n";
      continue;
    }

    const std::vector<std::pair<std::string, llvm::StringRef>> unitPaths = {
        {"MainFilePath", reader->getMainFilePath()},
        {"SysrootPath", reader->getSysrootPath()},
        {"WorkingDirectory", reader->getWorkingDirectory()},
        // TODO: OutputFile does not need to exist, but its path needs to match
        // the format expected by Xcode. Check the format instead of the
        // existence of the file.
        // {"OutputFile", reader->getOutputFile()},
    };

    for (const auto &pair : unitPaths) {
      const auto &key = pair.first;
      const auto &path = pair.second;
      if (path.empty()) {
        continue;
      }

      if (not fs::exists(path)) {
        exitStatus = EXIT_FAILURE;
        logMissingFile(unitName, key, path);
      }
    }

    reader->foreachDependency([&](const IndexUnitReader::DependencyInfo &info) {
      if (not fs::exists(info.FilePath)) {
        exitStatus = EXIT_FAILURE;
        logMissingFile(unitName, "DependencyPath", info.FilePath);
      }
      return true;
    });

    reader->foreachInclude([&](const IndexUnitReader::IncludeInfo &info) {
      if (not fs::exists(info.SourcePath)) {
        exitStatus = EXIT_FAILURE;
        logMissingFile(unitName, "IncludeSourcePath", info.SourcePath);
      }
      if (not fs::exists(info.TargetPath)) {
        exitStatus = EXIT_FAILURE;
        logMissingFile(unitName, "IncludeTargetPath", info.TargetPath);
      }
      return true;
    });
  }

  return exitStatus;
}
