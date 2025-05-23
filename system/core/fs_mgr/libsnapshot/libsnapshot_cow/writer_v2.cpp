//
// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "writer_v2.h"

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <future>
#include <limits>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <brotli/encode.h>
#include <libsnapshot/cow_format.h>
#include <libsnapshot/cow_reader.h>
#include <libsnapshot/cow_writer.h>
#include <lz4.h>
#include <zlib.h>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "android-base/parseint.h"
#include "android-base/strings.h"
#include "parser_v2.h"

// The info messages here are spammy, but as useful for update_engine. Disable
// them when running on the host.
#ifdef __ANDROID__
#define LOG_INFO LOG(INFO)
#else
#define LOG_INFO LOG(VERBOSE)
#endif

namespace android {
namespace snapshot {

static_assert(sizeof(off_t) == sizeof(uint64_t));

using android::base::unique_fd;

CowWriterV2::CowWriterV2(const CowOptions& options, unique_fd&& fd)
    : CowWriterBase(options, std::move(fd)) {
    SetupHeaders();
    SetupWriteOptions();
}

CowWriterV2::~CowWriterV2() {
    for (size_t i = 0; i < compress_threads_.size(); i++) {
        CompressWorker* worker = compress_threads_[i].get();
        if (worker) {
            worker->Finalize();
        }
    }

    bool ret = true;
    for (auto& t : threads_) {
        ret = t.get() && ret;
    }

    if (!ret) {
        LOG(ERROR) << "Compression failed";
    }
    compress_threads_.clear();
}

void CowWriterV2::SetupWriteOptions() {
    num_compress_threads_ = options_.num_compress_threads;

    if (!num_compress_threads_) {
        num_compress_threads_ = 1;
        // We prefer not to have more than two threads as the overhead of additional
        // threads is far greater than cutting down compression time.
        if (header_.cluster_ops &&
            android::base::GetBoolProperty("ro.virtual_ab.compression.threads", false)) {
            num_compress_threads_ = 2;
        }
    }

    if (header_.cluster_ops &&
        (android::base::GetBoolProperty("ro.virtual_ab.batch_writes", false) ||
         options_.batch_write)) {
        batch_write_ = true;
    }
}

void CowWriterV2::SetupHeaders() {
    header_ = {};
    header_.prefix.magic = kCowMagicNumber;
    header_.prefix.major_version = kCowVersionMajor;
    header_.prefix.minor_version = kCowVersionMinor;
    header_.prefix.header_size = sizeof(CowHeader);
    header_.footer_size = sizeof(CowFooter);
    header_.op_size = sizeof(CowOperation);
    header_.block_size = options_.block_size;
    header_.num_merge_ops = options_.num_merge_ops;
    header_.cluster_ops = options_.cluster_ops;
    header_.buffer_size = 0;
    footer_ = {};
    footer_.op.data_length = 64;
    footer_.op.type = kCowFooterOp;
}

bool CowWriterV2::ParseOptions() {
    auto parts = android::base::Split(options_.compression, ",");

    if (parts.size() > 2) {
        LOG(ERROR) << "failed to parse compression parameters: invalid argument count: "
                   << parts.size() << " " << options_.compression;
        return false;
    }
    auto algorithm = CompressionAlgorithmFromString(parts[0]);
    if (!algorithm) {
        LOG(ERROR) << "unrecognized compression: " << options_.compression;
        return false;
    }
    if (parts.size() > 1) {
        if (!android::base::ParseUint(parts[1], &compression_.compression_level)) {
            LOG(ERROR) << "failed to parse compression level invalid type: " << parts[1];
            return false;
        }
    } else {
        compression_.compression_level =
                CompressWorker::GetDefaultCompressionLevel(algorithm.value());
    }

    compression_.algorithm = *algorithm;

    if (options_.cluster_ops == 1) {
        LOG(ERROR) << "Clusters must contain at least two operations to function.";
        return false;
    }
    return true;
}

void CowWriterV2::InitBatchWrites() {
    if (batch_write_) {
        cowop_vec_ = std::make_unique<struct iovec[]>(header_.cluster_ops);
        data_vec_ = std::make_unique<struct iovec[]>(header_.cluster_ops);
        struct iovec* cowop_ptr = cowop_vec_.get();
        struct iovec* data_ptr = data_vec_.get();
        for (size_t i = 0; i < header_.cluster_ops; i++) {
            std::unique_ptr<CowOperation> op = std::make_unique<CowOperation>();
            cowop_ptr[i].iov_base = op.get();
            cowop_ptr[i].iov_len = sizeof(CowOperation);
            opbuffer_vec_.push_back(std::move(op));

            std::unique_ptr<uint8_t[]> buffer = std::make_unique<uint8_t[]>(header_.block_size * 2);
            data_ptr[i].iov_base = buffer.get();
            data_ptr[i].iov_len = header_.block_size * 2;
            databuffer_vec_.push_back(std::move(buffer));
        }

        current_op_pos_ = next_op_pos_;
        current_data_pos_ = next_data_pos_;
    }

    LOG_INFO << "Batch writes: " << (batch_write_ ? "enabled" : "disabled");
}

void CowWriterV2::InitWorkers() {
    if (num_compress_threads_ <= 1) {
        LOG_INFO << "Not creating new threads for compression.";
        return;
    }
    for (int i = 0; i < num_compress_threads_; i++) {
        std::unique_ptr<ICompressor> compressor =
                ICompressor::Create(compression_, header_.block_size);
        auto wt = std::make_unique<CompressWorker>(std::move(compressor), header_.block_size);
        threads_.emplace_back(std::async(std::launch::async, &CompressWorker::RunThread, wt.get()));
        compress_threads_.push_back(std::move(wt));
    }

    LOG_INFO << num_compress_threads_ << " thread used for compression";
}

bool CowWriterV2::Initialize(std::optional<uint64_t> label) {
    if (!InitFd() || !ParseOptions()) {
        return false;
    }
    if (!label) {
        if (!OpenForWrite()) {
            return false;
        }
    } else {
        if (!OpenForAppend(*label)) {
            return false;
        }
    }

    if (!compress_threads_.size()) {
        InitWorkers();
    }
    return true;
}

void CowWriterV2::InitPos() {
    next_op_pos_ = sizeof(header_) + header_.buffer_size;
    cluster_size_ = header_.cluster_ops * sizeof(CowOperation);
    if (header_.cluster_ops) {
        next_data_pos_ = next_op_pos_ + cluster_size_;
    } else {
        next_data_pos_ = next_op_pos_ + sizeof(CowOperation);
    }
    current_cluster_size_ = 0;
    current_data_size_ = 0;
}

bool CowWriterV2::OpenForWrite() {
    // This limitation is tied to the data field size in CowOperation.
    if (header_.block_size > std::numeric_limits<uint16_t>::max()) {
        LOG(ERROR) << "Block size is too large";
        return false;
    }

    if (lseek(fd_.get(), 0, SEEK_SET) < 0) {
        PLOG(ERROR) << "lseek failed";
        return false;
    }

    if (options_.scratch_space) {
        header_.buffer_size = BUFFER_REGION_DEFAULT_SIZE;
    }

    // Headers are not complete, but this ensures the file is at the right
    // position.
    if (!android::base::WriteFully(fd_, &header_, sizeof(header_))) {
        PLOG(ERROR) << "write failed";
        return false;
    }

    if (options_.scratch_space) {
        // Initialize the scratch space
        std::string data(header_.buffer_size, 0);
        if (!android::base::WriteFully(fd_, data.data(), header_.buffer_size)) {
            PLOG(ERROR) << "writing scratch space failed";
            return false;
        }
    }

    if (!Sync()) {
        LOG(ERROR) << "Header sync failed";
        return false;
    }

    if (lseek(fd_.get(), sizeof(header_) + header_.buffer_size, SEEK_SET) < 0) {
        PLOG(ERROR) << "lseek failed";
        return false;
    }

    InitPos();
    InitBatchWrites();

    return true;
}

bool CowWriterV2::OpenForAppend(uint64_t label) {
    if (!ReadCowHeader(fd_, &header_)) {
        return false;
    }

    CowParserV2 parser;
    if (!parser.Parse(fd_, header_, {label})) {
        return false;
    }
    if (header_.prefix.major_version > 2) {
        LOG(ERROR) << "CowWriterV2 tried to open incompatible version "
                   << header_.prefix.major_version;
        return false;
    }

    options_.block_size = header_.block_size;
    options_.cluster_ops = header_.cluster_ops;

    // Reset this, since we're going to reimport all operations.
    footer_.op.num_ops = 0;
    InitPos();

    for (const auto& op : *parser.ops()) {
        AddOperation(op);
    }

    if (lseek(fd_.get(), next_op_pos_, SEEK_SET) < 0) {
        PLOG(ERROR) << "lseek failed";
        return false;
    }

    InitBatchWrites();

    return EmitClusterIfNeeded();
}

bool CowWriterV2::EmitCopy(uint64_t new_block, uint64_t old_block, uint64_t num_blocks) {
    CHECK(!merge_in_progress_);

    for (size_t i = 0; i < num_blocks; i++) {
        CowOperation op = {};
        op.type = kCowCopyOp;
        op.new_block = new_block + i;
        op.source = old_block + i;
        if (!WriteOperation(op)) {
            return false;
        }
    }

    return true;
}

bool CowWriterV2::EmitRawBlocks(uint64_t new_block_start, const void* data, size_t size) {
    return EmitBlocks(new_block_start, data, size, 0, 0, kCowReplaceOp);
}

bool CowWriterV2::EmitXorBlocks(uint32_t new_block_start, const void* data, size_t size,
                                uint32_t old_block, uint16_t offset) {
    return EmitBlocks(new_block_start, data, size, old_block, offset, kCowXorOp);
}

bool CowWriterV2::CompressBlocks(size_t num_blocks, const void* data) {
    size_t num_threads = (num_blocks == 1) ? 1 : num_compress_threads_;
    size_t num_blocks_per_thread = num_blocks / num_threads;
    const uint8_t* iter = reinterpret_cast<const uint8_t*>(data);
    compressed_buf_.clear();
    if (num_threads <= 1) {
        if (!compressor_) {
            compressor_ = ICompressor::Create(compression_, header_.block_size);
        }
        return CompressWorker::CompressBlocks(compressor_.get(), options_.block_size, data,
                                              num_blocks, &compressed_buf_);
    }
    // Submit the blocks per thread. The retrieval of
    // compressed buffers has to be done in the same order.
    // We should not poll for completed buffers in a different order as the
    // buffers are tightly coupled with block ordering.
    for (size_t i = 0; i < num_threads; i++) {
        CompressWorker* worker = compress_threads_[i].get();
        if (i == num_threads - 1) {
            num_blocks_per_thread = num_blocks;
        }
        worker->EnqueueCompressBlocks(iter, num_blocks_per_thread);
        iter += (num_blocks_per_thread * header_.block_size);
        num_blocks -= num_blocks_per_thread;
    }

    for (size_t i = 0; i < num_threads; i++) {
        CompressWorker* worker = compress_threads_[i].get();
        if (!worker->GetCompressedBuffers(&compressed_buf_)) {
            return false;
        }
    }

    return true;
}

bool CowWriterV2::EmitBlocks(uint64_t new_block_start, const void* data, size_t size,
                             uint64_t old_block, uint16_t offset, uint8_t type) {
    CHECK(!merge_in_progress_);
    const uint8_t* iter = reinterpret_cast<const uint8_t*>(data);

    // Update engine can potentially send 100MB of blocks at a time. We
    // don't want to process all those blocks in one shot as it can
    // stress the memory. Hence, process the blocks in chunks.
    //
    // 1024 blocks is reasonable given we will end up using max
    // memory of ~4MB.
    const size_t kProcessingBlocks = 1024;
    size_t num_blocks = (size / header_.block_size);
    size_t i = 0;

    while (num_blocks) {
        size_t pending_blocks = (std::min(kProcessingBlocks, num_blocks));

        if (compression_.algorithm && num_compress_threads_ > 1) {
            if (!CompressBlocks(pending_blocks, iter)) {
                return false;
            }
            buf_iter_ = compressed_buf_.begin();
            CHECK(pending_blocks == compressed_buf_.size());
        }

        num_blocks -= pending_blocks;

        while (i < size / header_.block_size && pending_blocks) {
            CowOperation op = {};
            op.new_block = new_block_start + i;
            op.type = type;
            if (type == kCowXorOp) {
                op.source = (old_block + i) * header_.block_size + offset;
            } else {
                op.source = next_data_pos_;
            }

            if (compression_.algorithm) {
                auto data = [&, this]() {
                    if (num_compress_threads_ > 1) {
                        auto data = std::move(*buf_iter_);
                        buf_iter_++;
                        return data;
                    } else {
                        if (!compressor_) {
                            compressor_ = ICompressor::Create(compression_, header_.block_size);
                        }

                        auto data = compressor_->Compress(iter, header_.block_size);
                        return data;
                    }
                }();
                op.compression = compression_.algorithm;
                op.data_length = static_cast<uint16_t>(data.size());

                if (!WriteOperation(op, data.data(), data.size())) {
                    PLOG(ERROR) << "AddRawBlocks: write failed";
                    return false;
                }
            } else {
                op.data_length = static_cast<uint16_t>(header_.block_size);
                if (!WriteOperation(op, iter, header_.block_size)) {
                    PLOG(ERROR) << "AddRawBlocks: write failed";
                    return false;
                }
            }
            iter += header_.block_size;

            i += 1;
            pending_blocks -= 1;
        }

        CHECK(pending_blocks == 0);
    }
    return true;
}

bool CowWriterV2::EmitZeroBlocks(uint64_t new_block_start, uint64_t num_blocks) {
    CHECK(!merge_in_progress_);
    for (uint64_t i = 0; i < num_blocks; i++) {
        CowOperation op = {};
        op.type = kCowZeroOp;
        op.new_block = new_block_start + i;
        op.source = 0;
        WriteOperation(op);
    }
    return true;
}

bool CowWriterV2::EmitLabel(uint64_t label) {
    CHECK(!merge_in_progress_);
    CowOperation op = {};
    op.type = kCowLabelOp;
    op.source = label;
    return WriteOperation(op) && Sync();
}

bool CowWriterV2::EmitSequenceData(size_t num_ops, const uint32_t* data) {
    CHECK(!merge_in_progress_);
    size_t to_add = 0;
    size_t max_ops = (header_.block_size * 2) / sizeof(uint32_t);
    while (num_ops > 0) {
        CowOperation op = {};
        op.type = kCowSequenceOp;
        op.source = next_data_pos_;
        to_add = std::min(num_ops, max_ops);
        op.data_length = static_cast<uint16_t>(to_add * sizeof(uint32_t));
        if (!WriteOperation(op, data, op.data_length)) {
            PLOG(ERROR) << "AddSequenceData: write failed";
            return false;
        }
        num_ops -= to_add;
        data += to_add;
    }
    return true;
}

bool CowWriterV2::EmitCluster() {
    CowOperation op = {};
    op.type = kCowClusterOp;
    // Next cluster starts after remainder of current cluster and the next data block.
    op.source = current_data_size_ + cluster_size_ - current_cluster_size_ - sizeof(CowOperation);
    return WriteOperation(op);
}

bool CowWriterV2::EmitClusterIfNeeded() {
    // If there isn't room for another op and the cluster end op, end the current cluster
    if (cluster_size_ && cluster_size_ < current_cluster_size_ + 2 * sizeof(CowOperation)) {
        if (!EmitCluster()) return false;
    }
    return true;
}

bool CowWriterV2::Finalize() {
    if (!FlushCluster()) {
        LOG(ERROR) << "Finalize: FlushCluster() failed";
        return false;
    }

    auto continue_cluster_size = current_cluster_size_;
    auto continue_data_size = current_data_size_;
    auto continue_data_pos = next_data_pos_;
    auto continue_op_pos = next_op_pos_;
    auto continue_num_ops = footer_.op.num_ops;
    bool extra_cluster = false;

    // Blank out extra ops, in case we're in append mode and dropped ops.
    if (cluster_size_) {
        auto unused_cluster_space = cluster_size_ - current_cluster_size_;
        std::string clr;
        clr.resize(unused_cluster_space, '\0');
        if (lseek(fd_.get(), next_op_pos_, SEEK_SET) < 0) {
            PLOG(ERROR) << "Failed to seek to footer position.";
            return false;
        }
        if (!android::base::WriteFully(fd_, clr.data(), clr.size())) {
            PLOG(ERROR) << "clearing unused cluster area failed";
            return false;
        }
    }

    // Footer should be at the end of a file, so if there is data after the current block, end
    // it and start a new cluster.
    if (cluster_size_ && current_data_size_ > 0) {
        EmitCluster();
        extra_cluster = true;
    }

    footer_.op.ops_size = footer_.op.num_ops * sizeof(CowOperation);
    if (lseek(fd_.get(), next_op_pos_, SEEK_SET) < 0) {
        PLOG(ERROR) << "Failed to seek to footer position.";
        return false;
    }
    memset(&footer_.unused, 0, sizeof(footer_.unused));

    // Write out footer at end of file
    if (!android::base::WriteFully(fd_, reinterpret_cast<const uint8_t*>(&footer_),
                                   sizeof(footer_))) {
        PLOG(ERROR) << "write footer failed";
        return false;
    }

    // Remove excess data, if we're in append mode and threw away more data
    // than we wrote before.
    off_t offs = lseek(fd_.get(), 0, SEEK_CUR);
    if (offs < 0) {
        PLOG(ERROR) << "Failed to lseek to find current position";
        return false;
    }
    if (!Truncate(offs)) {
        return false;
    }

    // Reposition for additional Writing
    if (extra_cluster) {
        current_cluster_size_ = continue_cluster_size;
        current_data_size_ = continue_data_size;
        next_data_pos_ = continue_data_pos;
        next_op_pos_ = continue_op_pos;
        footer_.op.num_ops = continue_num_ops;
    }

    FlushCluster();

    return Sync();
}

uint64_t CowWriterV2::GetCowSize() {
    if (current_data_size_ > 0) {
        return next_data_pos_ + sizeof(footer_);
    } else {
        return next_op_pos_ + sizeof(footer_);
    }
}

bool CowWriterV2::GetDataPos(uint64_t* pos) {
    off_t offs = lseek(fd_.get(), 0, SEEK_CUR);
    if (offs < 0) {
        PLOG(ERROR) << "lseek failed";
        return false;
    }
    *pos = offs;
    return true;
}

bool CowWriterV2::EnsureSpaceAvailable(const uint64_t bytes_needed) const {
    if (bytes_needed > cow_image_size_) {
        LOG(ERROR) << "No space left on COW device. Required: " << bytes_needed
                   << ", available: " << cow_image_size_;
        errno = ENOSPC;
        return false;
    }
    return true;
}

bool CowWriterV2::FlushCluster() {
    ssize_t ret;

    if (op_vec_index_) {
        ret = pwritev(fd_.get(), cowop_vec_.get(), op_vec_index_, current_op_pos_);
        if (ret != (op_vec_index_ * sizeof(CowOperation))) {
            PLOG(ERROR) << "pwritev failed for CowOperation. Expected: "
                        << (op_vec_index_ * sizeof(CowOperation));
            return false;
        }
    }

    if (data_vec_index_) {
        ret = pwritev(fd_.get(), data_vec_.get(), data_vec_index_, current_data_pos_);
        if (ret != total_data_written_) {
            PLOG(ERROR) << "pwritev failed for data. Expected: " << total_data_written_;
            return false;
        }
    }

    total_data_written_ = 0;
    op_vec_index_ = 0;
    data_vec_index_ = 0;
    current_op_pos_ = next_op_pos_;
    current_data_pos_ = next_data_pos_;

    return true;
}

bool CowWriterV2::WriteOperation(const CowOperation& op, const void* data, size_t size) {
    if (!EnsureSpaceAvailable(next_op_pos_ + sizeof(op)) ||
        !EnsureSpaceAvailable(next_data_pos_ + size)) {
        return false;
    }

    if (batch_write_) {
        CowOperation* cow_op = reinterpret_cast<CowOperation*>(cowop_vec_[op_vec_index_].iov_base);
        std::memcpy(cow_op, &op, sizeof(CowOperation));
        op_vec_index_ += 1;

        if (data != nullptr && size > 0) {
            struct iovec* data_ptr = data_vec_.get();
            std::memcpy(data_ptr[data_vec_index_].iov_base, data, size);
            data_ptr[data_vec_index_].iov_len = size;
            data_vec_index_ += 1;
            total_data_written_ += size;
        }
    } else {
        if (lseek(fd_.get(), next_op_pos_, SEEK_SET) < 0) {
            PLOG(ERROR) << "lseek failed for writing operation.";
            return false;
        }
        if (!android::base::WriteFully(fd_, reinterpret_cast<const uint8_t*>(&op), sizeof(op))) {
            return false;
        }
        if (data != nullptr && size > 0) {
            if (!WriteRawData(data, size)) return false;
        }
    }

    AddOperation(op);

    if (batch_write_) {
        if (op_vec_index_ == header_.cluster_ops || data_vec_index_ == header_.cluster_ops ||
            op.type == kCowLabelOp || op.type == kCowClusterOp) {
            if (!FlushCluster()) {
                LOG(ERROR) << "Failed to flush cluster data";
                return false;
            }
        }
    }

    return EmitClusterIfNeeded();
}

void CowWriterV2::AddOperation(const CowOperation& op) {
    footer_.op.num_ops++;

    if (op.type == kCowClusterOp) {
        current_cluster_size_ = 0;
        current_data_size_ = 0;
    } else if (header_.cluster_ops) {
        current_cluster_size_ += sizeof(op);
        current_data_size_ += op.data_length;
    }

    next_data_pos_ += op.data_length + GetNextDataOffset(op, header_.cluster_ops);
    next_op_pos_ += sizeof(CowOperation) + GetNextOpOffset(op, header_.cluster_ops);
}

bool CowWriterV2::WriteRawData(const void* data, const size_t size) {
    if (!android::base::WriteFullyAtOffset(fd_, data, size, next_data_pos_)) {
        return false;
    }
    return true;
}

bool CowWriterV2::Sync() {
    if (is_dev_null_) {
        return true;
    }
    if (fsync(fd_.get()) < 0) {
        PLOG(ERROR) << "fsync failed";
        return false;
    }
    return true;
}

bool CowWriterV2::Truncate(off_t length) {
    if (is_dev_null_ || is_block_device_) {
        return true;
    }
    if (ftruncate(fd_.get(), length) < 0) {
        PLOG(ERROR) << "Failed to truncate.";
        return false;
    }
    return true;
}

}  // namespace snapshot
}  // namespace android
