#include "clang/Index/IndexRecordReader.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace clang;
using namespace clang::index;

static cl::list<std::string> RecordPaths(cl::Positional, cl::OneOrMore,
                                         cl::desc("<index-records>"));

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  for (const auto &recordPath : RecordPaths) {
    std::string readerError;
    auto reader =
        IndexRecordReader::createWithFilePath(recordPath, readerError);
    if (!reader) {
      errs() << "error: failed to read record file " << readerError << " -- "
             << recordPath << "\n";
      return EXIT_FAILURE;
    }

    outs() << "record: " << recordPath << "\n";
    reader->foreachDecl(
        /*NoCache=*/true, [&](const IndexRecordDecl *Rec) -> bool {
          outs() << " name: " << Rec->Name << " | usr: " << Rec->USR
                 << " | kind: " << getSymbolKindString(Rec->SymInfo.Kind);

          if (Rec->SymInfo.SubKind != SymbolSubKind::None) {
            outs() << " | subkind: "
                   << getSymbolSubKindString(Rec->SymInfo.SubKind);
          }
          if (Rec->SymInfo.Properties != 0) {
            outs() << " | properties: ";
            printSymbolProperties(Rec->SymInfo.Properties, outs());
          }
          if (Rec->Roles != 0) {
            outs() << " | roles: ";
            printSymbolRoles(Rec->Roles, outs());
          }
          if (Rec->RelatedRoles != 0) {
            outs() << " | relatedRoles: ";
            printSymbolRoles(Rec->RelatedRoles, outs());
          }
          outs() << "\n";
          return true;
        });

    // reader->foreachOccurrence([&](const IndexRecordOccurrence &Rec) -> bool {
    //   Rec.Relations.return true;
    // });
  }

  return EXIT_SUCCESS;
}
