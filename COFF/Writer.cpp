//===- Writer.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "Writer.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <functional>
#include <map>
#include <utility>

using namespace llvm;
using namespace llvm::object;

static const int PageSize = 4096;
static const int FileAlignment = 512;
static const int SectionAlignment = 4096;
static const int DOSStubSize = 64;
static const int NumberfOfDataDirectory = 16;
static const int HeaderSize =
    DOSStubSize + sizeof(llvm::COFF::PEMagic) +
    sizeof(llvm::object::coff_file_header) +
    sizeof(llvm::object::pe32plus_header) +
    sizeof(llvm::object::data_directory) * NumberfOfDataDirectory;

namespace lld {
namespace coff {

OutputSection::OutputSection(StringRef N, uint32_t SI)
    : Name(N), SectionIndex(SI) {
  memset(&Header, 0, sizeof(Header));
  strncpy(Header.Name, Name.data(), std::min(Name.size(), size_t(8)));
}

void OutputSection::setRVA(uint64_t RVA) {
  Header.VirtualAddress = RVA;
  for (Chunk *C : Chunks)
    C->setRVA(C->getRVA() + RVA);
}

void OutputSection::setFileOffset(uint64_t Off) {
  if (Header.SizeOfRawData == 0)
    return;
  Header.PointerToRawData = Off;
  for (Chunk *C : Chunks)
    C->setFileOff(C->getFileOff() + Off);
}

void OutputSection::addChunk(Chunk *C) {
  Chunks.push_back(C);
  uint64_t Off = Header.VirtualSize;
  Off = RoundUpToAlignment(Off, C->getAlign());
  C->setRVA(Off);
  C->setFileOff(Off);
  Off += C->getSize();
  Header.VirtualSize = Off;
  if (!C->isBSS())
    Header.SizeOfRawData = RoundUpToAlignment(Off, FileAlignment);
}

void OutputSection::addPermissions(uint32_t C) {
  Header.Characteristics = Header.Characteristics | (C & PermMask);
}

static StringRef dropDollar(StringRef S) { return S.substr(0, S.find('$')); }

void Writer::markLive() {
  Entry = cast<Defined>(Symtab->find(Config->EntryName));
  Entry->markLive();
  for (Chunk *C : Symtab->getChunks())
    if (C->isRoot())
      C->markLive();
}

void Writer::createSections() {
  std::map<StringRef, std::vector<Chunk *>> Map;
  for (Chunk *C : Symtab->getChunks()) {
    if (!C->isLive()) {
      if (Config->Verbose)
        C->printDiscardMessage();
      continue;
    }
    // '$' and all following characters in input section names are
    // discarded when determining output section. So, .text$foo
    // contributes to .text, for example. See PE/COFF spec 3.2.
    Map[dropDollar(C->getSectionName())].push_back(C);
  }

  // Input sections are ordered by their names including '$' parts,
  // which gives you some control over the output layout.
  auto Comp = [](Chunk *A, Chunk *B) {
    return A->getSectionName() < B->getSectionName();
  };
  for (auto &P : Map) {
    StringRef SectionName = P.first;
    std::vector<Chunk *> &Chunks = P.second;
    std::stable_sort(Chunks.begin(), Chunks.end(), Comp);
    auto Sec =
        llvm::make_unique<OutputSection>(SectionName, OutputSections.size());
    for (Chunk *C : Chunks) {
      C->setOutputSection(Sec.get());
      Sec->addChunk(C);
      Sec->addPermissions(C->getPermissions());
    }
    OutputSections.push_back(std::move(Sec));
  }
}

std::map<StringRef, std::vector<DefinedImportData *>> Writer::binImports() {
  // Group DLL-imported symbols by DLL name because that's how symbols
  // are layed out in the import descriptor table.
  std::map<StringRef, std::vector<DefinedImportData *>> Res;
  OutputSection *Text = createSection(".text");
  for (std::unique_ptr<ImportFile> &P : Symtab->ImportFiles) {
    for (std::unique_ptr<SymbolBody> &B : P->getSymbols()) {
      if (auto *Import = dyn_cast<DefinedImportData>(B.get())) {
        Res[Import->getDLLName()].push_back(Import);
        continue;
      }
      // Linker-created function thunks for DLL symbols are added to
      // .text section.
      Text->addChunk(cast<DefinedImportFunc>(B.get())->getChunk());
    }
  }

  // Sort symbols by name for each group.
  auto Comp = [](DefinedImportData *A, DefinedImportData *B) {
    return A->getName() < B->getName();
  };
  for (auto &P : Res) {
    std::vector<DefinedImportData *> &V = P.second;
    std::sort(V.begin(), V.end(), Comp);
  }
  return Res;
}

// Create .idata section contents.
void Writer::createImportTables() {
  if (Symtab->ImportFiles.empty())
    return;

  std::vector<ImportTable> Tabs;
  for (auto &P : binImports()) {
    StringRef DLLName = P.first;
    std::vector<DefinedImportData *> &Imports = P.second;
    Tabs.emplace_back(DLLName, Imports);
  }
  OutputSection *Idata = createSection(".idata");
  size_t NumChunks = Idata->getChunks().size();

  // Add the directory tables.
  for (ImportTable &T : Tabs)
    Idata->addChunk(T.DirTab);
  Idata->addChunk(new NullChunk(sizeof(llvm::COFF::ImportDirectoryTableEntry)));

  // Add the import lookup tables.
  for (ImportTable &T : Tabs) {
    for (LookupChunk *C : T.LookupTables)
      Idata->addChunk(C);
    Idata->addChunk(new NullChunk(8));
  }

  // Add the import address tables. Their contents are the same as the
  // lookup tables.
  for (ImportTable &T : Tabs) {
    for (LookupChunk *C : T.AddressTables)
      Idata->addChunk(C);
    Idata->addChunk(new NullChunk(8));
    ImportAddressTableSize += (T.AddressTables.size() + 1) * 8;
  }
  ImportAddressTable = Tabs[0].AddressTables[0];

  // Add the hint name table.
  for (ImportTable &T : Tabs)
    for (HintNameChunk *C : T.HintNameTables)
      Idata->addChunk(C);

  // Add DLL names.
  for (ImportTable &T : Tabs)
    Idata->addChunk(T.DLLName);

  // Claim ownership of all chunks in the .idata section.
  for (size_t I = NumChunks, E = Idata->getChunks().size(); I < E; ++I)
    Chunks.push_back(std::unique_ptr<Chunk>(Idata->getChunks()[I]));
}

// The Windows loader doesn't seem to like empty sections,
// so we remove them if any.
void Writer::removeEmptySections() {
  auto IsEmpty = [](const std::unique_ptr<OutputSection> &S) {
    return S->getVirtualSize() == 0;
  };
  OutputSections.erase(
      std::remove_if(OutputSections.begin(), OutputSections.end(), IsEmpty),
      OutputSections.end());
}

// Visits all sections to assign incremental, non-overlapping RVAs and
// file offsets.
void Writer::assignAddresses() {
  uint64_t HeaderEnd = RoundUpToAlignment(
      HeaderSize + sizeof(coff_section) * OutputSections.size(), PageSize);
  uint64_t RVA = 0x1000; // The first page is kept unmapped.
  uint64_t FileOff = HeaderEnd;
  for (std::unique_ptr<OutputSection> &Sec : OutputSections) {
    Sec->setRVA(RVA);
    Sec->setFileOffset(FileOff);
    RVA += RoundUpToAlignment(Sec->getVirtualSize(), PageSize);
    FileOff += RoundUpToAlignment(Sec->getRawSize(), FileAlignment);
  }
  SizeOfImage = HeaderEnd + RoundUpToAlignment(RVA - 0x1000, PageSize);
  FileSize = HeaderEnd + RoundUpToAlignment(FileOff - HeaderEnd, FileAlignment);
}

void Writer::writeHeader() {
  // Write DOS stub
  uint8_t *P = Buffer->getBufferStart();
  auto *DOS = reinterpret_cast<dos_header *>(P);
  P += DOSStubSize;
  DOS->Magic[0] = 'M';
  DOS->Magic[1] = 'Z';
  DOS->AddressOfRelocationTable = sizeof(dos_header);
  DOS->AddressOfNewExeHeader = DOSStubSize;

  // Write PE magic
  memcpy(P, llvm::COFF::PEMagic, sizeof(llvm::COFF::PEMagic));
  P += sizeof(llvm::COFF::PEMagic);

  // Write COFF header
  COFF = reinterpret_cast<coff_file_header *>(P);
  P += sizeof(coff_file_header);
  COFF->Machine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
  COFF->NumberOfSections = OutputSections.size();
  COFF->Characteristics = (llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE |
                           llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED |
                           llvm::COFF::IMAGE_FILE_LARGE_ADDRESS_AWARE);
  COFF->SizeOfOptionalHeader =
      sizeof(pe32plus_header) +
      sizeof(llvm::object::data_directory) * NumberfOfDataDirectory;

  // Write PE header
  PE = reinterpret_cast<pe32plus_header *>(P);
  P += sizeof(pe32plus_header);
  PE->Magic = llvm::COFF::PE32Header::PE32_PLUS;
  PE->ImageBase = Config->ImageBase;
  PE->SectionAlignment = SectionAlignment;
  PE->FileAlignment = FileAlignment;
  PE->MajorOperatingSystemVersion = 6;
  PE->MajorSubsystemVersion = 6;
  PE->Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI;
  PE->SizeOfImage = SizeOfImage;
  PE->AddressOfEntryPoint = Entry->getRVA();
  PE->SizeOfStackReserve = 1024 * 1024;
  PE->SizeOfStackCommit = 4096;
  PE->SizeOfHeapReserve = 1024 * 1024;
  PE->SizeOfHeapCommit = 4096;
  PE->NumberOfRvaAndSize = NumberfOfDataDirectory;
  if (OutputSection *Text = findSection(".text")) {
    PE->BaseOfCode = Text->getRVA();
    PE->SizeOfCode = Text->getRawSize();
  }
  PE->SizeOfInitializedData = getSizeOfInitializedData();

  // Write data directory
  DataDirectory = reinterpret_cast<data_directory *>(P);
  P += sizeof(data_directory) * NumberfOfDataDirectory;
  if (OutputSection *Idata = findSection(".idata")) {
    using namespace llvm::COFF;
    DataDirectory[IMPORT_TABLE].RelativeVirtualAddress = Idata->getRVA();
    DataDirectory[IMPORT_TABLE].Size = Idata->getVirtualSize();
    DataDirectory[IAT].RelativeVirtualAddress = ImportAddressTable->getRVA();
    DataDirectory[IAT].Size = ImportAddressTableSize;
  }

  // Write section table
  SectionTable = reinterpret_cast<coff_section *>(P);
  int Idx = 0;
  for (std::unique_ptr<OutputSection> &Out : OutputSections)
    SectionTable[Idx++] = *Out->getHeader();
  PE->SizeOfHeaders = RoundUpToAlignment(
      HeaderSize + sizeof(coff_section) * OutputSections.size(), FileAlignment);
}

std::error_code Writer::openFile(StringRef Path) {
  if (auto EC = FileOutputBuffer::create(Path, FileSize, Buffer,
                                         FileOutputBuffer::F_executable))
    return make_dynamic_error_code(Twine("Failed to open ") + Path + ": " +
                                   EC.message());
  return std::error_code();
}

// Write section contents to a mmap'ed file.
void Writer::writeSections() {
  uint8_t *P = Buffer->getBufferStart();
  for (std::unique_ptr<OutputSection> &Sec : OutputSections) {
    // Fill gaps between functions in .text with INT3 instructions
    // instead of leaving as NUL bytes (which can be interpreted as
    // ADD instructions).
    if (Sec->getPermissions() & llvm::COFF::IMAGE_SCN_CNT_CODE)
      memset(P + Sec->getFileOff(), 0xCC, Sec->getRawSize());
    for (Chunk *C : Sec->getChunks())
      if (!C->isBSS())
        memcpy(P + C->getFileOff(), C->getData(), C->getSize());
  }
}

OutputSection *Writer::findSection(StringRef Name) {
  for (std::unique_ptr<OutputSection> &S : OutputSections)
    if (S->getName() == Name)
      return S.get();
  return nullptr;
}

uint32_t Writer::getSizeOfInitializedData() {
  using llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA;
  uint32_t Res = 0;
  for (std::unique_ptr<OutputSection> &S : OutputSections)
    if (S->getPermissions() & IMAGE_SCN_CNT_INITIALIZED_DATA)
      Res += S->getRawSize();
  return Res;
}

// Returns an existing section or create a new one if not found.
OutputSection *Writer::createSection(StringRef Name) {
  using namespace llvm::COFF;
  if (auto *Sec = findSection(Name))
    return Sec;
  const auto Read = IMAGE_SCN_MEM_READ;
  const auto Write = IMAGE_SCN_MEM_WRITE;
  const auto Execute = IMAGE_SCN_MEM_EXECUTE;
  uint32_t Perm =
      StringSwitch<uint32_t>(Name)
          .Case(".bss", IMAGE_SCN_CNT_UNINITIALIZED_DATA | Read | Write)
          .Case(".data", IMAGE_SCN_CNT_INITIALIZED_DATA | Read | Write)
          .Case(".idata", IMAGE_SCN_CNT_INITIALIZED_DATA | Read)
          .Case(".rdata", IMAGE_SCN_CNT_INITIALIZED_DATA | Read)
          .Case(".text", IMAGE_SCN_CNT_CODE | Read | Execute)
          .Default(0);
  if (!Perm)
    llvm_unreachable("unknown section name");
  auto Sec = new OutputSection(Name, OutputSections.size());
  Sec->addPermissions(Perm);
  OutputSections.push_back(std::unique_ptr<OutputSection>(Sec));
  return Sec;
}

void Writer::applyRelocations() {
  uint8_t *Buf = Buffer->getBufferStart();
  for (std::unique_ptr<OutputSection> &Sec : OutputSections)
    for (Chunk *C : Sec->getChunks())
      C->applyRelocations(Buf);
}

std::error_code Writer::write(StringRef OutputPath) {
  markLive();
  createSections();
  createImportTables();
  assignAddresses();
  removeEmptySections();
  if (auto EC = openFile(OutputPath))
    return EC;
  writeHeader();
  writeSections();
  applyRelocations();
  if (auto EC = Buffer->commit())
    return EC;
  return std::error_code();
}

} // namespace coff
} // namespace lld
