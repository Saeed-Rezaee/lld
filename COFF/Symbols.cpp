//===- Symbols.cpp --------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "Symbols.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm::object;
using llvm::sys::fs::identify_magic;
using llvm::sys::fs::file_magic;

namespace lld {
namespace coff {

// Returns 1, 0 or -1 if this symbol should take precedence over the
// Other in the symbol table, tie or lose, respectively.
int Defined::compare(SymbolBody *Other) {
  if (!isa<Defined>(Other))
    return 1;
  auto *X = dyn_cast<DefinedRegular>(this);
  auto *Y = dyn_cast<DefinedRegular>(Other);
  if (!X || !Y)
    return 0;

  // Common symbols are weaker than other types of defined symbols.
  if (X->isCommon() && Y->isCommon())
    return (X->getCommonSize() < Y->getCommonSize()) ? -1 : 1;
  if (X->isCommon())
    return -1;
  if (Y->isCommon())
    return 1;

  if (X->isCOMDAT() && Y->isCOMDAT())
    return 1;
  return 0;
}

int CanBeDefined::compare(SymbolBody *Other) {
  if (isa<Defined>(Other))
    return -1;

  // Undefined symbols with weak aliases will turn into defined
  // symbols if they remain undefined, so we don't need to resolve
  // such symbols.
  if (auto *O = dyn_cast<Undefined>(Other))
    if (O->getWeakAlias())
      return -1;
  return 1;
}

int Undefined::compare(SymbolBody *Other) {
  if (isa<Defined>(Other))
    return -1;
  if (isa<CanBeDefined>(Other))
    return getWeakAlias() ? 1 : -1;
  if (cast<Undefined>(Other)->getWeakAlias())
    return -1;
  return 1;
}

ErrorOr<std::unique_ptr<InputFile>> CanBeDefined::getMember() {
  auto MBRefOrErr = File->getMember(&Sym);
  if (auto EC = MBRefOrErr.getError())
    return EC;
  MemoryBufferRef MBRef = MBRefOrErr.get();

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (MBRef.getBuffer().empty())
    return nullptr;

  file_magic Magic = identify_magic(MBRef.getBuffer());
  if (Magic == file_magic::coff_import_library)
    return llvm::make_unique<ImportFile>(MBRef);

  if (Magic != file_magic::coff_object)
    return make_dynamic_error_code("unknown file type");

  ErrorOr<std::unique_ptr<ObjectFile>> FileOrErr
    = ObjectFile::create(MBRef.getBufferIdentifier(), MBRef);
  if (auto EC = FileOrErr.getError())
    return EC;
  std::unique_ptr<ObjectFile> Obj = std::move(FileOrErr.get());
  Obj->setParentName(File->getName());
  return std::move(Obj);
}

}
}
