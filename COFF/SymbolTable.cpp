//===- SymbolTable.cpp ----------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "Driver.h"
#include "Error.h"
#include "SymbolTable.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/LTO/LTOCodeGenerator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace lld {
namespace coff {

SymbolTable::SymbolTable() {
  addSymbol(new DefinedAbsolute("__ImageBase", Config->ImageBase));
  if (!Config->EntryName.empty())
    addUndefined(Config->EntryName);
}

void SymbolTable::addFile(std::unique_ptr<InputFile> File) {
  InputFile *FileP = File.release();
  TG.run([this, FileP] {
    if (HasError)
      return;
    if (auto EC = FileP->parse()) {
      if (!HasError.exchange(true))
        LastError = EC;
      return;
    }
    std::error_code EC;
    if (auto *P = dyn_cast<ObjectFile>(FileP)) {
      EC = addObject(P);
    } else if (auto *P = dyn_cast<ArchiveFile>(FileP)) {
      EC = addArchive(P);
    } else if (auto *P = dyn_cast<BitcodeFile>(FileP)) {
      EC = addBitcode(P);
    } else {
      EC = addImport(cast<ImportFile>(FileP));
    }
    if (EC)
      if (!HasError.exchange(true))
        LastError = EC;
  });
}

std::error_code SymbolTable::wait() {
  TG.wait();
  return HasError ? LastError : std::error_code();
}

std::error_code SymbolTable::addObject(ObjectFile *File) {
  ObjectFiles.emplace_back(File);
  for (SymbolBody *Body : File->getSymbols())
    if (Body->isExternal())
      if (auto EC = resolve(Body))
        return EC;

  // If an object file contains .drectve section, read it and add
  // files listed in the section.
  StringRef Dir = File->getDirectives();
  if (!Dir.empty()) {
    std::vector<std::unique_ptr<InputFile>> Libs;
    if (auto EC = Driver->parseDirectives(Dir, &Libs))
      return EC;
    for (std::unique_ptr<InputFile> &Lib : Libs)
      addFile(std::move(Lib));
  }
  return std::error_code();
}

std::error_code SymbolTable::addArchive(ArchiveFile *File) {
  ArchiveFiles.push_back(std::unique_ptr<ArchiveFile>(File));
  for (SymbolBody *Body : File->getSymbols())
    if (auto EC = resolve(Body))
      return EC;
  return std::error_code();
}

std::error_code SymbolTable::addBitcode(BitcodeFile *File) {
  BitcodeFiles.push_back(std::unique_ptr<BitcodeFile>(File));
  for (SymbolBody *Body : File->getSymbols())
    if (Body->isExternal())
      if (auto EC = resolve(Body))
        return EC;

  // TODO: Handle linker directives.
  return std::error_code();
}

std::error_code SymbolTable::addImport(ImportFile *File) {
  ImportFiles.push_back(std::unique_ptr<ImportFile>(File));
  for (SymbolBody *Body : File->getSymbols())
    if (auto EC = resolve(Body))
      return EC;
  return std::error_code();
}

bool SymbolTable::reportRemainingUndefines() {
  bool Ret = false;
  for (auto &I : Symtab) {
    Symbol *Sym = I.second;
    auto *Undef = dyn_cast<Undefined>(&*Sym->Body);
    if (!Undef)
      continue;
    if (SymbolBody *Alias = Undef->getWeakAlias()) {
      Sym->Body = Alias->getReplacement();
      if (!isa<Defined>(&*Sym->Body)) {
        // Aliases are yet another symbols pointed by other symbols
        // that could also remain undefined.
        llvm::errs() << "undefined symbol: " << Undef->getName() << "\n";
        Ret = true;
      }
      continue;
    }
    llvm::errs() << "undefined symbol: " << Undef->getName() << "\n";
    Ret = true;
  }
  return Ret;
}

// This function resolves conflicts if there's an existing symbol with
// the same name. Decisions are made based on symbol type.
std::error_code SymbolTable::resolve(SymbolBody *New) {
  // Find an existing Symbol or create and insert a new one.
  StringRef Name = New->getName();
  Symbol *Sym = new Symbol(New);
  auto P = Symtab.insert(std::make_pair(Name, Sym));
  if (P.second) {
    // NewVal is inserted
    New->setBackref(Sym);
    return std::error_code();
  }
  // There's an existing SymbolBody
  Sym = P.first->second;
  New->setBackref(Sym);

  // compare() returns -1, 0, or 1 if the lhs symbol is less preferable,
  // equivalent (conflicting), or more preferable, respectively.
  SymbolBody *Existing, *Selected;
  for (;;) {
    Existing = Sym->Body;
    int comp = Existing->compare(New);
    if (comp < 0) {
      if (!Sym->Body.compare_exchange_strong(Existing, New))
        continue;
      Selected = New;
      break;
    }
    if (comp == 0) {
      llvm::errs() << "duplicate symbol: " << Name << "\n";
      return make_error_code(LLDError::DuplicateSymbols);
    }
    Selected = Existing;
    break;
  }

  // If we have an Undefined symbol for a Lazy symbol, we need
  // to read an archive member to replace the Lazy symbol with
  // a Defined symbol.
  if (isa<Undefined>(Existing) || isa<Undefined>(New))
    if (auto *B = dyn_cast<Lazy>(Selected))
      return addMemberFile(B);
  return std::error_code();
}

// Reads an archive member file pointed by a given symbol.
std::error_code SymbolTable::addMemberFile(Lazy *Body) {
  auto FileOrErr = Body->getMember();
  if (auto EC = FileOrErr.getError())
    return EC;
  std::unique_ptr<InputFile> File = std::move(FileOrErr.get());

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (!File)
    return std::error_code();
  if (Config->Verbose)
    llvm::dbgs() << "Loaded " << File->getShortName() << " for "
                 << Body->getName() << "\n";
  addFile(std::move(File));
  return std::error_code();
}

std::vector<Chunk *> SymbolTable::getChunks() {
  std::vector<Chunk *> Res;
  for (std::unique_ptr<ObjectFile> &File : ObjectFiles) {
    std::vector<Chunk *> &V = File->getChunks();
    Res.insert(Res.end(), V.begin(), V.end());
  }
  return Res;
}

Defined *SymbolTable::find(StringRef Name) {
  auto It = Symtab.find(Name);
  if (It == Symtab.end())
    return nullptr;
  if (auto *Def = dyn_cast<Defined>(&*It->second->Body))
    return Def;
  return nullptr;
}

// Windows specific -- Link default entry point name.
ErrorOr<StringRef> SymbolTable::findDefaultEntry() {
  // User-defined main functions and their corresponding entry points.
  static const char *Entries[][2] = {
      {"main", "mainCRTStartup"},
      {"wmain", "wmainCRTStartup"},
      {"WinMain", "WinMainCRTStartup"},
      {"wWinMain", "wWinMainCRTStartup"},
  };
  for (auto E : Entries) {
    if (find(E[1]))
      return StringRef(E[1]);
    if (!find(E[0]))
      continue;
    SymbolBody *Body = new Undefined(E[1]);
    if (auto EC = addSymbol(Body))
      return EC;
    return StringRef(E[1]);
  }
  llvm::errs() << "entry point must be defined\n";
  return make_error_code(LLDError::InvalidOption);
}

std::error_code SymbolTable::addUndefined(StringRef Name) {
  return addSymbol(new Undefined(Name));
}

// Resolve To, and make From an alias to To.
std::error_code SymbolTable::rename(StringRef From, StringRef To) {
  SymbolBody *Body = new Undefined(To);
  if (auto EC = resolve(Body))
    return EC;
  Symtab[From]->Body = Body->getReplacement();
  return std::error_code();
}

std::error_code SymbolTable::addSymbol(SymbolBody *Body) {
  OwningSymbols.push_back(std::unique_ptr<SymbolBody>(Body));
  return resolve(Body);
}

void SymbolTable::dump() {
  for (auto &P : Symtab) {
    Symbol *Ref = P.second;
    if (auto *Body = dyn_cast<Defined>(&*Ref->Body))
      llvm::dbgs() << Twine::utohexstr(Config->ImageBase + Body->getRVA())
                   << " " << Body->getName() << "\n";
  }
}

std::error_code SymbolTable::addCombinedLTOObject() {
  if (BitcodeFiles.empty())
    return std::error_code();

  llvm::LTOCodeGenerator CG;

  // All symbols referenced by non-bitcode objects must be preserved.
  for (std::unique_ptr<ObjectFile> &File : ObjectFiles)
    for (SymbolBody *Body : File->getSymbols())
      if (auto *S = dyn_cast<DefinedBitcode>(Body->getReplacement()))
        CG.addMustPreserveSymbol(S->getName());

  // Likewise for the linker-generated reference to the entry point.
  if (auto *S = dyn_cast<DefinedBitcode>(&*Symtab[Config->EntryName]->Body))
    CG.addMustPreserveSymbol(S->getName());

  CG.setModule(BitcodeFiles[0]->releaseModule());
  for (unsigned I = 1, E = BitcodeFiles.size(); I != E; ++I)
    CG.addModule(BitcodeFiles[I]->getModule());

  std::string ErrMsg;
  LTOObjectFile = CG.compile(false, false, false, ErrMsg);
  if (!LTOObjectFile) {
    llvm::errs() << ErrMsg << '\n';
    return make_error_code(LLDError::BrokenFile);
  }

  // Create an object file and add it to the symbol table by replacing any
  // DefinedBitcode symbols with the definitions in the object file.
  auto Obj = new ObjectFile(LTOObjectFile->getMemBufferRef());
  ObjectFiles.emplace_back(Obj);
  if (auto EC = Obj->parse())
    return EC;
  for (SymbolBody *Body : Obj->getSymbols()) {
    if (!Body->isExternal())
      continue;

    // Find an existing Symbol. We should not see any new symbols at this point.
    StringRef Name = Body->getName();
    Symbol *Sym = Symtab[Name];
    if (!Sym) {
      llvm::errs() << "LTO: unexpected new symbol: " << Name << '\n';
      return make_error_code(LLDError::BrokenFile);
    }
    Body->setBackref(Sym);

    if (isa<DefinedBitcode>(&*Sym->Body)) {
      // The symbol should now be defined.
      if (!isa<Defined>(Body)) {
        llvm::errs() << "LTO: undefined symbol: " << Name << '\n';
        return make_error_code(LLDError::BrokenFile);
      }
      Sym->Body = Body;
    }
  }

  return std::error_code();
}

} // namespace coff
} // namespace lld
