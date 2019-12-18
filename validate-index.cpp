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
      errs() << "error: failed to read unit file " << unitName << " -- "
             << readerError << "\n";
      return EXIT_FAILURE;
    }

    const std::vector<std::pair<std::string, std::string>> unitPaths = {
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
        outs() << unitName << ": " << key << ": " << path << "\n";
      }
    }

    reader->foreachDependency([&](const IndexUnitReader::DependencyInfo &info) {
      if (not fs::exists(info.FilePath)) {
        exitStatus = EXIT_FAILURE;
        outs() << unitName << ": DependencyPath: " << info.FilePath << "\n";
      }
      return true;
    });

    reader->foreachInclude([&](const IndexUnitReader::IncludeInfo &info) {
      if (not fs::exists(info.SourcePath)) {
        exitStatus = EXIT_FAILURE;
        outs() << unitName << ": IncludeSourcePath: " << info.SourcePath
               << "\n";
      }
      if (not fs::exists(info.TargetPath)) {
        exitStatus = EXIT_FAILURE;
        outs() << unitName << ": IncludeTargetPath: " << info.TargetPath
               << "\n";
      }
      return true;
    });
  }

  return exitStatus;
}
