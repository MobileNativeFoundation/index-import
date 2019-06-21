#include "clang/Basic/FileManager.h"
#include "clang/Index/IndexUnitReader.h"
#include "clang/Index/IndexUnitWriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <copyfile.h>

using namespace llvm;
using namespace llvm::sys;
using namespace clang;
using namespace clang::index;
using namespace clang::index::writer;

static cl::list<std::string> PathRemaps("remap", cl::OneOrMore,
                                        cl::desc("Path remapping substitution"),
                                        cl::value_desc("regex=replacement"));
static cl::alias PathRemapsAlias("r", cl::aliasopt(PathRemaps));

static cl::opt<std::string> InputIndexPath(cl::Positional, cl::Required,
                                           cl::desc("<input-indexstore>"));

static cl::opt<std::string> OutputIndexPath(cl::Positional, cl::Required,
                                            cl::desc("<output-indexstore>"));

static cl::opt<bool> Verbose("verbose",
                             cl::desc("Print path remapping results"));
static cl::alias VerboseAlias("V", cl::aliasopt(Verbose));

struct Remapper {
public:
  // Parse the the path remapping command line flags. This converts strings of
  // "X=Y" into a (regex, string) pair. Another way of looking at it: each remap
  // is equivalent to the s/pattern/replacement/ operator.
  static Remapper createFromCommandLine(const cl::list<std::string> &remaps) {
    Remapper remapper;
    for (const auto &remap : remaps) {
      auto divider = remap.find('=');
      std::regex pattern{remap.substr(0, divider)};
      auto replacement = remap.substr(divider + 1);
      remapper.addRemap(pattern, replacement);
    }
    return remapper;
  }

  std::string remap(const std::string &input) const {
    for (const auto &remap : this->_remaps) {
      const auto &pattern = std::get<std::regex>(remap);
      const auto &replacement = std::get<std::string>(remap);

      std::smatch match;
      if (std::regex_search(input, match, pattern)) {
        // I haven't seen this design in other regex APIs, and is worth some
        // explanation. The replacement string is conceptually a format string.
        // The format() function takes no explicit arguments, instead it gets
        // the values from the match object.
        auto substitution = match.format(replacement);
        return match.prefix().str() + substitution + match.suffix().str();
      }
    }

    // No patterns matched, return the input unaltered.
    return input;
  }

private:
  void addRemap(const std::regex &pattern, const std::string &replacement) {
    this->_remaps.emplace_back(pattern, replacement);
  }

  std::vector<std::pair<std::regex, std::string>> _remaps;
};

// Helper for working with index::writer::OpaqueModule. Provides the following:
//   1. Storage for module name StringRef values
//   2. Function to store module names, and return an OpaqueModule handle
//   3. Implementation of ModuleInfoWriterCallback
struct ModuleNameScope {
  // Stores a copy of `moduleName` and returns a handle.
  OpaqueModule getReference(StringRef moduleName) {
    const auto insertion = this->_moduleNames.insert(moduleName);
    return &(*insertion.first);
  }

  // Implementation of ModuleInfoWriterCallback, which is an unusual API. When
  // adding dependencies to Units, the module name is passed not as a string,
  // but instead as an opaque pointer. This callback then maps the opaque
  // pointer to a module name string.
  static ModuleInfo getModuleInfo(OpaqueModule ref, SmallVectorImpl<char> &) {
    // In this implementation, `mod` is a pointer into `_moduleNames`.
    auto moduleName = static_cast<const StringRef *>(ref);
    return {*moduleName};
  }

private:
  std::set<StringRef> _moduleNames;
};

static IndexUnitWriter remapUnit(const std::unique_ptr<IndexUnitReader> &reader,
                                 const Remapper &remapper, FileManager &fileMgr,
                                 ModuleNameScope &moduleNames) {
  // The set of remapped paths.
  auto workingDir = remapper.remap(reader->getWorkingDirectory());
  auto outputFile = remapper.remap(reader->getOutputFile());
  auto mainFilePath = remapper.remap(reader->getMainFilePath());
  auto sysrootPath = remapper.remap(reader->getSysrootPath());

  if (Verbose) {
    llvm::outs() << "MainFilePath: " << mainFilePath << "\n"
                 << "OutputFile: " << outputFile << "\n"
                 << "WorkingDir: " << workingDir << "\n"
                 << "SysrootPath: " << sysrootPath << "\n";
  }

  auto &fsOpts = fileMgr.getFileSystemOpts();
  fsOpts.WorkingDir = workingDir;

  auto writer = IndexUnitWriter(
      fileMgr, OutputIndexPath, reader->getProviderIdentifier(),
      reader->getProviderVersion(), outputFile, reader->getModuleName(),
      fileMgr.getFile(mainFilePath), reader->isSystemUnit(),
      reader->isModuleUnit(), reader->isDebugCompilation(), reader->getTarget(),
      sysrootPath, moduleNames.getModuleInfo);

  reader->foreachDependency([&](const IndexUnitReader::DependencyInfo &info) {
    const auto name = info.UnitOrRecordName;
    const auto moduleNameRef = moduleNames.getReference(info.ModuleName);
    const auto isSystem = info.IsSystem;

    const auto filePath = remapper.remap(info.FilePath);
    auto file = fileMgr.getFile(filePath);
    // The unit may reference a file that does not exist, for example if a build
    // was cleaned. This can cause assertions, or lead to missing file paths in
    // the unit file. When a file does not exist, fall back to getVirtualFile(),
    // which accepts missing files.
    if (not file) {
      file = fileMgr.getVirtualFile(filePath, /*size*/ 0, /*mod time*/ 0);
    }

    if (Verbose) {
      llvm::outs() << "DependencyFilePath: " << filePath << "\n";
    }

    switch (info.Kind) {
    case IndexUnitReader::DependencyKind::Unit: {
      // The UnitOrRecordName from the input is not used. This is because the
      // unit name must be computed from the new (remapped) file path.
      //
      // However, a name is only computed if the input has a name. If the
      // input does not have a name, then don't write a name to the output.
      SmallString<128> unitName;
      if (name != "") {
        writer.getUnitNameForOutputFile(filePath, unitName);
        if (Verbose) {
          llvm::outs() << "DependencyUnitName: " << unitName << "\n";
        }
      }

      writer.addUnitDependency(unitName, file, isSystem, moduleNameRef);
      break;
    }
    case IndexUnitReader::DependencyKind::Record:
      writer.addRecordFile(name, file, isSystem, moduleNameRef);
      break;
    case IndexUnitReader::DependencyKind::File:
      writer.addFileDependency(file, isSystem, moduleNameRef);
      break;
    }
    return true;
  });

  reader->foreachInclude([&](const IndexUnitReader::IncludeInfo &info) {
    // Note this isn't revelevnt to Swift.
    writer.addInclude(fileMgr.getFile(info.SourcePath), info.SourceLine,
                      fileMgr.getFile(info.TargetPath));
    return true;
  });

  return writer;
}

static bool cloneRecords(StringRef recordsDirectory,
                         const std::string &inputIndexPath,
                         const std::string &outputIndexPath) {
  bool success = true;

  std::error_code dirError;
  fs::recursive_directory_iterator dir{recordsDirectory, dirError};
  fs::recursive_directory_iterator end;
  for (; dir != end && !dirError; dir.increment(dirError)) {
    const auto status = dir->status();
    if (status.getError()) {
      success = false;
      llvm::errs() << "error: Could not access file status of path "
                   << dir->path() << "\n";
      continue;
    }

    auto inputPath = dir->path();
    SmallString<128> outputPath{inputPath};
    path::replace_path_prefix(outputPath, inputIndexPath, outputIndexPath);

    std::error_code failed;
    if (status->type() == fs::file_type::directory_file) {
      if (not fs::exists(outputPath)) {
        failed = fs::create_directory(outputPath);
      }
    } else if (status->type() == fs::file_type::regular_file) {
      // Two record files of the same name are guaranteed to have the same
      // contents (because they include a hash of their contents in their
      // file name). If the destination record file already exists, it
      // doesn't need to be cloned or copied.
      if (not fs::exists(outputPath)) {
        failed = fs::copy_file(inputPath, outputPath);
      }
    }

    if (failed) {
      success = false;
      llvm::errs() << "error: " << strerror(errno) << "\n"
                   << "\tcould not copy record file: " << inputPath << "\n";
    }
  }

  if (dirError) {
    success = false;
    llvm::errs() << "error: aborted while reading from records directory: "
                 << dirError.message() << "\n";
  }

  return success;
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  auto remapper = Remapper::createFromCommandLine(PathRemaps);

  std::string initOutputIndexError;
  if (IndexUnitWriter::initIndexDirectory(OutputIndexPath,
                                          initOutputIndexError)) {
    llvm::errs() << "error: failed to initialize index store; "
                 << initOutputIndexError << "\n";
    return EXIT_FAILURE;
  }

  SmallString<256> unitDirectory;
  path::append(unitDirectory, InputIndexPath, "v5", "units");
  SmallString<256> recordsDirectory;
  path::append(recordsDirectory, InputIndexPath, "v5", "records");

  if (not fs::is_directory(unitDirectory) ||
      not fs::is_directory(recordsDirectory)) {
    llvm::errs() << "error: invalid index store directory " << InputIndexPath
                 << "\n";
    return EXIT_FAILURE;
  }

  bool success = true;

  if (not cloneRecords(recordsDirectory, InputIndexPath, OutputIndexPath)) {
    success = false;
  }

  FileSystemOptions fsOpts;
  FileManager fileMgr{fsOpts};

  std::error_code dirError;
  fs::directory_iterator dir{unitDirectory, dirError};
  fs::directory_iterator end;
  while (dir != end && !dirError) {
    const auto unitPath = dir->path();
    dir.increment(dirError);

    if (unitPath.empty()) {
      // The directory iterator returns a single empty path, ignore it.
      continue;
    }

    std::string unitReadError;
    auto reader = IndexUnitReader::createWithFilePath(unitPath, unitReadError);
    if (not reader) {
      llvm::errs() << "error: failed to read unit file " << unitPath << "\n"
                   << unitReadError;
      success = false;
      continue;
    }

    ModuleNameScope moduleNames;
    auto writer = remapUnit(reader, remapper, fileMgr, moduleNames);

    std::string unitWriteError;
    if (writer.write(unitWriteError)) {
      llvm::errs() << "error: failed to write index store; " << unitWriteError
                   << "\n";
      success = false;
    }
  }

  if (dirError) {
    llvm::errs() << "error: aborted while reading from unit directory: "
                 << dirError.message() << "\n";
    success = false;
  }

  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
