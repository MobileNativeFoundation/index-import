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

static cl::list<std::string> InputIndexPaths(cl::Positional, cl::OneOrMore,
                                             cl::desc("<input-indexstores>"));

static cl::list<std::string> RemapFilePaths("import-output-file",
                                             cl::desc("import-output-file="));
static cl::opt<std::string> OutputIndexPath(cl::Positional, cl::Required,
                                            cl::desc("<output-indexstore>"));

static cl::opt<unsigned> ParallelStride(
    "parallel-stride", cl::init(32),
    cl::desc(
        "Stride for parallel operations. 0 to disable parallel processing"));

static cl::opt<bool>
    Incremental("incremental",
                cl::desc("Only transfer units if they are newer"));

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

// Append the path of a record inside of an index
void appendInteriorRecordPath(StringRef RecordName,
                                     SmallVectorImpl<char> &PathBuf) {
  // To avoid putting a huge number of files into the records directory, it creates
  // subdirectories based on the last 2 characters from the hash.
  // Note: the actual record name is a function of the bits in the record
  StringRef hash2chars = RecordName.substr(RecordName.size()-2);
  sys::path::append(PathBuf, hash2chars);
  sys::path::append(PathBuf, RecordName);
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


// Returns None if the Unit file is already up to date
static Optional<IndexUnitWriter>
importUnit(StringRef outputUnitsPath, StringRef inputUnitPath,
          StringRef outputRecordsPath, StringRef inputRecordsPath,
          const std::unique_ptr<IndexUnitReader> &reader,
          const Remapper &remapper, FileManager &fileMgr,
          ModuleNameScope &moduleNames) {
  // The set of remapped paths.
  auto workingDir = remapper.remap(reader->getWorkingDirectory());
  auto outputFile = remapper.remap(reader->getOutputFile());

  // Cloning records when we've got an output records path
  const auto cloneDepRecords = !outputRecordsPath.empty();

  if (Incremental) {
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
    SmallString<128> inputRecordPath;
    SmallString<128> outputRecordPath;
    SmallString<128> outputRecordInterDir;
    std::error_code createRecordDirFailed;

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
      if (cloneDepRecords) {
        sys::path::append(outputRecordPath, outputRecordsPath);
        appendInteriorRecordPath(info.UnitOrRecordName, outputRecordPath);

        // Compute/create the new interior directory by dropping the file name
        outputRecordInterDir = outputRecordPath;
        sys::path::remove_filename(outputRecordInterDir);
        createRecordDirFailed = fs::create_directory(outputRecordInterDir);
        if (createRecordDirFailed && createRecordDirFailed != std::errc::file_exists) {
          errs() << "error: failed create output record dir" << outputRecordInterDir << "\n";
        }
        sys::path::append(inputRecordPath, inputRecordsPath);
        appendInteriorRecordPath(info.UnitOrRecordName, inputRecordPath);
        cloneRecord(StringRef(inputRecordPath), StringRef(outputRecordPath));
      }
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
                       const std::string &outputIndexPath) {
  SmallString<256> unitDirectory;
  path::append(unitDirectory, InputIndexPath, "v5", "units");
  SmallString<256> recordsDirectory;
  path::append(recordsDirectory, InputIndexPath, "v5", "records");
  SmallString<256> outputUnitDirectory;
  path::append(outputUnitDirectory, OutputIndexPath, "v5", "units");
  SmallString<256> outputRecordsDirectory;
  path::append(outputRecordsDirectory, OutputIndexPath, "v5", "records");

  if (not fs::is_directory(unitDirectory) ||
      not fs::is_directory(recordsDirectory)) {
    errs() << "error: invalid index store directory " << InputIndexPath << "\n";
    return false;
  }

  bool success = true;
  FileSystemOptions fsOpts;
  FileManager fileMgr{fsOpts};

  auto handleUnitPath = [&](StringRef unitPath, StringRef outputRecordsPath_) {
    std::string unitReadError;
    auto reader = IndexUnitReader::createWithFilePath(unitPath, unitReadError);
    if (not reader) {
      errs() << "error: failed to read unit file " << unitPath << "\n"
             << unitReadError;
      success = false;
      return;
    }

    ModuleNameScope moduleNames;
    auto writer = importUnit(outputUnitDirectory, unitPath, outputRecordsPath_,
        recordsDirectory, reader, remapper, fileMgr, moduleNames);

    if (writer.hasValue()) {
      std::string unitWriteError;
      if (writer->write(unitWriteError)) {
        errs() << "error: failed to write index store; " << unitWriteError
               << "\n";
        success = false;
      }
    }
  };

  // Map over the file paths that the user provided
  if (RemapFilePaths.size()) {
    for (auto & path : RemapFilePaths) {
      SmallString<256> outPath;
      getUnitPathForOutputFile(unitDirectory, normalizePath(path), outPath, fileMgr);
      handleUnitPath(outPath.c_str(), outputRecordsDirectory);
    }
    return success;
  }

  // This batch clones records in the entire index. If we're importing individual
  // ouput files we don't want this.
  if (not cloneRecords(recordsDirectory, InputIndexPath, outputIndexPath)) {
    success = false;
  }

  // Process and map the entire index directory
  std::error_code dirError;
  fs::directory_iterator dir{unitDirectory, dirError};
  fs::directory_iterator end;
  while (dir != end && !dirError) {
    const auto unitPath = dir->path();
    dir.increment(dirError);
    handleUnitPath(unitPath, "");
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
