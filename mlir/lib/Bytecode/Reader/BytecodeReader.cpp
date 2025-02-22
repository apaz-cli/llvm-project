//===- BytecodeReader.cpp - MLIR Bytecode Reader --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// TODO: Support for big-endian architectures.
// TODO: Properly preserve use lists of values.

#include "mlir/Bytecode/BytecodeReader.h"
#include "../Encoding.h"
#include "mlir/AsmParser/AsmParser.h"
#include "mlir/IR/BuiltinDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/SaveAndRestore.h"

#define DEBUG_TYPE "mlir-bytecode-reader"

using namespace mlir;

/// Stringify the given section ID.
static std::string toString(bytecode::Section::ID sectionID) {
  switch (sectionID) {
  case bytecode::Section::kString:
    return "String (0)";
  case bytecode::Section::kDialect:
    return "Dialect (1)";
  case bytecode::Section::kAttrType:
    return "AttrType (2)";
  case bytecode::Section::kAttrTypeOffset:
    return "AttrTypeOffset (3)";
  case bytecode::Section::kIR:
    return "IR (4)";
  default:
    return ("Unknown (" + Twine(static_cast<unsigned>(sectionID)) + ")").str();
  }
}

//===----------------------------------------------------------------------===//
// EncodingReader
//===----------------------------------------------------------------------===//

namespace {
class EncodingReader {
public:
  explicit EncodingReader(ArrayRef<uint8_t> contents, Location fileLoc)
      : dataIt(contents.data()), dataEnd(contents.end()), fileLoc(fileLoc) {}
  explicit EncodingReader(StringRef contents, Location fileLoc)
      : EncodingReader({reinterpret_cast<const uint8_t *>(contents.data()),
                        contents.size()},
                       fileLoc) {}

  /// Returns true if the entire section has been read.
  bool empty() const { return dataIt == dataEnd; }

  /// Returns the remaining size of the bytecode.
  size_t size() const { return dataEnd - dataIt; }

  /// Emit an error using the given arguments.
  template <typename... Args>
  LogicalResult emitError(Args &&...args) const {
    return ::emitError(fileLoc).append(std::forward<Args>(args)...);
  }

  /// Parse a single byte from the stream.
  template <typename T>
  LogicalResult parseByte(T &value) {
    if (empty())
      return emitError("attempting to parse a byte at the end of the bytecode");
    value = static_cast<T>(*dataIt++);
    return success();
  }
  /// Parse a range of bytes of 'length' into the given result.
  LogicalResult parseBytes(size_t length, ArrayRef<uint8_t> &result) {
    if (length > size()) {
      return emitError("attempting to parse ", length, " bytes when only ",
                       size(), " remain");
    }
    result = {dataIt, length};
    dataIt += length;
    return success();
  }
  /// Parse a range of bytes of 'length' into the given result, which can be
  /// assumed to be large enough to hold `length`.
  LogicalResult parseBytes(size_t length, uint8_t *result) {
    if (length > size()) {
      return emitError("attempting to parse ", length, " bytes when only ",
                       size(), " remain");
    }
    memcpy(result, dataIt, length);
    dataIt += length;
    return success();
  }

  /// Parse a variable length encoded integer from the byte stream. The first
  /// encoded byte contains a prefix in the low bits indicating the encoded
  /// length of the value. This length prefix is a bit sequence of '0's followed
  /// by a '1'. The number of '0' bits indicate the number of _additional_ bytes
  /// (not including the prefix byte). All remaining bits in the first byte,
  /// along with all of the bits in additional bytes, provide the value of the
  /// integer encoded in little-endian order.
  LogicalResult parseVarInt(uint64_t &result) {
    // Parse the first byte of the encoding, which contains the length prefix.
    if (failed(parseByte(result)))
      return failure();

    // Handle the overwhelmingly common case where the value is stored in a
    // single byte. In this case, the first bit is the `1` marker bit.
    if (LLVM_LIKELY(result & 1)) {
      result >>= 1;
      return success();
    }

    // Handle the overwhelming uncommon case where the value required all 8
    // bytes (i.e. a really really big number). In this case, the marker byte is
    // all zeros: `00000000`.
    if (LLVM_UNLIKELY(result == 0))
      return parseBytes(sizeof(result), reinterpret_cast<uint8_t *>(&result));
    return parseMultiByteVarInt(result);
  }

  /// Parse a variable length encoded integer whose low bit is used to encode an
  /// unrelated flag, i.e: `(integerValue << 1) | (flag ? 1 : 0)`.
  LogicalResult parseVarIntWithFlag(uint64_t &result, bool &flag) {
    if (failed(parseVarInt(result)))
      return failure();
    flag = result & 1;
    result >>= 1;
    return success();
  }

  /// Skip the first `length` bytes within the reader.
  LogicalResult skipBytes(size_t length) {
    if (length > size()) {
      return emitError("attempting to skip ", length, " bytes when only ",
                       size(), " remain");
    }
    dataIt += length;
    return success();
  }

  /// Parse a null-terminated string into `result` (without including the NUL
  /// terminator).
  LogicalResult parseNullTerminatedString(StringRef &result) {
    const char *startIt = (const char *)dataIt;
    const char *nulIt = (const char *)memchr(startIt, 0, size());
    if (!nulIt)
      return emitError(
          "malformed null-terminated string, no null character found");

    result = StringRef(startIt, nulIt - startIt);
    dataIt = (const uint8_t *)nulIt + 1;
    return success();
  }

  /// Parse a section header, placing the kind of section in `sectionID` and the
  /// contents of the section in `sectionData`.
  LogicalResult parseSection(bytecode::Section::ID &sectionID,
                             ArrayRef<uint8_t> &sectionData) {
    uint64_t length;
    if (failed(parseByte(sectionID)) || failed(parseVarInt(length)))
      return failure();
    if (sectionID >= bytecode::Section::kNumSections)
      return emitError("invalid section ID: ", unsigned(sectionID));

    // Parse the actua section data now that we have its length.
    return parseBytes(static_cast<size_t>(length), sectionData);
  }

private:
  /// Parse a variable length encoded integer from the byte stream. This method
  /// is a fallback when the number of bytes used to encode the value is greater
  /// than 1, but less than the max (9). The provided `result` value can be
  /// assumed to already contain the first byte of the value.
  /// NOTE: This method is marked noinline to avoid pessimizing the common case
  /// of single byte encoding.
  LLVM_ATTRIBUTE_NOINLINE LogicalResult parseMultiByteVarInt(uint64_t &result) {
    // Count the number of trailing zeros in the marker byte, this indicates the
    // number of trailing bytes that are part of the value. We use `uint32_t`
    // here because we only care about the first byte, and so that be actually
    // get ctz intrinsic calls when possible (the `uint8_t` overload uses a loop
    // implementation).
    uint32_t numBytes =
        llvm::countTrailingZeros<uint32_t>(result, llvm::ZB_Undefined);
    assert(numBytes > 0 && numBytes <= 7 &&
           "unexpected number of trailing zeros in varint encoding");

    // Parse in the remaining bytes of the value.
    if (failed(parseBytes(numBytes, reinterpret_cast<uint8_t *>(&result) + 1)))
      return failure();

    // Shift out the low-order bits that were used to mark how the value was
    // encoded.
    result >>= (numBytes + 1);
    return success();
  }

  /// The current data iterator, and an iterator to the end of the buffer.
  const uint8_t *dataIt, *dataEnd;

  /// A location for the bytecode used to report errors.
  Location fileLoc;
};
} // namespace

/// Resolve an index into the given entry list. `entry` may either be a
/// reference, in which case it is assigned to the corresponding value in
/// `entries`, or a pointer, in which case it is assigned to the address of the
/// element in `entries`.
template <typename RangeT, typename T>
static LogicalResult resolveEntry(EncodingReader &reader, RangeT &entries,
                                  uint64_t index, T &entry,
                                  StringRef entryStr) {
  if (index >= entries.size())
    return reader.emitError("invalid ", entryStr, " index: ", index);

  // If the provided entry is a pointer, resolve to the address of the entry.
  if constexpr (std::is_convertible_v<llvm::detail::ValueOfRange<RangeT>, T>)
    entry = entries[index];
  else
    entry = &entries[index];
  return success();
}

/// Parse and resolve an index into the given entry list.
template <typename RangeT, typename T>
static LogicalResult parseEntry(EncodingReader &reader, RangeT &entries,
                                T &entry, StringRef entryStr) {
  uint64_t entryIdx;
  if (failed(reader.parseVarInt(entryIdx)))
    return failure();
  return resolveEntry(reader, entries, entryIdx, entry, entryStr);
}

//===----------------------------------------------------------------------===//
// BytecodeDialect
//===----------------------------------------------------------------------===//

namespace {
/// This struct represents a dialect entry within the bytecode.
struct BytecodeDialect {
  /// Load the dialect into the provided context if it hasn't been loaded yet.
  /// Returns failure if the dialect couldn't be loaded *and* the provided
  /// context does not allow unregistered dialects. The provided reader is used
  /// for error emission if necessary.
  LogicalResult load(EncodingReader &reader, MLIRContext *ctx) {
    if (dialect)
      return success();
    Dialect *loadedDialect = ctx->getOrLoadDialect(name);
    if (!loadedDialect && !ctx->allowsUnregisteredDialects()) {
      return reader.emitError(
          "dialect '", name,
          "' is unknown. If this is intended, please call "
          "allowUnregisteredDialects() on the MLIRContext, or use "
          "-allow-unregistered-dialect with the MLIR tool used.");
    }
    dialect = loadedDialect;
    return success();
  }

  /// The loaded dialect entry. This field is None if we haven't attempted to
  /// load, nullptr if we failed to load, otherwise the loaded dialect.
  Optional<Dialect *> dialect;

  /// The name of the dialect.
  StringRef name;
};

/// This struct represents an operation name entry within the bytecode.
struct BytecodeOperationName {
  BytecodeOperationName(BytecodeDialect *dialect, StringRef name)
      : dialect(dialect), name(name) {}

  /// The loaded operation name, or None if it hasn't been processed yet.
  Optional<OperationName> opName;

  /// The dialect that owns this operation name.
  BytecodeDialect *dialect;

  /// The name of the operation, without the dialect prefix.
  StringRef name;
};
} // namespace

/// Parse a single dialect group encoded in the byte stream.
static LogicalResult parseDialectGrouping(
    EncodingReader &reader, MutableArrayRef<BytecodeDialect> dialects,
    function_ref<LogicalResult(BytecodeDialect *)> entryCallback) {
  // Parse the dialect and the number of entries in the group.
  BytecodeDialect *dialect;
  if (failed(parseEntry(reader, dialects, dialect, "dialect")))
    return failure();
  uint64_t numEntries;
  if (failed(reader.parseVarInt(numEntries)))
    return failure();

  for (uint64_t i = 0; i < numEntries; ++i)
    if (failed(entryCallback(dialect)))
      return failure();
  return success();
}

//===----------------------------------------------------------------------===//
// Attribute/Type Reader
//===----------------------------------------------------------------------===//

namespace {
/// This class provides support for reading attribute and type entries from the
/// bytecode. Attribute and Type entries are read lazily on demand, so we use
/// this reader to manage when to actually parse them from the bytecode.
class AttrTypeReader {
  /// This class represents a single attribute or type entry.
  template <typename T>
  struct Entry {
    /// The entry, or null if it hasn't been resolved yet.
    T entry = {};
    /// The parent dialect of this entry.
    BytecodeDialect *dialect = nullptr;
    /// A flag indicating if the entry was encoded using a custom encoding,
    /// instead of using the textual assembly format.
    bool hasCustomEncoding = false;
    /// The raw data of this entry in the bytecode.
    ArrayRef<uint8_t> data;
  };
  using AttrEntry = Entry<Attribute>;
  using TypeEntry = Entry<Type>;

public:
  AttrTypeReader(Location fileLoc) : fileLoc(fileLoc) {}

  /// Initialize the attribute and type information within the reader.
  LogicalResult initialize(MutableArrayRef<BytecodeDialect> dialects,
                           ArrayRef<uint8_t> sectionData,
                           ArrayRef<uint8_t> offsetSectionData);

  /// Resolve the attribute or type at the given index. Returns nullptr on
  /// failure.
  Attribute resolveAttribute(size_t index) {
    return resolveEntry(attributes, index, "Attribute");
  }
  Type resolveType(size_t index) { return resolveEntry(types, index, "Type"); }

private:
  /// Resolve the given entry at `index`.
  template <typename T>
  T resolveEntry(SmallVectorImpl<Entry<T>> &entries, size_t index,
                 StringRef entryType);

  /// Parse the value defined within the given reader. `code` indicates how the
  /// entry was encoded.
  LogicalResult parseEntry(EncodingReader &reader, bool hasCustomEncoding,
                           Attribute &result);
  LogicalResult parseEntry(EncodingReader &reader, bool hasCustomEncoding,
                           Type &result);

  /// The set of attribute and type entries.
  SmallVector<AttrEntry> attributes;
  SmallVector<TypeEntry> types;

  /// A location used for error emission.
  Location fileLoc;
};
} // namespace

LogicalResult
AttrTypeReader::initialize(MutableArrayRef<BytecodeDialect> dialects,
                           ArrayRef<uint8_t> sectionData,
                           ArrayRef<uint8_t> offsetSectionData) {
  EncodingReader offsetReader(offsetSectionData, fileLoc);

  // Parse the number of attribute and type entries.
  uint64_t numAttributes, numTypes;
  if (failed(offsetReader.parseVarInt(numAttributes)) ||
      failed(offsetReader.parseVarInt(numTypes)))
    return failure();
  attributes.resize(numAttributes);
  types.resize(numTypes);

  // A functor used to accumulate the offsets for the entries in the given
  // range.
  uint64_t currentOffset = 0;
  auto parseEntries = [&](auto &&range) {
    size_t currentIndex = 0, endIndex = range.size();

    // Parse an individual entry.
    auto parseEntryFn = [&](BytecodeDialect *dialect) {
      auto &entry = range[currentIndex++];

      uint64_t entrySize;
      if (failed(offsetReader.parseVarIntWithFlag(entrySize,
                                                  entry.hasCustomEncoding)))
        return failure();

      // Verify that the offset is actually valid.
      if (currentOffset + entrySize > sectionData.size()) {
        return offsetReader.emitError(
            "Attribute or Type entry offset points past the end of section");
      }

      entry.data = sectionData.slice(currentOffset, entrySize);
      entry.dialect = dialect;
      currentOffset += entrySize;
      return success();
    };
    while (currentIndex != endIndex)
      if (failed(parseDialectGrouping(offsetReader, dialects, parseEntryFn)))
        return failure();
    return success();
  };

  // Process each of the attributes, and then the types.
  if (failed(parseEntries(attributes)) || failed(parseEntries(types)))
    return failure();

  // Ensure that we read everything from the section.
  if (!offsetReader.empty()) {
    return offsetReader.emitError(
        "unexpected trailing data in the Attribute/Type offset section");
  }
  return success();
}

template <typename T>
T AttrTypeReader::resolveEntry(SmallVectorImpl<Entry<T>> &entries, size_t index,
                               StringRef entryType) {
  if (index >= entries.size()) {
    emitError(fileLoc) << "invalid " << entryType << " index: " << index;
    return {};
  }

  // If the entry has already been resolved, there is nothing left to do.
  Entry<T> &entry = entries[index];
  if (entry.entry)
    return entry.entry;

  // Parse the entry.
  EncodingReader reader(entry.data, fileLoc);
  if (failed(parseEntry(reader, entry.hasCustomEncoding, entry.entry)))
    return T();
  if (!reader.empty()) {
    (void)reader.emitError("unexpected trailing bytes after " + entryType +
                           " entry");
    return T();
  }
  return entry.entry;
}

LogicalResult AttrTypeReader::parseEntry(EncodingReader &reader,
                                         bool hasCustomEncoding,
                                         Attribute &result) {
  // Handle the fallback case, where the attribute was encoded using its
  // assembly format.
  if (!hasCustomEncoding) {
    StringRef attrStr;
    if (failed(reader.parseNullTerminatedString(attrStr)))
      return failure();

    size_t numRead = 0;
    if (!(result = parseAttribute(attrStr, fileLoc->getContext(), numRead)))
      return failure();
    if (numRead != attrStr.size()) {
      return reader.emitError(
          "trailing characters found after Attribute assembly format: ",
          attrStr.drop_front(numRead));
    }
    return success();
  }

  return reader.emitError("unexpected Attribute encoding");
}

LogicalResult AttrTypeReader::parseEntry(EncodingReader &reader,
                                         bool hasCustomEncoding, Type &result) {
  // Handle the fallback case, where the type was encoded using its
  // assembly format.
  if (!hasCustomEncoding) {
    StringRef typeStr;
    if (failed(reader.parseNullTerminatedString(typeStr)))
      return failure();

    size_t numRead = 0;
    if (!(result = parseType(typeStr, fileLoc->getContext(), numRead)))
      return failure();
    if (numRead != typeStr.size()) {
      return reader.emitError(
          "trailing characters found after Type assembly format: " +
          typeStr.drop_front(numRead));
    }
    return success();
  }

  return reader.emitError("unexpected Type encoding");
}

//===----------------------------------------------------------------------===//
// Bytecode Reader
//===----------------------------------------------------------------------===//

namespace {
/// This class is used to read a bytecode buffer and translate it into MLIR.
class BytecodeReader {
public:
  BytecodeReader(Location fileLoc, const ParserConfig &config)
      : config(config), fileLoc(fileLoc), attrTypeReader(fileLoc),
        // Use the builtin unrealized conversion cast operation to represent
        // forward references to values that aren't yet defined.
        forwardRefOpState(UnknownLoc::get(config.getContext()),
                          "builtin.unrealized_conversion_cast", ValueRange(),
                          NoneType::get(config.getContext())) {}

  /// Read the bytecode defined within `buffer` into the given block.
  LogicalResult read(llvm::MemoryBufferRef buffer, Block *block);

private:
  /// Return the context for this config.
  MLIRContext *getContext() const { return config.getContext(); }

  /// Parse the bytecode version.
  LogicalResult parseVersion(EncodingReader &reader);

  //===--------------------------------------------------------------------===//
  // Dialect Section

  LogicalResult parseDialectSection(ArrayRef<uint8_t> sectionData);

  /// Parse an operation name reference using the given reader.
  FailureOr<OperationName> parseOpName(EncodingReader &reader);

  //===--------------------------------------------------------------------===//
  // Attribute/Type Section

  /// Parse an attribute or type using the given reader. Returns nullptr in the
  /// case of failure.
  Attribute parseAttribute(EncodingReader &reader);
  Type parseType(EncodingReader &reader);

  template <typename T>
  T parseAttribute(EncodingReader &reader) {
    if (Attribute attr = parseAttribute(reader)) {
      if (auto derivedAttr = attr.dyn_cast<T>())
        return derivedAttr;
      (void)reader.emitError("expected attribute of type: ",
                             llvm::getTypeName<T>(), ", but got: ", attr);
    }
    return T();
  }

  //===--------------------------------------------------------------------===//
  // IR Section

  /// This struct represents the current read state of a range of regions. This
  /// struct is used to enable iterative parsing of regions.
  struct RegionReadState {
    RegionReadState(Operation *op, bool isIsolatedFromAbove)
        : RegionReadState(op->getRegions(), isIsolatedFromAbove) {}
    RegionReadState(MutableArrayRef<Region> regions, bool isIsolatedFromAbove)
        : curRegion(regions.begin()), endRegion(regions.end()),
          isIsolatedFromAbove(isIsolatedFromAbove) {}

    /// The current regions being read.
    MutableArrayRef<Region>::iterator curRegion, endRegion;

    /// The number of values defined immediately within this region.
    unsigned numValues = 0;

    /// The current blocks of the region being read.
    SmallVector<Block *> curBlocks;
    Region::iterator curBlock = {};

    /// The number of operations remaining to be read from the current block
    /// being read.
    uint64_t numOpsRemaining = 0;

    /// A flag indicating if the regions being read are isolated from above.
    bool isIsolatedFromAbove = false;
  };

  LogicalResult parseIRSection(ArrayRef<uint8_t> sectionData, Block *block);
  LogicalResult parseRegions(EncodingReader &reader,
                             std::vector<RegionReadState> &regionStack,
                             RegionReadState &readState);
  FailureOr<Operation *> parseOpWithoutRegions(EncodingReader &reader,
                                               RegionReadState &readState,
                                               bool &isIsolatedFromAbove);

  LogicalResult parseRegion(EncodingReader &reader, RegionReadState &readState);
  LogicalResult parseBlock(EncodingReader &reader, RegionReadState &readState);
  LogicalResult parseBlockArguments(EncodingReader &reader, Block *block);

  //===--------------------------------------------------------------------===//
  // String Section

  LogicalResult parseStringSection(ArrayRef<uint8_t> sectionData);

  /// Parse a shared string from the string section. The shared string is
  /// encoded using an index to a corresponding string in the string section.
  LogicalResult parseSharedString(EncodingReader &reader, StringRef &result) {
    return parseEntry(reader, strings, result, "string");
  }

  //===--------------------------------------------------------------------===//
  // Value Processing

  /// Parse an operand reference using the given reader. Returns nullptr in the
  /// case of failure.
  Value parseOperand(EncodingReader &reader);

  /// Sequentially define the given value range.
  LogicalResult defineValues(EncodingReader &reader, ValueRange values);

  /// Create a value to use for a forward reference.
  Value createForwardRef();

  //===--------------------------------------------------------------------===//
  // Fields

  /// This class represents a single value scope, in which a value scope is
  /// delimited by isolated from above regions.
  struct ValueScope {
    /// Push a new region state onto this scope, reserving enough values for
    /// those defined within the current region of the provided state.
    void push(RegionReadState &readState) {
      nextValueIDs.push_back(values.size());
      values.resize(values.size() + readState.numValues);
    }

    /// Pop the values defined for the current region within the provided region
    /// state.
    void pop(RegionReadState &readState) {
      values.resize(values.size() - readState.numValues);
      nextValueIDs.pop_back();
    }

    /// The set of values defined in this scope.
    std::vector<Value> values;

    /// The ID for the next defined value for each region current being
    /// processed in this scope.
    SmallVector<unsigned, 4> nextValueIDs;
  };

  /// The configuration of the parser.
  const ParserConfig &config;

  /// A location to use when emitting errors.
  Location fileLoc;

  /// The reader used to process attribute and types within the bytecode.
  AttrTypeReader attrTypeReader;

  /// The version of the bytecode being read.
  uint64_t version = 0;

  /// The producer of the bytecode being read.
  StringRef producer;

  /// The table of IR units referenced within the bytecode file.
  SmallVector<BytecodeDialect> dialects;
  SmallVector<BytecodeOperationName> opNames;

  /// The table of strings referenced within the bytecode file.
  SmallVector<StringRef> strings;

  /// The current set of available IR value scopes.
  std::vector<ValueScope> valueScopes;
  /// A block containing the set of operations defined to create forward
  /// references.
  Block forwardRefOps;
  /// A block containing previously created, and no longer used, forward
  /// reference operations.
  Block openForwardRefOps;
  /// An operation state used when instantiating forward references.
  OperationState forwardRefOpState;
};
} // namespace

LogicalResult BytecodeReader::read(llvm::MemoryBufferRef buffer, Block *block) {
  EncodingReader reader(buffer.getBuffer(), fileLoc);

  // Skip over the bytecode header, this should have already been checked.
  if (failed(reader.skipBytes(StringRef("ML\xefR").size())))
    return failure();
  // Parse the bytecode version and producer.
  if (failed(parseVersion(reader)) ||
      failed(reader.parseNullTerminatedString(producer)))
    return failure();

  // Add a diagnostic handler that attaches a note that includes the original
  // producer of the bytecode.
  ScopedDiagnosticHandler diagHandler(getContext(), [&](Diagnostic &diag) {
    diag.attachNote() << "in bytecode version " << version
                      << " produced by: " << producer;
    return failure();
  });

  // Parse the raw data for each of the top-level sections of the bytecode.
  Optional<ArrayRef<uint8_t>> sectionDatas[bytecode::Section::kNumSections];
  while (!reader.empty()) {
    // Read the next section from the bytecode.
    bytecode::Section::ID sectionID;
    ArrayRef<uint8_t> sectionData;
    if (failed(reader.parseSection(sectionID, sectionData)))
      return failure();

    // Check for duplicate sections, we only expect one instance of each.
    if (sectionDatas[sectionID]) {
      return reader.emitError("duplicate top-level section: ",
                              toString(sectionID));
    }
    sectionDatas[sectionID] = sectionData;
  }
  // Check that all of the sections were found.
  for (int i = 0; i < bytecode::Section::kNumSections; ++i) {
    if (!sectionDatas[i]) {
      return reader.emitError("missing data for top-level section: ",
                              toString(bytecode::Section::ID(i)));
    }
  }

  // Process the string section first.
  if (failed(parseStringSection(*sectionDatas[bytecode::Section::kString])))
    return failure();

  // Process the dialect section.
  if (failed(parseDialectSection(*sectionDatas[bytecode::Section::kDialect])))
    return failure();

  // Process the attribute and type section.
  if (failed(attrTypeReader.initialize(
          dialects, *sectionDatas[bytecode::Section::kAttrType],
          *sectionDatas[bytecode::Section::kAttrTypeOffset])))
    return failure();

  // Finally, process the IR section.
  return parseIRSection(*sectionDatas[bytecode::Section::kIR], block);
}

LogicalResult BytecodeReader::parseVersion(EncodingReader &reader) {
  if (failed(reader.parseVarInt(version)))
    return failure();

  // Validate the bytecode version.
  uint64_t currentVersion = bytecode::kVersion;
  if (version < currentVersion) {
    return reader.emitError("bytecode version ", version,
                            " is older than the current version of ",
                            currentVersion, ", and upgrade is not supported");
  }
  if (version > currentVersion) {
    return reader.emitError("bytecode version ", version,
                            " is newer than the current version ",
                            currentVersion);
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Dialect Section

LogicalResult
BytecodeReader::parseDialectSection(ArrayRef<uint8_t> sectionData) {
  EncodingReader sectionReader(sectionData, fileLoc);

  // Parse the number of dialects in the section.
  uint64_t numDialects;
  if (failed(sectionReader.parseVarInt(numDialects)))
    return failure();
  dialects.resize(numDialects);

  // Parse each of the dialects.
  for (uint64_t i = 0; i < numDialects; ++i)
    if (failed(parseSharedString(sectionReader, dialects[i].name)))
      return failure();

  // Parse the operation names, which are grouped by dialect.
  auto parseOpName = [&](BytecodeDialect *dialect) {
    StringRef opName;
    if (failed(parseSharedString(sectionReader, opName)))
      return failure();
    opNames.emplace_back(dialect, opName);
    return success();
  };
  while (!sectionReader.empty())
    if (failed(parseDialectGrouping(sectionReader, dialects, parseOpName)))
      return failure();
  return success();
}

FailureOr<OperationName> BytecodeReader::parseOpName(EncodingReader &reader) {
  BytecodeOperationName *opName = nullptr;
  if (failed(parseEntry(reader, opNames, opName, "operation name")))
    return failure();

  // Check to see if this operation name has already been resolved. If we
  // haven't, load the dialect and build the operation name.
  if (!opName->opName) {
    if (failed(opName->dialect->load(reader, getContext())))
      return failure();
    opName->opName.emplace((opName->dialect->name + "." + opName->name).str(),
                           getContext());
  }
  return *opName->opName;
}

//===----------------------------------------------------------------------===//
// Attribute/Type Section

Attribute BytecodeReader::parseAttribute(EncodingReader &reader) {
  uint64_t attrIdx;
  if (failed(reader.parseVarInt(attrIdx)))
    return Attribute();
  return attrTypeReader.resolveAttribute(attrIdx);
}

Type BytecodeReader::parseType(EncodingReader &reader) {
  uint64_t typeIdx;
  if (failed(reader.parseVarInt(typeIdx)))
    return Type();
  return attrTypeReader.resolveType(typeIdx);
}

//===----------------------------------------------------------------------===//
// IR Section

LogicalResult BytecodeReader::parseIRSection(ArrayRef<uint8_t> sectionData,
                                             Block *block) {
  EncodingReader reader(sectionData, fileLoc);

  // A stack of operation regions currently being read from the bytecode.
  std::vector<RegionReadState> regionStack;

  // Parse the top-level block using a temporary module operation.
  OwningOpRef<ModuleOp> moduleOp = ModuleOp::create(fileLoc);
  regionStack.emplace_back(*moduleOp, /*isIsolatedFromAbove=*/true);
  regionStack.back().curBlocks.push_back(moduleOp->getBody());
  regionStack.back().curBlock = regionStack.back().curRegion->begin();
  if (failed(parseBlock(reader, regionStack.back())))
    return failure();
  valueScopes.emplace_back(ValueScope());
  valueScopes.back().push(regionStack.back());

  // Iteratively parse regions until everything has been resolved.
  while (!regionStack.empty())
    if (failed(parseRegions(reader, regionStack, regionStack.back())))
      return failure();
  if (!forwardRefOps.empty()) {
    return reader.emitError(
        "not all forward unresolved forward operand references");
  }

  // Verify that the parsed operations are valid.
  if (failed(verify(*moduleOp)))
    return failure();

  // Splice the parsed operations over to the provided top-level block.
  auto &parsedOps = moduleOp->getBody()->getOperations();
  auto &destOps = block->getOperations();
  destOps.splice(destOps.empty() ? destOps.end() : std::prev(destOps.end()),
                 parsedOps, parsedOps.begin(), parsedOps.end());
  return success();
}

LogicalResult
BytecodeReader::parseRegions(EncodingReader &reader,
                             std::vector<RegionReadState> &regionStack,
                             RegionReadState &readState) {
  // Read the regions of this operation.
  for (; readState.curRegion != readState.endRegion; ++readState.curRegion) {
    // If the current block hasn't been setup yet, parse the header for this
    // region.
    if (readState.curBlock == Region::iterator()) {
      if (failed(parseRegion(reader, readState)))
        return failure();

      // If the region is empty, there is nothing to more to do.
      if (readState.curRegion->empty())
        continue;
    }

    // Parse the blocks within the region.
    do {
      while (readState.numOpsRemaining--) {
        // Read in the next operation. We don't read its regions directly, we
        // handle those afterwards as necessary.
        bool isIsolatedFromAbove = false;
        FailureOr<Operation *> op =
            parseOpWithoutRegions(reader, readState, isIsolatedFromAbove);
        if (failed(op))
          return failure();

        // If the op has regions, add it to the stack for processing.
        if ((*op)->getNumRegions()) {
          regionStack.emplace_back(*op, isIsolatedFromAbove);

          // If the op is isolated from above, push a new value scope.
          if (isIsolatedFromAbove)
            valueScopes.emplace_back(ValueScope());
          return success();
        }
      }

      // Move to the next block of the region.
      if (++readState.curBlock == readState.curRegion->end())
        break;
      if (failed(parseBlock(reader, readState)))
        return failure();
    } while (true);

    // Reset the current block and any values reserved for this region.
    readState.curBlock = {};
    valueScopes.back().pop(readState);
  }

  // When the regions have been fully parsed, pop them off of the read stack. If
  // the regions were isolated from above, we also pop the last value scope.
  if (readState.isIsolatedFromAbove)
    valueScopes.pop_back();
  regionStack.pop_back();
  return success();
}

FailureOr<Operation *>
BytecodeReader::parseOpWithoutRegions(EncodingReader &reader,
                                      RegionReadState &readState,
                                      bool &isIsolatedFromAbove) {
  // Parse the name of the operation.
  FailureOr<OperationName> opName = parseOpName(reader);
  if (failed(opName))
    return failure();

  // Parse the operation mask, which indicates which components of the operation
  // are present.
  uint8_t opMask;
  if (failed(reader.parseByte(opMask)))
    return failure();

  /// Parse the location.
  LocationAttr opLoc = parseAttribute<LocationAttr>(reader);
  if (!opLoc)
    return failure();

  // With the location and name resolved, we can start building the operation
  // state.
  OperationState opState(opLoc, *opName);

  // Parse the attributes of the operation.
  if (opMask & bytecode::OpEncodingMask::kHasAttrs) {
    DictionaryAttr dictAttr = parseAttribute<DictionaryAttr>(reader);
    if (!dictAttr)
      return failure();
    opState.attributes = dictAttr;
  }

  /// Parse the results of the operation.
  if (opMask & bytecode::OpEncodingMask::kHasResults) {
    uint64_t numResults;
    if (failed(reader.parseVarInt(numResults)))
      return failure();
    opState.types.resize(numResults);
    for (int i = 0, e = numResults; i < e; ++i)
      if (!(opState.types[i] = parseType(reader)))
        return failure();
  }

  /// Parse the operands of the operation.
  if (opMask & bytecode::OpEncodingMask::kHasOperands) {
    uint64_t numOperands;
    if (failed(reader.parseVarInt(numOperands)))
      return failure();
    opState.operands.resize(numOperands);
    for (int i = 0, e = numOperands; i < e; ++i)
      if (!(opState.operands[i] = parseOperand(reader)))
        return failure();
  }

  /// Parse the successors of the operation.
  if (opMask & bytecode::OpEncodingMask::kHasSuccessors) {
    uint64_t numSuccs;
    if (failed(reader.parseVarInt(numSuccs)))
      return failure();
    opState.successors.resize(numSuccs);
    for (int i = 0, e = numSuccs; i < e; ++i) {
      if (failed(parseEntry(reader, readState.curBlocks, opState.successors[i],
                            "successor")))
        return failure();
    }
  }

  /// Parse the regions of the operation.
  if (opMask & bytecode::OpEncodingMask::kHasInlineRegions) {
    uint64_t numRegions;
    if (failed(reader.parseVarIntWithFlag(numRegions, isIsolatedFromAbove)))
      return failure();

    opState.regions.reserve(numRegions);
    for (int i = 0, e = numRegions; i < e; ++i)
      opState.regions.push_back(std::make_unique<Region>());
  }

  // Create the operation at the back of the current block.
  Operation *op = Operation::create(opState);
  readState.curBlock->push_back(op);

  // If the operation had results, update the value references.
  if (op->getNumResults() && failed(defineValues(reader, op->getResults())))
    return failure();

  return op;
}

LogicalResult BytecodeReader::parseRegion(EncodingReader &reader,
                                          RegionReadState &readState) {
  // Parse the number of blocks in the region.
  uint64_t numBlocks;
  if (failed(reader.parseVarInt(numBlocks)))
    return failure();

  // If the region is empty, there is nothing else to do.
  if (numBlocks == 0)
    return success();

  // Parse the number of values defined in this region.
  uint64_t numValues;
  if (failed(reader.parseVarInt(numValues)))
    return failure();
  readState.numValues = numValues;

  // Create the blocks within this region. We do this before processing so that
  // we can rely on the blocks existing when creating operations.
  readState.curBlocks.clear();
  readState.curBlocks.reserve(numBlocks);
  for (uint64_t i = 0; i < numBlocks; ++i) {
    readState.curBlocks.push_back(new Block());
    readState.curRegion->push_back(readState.curBlocks.back());
  }

  // Prepare the current value scope for this region.
  valueScopes.back().push(readState);

  // Parse the entry block of the region.
  readState.curBlock = readState.curRegion->begin();
  return parseBlock(reader, readState);
}

LogicalResult BytecodeReader::parseBlock(EncodingReader &reader,
                                         RegionReadState &readState) {
  bool hasArgs;
  if (failed(reader.parseVarIntWithFlag(readState.numOpsRemaining, hasArgs)))
    return failure();

  // Parse the arguments of the block.
  if (hasArgs && failed(parseBlockArguments(reader, &*readState.curBlock)))
    return failure();

  // We don't parse the operations of the block here, that's done elsewhere.
  return success();
}

LogicalResult BytecodeReader::parseBlockArguments(EncodingReader &reader,
                                                  Block *block) {
  // Parse the value ID for the first argument, and the number of arguments.
  uint64_t numArgs;
  if (failed(reader.parseVarInt(numArgs)))
    return failure();

  SmallVector<Type> argTypes;
  SmallVector<Location> argLocs;
  argTypes.reserve(numArgs);
  argLocs.reserve(numArgs);

  while (numArgs--) {
    Type argType = parseType(reader);
    if (!argType)
      return failure();
    LocationAttr argLoc = parseAttribute<LocationAttr>(reader);
    if (!argLoc)
      return failure();

    argTypes.push_back(argType);
    argLocs.push_back(argLoc);
  }
  block->addArguments(argTypes, argLocs);
  return defineValues(reader, block->getArguments());
}

//===----------------------------------------------------------------------===//
// String Section

LogicalResult
BytecodeReader::parseStringSection(ArrayRef<uint8_t> sectionData) {
  EncodingReader stringReader(sectionData, fileLoc);

  // Parse the number of strings in the section.
  uint64_t numStrings;
  if (failed(stringReader.parseVarInt(numStrings)))
    return failure();
  strings.resize(numStrings);

  // Parse each of the strings. The sizes of the strings are encoded in reverse
  // order, so that's the order we populate the table.
  size_t stringDataEndOffset = sectionData.size();
  size_t totalStringDataSize = 0;
  for (StringRef &string : llvm::reverse(strings)) {
    uint64_t stringSize;
    if (failed(stringReader.parseVarInt(stringSize)))
      return failure();
    if (stringDataEndOffset < stringSize) {
      return stringReader.emitError(
          "string size exceeds the available data size");
    }

    // Extract the string from the data, dropping the null character.
    size_t stringOffset = stringDataEndOffset - stringSize;
    string = StringRef(
        reinterpret_cast<const char *>(sectionData.data() + stringOffset),
        stringSize - 1);
    stringDataEndOffset = stringOffset;

    // Update the total string data size.
    totalStringDataSize += stringSize;
  }

  // Check that the only remaining data was for the strings
  if (stringReader.size() != totalStringDataSize) {
    return stringReader.emitError("unexpected trailing data between the "
                                  "offsets for strings and their data");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Value Processing

Value BytecodeReader::parseOperand(EncodingReader &reader) {
  std::vector<Value> &values = valueScopes.back().values;
  Value *value = nullptr;
  if (failed(parseEntry(reader, values, value, "value")))
    return Value();

  // Create a new forward reference if necessary.
  if (!*value)
    *value = createForwardRef();
  return *value;
}

LogicalResult BytecodeReader::defineValues(EncodingReader &reader,
                                           ValueRange newValues) {
  ValueScope &valueScope = valueScopes.back();
  std::vector<Value> &values = valueScope.values;

  unsigned &valueID = valueScope.nextValueIDs.back();
  unsigned valueIDEnd = valueID + newValues.size();
  if (valueIDEnd > values.size()) {
    return reader.emitError(
        "value index range was outside of the expected range for "
        "the parent region, got [",
        valueID, ", ", valueIDEnd, "), but the maximum index was ",
        values.size() - 1);
  }

  // Assign the values and update any forward references.
  for (unsigned i = 0, e = newValues.size(); i != e; ++i, ++valueID) {
    Value newValue = newValues[i];

    // Check to see if a definition for this value already exists.
    if (Value oldValue = std::exchange(values[valueID], newValue)) {
      Operation *forwardRefOp = oldValue.getDefiningOp();

      // Assert that this is a forward reference operation. Given how we compute
      // definition ids (incrementally as we parse), it shouldn't be possible
      // for the value to be defined any other way.
      assert(forwardRefOp && forwardRefOp->getBlock() == &forwardRefOps &&
             "value index was already defined?");

      oldValue.replaceAllUsesWith(newValue);
      forwardRefOp->moveBefore(&openForwardRefOps, openForwardRefOps.end());
    }
  }
  return success();
}

Value BytecodeReader::createForwardRef() {
  // Check for an avaliable existing operation to use. Otherwise, create a new
  // fake operation to use for the reference.
  if (!openForwardRefOps.empty()) {
    Operation *op = &openForwardRefOps.back();
    op->moveBefore(&forwardRefOps, forwardRefOps.end());
  } else {
    forwardRefOps.push_back(Operation::create(forwardRefOpState));
  }
  return forwardRefOps.back().getResult(0);
}

//===----------------------------------------------------------------------===//
// Entry Points
//===----------------------------------------------------------------------===//

bool mlir::isBytecode(llvm::MemoryBufferRef buffer) {
  return buffer.getBuffer().startswith("ML\xefR");
}

LogicalResult mlir::readBytecodeFile(llvm::MemoryBufferRef buffer, Block *block,
                                     const ParserConfig &config) {
  Location sourceFileLoc =
      FileLineColLoc::get(config.getContext(), buffer.getBufferIdentifier(),
                          /*line=*/0, /*column=*/0);
  if (!isBytecode(buffer)) {
    return emitError(sourceFileLoc,
                     "input buffer is not an MLIR bytecode file");
  }

  BytecodeReader reader(sourceFileLoc, config);
  return reader.read(buffer, block);
}
