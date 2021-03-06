// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/log_writer.h"

#include <cstdint>

#include "leveldb/env.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {
namespace log {

static void InitTypeCrc(uint32_t* type_crc) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    char t = static_cast<char>(i);
    type_crc[i] = crc32c::Value(&t, 1);
  }
}

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {
  InitTypeCrc(type_crc_);
}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(dest_length % kBlockSize) {
  InitTypeCrc(type_crc_);
}

Writer::~Writer() = default;

// 若一条记录较大，则可能会分成几个chunk存储在若干个block中
// 日志的数据部分(也就是本函数的参数 slice ) 是 WriteBatchInternal::Contents(write_batch)
Status Writer::AddRecord(const Slice& slice) {
  const char* ptr = slice.data();
  size_t left = slice.size();  // 数据部分的字节数

  // Fragment the record if necessary and emit it.  Note that if slice
  // is empty, we still want to iterate once to emit a single
  // zero-length record
  Status s;
  bool begin = true;
  // 一个Block中可以有多个Record
  do {
    // 当前block的剩余空间
    const int leftover = kBlockSize - block_offset_;
    assert(leftover >= 0);
    // Block 剩余空间 少于 head size，开辟一个新的 block
    if (leftover < kHeaderSize) {
      // Switch to a new block
      if (leftover > 0) {
        // Fill the trailer (literal below relies on kHeaderSize being 7)
        static_assert(kHeaderSize == 7, "");
        // 最多放6个, 最后要放 leftover 个 \x00
        dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", leftover));  // padding 对齐
      }
      block_offset_ = 0;
    }

    // Invariant: we never leave < kHeaderSize bytes in a block.
    assert(kBlockSize - block_offset_ - kHeaderSize >= 0);

    // 当前Block剩余的空间
    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    // 本次能写入的数据
    const size_t fragment_length = (left < avail) ? left : avail;

    RecordType type;
    // 数据能否在 本Block 中放完
    const bool end = (left == fragment_length);
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }
    // 向文件中写入一个Record
    s = EmitPhysicalRecord(type, ptr, fragment_length);
    // 计算还有多少数据
    ptr += fragment_length;  // 移动数据指针
    left -= fragment_length;  // 更新剩余数据大小
    begin = false;
  } while (s.ok() && left > 0);
  return s;
}
// 将 一个 Chunk(Record根据Block进行切分的) 写入文件（落盘）
// 改名 EmitPhysicalChunk 更合适
// 格式在 log_format.h 中定义
Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr,
                                  size_t length) {
  assert(length <= 0xffff);  // Must fit in two bytes
  assert(block_offset_ + kHeaderSize + length <= kBlockSize);

  // Format the header
  char buf[kHeaderSize];
  // 0 1 2 3 | 4 5 | 6
  // length
  buf[4] = static_cast<char>(length & 0xff);
  buf[5] = static_cast<char>(length >> 8);
  // type
  buf[6] = static_cast<char>(t);

  // Compute the crc of the record type and the payload.
  uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
  crc = crc32c::Mask(crc);  // Adjust for storage
  // 设置 32位（4byte）数据
  EncodeFixed32(buf, crc);

  // Write the header and the payload
  Status s = dest_->Append(Slice(buf, kHeaderSize));  // 写入 record Header
  if (s.ok()) {
    s = dest_->Append(Slice(ptr, length));  // 写入 record Data
    if (s.ok()) {
      s = dest_->Flush();
    }
  }
  block_offset_ += kHeaderSize + length;
  return s;
}

}  // namespace log
}  // namespace leveldb
