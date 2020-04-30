#include "clang/Basic/FileManager.h"
#include "clang/Index/IndexUnitReader.h"
#include "clang/Index/IndexUnitWriter.h"
#include "llvm/ADT/APInt.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
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

static cl::opt<std::string> UnitPathsFile(
    "unit-paths-file",
    cl::desc("Path to a file containing paths to unit files. If specified "
             "record-paths-file must also be specified"));

static cl::opt<std::string> RecordPathsFile(
    "record-paths-file",
    cl::desc("Path to a file containing paths to record files. If specified "
             "unit-paths-file must also be specified"));

static cl::list<std::string> InputIndexPaths(cl::Positional, cl::ZeroOrMore,
                                             cl::desc("<input-indexstores>"));

static cl::opt<std::string> OutputIndexPath(cl::Positional, cl::Required,
                                            cl::desc("<output-indexstore>"));

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

// Returns a FileEntry for any non-empty path.
static const FileEntry *getFileEntry(FileManager &fileMgr, StringRef path) {
  if (path.empty()) {
    return nullptr;
  }

  // Use getVirtualFile to handle both valid and invalid paths.
  return fileMgr.getVirtualFile(path, /*size*/ 0, /*modtime*/ 0);
}

void getUnitPathForOutputFile(StringRef unitsPath, StringRef filePath,
                              SmallVectorImpl<char> &str,
                              FileManager &fileMgr) {
  str.append(unitsPath.begin(), unitsPath.end());
  str.push_back('/');
  SmallString<256> absPath(filePath);
  fileMgr.makeAbsolutePath(absPath);
  StringRef fname = sys::path::filename(absPath);
  str.append(fname.begin(), fname.end());
  str.push_back('-');
  llvm::hash_code pathHashVal = llvm::hash_value(absPath);
  llvm::APInt(64, pathHashVal).toString(str, 36, /*Signed=*/false);
}

Optional<bool>
isUnitUpToDateForOutputFile(StringRef unitsPath, StringRef filePath,
                            Optional<StringRef> timeCompareFilePath,
                            FileManager &fileMgr, std::string &error) {
  SmallString<256> unitPath;
  getUnitPathForOutputFile(unitsPath, filePath, unitPath, fileMgr);

  llvm::sys::fs::file_status unitStat;
  if (std::error_code ec = llvm::sys::fs::status(unitPath.c_str(), unitStat)) {
    if (ec != llvm::errc::no_such_file_or_directory) {
      llvm::raw_string_ostream err(error);
      err << "could not access path '" << unitPath << "': " << ec.message();
      return None;
    }
    return false;
  }

  if (!timeCompareFilePath.hasValue())
    return true;

  llvm::sys::fs::file_status compareStat;
  if (std::error_code ec =
          llvm::sys::fs::status(*timeCompareFilePath, compareStat)) {
    if (ec != llvm::errc::no_such_file_or_directory) {
      llvm::raw_string_ostream err(error);
      err << "could not access path '" << *timeCompareFilePath
          << "': " << ec.message();
      return None;
    }
    return true;
  }

  // Return true (unit is up-to-date) if the file to compare is older than the
  // unit file.
  return compareStat.getLastModificationTime() <=
         unitStat.getLastModificationTime();
}

// Returns true if the Unit file of given output file already exists and is
// older than the input file.
static bool isUnitUpToDate(StringRef unitsPath, StringRef outputFile,
                           StringRef inputFile, FileManager &fileMgr) {
  std::string error;
  auto isUpToDateOpt = isUnitUpToDateForOutputFile(unitsPath, outputFile,
                                                   inputFile, fileMgr, error);
  if (!isUpToDateOpt.hasValue()) {
    errs() << "error: failed file status check:\n" << error << "\n";
    return false;
  }

  return *isUpToDateOpt;
}

// Returns None if the Unit file is already up to date
static Optional<IndexUnitWriter>
remapUnit(StringRef outputUnitsPath, StringRef inputUnitPath,
          const std::unique_ptr<IndexUnitReader> &reader,
          const Remapper &remapper, FileManager &fileMgr,
          ModuleNameScope &moduleNames) {
  // The set of remapped paths.
  auto workingDir = remapper.remap(reader->getWorkingDirectory());
  auto outputFile = remapper.remap(reader->getOutputFile());

  // Check if the unit file is already up to date
  SmallString<256> remappedOutputFilePath;
  if (outputFile[0] != '/') {
    // Convert outputFile to absolute path
    path::append(remappedOutputFilePath, workingDir, outputFile);
  } else {
    remappedOutputFilePath = outputFile;
  }
  if (isUnitUpToDate(outputUnitsPath, remappedOutputFilePath, inputUnitPath,
                     fileMgr)) {
    return None;
  }

  auto mainFilePath = remapper.remap(reader->getMainFilePath());
  auto sysrootPath = remapper.remap(reader->getSysrootPath());

  auto &fsOpts = fileMgr.getFileSystemOpts();
  fsOpts.WorkingDir = workingDir;

  auto writer = IndexUnitWriter(
      fileMgr, OutputIndexPath, reader->getProviderIdentifier(),
      reader->getProviderVersion(), outputFile, reader->getModuleName(),
      getFileEntry(fileMgr, mainFilePath), reader->isSystemUnit(),
      reader->isModuleUnit(), reader->isDebugCompilation(), reader->getTarget(),
      sysrootPath, moduleNames.getModuleInfo);

  reader->foreachDependency([&](const IndexUnitReader::DependencyInfo &info) {
    const auto name = info.UnitOrRecordName;
    const auto moduleNameRef = moduleNames.getReference(info.ModuleName);
    const auto isSystem = info.IsSystem;

    const auto filePath = remapper.remap(info.FilePath);
    const auto file = getFileEntry(fileMgr, filePath);

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
    const auto sourcePath = remapper.remap(info.SourcePath);
    const auto targetPath = remapper.remap(info.TargetPath);

    // Note this isn't relevant to Swift.
    writer.addInclude(getFileEntry(fileMgr, sourcePath), info.SourceLine,
                      getFileEntry(fileMgr, targetPath));
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

static bool createDirectories(StringRef path) {
  std::error_code failed =
      fs::create_directories(path, /*IgnoreExisting=*/true);
  if (failed) {
    errs() << "Could not create directory `" << path
           << "`: " << failed.message() << "\n";
    return false;
  }
  return true;
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
      if (not createDirectories(outputPath)) {
        success = false;
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

static bool cloneRecords(const std::vector<std::string> &recordPaths,
                         const std::string &outputIndexPath) {
  bool success = true;

  for (auto inputPath : recordPaths) {
    auto inputIndexPath = path::parent_path(
        path::parent_path(path::parent_path(path::parent_path(inputPath))));

    SmallString<128> outputPath{inputPath};
    path::replace_path_prefix(outputPath, inputIndexPath, outputIndexPath);

    if (not cloneRecord(inputPath, outputPath)) {
      bool innerSuccess = true;
      if (errno == ENOENT) {
        // We need to create the directories below it
        if (not createDirectories(path::parent_path(outputPath))) {
          innerSuccess = false;
        } else if (not cloneRecord(inputPath, outputPath)) {
          innerSuccess = false;
        }
      }

      if (!innerSuccess) {
        success = false;
        errs() << "Could not copy record file from `" << inputPath << "` to `"
               << outputPath << "`: " << strerror(errno) << "\n";
      }
    }
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
                       const std::string &outputIndexPath) {
  SmallString<256> unitDirectory;
  path::append(unitDirectory, InputIndexPath, "v5", "units");
  SmallString<256> recordsDirectory;
  path::append(recordsDirectory, InputIndexPath, "v5", "records");
  SmallString<256> outputUnitDirectory;
  path::append(outputUnitDirectory, OutputIndexPath, "v5", "units");

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

    ModuleNameScope moduleNames;
    auto writer = remapUnit(outputUnitDirectory, unitPath, reader, remapper,
                            fileMgr, moduleNames);

    if (writer.hasValue()) {
      std::string unitWriteError;
      if (writer->write(unitWriteError)) {
        errs() << "error: failed to write index store; " << unitWriteError
               << "\n";
        success = false;
      }
    }
  }

  if (dirError) {
    errs() << "error: aborted while reading from unit directory: "
           << dirError.message() << "\n";
    success = false;
  }
  return success;
}

static bool remapUnits(const Remapper &remapper,
                       const std::vector<std::string> &unitPaths,
                       const SmallString<256> &outputUnitDirectory) {
  FileSystemOptions fsOpts;
  FileManager fileMgr{fsOpts};

  bool success = true;

  for (const auto unitPath : unitPaths) {
    std::string unitReadError;
    auto reader = IndexUnitReader::createWithFilePath(unitPath, unitReadError);
    if (not reader) {
      errs() << "error: failed to read unit file " << unitPath << "\n"
             << unitReadError;
      success = false;
      continue;
    }

    ModuleNameScope moduleNames;
    auto writer = remapUnit(outputUnitDirectory, unitPath, reader, remapper,
                            fileMgr, moduleNames);

    if (writer.hasValue()) {
      std::string unitWriteError;
      if (writer->write(unitWriteError)) {
        errs() << "error: failed to write index store; " << unitWriteError
               << "\n";
        success = false;
      }
    }
  }

  return success;
}

static bool remapIndex(const Remapper &remapper,
                       const std::vector<std::string> &unitPaths,
                       const std::vector<std::string> &recordPaths,
                       const std::string &outputIndexPath) {
  SmallString<256> outputUnitDirectory;
  path::append(outputUnitDirectory, OutputIndexPath, "v5", "units");

  __block bool success = true;

  if (not cloneRecords(recordPaths, outputIndexPath)) {
    success = false;
  }

  if (ParallelStride == 0 || ParallelStride >= unitPaths.size()) {
    if (not remapUnits(remapper, unitPaths, outputUnitDirectory)) {
      success = false;
    }
    return success;
  }

  // Process the units in groups according to the parallel stride.
  const size_t stride = static_cast<size_t>(ParallelStride);
  const size_t length = unitPaths.size();
  const size_t numStrides = ((length - 1) / stride) + 1;

  dispatch_apply(numStrides, DISPATCH_APPLY_AUTO, ^(size_t strideIndex) {
    const size_t offset = strideIndex * stride;
    auto start = unitPaths.begin() + offset;
    auto end = unitPaths.begin() + std::min(offset + stride, length);
    std::vector<std::string> unitPathsSlice(start, end);
    if (not remapUnits(remapper, unitPathsSlice, outputUnitDirectory)) {
      success = false;
    }
  });

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

  // If given the paths to individual unit paths and records, use those instead
  // of enumerating directories
  if (!UnitPathsFile.empty() || !RecordPathsFile.empty()) {
    if (UnitPathsFile.empty()) {
      errs() << "record-paths-file set but not unit-paths-file.\n";
      return EXIT_FAILURE;
    }
    if (RecordPathsFile.empty()) {
      errs() << "unit-paths-file set but not record-paths-file.\n";
      return EXIT_FAILURE;
    }

    std::ifstream unitPathsStream(UnitPathsFile);
    std::vector<std::string> unitPaths;
    std::string unitPath;
    while (unitPathsStream >> unitPath) {
      unitPaths.push_back(unitPath);
    }

    std::ifstream recordPathsStream(RecordPathsFile);
    std::vector<std::string> recordPaths;
    std::string recordPath;
    while (recordPathsStream >> recordPath) {
      recordPaths.push_back(recordPath);
    }

    return remapIndex(remapper, unitPaths, recordPaths, OutputIndexPath)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
  } else if (InputIndexPaths.empty()) {
    errs() << "input-indexstores must be set if unit-paths-file and "
              "record-paths-file are not.\n";
    return EXIT_FAILURE;
  }

  if (ParallelStride == 0 || ParallelStride >= InputIndexPaths.size()) {
    bool success = true;
    for (auto &InputIndexPath : InputIndexPaths) {
      InputIndexPath = normalizePath(InputIndexPath);
      if (not remapIndex(remapper, InputIndexPath, OutputIndexPath)) {
        success = false;
      }
    }
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  // Process the data stores in groups according to the parallel stride.
  const size_t stride = static_cast<size_t>(ParallelStride);
  const size_t length = InputIndexPaths.size();
  const size_t numStrides = ((length - 1) / stride) + 1;

  __block bool success = true;
  dispatch_apply(numStrides, DISPATCH_APPLY_AUTO, ^(size_t strideIndex) {
    const size_t start = strideIndex * stride;
    const size_t end = std::min(start + stride, length);
    for (size_t index = start; index < end; ++index) {
      std::string InputIndexPath = normalizePath(InputIndexPaths[index]);
      if (not remapIndex(remapper, InputIndexPath, OutputIndexPath)) {
        success = false;
      }
    }
  });

  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
