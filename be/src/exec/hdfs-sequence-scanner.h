// Copyright (c) 2012 Cloudera, Inc. All rights reserved.

#ifndef IMPALA_EXEC_HDFS_SEQUENCE_SCANNER_H
#define IMPALA_EXEC_HDFS_SEQUENCE_SCANNER_H

#include "exec/hdfs-scanner.h"
#include "exec/buffered-byte-stream.h"
#include "exec/delimited-text-parser.h"

namespace impala {

// This scanner parses Sequence file located in HDFS, and writes the
// content as tuples in the Impala in-memory representation of data, e.g.
// (tuples, rows, row batches).
// org.apache.hadoop.io.SequenceFile is the original SequenceFile implementation
// and should be viewed as the canonical definition of this format. If
// anything is unclear in this file you should consult the code in
// org.apache.hadoop.io.SequenceFile.
//
// The following is a pseudo-BNF grammar for SequenceFile. Comments are prefixed
// with dashes:
//
// seqfile ::=
//   <file-header>
//   <record-block>+
//
// record-block ::=
//   <record>+
//   <file-sync-hash>
//
// file-header ::=
//   <file-version-header>
//   <file-key-class-name>
//   <file-value-class-name>
//   <file-is-compressed>
//   <file-is-block-compressed>
//   [<file-compression-codec-class>]
//   <file-header-metadata>
//   <file-sync-field>
//
// file-version-header ::= Byte[4] {'S', 'E', 'Q', 6}
//
// -- The name of the Java class responsible for reading the key buffer
//
// file-key-class-name ::=
//   Text {"org.apache.hadoop.io.BytesWritable"}
//
// -- The name of the Java class responsible for reading the value buffer
//
// file-value-class-name ::=
//   Text {"org.apache.hadoop.io.Text"}
//
// -- Boolean variable indicating whether or not the file uses compression
// -- for key/values in this file
//
// file-is-compressed ::= Byte[1]
//
// -- A boolean field indicating whether or not the file is block compressed.
//
// file-is-block-compressed ::= Byte[1] {false}
//
// -- The Java class name of the compression codec iff <file-is-compressed>
// -- is true. The named class must implement
// -- org.apache.hadoop.io.compress.CompressionCodec.
// -- The expected value is org.apache.hadoop.io.compress.GzipCodec.
//
// file-compression-codec-class ::= Text
//
// -- A collection of key-value pairs defining metadata values for the
// -- file. The Map is serialized using standard JDK serialization, i.e.
// -- an Int corresponding to the number of key-value pairs, followed by
// -- Text key and value pairs.
//
// file-header-metadata ::= Map<Text, Text>
//
// -- A 16 byte marker that is generated by the writer. This marker appears
// -- at regular intervals at the beginning of records or record blocks
// -- intended to enable readers to skip to a random part of the file
// -- the sync hash is preceeded by a length of -1, refered to as the sync marker
//
// file-sync-hash ::= Byte[16]
//
// -- Records are all of one type as determined by the compression bits in the header
//
// record ::=
//   <uncompressed-record>     |
//   <block-compressed-record> |
//   <record-compressed-record>
//
// uncompressed-record ::=
//   <record-length>
//   <key-length>
//   <key>
//   <value>
//
// record-compressed-record ::=
//   <record-length>
//   <key-length>
//   <key>
//   <compressed-value>
//
// block-compessed-record ::=
//   <file-sync-field>
//   <key-lengths-block-size>
//   <key-lengths-block>
//   <keys-block-size>
//   <keys-block>
//   <value-lengths-block-size>
//   <value-lengths-block>
//   <values-block-size>
//   <values-block>
//
// record-length := Int
// key-length := Int
// keys-lengths-block-size> := Int
// value-lengths-block-size> := Int
//
// keys-block :: = Byte[keys-block-size]
// values-block :: = Byte[values-block-size]
//
// -- The key-lengths and value-lengths blocks are are a sequence of lengths encoded
// -- in ZeroCompressedInteger (VInt) format.
//
// key-lengths-block :: = Byte[key-lengths-block-size]
// value-lengths-block :: = Byte[value-lengths-block-size]
//
// Byte ::= An eight-bit byte
//
// VInt ::= Variable length integer. The high-order bit of each byte
// indicates whether more bytes remain to be read. The low-order seven
// bits are appended as increasingly more significant bits in the
// resulting integer value.
//
// Int ::= A four-byte integer in big-endian format.
//
// Text ::= VInt, Chars (Length prefixed UTF-8 characters)

class HdfsSequenceScanner : public HdfsScanner {
 public:
  HdfsSequenceScanner(HdfsScanNode* scan_node, const TupleDescriptor* tuple_desc,
                      Tuple* template_tuple, MemPool* tuple_pool);

  virtual ~HdfsSequenceScanner();
  virtual Status Prepare(RuntimeState* state, ByteStream* byte_stream);
  virtual Status GetNext(RuntimeState* state, RowBatch* row_batch, bool* eosr);

 private:
  // Sync indicator
  const static int SYNC_MARKER = -1;

  // Size of the sync hash field
  const static int SYNC_HASH_SIZE = 16;

  // The key class name located in the SeqFile Header.
  // This is always "org.apache.hadoop.io.BytesWritable"
  static const char* const SEQFILE_KEY_CLASS_NAME;

  // The value class name located in the SeqFile Header.
  // This is always "org.apache.hadoop.io.Text"
  static const char* const SEQFILE_VALUE_CLASS_NAME;

  // The four byte SeqFile version header present at the beginning of every
  // SeqFile file: {'S', 'E', 'Q', 6}
  static const uint8_t SEQFILE_VERSION_HEADER[4];

  // The key should always be 4 bytes.
  static const int SEQFILE_KEY_LENGTH;

  // Initialises any state required at the beginning of a new scan range.
  // If not at the begining of the file it will trigger a search for the
  // next sync block, where the scan will start.
  virtual Status InitCurrentScanRange(RuntimeState* state,
                                      HdfsScanRange* scan_range, ByteStream* byte_stream);

  // Writes the intermediate parsed data in to slots, outputting
  // tuples to row_batch as they complete.
  // Input Parameters:
  //  state: Runtime state into which we log errors
  //  row_batch: Row batch into which to write new tuples
  //  num_fields: Total number of fields contained in parsed_data_
  // Input/Output Parameters
  //  row_idx: Index of current row in row_batch.
  Status WriteFields(RuntimeState* state, RowBatch*
                     row_batch, int num_fields, int* row_idx);

  // Find the first record of a scan range.
  // If the scan range is not at the beginning of the file then this is called to
  // move the buffered_byte_stream_ seek point to before the next sync field.
  // If there is none present then the buffered_byte_stream_ will be beyond the
  // end of the scan range and the scan will end.
  Status FindFirstRecord(RuntimeState *state);

  // Read the current Sequence file header from the begining of the file.
  // Verifies:
  //   version number
  //   key and data classes
  // Sets:
  //   is_compressed_
  //   is_blk_compressed_
  //   compression_codec_
  //   sync_
  Status ReadFileHeader();

  // Read the Sequence file Header Metadata section in the current file.
  // We don't use this information, so it is just skipped.
  Status ReadFileHeaderMetadata();

  // Read and validate a RowGroup sync field.
  Status ReadSync();

  // Read the record header, return if there was a sync block.
  // Sets:
  //   current_block_length_
  Status ReadBlockHeader(bool* sync);

  // Find first record in a scan range.
  // Sets the current_byte_stream_ to this record.
  Status FindFirstRecord();

  // Read compressed blocks and iterate through the records in each block.
  // Output:
  //   record_ptr: ponter to the record.
  //   record_len: length of the record
  //   eors: set to true if we are at the end of the scan range.
  Status GetRecordFromCompressedBlock(RuntimeState *state,
                                      uint8_t** record_ptr, int64_t *record_len, bool *eors);

  // Read compressed or uncompressed records from the byte stream into memory
  // in unparsed_data_buffer_pool_.
  // Output:
  //   record_ptr: ponter to the record.
  //   record_len: length of the record
  //   eors: set to true if we are at the end of the scan range.
  Status GetRecord(uint8_t** record_ptr, int64_t *record_len, bool *eosr);

  // Read a compressed block.
  // Decompress to unparsed_data_buffer_ allocated from unparsed_data_buffer_pool_.
  Status ReadCompressedBlock(RuntimeState *state);

  // read and verify a sync block.
  Status CheckSync();

  // a buffered byte stream to wrap the stream we are passed.
  boost::scoped_ptr<BufferedByteStream> buffered_byte_stream_;

  // Helper class for picking fields and rows from delimited text.
  boost::scoped_ptr<DelimitedTextParser> delimited_text_parser_;
  std::vector<DelimitedTextParser::FieldLocation> field_locations_;

  // Parser to find the first record. This uses different delimiters.
  boost::scoped_ptr<DelimitedTextParser> find_first_parser_;

  // Helper class for converting text fields to internal types.
  boost::scoped_ptr<TextConverter> text_converter_;

  // Runtime state for reporting file parsing errors.
  RuntimeState* runtime_state_;

  // The original byte stream we are passed.
  ByteStream* unbuffered_byte_stream_;

  // The sync hash read in from the file header.
  uint8_t sync_[SYNC_HASH_SIZE];

  // File compression or not.
  bool is_compressed_;
  // Block compression or not.
  bool is_blk_compressed_;

  // The decompressor class to use.
  boost::scoped_ptr<Decompressor> decompressor_;

  // Location (file name) of previous scan range.
  std::string previous_location_;

  // Byte offset of the scan range.
  int end_of_scan_range_;

  // Length of the current sequence file block (or record).
  int current_block_length_;

  // Length of the current key.  This should always be SEQFILE_KEY_LENGTH.
  int current_key_length_;

  // Pool for allocating the unparsed_data_buffer_.
  boost::scoped_ptr<MemPool> unparsed_data_buffer_pool_;

  // Buffer for data read from HDFS or from decompressing the HDFS data.
  uint8_t* unparsed_data_buffer_;

  // Size of the unparsed_data_buffer_.
  int64_t unparsed_data_buffer_size_;

  // Number of buffered records unparsed_data_buffer_ from block compressed data.
  int64_t num_buffered_records_in_compressed_block_;

  // Next record from block compressed data.
  uint8_t* next_record_in_compressed_block_;

  // Temporary buffer used for reading headers and compressed data.
  // It will grow to be big enough for the largest compressed record or block.
  std::vector<uint8_t> scratch_buf_;
};

} // namespace impala

#endif // IMPALA_EXEC_HDFS_SEQUENCE_SCANNER_H
