//===- Chunks.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_CHUNKS_H
#define LLD_COFF_CHUNKS_H

#include "lld/Core/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/COFF.h"
#include <map>
#include <vector>

using llvm::object::COFFSymbolRef;
using llvm::object::SectionRef;
using llvm::object::coff_relocation;
using llvm::object::coff_section;
using llvm::sys::fs::file_magic;

namespace lld {
namespace coff {

class Defined;
class DefinedImportData;
class ObjectFile;
class OutputSection;

// A Chunk represents a chunk of data that will occupy space in the
// output (if the resolver chose that). It may or may not be backed by
// a section of an input file. It could be linker-created data, or
// doesn't even have actual data (if common or bss).
class Chunk {
public:
  virtual ~Chunk() {}

  // Returns the pointer to data. It is illegal to call this function if
  // this is a common or BSS chunk.
  virtual const uint8_t *getData() const = 0;

  // Returns the size of this chunk (even if this is a common or BSS.)
  virtual size_t getSize() const = 0;

  // The writer sets and uses the addresses.
  uint64_t getRVA() { return RVA; }
  uint64_t getFileOff() { return FileOff; }
  uint32_t getAlign() { return Align; }
  void setRVA(uint64_t V) { RVA = V; }
  void setFileOff(uint64_t V) { FileOff = V; }

  // Applies relocations, assuming Buffer points to beginning of an
  // mmap'ed output file. Because this function uses file offsets and
  // RVA values of other chunks, you need to set them properly before
  // calling this function.
  virtual void applyRelocations(uint8_t *Buffer) {}

  virtual bool isBSS() const { return false; }
  virtual bool isCOMDAT() const { return false; }

  // Returns readable/writable/executable bits.
  virtual uint32_t getPermissions() const { return 0; }

  // Returns the section name if this is a section chunk.
  // It is illegal to call this function on non-section chunks.
  virtual StringRef getSectionName() const {
    llvm_unreachable("internal error");
  }

  // Called if the garbage collector decides to not include this chunk
  // in a final output. It's supposed to print out a log message. It
  // is illegal to call this function on non-section chunks because
  // only section chunks are subject of garbage collection.
  virtual void printDiscardMessage() { llvm_unreachable("internal error"); }

  // Used by the garbage collector.
  virtual bool isRoot() { return false; }
  virtual bool isLive() { return true; }
  virtual void markLive() {}

  // An output section has pointers to chunks in the section, and each
  // chunk has a back pointer to an output section.
  void setOutputSection(OutputSection *O) { Out = O; }
  OutputSection *getOutputSection() { return Out; }

protected:
  // The RVA of this chunk in the output. The writer sets a value.
  uint64_t RVA = 0;

  // The offset from beginning of the output file. The writer sets a
  // value.
  uint64_t FileOff = 0;

  // The alignment of this chunk. The writer uses the value.
  uint32_t Align = 1;

  // The output section for this chunk.
  OutputSection *Out = nullptr;
};

// A chunk representing a section of an input file.
class SectionChunk : public Chunk {
public:
  SectionChunk(ObjectFile *File, const coff_section *Header,
               uint32_t SectionIndex);
  const uint8_t *getData() const override;
  size_t getSize() const override { return Header->SizeOfRawData; }
  void applyRelocations(uint8_t *Buffer) override;
  bool isBSS() const override;
  bool isCOMDAT() const override;
  uint32_t getPermissions() const override;
  StringRef getSectionName() const override { return SectionName; }
  void printDiscardMessage() override;

  bool isRoot() override;
  void markLive() override;
  bool isLive() override { return isRoot() || Live; }

  // Adds COMDAT associative sections to this COMDAT section. A chunk
  // and its children are treated as a group by the garbage collector.
  void addAssociative(SectionChunk *Child);

private:
  SectionRef getSectionRef();
  void applyReloc(uint8_t *Buffer, const coff_relocation *Rel);

  // A file this chunk was created from.
  ObjectFile *File;

  const coff_section *Header;
  uint32_t SectionIndex;
  StringRef SectionName;
  ArrayRef<uint8_t> Data;
  bool Live = false;
  std::vector<Chunk *> AssocChildren;
  bool IsAssocChild = false;
};

// A chunk for common symbols. Common chunks don't have actual data.
class CommonChunk : public Chunk {
public:
  CommonChunk(const COFFSymbolRef S) : Sym(S) {}
  size_t getSize() const override { return Sym.getValue(); }
  bool isBSS() const override { return true; }
  uint32_t getPermissions() const override;
  StringRef getSectionName() const override { return ".bss"; }

  const uint8_t *getData() const override {
    llvm_unreachable("internal error");
  }

private:
  const COFFSymbolRef Sym;
};

// A chunk for linker-created strings.
class StringChunk : public Chunk {
public:
  StringChunk(StringRef S) : Data(S.size() + 1) {
    memcpy(Data.data(), S.data(), S.size());
    Data[S.size()] = 0;
  }

  const uint8_t *getData() const override { return &Data[0]; }
  size_t getSize() const override { return Data.size(); }

private:
  std::vector<uint8_t> Data;
};

static const uint8_t ImportFuncData[] = {
    0xff, 0x25, 0x00, 0x00, 0x00, 0x00, // JMP *0x0
};

// A chunk for DLL import jump table entry. In a final output, it's
// contents will be a JMP instruction to some __imp_ symbol.
class ImportFuncChunk : public Chunk {
public:
  ImportFuncChunk(Defined *S)
      : ImpSymbol(S),
        Data(ImportFuncData, ImportFuncData + sizeof(ImportFuncData)) {}

  const uint8_t *getData() const override { return &Data[0]; }
  size_t getSize() const override { return Data.size(); }
  void applyRelocations(uint8_t *Buffer) override;

private:
  Defined *ImpSymbol;
  std::vector<uint8_t> Data;
};

// A chunk for the import descriptor table.
class HintNameChunk : public Chunk {
public:
  HintNameChunk(StringRef Name);
  const uint8_t *getData() const override { return Data.data(); }
  size_t getSize() const override { return Data.size(); }
  void applyRelocations(uint8_t *Buffer) override {}

private:
  std::vector<uint8_t> Data;
};

// A chunk for the import descriptor table.
class LookupChunk : public Chunk {
public:
  LookupChunk(HintNameChunk *H) : HintName(H) {}
  const uint8_t *getData() const override { return (const uint8_t *)&Ent; }
  size_t getSize() const override { return sizeof(Ent); }
  void applyRelocations(uint8_t *Buffer) override;
  HintNameChunk *HintName;

private:
  uint64_t Ent = 0;
};

// A chunk for the import descriptor table.
class DirectoryChunk : public Chunk {
public:
  DirectoryChunk(StringChunk *N) : DLLName(N) {}
  const uint8_t *getData() const override { return (const uint8_t *)&Ent; }
  size_t getSize() const override { return sizeof(Ent); }
  void applyRelocations(uint8_t *Buffer) override;

  StringChunk *DLLName;
  LookupChunk *LookupTab;
  LookupChunk *AddressTab;

private:
  llvm::COFF::ImportDirectoryTableEntry Ent = {};
};

// A chunk for the import descriptor table.
class NullChunk : public Chunk {
public:
  NullChunk(size_t Size) : Data(Size) {}
  const uint8_t *getData() const override { return Data.data(); }
  size_t getSize() const override { return Data.size(); }
  void applyRelocations(uint8_t *Buffer) override {}

private:
  std::vector<uint8_t> Data;
};

// ImportTable creates a set of import table chunks for a given
// DLL-imported symbols.
class ImportTable {
public:
  ImportTable(StringRef DLLName, std::vector<DefinedImportData *> &Symbols);
  StringChunk *DLLName;
  DirectoryChunk *DirTab;
  std::vector<LookupChunk *> LookupTables;
  std::vector<LookupChunk *> AddressTables;
  std::vector<HintNameChunk *> HintNameTables;
};

} // namespace coff
} // namespace lld

#endif
