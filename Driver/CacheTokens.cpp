//===--- CacheTokens.cpp - Caching of lexer tokens for PTH support --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This provides a possible implementation of PTH support for Clang that is
// based on caching lexed tokens and identifiers.
//
//===----------------------------------------------------------------------===//

#include "clang.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/System/Path.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Streams.h"

using namespace clang;

typedef uint32_t Offset;

static void Emit8(llvm::raw_ostream& Out, uint32_t V) {
  Out << (unsigned char)(V);
}

static void Emit16(llvm::raw_ostream& Out, uint32_t V) {
  Out << (unsigned char)(V);
  Out << (unsigned char)(V >>  8);
  assert((V >> 16) == 0);
}

static void Emit32(llvm::raw_ostream& Out, uint32_t V) {
  Out << (unsigned char)(V);
  Out << (unsigned char)(V >>  8);
  Out << (unsigned char)(V >> 16);
  Out << (unsigned char)(V >> 24);
}

static void Emit64(llvm::raw_ostream& Out, uint64_t V) {
  Out << (unsigned char)(V);
  Out << (unsigned char)(V >>  8);
  Out << (unsigned char)(V >> 16);
  Out << (unsigned char)(V >> 24);
  Out << (unsigned char)(V >> 32);
  Out << (unsigned char)(V >> 40);
  Out << (unsigned char)(V >> 48);
  Out << (unsigned char)(V >> 56);
}

static void Pad(llvm::raw_fd_ostream& Out, unsigned A) {
  Offset off = (Offset) Out.tell();
  uint32_t n = ((uintptr_t)(off+A-1) & ~(uintptr_t)(A-1)) - off;
  for ( ; n ; --n ) Emit8(Out, 0);
}

// Bernstein hash function:
// This is basically copy-and-paste from StringMap.  This likely won't
// stay here, which is why I didn't both to expose this function from
// String Map.
static unsigned BernsteinHash(const char* x) {  
  unsigned int R = 0;
  for ( ; *x != '\0' ; ++x) R = R * 33 + *x;
  return R + (R >> 5);
}

//===----------------------------------------------------------------------===//
// On Disk Hashtable Logic.  This will eventually get refactored and put
// elsewhere.
//===----------------------------------------------------------------------===//

template<typename Info>
class OnDiskChainedHashTableGenerator {
  unsigned NumBuckets;
  unsigned NumEntries;
  llvm::BumpPtrAllocator BA;
  
  class Item {
  public:
    typename Info::key_type key;
    typename Info::data_type data;
    Item *next;
    const uint32_t hash;
    
    Item(typename Info::key_type_ref k, typename Info::data_type_ref d)
    : key(k), data(d), next(0), hash(Info::ComputeHash(k)) {}
  };
  
  class Bucket { 
  public:
    Offset off;
    Item*  head;
    unsigned length;
    
    Bucket() {}
  };
  
  Bucket* Buckets;
  
private:
  void insert(Bucket* b, size_t size, Item* E) {
    unsigned idx = E->hash & (size - 1);
    Bucket& B = b[idx];
    E->next = B.head;
    ++B.length;
    B.head = E;
  }
  
  void resize(size_t newsize) {
    Bucket* newBuckets = (Bucket*) calloc(newsize, sizeof(Bucket));
    // Populate newBuckets with the old entries.
    for (unsigned i = 0; i < NumBuckets; ++i)
      for (Item* E = Buckets[i].head; E ; ) {
        Item* N = E->next;
        E->next = 0;
        insert(newBuckets, newsize, E);
        E = N;
      }
    
    free(Buckets);
    NumBuckets = newsize;
    Buckets = newBuckets;
  }  
  
public:
  
  void insert(typename Info::key_type_ref key,
              typename Info::data_type_ref data) {

    ++NumEntries;
    if (4*NumEntries >= 3*NumBuckets) resize(NumBuckets*2);
    insert(Buckets, NumBuckets, new (BA.Allocate<Item>()) Item(key, data));
  }
  
  Offset Emit(llvm::raw_fd_ostream& out) {
    // Emit the payload of the table.
    for (unsigned i = 0; i < NumBuckets; ++i) {
      Bucket& B = Buckets[i];
      if (!B.head) continue;
      
      // Store the offset for the data of this bucket.
      B.off = out.tell();
      
      // Write out the number of items in the bucket.
      Emit16(out, B.length);
      
      // Write out the entries in the bucket.
      for (Item *I = B.head; I ; I = I->next) {
        Emit32(out, I->hash);
        const std::pair<unsigned, unsigned>& Len = 
          Info::EmitKeyDataLength(out, I->key, I->data);
        Info::EmitKey(out, I->key, Len.first);
        Info::EmitData(out, I->key, I->data, Len.second);
      }
    }
    
    // Emit the hashtable itself.
    Pad(out, 4);
    Offset TableOff = out.tell();
    Emit32(out, NumBuckets);
    Emit32(out, NumEntries);
    for (unsigned i = 0; i < NumBuckets; ++i) Emit32(out, Buckets[i].off);
    
    return TableOff;
  }
  
  OnDiskChainedHashTableGenerator() {
    NumEntries = 0;
    NumBuckets = 64;    
    // Note that we do not need to run the constructors of the individual
    // Bucket objects since 'calloc' returns bytes that are all 0.
    Buckets = (Bucket*) calloc(NumBuckets, sizeof(Bucket));
  }
  
  ~OnDiskChainedHashTableGenerator() {
    free(Buckets);
  }
};

//===----------------------------------------------------------------------===//
// PTH-specific stuff.
//===----------------------------------------------------------------------===//

namespace {
class VISIBILITY_HIDDEN PTHEntry {
  Offset TokenData, PPCondData;  

public:  
  PTHEntry() {}

  PTHEntry(Offset td, Offset ppcd)
    : TokenData(td), PPCondData(ppcd) {}
  
  Offset getTokenOffset() const { return TokenData; }  
  Offset getPPCondTableOffset() const { return PPCondData; }
};
  
  
class VISIBILITY_HIDDEN PTHEntryKeyVariant {
  union { const FileEntry* FE; const DirectoryEntry* DE; const char* Path; };
  enum { IsFE = 0x1, IsDE = 0x2, IsNoExist = 0x0 } Kind;
public:
  PTHEntryKeyVariant(const FileEntry *fe) : FE(fe), Kind(IsFE) {}
  PTHEntryKeyVariant(const DirectoryEntry *de) : DE(de), Kind(IsDE) {}
  PTHEntryKeyVariant(const char* path) : Path(path), Kind(IsNoExist) {}
  
  const FileEntry *getFile() const { return Kind == IsFE ? FE : 0; }
  const DirectoryEntry *getDir() const { return Kind == IsDE ? DE : 0; }
  const char* getNameOfNonExistantFile() const {
    return Kind == IsNoExist ? Path : 0;
  }
  
  const char* getCString() const {
    switch (Kind) {
      case IsFE: return FE->getName();
      case IsDE: return DE->getName();
      default: return Path;
    }
  }
  
  unsigned getKind() const { return (unsigned) Kind; }
  
  void EmitData(llvm::raw_ostream& Out) {
    switch (Kind) {
      case IsFE:
        // Emit stat information.
        ::Emit32(Out, FE->getInode());
        ::Emit32(Out, FE->getDevice());
        ::Emit16(Out, FE->getFileMode());
        ::Emit64(Out, FE->getModificationTime());
        ::Emit64(Out, FE->getSize());
        break;
      case IsDE:
        // FIXME
      default: break;
        // Emit nothing.        
    }
  }
  
  unsigned getRepresentationLength() const {
    switch (Kind) {
      case IsFE: return 4 + 4 + 2 + 8 + 8;
      case IsDE: // FIXME
      default: return 0;
    }
  }
};
  
class VISIBILITY_HIDDEN FileEntryPTHEntryInfo {
public:
  typedef PTHEntryKeyVariant key_type;
  typedef key_type key_type_ref;
  
  typedef PTHEntry data_type;
  typedef const PTHEntry& data_type_ref;
  
  static unsigned ComputeHash(PTHEntryKeyVariant V) {
    return BernsteinHash(V.getCString());
  }
  
  static std::pair<unsigned,unsigned> 
  EmitKeyDataLength(llvm::raw_ostream& Out, PTHEntryKeyVariant V,
                    const PTHEntry& E) {

    unsigned n = strlen(V.getCString()) + 1 + 1;
    ::Emit16(Out, n);
    
    unsigned m = V.getRepresentationLength() + (V.getFile() ? 4 + 4 : 0);
    ::Emit8(Out, m);

    return std::make_pair(n, m);
  }
  
  static void EmitKey(llvm::raw_ostream& Out, PTHEntryKeyVariant V, unsigned n){
    // Emit the entry kind.
    ::Emit8(Out, (unsigned) V.getKind());
    // Emit the string.
    Out.write(V.getCString(), n - 1);
  }
  
  static void EmitData(llvm::raw_ostream& Out, PTHEntryKeyVariant V, 
                       const PTHEntry& E, unsigned) {


    // For file entries emit the offsets into the PTH file for token data
    // and the preprocessor blocks table.
    if (V.getFile()) {
      ::Emit32(Out, E.getTokenOffset());
      ::Emit32(Out, E.getPPCondTableOffset());
    }
    
    // Emit any other data associated with the key (i.e., stat information).
    V.EmitData(Out);
  }        
};
  
class OffsetOpt {
  bool valid;
  Offset off;
public:
  OffsetOpt() : valid(false) {}
  bool hasOffset() const { return valid; }
  Offset getOffset() const { assert(valid); return off; }
  void setOffset(Offset o) { off = o; valid = true; }
};
} // end anonymous namespace

typedef OnDiskChainedHashTableGenerator<FileEntryPTHEntryInfo> PTHMap;
typedef llvm::DenseMap<const IdentifierInfo*,uint32_t> IDMap;
typedef llvm::StringMap<OffsetOpt, llvm::BumpPtrAllocator> CachedStrsTy;

namespace {
class VISIBILITY_HIDDEN PTHWriter {
  IDMap IM;
  llvm::raw_fd_ostream& Out;
  Preprocessor& PP;
  uint32_t idcount;
  PTHMap PM;
  CachedStrsTy CachedStrs;
  Offset CurStrOffset;
  std::vector<llvm::StringMapEntry<OffsetOpt>*> StrEntries;

  //// Get the persistent id for the given IdentifierInfo*.
  uint32_t ResolveID(const IdentifierInfo* II);
  
  /// Emit a token to the PTH file.
  void EmitToken(const Token& T);

  void Emit8(uint32_t V) {
    Out << (unsigned char)(V);
  }
    
  void Emit16(uint32_t V) { ::Emit16(Out, V); }
  
  void Emit24(uint32_t V) {
    Out << (unsigned char)(V);
    Out << (unsigned char)(V >>  8);
    Out << (unsigned char)(V >> 16);
    assert((V >> 24) == 0);
  }

  void Emit32(uint32_t V) { ::Emit32(Out, V); }

  void EmitBuf(const char* I, const char* E) {
    for ( ; I != E ; ++I) Out << *I;
  }
  
  /// EmitIdentifierTable - Emits two tables to the PTH file.  The first is
  ///  a hashtable mapping from identifier strings to persistent IDs.
  ///  The second is a straight table mapping from persistent IDs to string data
  ///  (the keys of the first table).
  std::pair<Offset, Offset> EmitIdentifierTable();
  
  /// EmitFileTable - Emit a table mapping from file name strings to PTH
  /// token data.
  Offset EmitFileTable() { return PM.Emit(Out); }

  PTHEntry LexTokens(Lexer& L);
  Offset EmitCachedSpellings();
  
public:
  PTHWriter(llvm::raw_fd_ostream& out, Preprocessor& pp) 
    : Out(out), PP(pp), idcount(0), CurStrOffset(0) {}
    
  void GeneratePTH();
};
} // end anonymous namespace
  
uint32_t PTHWriter::ResolveID(const IdentifierInfo* II) {  
  // Null IdentifierInfo's map to the persistent ID 0.
  if (!II)
    return 0;
  
  IDMap::iterator I = IM.find(II);

  if (I == IM.end()) {
    IM[II] = ++idcount; // Pre-increment since '0' is reserved for NULL.
    return idcount;
  }
  
  return I->second; // We've already added 1.
}

void PTHWriter::EmitToken(const Token& T) {
  Emit32(((uint32_t) T.getKind()) |
         (((uint32_t) T.getFlags()) << 8) |
         (((uint32_t) T.getLength()) << 16));

  // Literals (strings, numbers, characters) get cached spellings.
  if (T.isLiteral()) {
    // FIXME: This uses the slow getSpelling().  Perhaps we do better
    // in the future?  This only slows down PTH generation.
    const std::string &spelling = PP.getSpelling(T);
    const char* s = spelling.c_str();
    
    // Get the string entry.
    llvm::StringMapEntry<OffsetOpt> *E =
    &CachedStrs.GetOrCreateValue(s, s+spelling.size());
    
    if (!E->getValue().hasOffset()) {
      E->getValue().setOffset(CurStrOffset);
      StrEntries.push_back(E);
      CurStrOffset += spelling.size() + 1;
    }
    
    Emit32(E->getValue().getOffset());
  }
  else
    Emit32(ResolveID(T.getIdentifierInfo()));
    
  Emit32(PP.getSourceManager().getFileOffset(T.getLocation()));
}

PTHEntry PTHWriter::LexTokens(Lexer& L) {
  // Pad 0's so that we emit tokens to a 4-byte alignment.
  // This speed up reading them back in.
  Pad(Out, 4);
  Offset off = (Offset) Out.tell();
  
  // Keep track of matching '#if' ... '#endif'.
  typedef std::vector<std::pair<Offset, unsigned> > PPCondTable;
  PPCondTable PPCond;
  std::vector<unsigned> PPStartCond;
  bool ParsingPreprocessorDirective = false;
  Token Tok;
  
  do {
    L.LexFromRawLexer(Tok);
  NextToken:

    if ((Tok.isAtStartOfLine() || Tok.is(tok::eof)) &&
        ParsingPreprocessorDirective) {
      // Insert an eom token into the token cache.  It has the same
      // position as the next token that is not on the same line as the
      // preprocessor directive.  Observe that we continue processing
      // 'Tok' when we exit this branch.
      Token Tmp = Tok;
      Tmp.setKind(tok::eom);
      Tmp.clearFlag(Token::StartOfLine);
      Tmp.setIdentifierInfo(0);
      EmitToken(Tmp);
      ParsingPreprocessorDirective = false;
    }
    
    if (Tok.is(tok::identifier)) {
      Tok.setIdentifierInfo(PP.LookUpIdentifierInfo(Tok));
      EmitToken(Tok);
      continue;
    }

    if (Tok.is(tok::hash) && Tok.isAtStartOfLine()) {
      // Special processing for #include.  Store the '#' token and lex
      // the next token.
      assert(!ParsingPreprocessorDirective);
      Offset HashOff = (Offset) Out.tell();
      EmitToken(Tok);

      // Get the next token.
      L.LexFromRawLexer(Tok);
            
      assert(!Tok.isAtStartOfLine());
      
      // Did we see 'include'/'import'/'include_next'?
      if (!Tok.is(tok::identifier)) {
        EmitToken(Tok);
        continue;
      }
      
      IdentifierInfo* II = PP.LookUpIdentifierInfo(Tok);
      Tok.setIdentifierInfo(II);
      tok::PPKeywordKind K = II->getPPKeywordID();
      
      assert(K != tok::pp_not_keyword);
      ParsingPreprocessorDirective = true;
      
      switch (K) {
      default:
        break;
      case tok::pp_include:
      case tok::pp_import:
      case tok::pp_include_next: {        
        // Save the 'include' token.
        EmitToken(Tok);
        // Lex the next token as an include string.
        L.setParsingPreprocessorDirective(true);
        L.LexIncludeFilename(Tok); 
        L.setParsingPreprocessorDirective(false);
        assert(!Tok.isAtStartOfLine());
        if (Tok.is(tok::identifier))
          Tok.setIdentifierInfo(PP.LookUpIdentifierInfo(Tok));
        
        break;
      }
      case tok::pp_if:
      case tok::pp_ifdef:
      case tok::pp_ifndef: {
        // Add an entry for '#if' and friends.  We initially set the target
        // index to 0.  This will get backpatched when we hit #endif.
        PPStartCond.push_back(PPCond.size());
        PPCond.push_back(std::make_pair(HashOff, 0U));
        break;
      }
      case tok::pp_endif: {
        // Add an entry for '#endif'.  We set the target table index to itself.
        // This will later be set to zero when emitting to the PTH file.  We
        // use 0 for uninitialized indices because that is easier to debug.
        unsigned index = PPCond.size();
        // Backpatch the opening '#if' entry.
        assert(!PPStartCond.empty());
        assert(PPCond.size() > PPStartCond.back());
        assert(PPCond[PPStartCond.back()].second == 0);
        PPCond[PPStartCond.back()].second = index;
        PPStartCond.pop_back();        
        // Add the new entry to PPCond.      
        PPCond.push_back(std::make_pair(HashOff, index));
        EmitToken(Tok);
        
        // Some files have gibberish on the same line as '#endif'.
        // Discard these tokens.
        do L.LexFromRawLexer(Tok); while (!Tok.is(tok::eof) &&
                                          !Tok.isAtStartOfLine());
        // We have the next token in hand.
        // Don't immediately lex the next one.
        goto NextToken;        
      }
      case tok::pp_elif:
      case tok::pp_else: {
        // Add an entry for #elif or #else.
        // This serves as both a closing and opening of a conditional block.
        // This means that its entry will get backpatched later.
        unsigned index = PPCond.size();
        // Backpatch the previous '#if' entry.
        assert(!PPStartCond.empty());
        assert(PPCond.size() > PPStartCond.back());
        assert(PPCond[PPStartCond.back()].second == 0);
        PPCond[PPStartCond.back()].second = index;
        PPStartCond.pop_back();
        // Now add '#elif' as a new block opening.
        PPCond.push_back(std::make_pair(HashOff, 0U));
        PPStartCond.push_back(index);
        break;
      }
      }
    }
    
    EmitToken(Tok);
  }
  while (Tok.isNot(tok::eof));

  assert(PPStartCond.empty() && "Error: imblanced preprocessor conditionals.");

  // Next write out PPCond.
  Offset PPCondOff = (Offset) Out.tell();

  // Write out the size of PPCond so that clients can identifer empty tables.
  Emit32(PPCond.size());

  for (unsigned i = 0, e = PPCond.size(); i!=e; ++i) {
    Emit32(PPCond[i].first - off);
    uint32_t x = PPCond[i].second;
    assert(x != 0 && "PPCond entry not backpatched.");
    // Emit zero for #endifs.  This allows us to do checking when
    // we read the PTH file back in.
    Emit32(x == i ? 0 : x);
  }

  return PTHEntry(off, PPCondOff);
}

Offset PTHWriter::EmitCachedSpellings() {
  // Write each cached strings to the PTH file.
  Offset SpellingsOff = Out.tell();
  
  for (std::vector<llvm::StringMapEntry<OffsetOpt>*>::iterator
       I = StrEntries.begin(), E = StrEntries.end(); I!=E; ++I) {

    const char* data = (*I)->getKeyData();
    EmitBuf(data, data + (*I)->getKeyLength());
    Emit8('\0');
  }
  
  return SpellingsOff;
}

void PTHWriter::GeneratePTH() {
  // Generate the prologue.
  Out << "cfe-pth";
  Emit32(PTHManager::Version);
  
  // Leave 4 words for the prologue.
  Offset PrologueOffset = Out.tell();
  for (unsigned i = 0; i < 4 * sizeof(uint32_t); ++i) Emit8(0);
  
  // Iterate over all the files in SourceManager.  Create a lexer
  // for each file and cache the tokens.
  SourceManager &SM = PP.getSourceManager();
  const LangOptions &LOpts = PP.getLangOptions();
  
  for (SourceManager::fileinfo_iterator I = SM.fileinfo_begin(),
       E = SM.fileinfo_end(); I != E; ++I) {
    const SrcMgr::ContentCache &C = *I->second;
    const FileEntry *FE = C.Entry;
    
    // FIXME: Handle files with non-absolute paths.
    llvm::sys::Path P(FE->getName());
    if (!P.isAbsolute())
      continue;

    const llvm::MemoryBuffer *B = C.getBuffer();
    if (!B) continue;

    FileID FID = SM.createFileID(FE, SourceLocation(), SrcMgr::C_User);
    Lexer L(FID, SM, LOpts);
    PM.insert(FE, LexTokens(L));
  }

  // Write out the identifier table.
  const std::pair<Offset,Offset>& IdTableOff = EmitIdentifierTable();
  
  // Write out the cached strings table.
  Offset SpellingOff = EmitCachedSpellings();
  
  // Write out the file table.
  Offset FileTableOff = EmitFileTable();  
  
  // Finally, write the prologue.
  Out.seek(PrologueOffset);
  Emit32(IdTableOff.first);
  Emit32(IdTableOff.second);
  Emit32(FileTableOff);
  Emit32(SpellingOff);
}

void clang::CacheTokens(Preprocessor& PP, const std::string& OutFile) {
  // Lex through the entire file.  This will populate SourceManager with
  // all of the header information.
  Token Tok;
  PP.EnterMainSourceFile();
  do { PP.Lex(Tok); } while (Tok.isNot(tok::eof));
  
  // Open up the PTH file.
  std::string ErrMsg;
  llvm::raw_fd_ostream Out(OutFile.c_str(), true, ErrMsg);
  
  if (!ErrMsg.empty()) {
    llvm::errs() << "PTH error: " << ErrMsg << "\n";
    return;
  }
  
  // Create the PTHWriter and generate the PTH file.
  PTHWriter PW(Out, PP);
  PW.GeneratePTH();
}

//===----------------------------------------------------------------------===//

namespace {
class VISIBILITY_HIDDEN PTHIdKey {
public:
  const IdentifierInfo* II;
  uint32_t FileOffset;
};

class VISIBILITY_HIDDEN PTHIdentifierTableTrait {
public:
  typedef PTHIdKey* key_type;
  typedef key_type  key_type_ref;
  
  typedef uint32_t  data_type;
  typedef data_type data_type_ref;
  
  static unsigned ComputeHash(PTHIdKey* key) {
    return BernsteinHash(key->II->getName());
  }
  
  static std::pair<unsigned,unsigned> 
  EmitKeyDataLength(llvm::raw_ostream& Out, const PTHIdKey* key, uint32_t) {    
    unsigned n = strlen(key->II->getName()) + 1;
    ::Emit16(Out, n);
    return std::make_pair(n, sizeof(uint32_t));
  }
  
  static void EmitKey(llvm::raw_fd_ostream& Out, PTHIdKey* key, unsigned n) {
    // Record the location of the key data.  This is used when generating
    // the mapping from persistent IDs to strings.
    key->FileOffset = Out.tell();
    Out.write(key->II->getName(), n);
  }
  
  static void EmitData(llvm::raw_ostream& Out, PTHIdKey*, uint32_t pID,
                       unsigned) {
    ::Emit32(Out, pID);
  }        
};
} // end anonymous namespace

/// EmitIdentifierTable - Emits two tables to the PTH file.  The first is
///  a hashtable mapping from identifier strings to persistent IDs.  The second
///  is a straight table mapping from persistent IDs to string data (the
///  keys of the first table).
///
std::pair<Offset,Offset> PTHWriter::EmitIdentifierTable() {
  // Build two maps:
  //  (1) an inverse map from persistent IDs -> (IdentifierInfo*,Offset)
  //  (2) a map from (IdentifierInfo*, Offset)* -> persistent IDs

  // Note that we use 'calloc', so all the bytes are 0.
  PTHIdKey* IIDMap = (PTHIdKey*) calloc(idcount, sizeof(PTHIdKey));

  // Create the hashtable.
  OnDiskChainedHashTableGenerator<PTHIdentifierTableTrait> IIOffMap;
  
  // Generate mapping from persistent IDs -> IdentifierInfo*.
  for (IDMap::iterator I=IM.begin(), E=IM.end(); I!=E; ++I) {
    // Decrement by 1 because we are using a vector for the lookup and
    // 0 is reserved for NULL.
    assert(I->second > 0);
    assert(I->second-1 < idcount);
    unsigned idx = I->second-1;
    
    // Store the mapping from persistent ID to IdentifierInfo*
    IIDMap[idx].II = I->first;
    
    // Store the reverse mapping in a hashtable.
    IIOffMap.insert(&IIDMap[idx], I->second);
  }
  
  // Write out the inverse map first.  This causes the PCIDKey entries to
  // record PTH file offsets for the string data.  This is used to write
  // the second table.
  Offset StringTableOffset = IIOffMap.Emit(Out);
  
  // Now emit the table mapping from persistent IDs to PTH file offsets.  
  Offset IDOff = Out.tell();
  Emit32(idcount);  // Emit the number of identifiers.
  for (unsigned i = 0 ; i < idcount; ++i) Emit32(IIDMap[i].FileOffset);
  
  // Finally, release the inverse map.
  free(IIDMap);
  
  return std::make_pair(IDOff, StringTableOffset);
}


