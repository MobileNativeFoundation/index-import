#include "clang/Index/IndexUnitReader.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace clang::index;

static cl::list<std::string> UnitPaths(cl::Positional, cl::OneOrMore,
                                       cl::desc("<index-units>"));

static const char *_dependencyKindName(IndexUnitReader::DependencyKind kind) {
  switch (kind) {
  case IndexUnitReader::DependencyKind::Unit:
    return "Unit";
  case IndexUnitReader::DependencyKind::Record:
    return "Record";
  case IndexUnitReader::DependencyKind::File:
    return "File";
  }
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  for (const auto &unitPath : UnitPaths) {
    std::string readerError;
    auto reader = IndexUnitReader::createWithFilePath(unitPath, readerError);
    if (not reader) {
      errs() << "error: failed to read unit file " << unitPath << " -- "
             << readerError << "\n";
      return EXIT_FAILURE;
    }

    // Output using a yaml format.

    outs() << "---\n";
    outs() << "# " << unitPath << "\n";
    outs() << "WorkingDirectory: " << reader->getWorkingDirectory() << "\n"
           << "MainFilePath: " << reader->getMainFilePath() << "\n"
           << "OutputFile: " << reader->getOutputFile() << "\n"
           << "ModuleName: " << reader->getModuleName() << "\n"
           << "IsSystemUnit: " << reader->isSystemUnit() << "\n"
           << "IsModuleUnit: " << reader->isModuleUnit() << "\n"
           << "IsDebugCompilation: " << reader->isDebugCompilation() << "\n"
           << "CompilationTarget: " << reader->getTarget() << "\n"
           << "SysrootPath: " << reader->getSysrootPath() << "\n"
           << "ProviderIdentifier: " << reader->getProviderIdentifier() << "\n"
           << "ProviderVersion: " << reader->getProviderVersion() << "\n";

    bool needsHeader = true;
    reader->foreachDependency([&](const IndexUnitReader::DependencyInfo &info) {
      if (needsHeader) {
        outs() << "Dependencies:\n";
        needsHeader = false;
      }

      outs() << "\tDependencyKind: " << _dependencyKindName(info.Kind) << "\n"
             << "\tIsSystem: " << info.IsSystem << "\n"
             << "\tUnitOrRecordName: " << info.UnitOrRecordName << "\n"
             << "\tFilePath: " << info.FilePath << "\n"
             << "\tModuleName: " << info.ModuleName << "\n";
      return true;
    });

    needsHeader = true;
    reader->foreachInclude([&](const IndexUnitReader::IncludeInfo &info) {
      if (needsHeader) {
        outs() << "Includes:\n";
        needsHeader = false;
      }

      outs() << "\tSourcePath: " << info.SourcePath << "\n"
             << "\tSourceLine: " << info.SourceLine << "\n"
             << "\tTargetPath: " << info.TargetPath << "\n";
      return true;
    });
  }

  return EXIT_SUCCESS;
}
