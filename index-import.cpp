#include "clang/Basic/FileManager.h"
#include "clang/Index/IndexUnitReader.h"
#include "clang/Index/IndexUnitWriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <copyfile.h>

#include <dispatch/dispatch.h>

using namespace llvm;
using namespace llvm::sys;
using namespace clang;
using namespace clang::index;
using namespace clang::index::writer;

static cl::list<std::string> PathRemaps("remap", cl::OneOrMore,
                                        cl::desc("Path remapping substitution"),
                                        cl::value_desc("regex=replacement"));
static cl::alias PathRemapsAlias("r", cl::aliasopt(PathRemaps));

static cl::list<std::string> InputIndexPaths(cl::Positional, cl::OneOrMore,
                                             cl::desc("<input-indexstores>"));

static cl::opt<std::string> OutputIndexPath(cl::Positional, cl::Required,
                                            cl::desc("<output-indexstore>"));

static cl::opt<bool> Verbose("verbose",
                             cl::desc("Print path remapping results"));
static cl::alias VerboseAlias("V", cl::aliasopt(Verbose));

static cl::opt<unsigned> ParallelStride(
    "parallel-stride", cl::init(32),
    cl::desc(
        "Stride for parallel operations. 0 to disable parallel processing"));

struct Remapper {
public:
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
                                 ModuleNameScope &moduleNames,
                                 std::ostream *outs) {
  // The set of remapped paths.
  auto workingDir = remapper.remap(reader->getWorkingDirectory());
  auto outputFile = remapper.remap(reader->getOutputFile());
  auto mainFilePath = remapper.remap(reader->getMainFilePath());
  auto sysrootPath = remapper.remap(reader->getSysrootPath());

  if (Verbose) {
    *outs << "MainFilePath: " << mainFilePath << "\n"
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
      *outs << "DependencyFilePath: " << filePath << "\n";
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
          *outs << "DependencyUnitName: " << unitName.c_str() << "\n";
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

static bool cloneRecord(StringRef from, StringRef to) {
  // Two record files of the same name are guaranteed to have the same
  // contents, because the filename contains a hash of its contents. If the
  // destination record file already exists, no action needs to be taken.
  if (fs::exists(to)) {
    return true;
  }

  // With swift-5.1, fs::copy_file supports cloning. Until then, use copyfile.
  int failed =
      copyfile(from.str().c_str(), to.str().c_str(), nullptr, COPYFILE_CLONE);

  // In parallel mode we might be racing against other threads trying to create
  // the same record. To handle this, just silently drop file exists errors.
  return (failed == 0 || errno == EEXIST);
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
      errs() << "error: Could not access file status of path " << dir->path()
             << "\n";
      continue;
    }

    auto inputPath = dir->path();
    SmallString<128> outputPath{inputPath};
    path::replace_path_prefix(outputPath, inputIndexPath, outputIndexPath);

    if (status->type() == fs::file_type::directory_file) {
      std::error_code failed = fs::create_directory(outputPath);
      if (failed && failed != std::errc::file_exists) {
        success = false;
        errs() << "Could not create directory `" << outputPath
               << "`: " << failed.message() << "\n";
      }
    } else if (status->type() == fs::file_type::regular_file) {
      if (not cloneRecord(inputPath, outputPath)) {
        success = false;
        errs() << "Could not copy record file from `" << inputPath << "` to `"
               << outputPath << "`: " << strerror(errno) << "\n";
      }
    }
  }

  if (dirError) {
    success = false;
    errs() << "error: aborted while reading from records directory: "
           << dirError.message() << "\n";
  }

  return success;
}

// Normalize a path by removing /./ or // from it.
static std::string normalizePath(StringRef Path) {
  SmallString<128> NormalizedPath;
  for (auto I = path::begin(Path), E = path::end(Path); I != E; ++I) {
    if (*I != ".")
      sys::path::append(NormalizedPath, *I);
  }
  return NormalizedPath.str();
}

static bool remapIndex(const Remapper &remapper,
                       const std::string &InputIndexPath,
                       const std::string &outputIndexPath, std::ostream *outs) {
  if (Verbose) {
    *outs << "Remapping Index Store at: '" << InputIndexPath << "' to '"
          << OutputIndexPath << "'\n";
  }

  SmallString<256> unitDirectory;
  path::append(unitDirectory, InputIndexPath, "v5", "units");
  SmallString<256> recordsDirectory;
  path::append(recordsDirectory, InputIndexPath, "v5", "records");

  if (not fs::is_directory(unitDirectory) ||
      not fs::is_directory(recordsDirectory)) {
    errs() << "error: invalid index store directory " << InputIndexPath << "\n";
    return false;
  }
  bool success = true;

  if (not cloneRecords(recordsDirectory, InputIndexPath, outputIndexPath)) {
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
      errs() << "error: failed to read unit file " << unitPath << "\n"
             << unitReadError;
      success = false;
      continue;
    }
    if (Verbose) {
      *outs << "Remapping file " << unitPath << "\n";
    }

    ModuleNameScope moduleNames;
    auto writer = remapUnit(reader, remapper, fileMgr, moduleNames, outs);

    std::string unitWriteError;
    if (writer.write(unitWriteError)) {
      errs() << "error: failed to write index store; " << unitWriteError
             << "\n";
      success = false;
    }
  }

  if (dirError) {
    errs() << "error: aborted while reading from unit directory: "
           << dirError.message() << "\n";
    success = false;
  }
  return success;
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  OutputIndexPath = normalizePath(OutputIndexPath);

  Remapper remapper;
  // Parse the the path remapping command line flags. This converts strings of
  // "X=Y" into a (regex, string) pair. Another way of looking at it: each
  // remap is equivalent to the s/pattern/replacement/ operator.
  auto errors = 0;
  for (const auto &remap : PathRemaps) {
    auto divider = remap.find('=');
    auto pattern = remap.substr(0, divider);
    try {
      std::regex re(pattern);
      auto replacement = remap.substr(divider + 1);
      remapper.addRemap(re, replacement);
    } catch (const std::regex_error &e) {
      errs() << "Error parsing regular expression: '" << pattern << "':\n"
             << e.what() << "\n";
      errors++;
    }
  }

  if (errors) {
    errs() << "Aborting due to " << errors << " error"
           << ((errors > 1) ? "s" : "") << ".\n";
    return EXIT_FAILURE;
  }

  std::string initOutputIndexError;
  if (IndexUnitWriter::initIndexDirectory(OutputIndexPath,
                                          initOutputIndexError)) {
    errs() << "error: failed to initialize index store; "
           << initOutputIndexError << "\n";
    return EXIT_FAILURE;
  }

  if (ParallelStride == 0 || ParallelStride >= InputIndexPaths.size()) {
    bool success = true;
    for (auto &InputIndexPath : InputIndexPaths) {
      InputIndexPath = normalizePath(InputIndexPath);

      if (not remapIndex(remapper, InputIndexPath, OutputIndexPath,
                         &std::cout)) {
        success = false;
      }
    }
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  // Process the data stores in groups according to the parallel stride.
  const size_t stride = static_cast<size_t>(ParallelStride);
  const size_t length = InputIndexPaths.size();
  const size_t numStrides = ((length - 1) / stride) + 1;

  auto lock = dispatch_semaphore_create(1);
  __block bool success = true;
  dispatch_apply(numStrides, DISPATCH_APPLY_AUTO, ^(size_t strideIndex) {
    const size_t start = strideIndex * stride;
    const size_t end = std::min(start + stride, length);
    for (size_t index = start; index < end; ++index) {
      std::string InputIndexPath = normalizePath(InputIndexPaths[index]);

      std::ostringstream outs{};
      if (not remapIndex(remapper, InputIndexPath, OutputIndexPath, &outs)) {
        success = false;
      }

      if (Verbose) {
        dispatch_semaphore_wait(lock, DISPATCH_TIME_FOREVER);
        std::cout << outs.str();
        dispatch_semaphore_signal(lock);
      }
    }
  });
  dispatch_release(lock);

  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
