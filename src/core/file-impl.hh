/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2016 ScyllaDB
 */

#pragma once

#include <seastar/core/file.hh>
#include <seastar/core/shared_ptr.hh>

#include <deque>
#include <atomic>

namespace seastar {
class io_queue;

namespace internal {

// Given a properly aligned vector of iovecs, ensures that it respects the
// IOV_MAX limit, by trimming if necessary. The modified vector still satisfied
// the alignment requirements.
// Returns the final total length of all iovecs.
size_t sanitize_iovecs(std::vector<iovec>& iov, size_t disk_alignment) noexcept;

}

class posix_file_handle_impl : public seastar::file_handle_impl {
    int _fd;
    std::atomic<unsigned>* _refcount;
    dev_t _device_id;
    open_flags _open_flags;
    uint32_t _memory_dma_alignment;
    uint32_t _disk_read_dma_alignment;
    uint32_t _disk_write_dma_alignment;
public:
    posix_file_handle_impl(int fd, open_flags f, std::atomic<unsigned>* refcount, dev_t device_id,
            uint32_t memory_dma_alignment,
            uint32_t disk_read_dma_alignment,
            uint32_t disk_write_dma_alignment)
            : _fd(fd), _refcount(refcount), _device_id(device_id), _open_flags(f)
            , _memory_dma_alignment(memory_dma_alignment)
            , _disk_read_dma_alignment(disk_read_dma_alignment)
            , _disk_write_dma_alignment(disk_write_dma_alignment) {
    }
    virtual ~posix_file_handle_impl();
    posix_file_handle_impl(const posix_file_handle_impl&) = delete;
    posix_file_handle_impl(posix_file_handle_impl&&) = delete;
    virtual shared_ptr<file_impl> to_file() && override;
    virtual std::unique_ptr<seastar::file_handle_impl> clone() const override;
};

class posix_file_impl : public file_impl {
    std::atomic<unsigned>* _refcount = nullptr;
    dev_t _device_id;
    io_queue* _io_queue;
    open_flags _open_flags;
public:
    int _fd;
    posix_file_impl(int fd, open_flags, file_open_options options, dev_t device_id,
            uint32_t block_size);
    posix_file_impl(int fd, open_flags, std::atomic<unsigned>* refcount, dev_t device_id,
            uint32_t memory_dma_alignment,
            uint32_t disk_read_dma_alignment,
            uint32_t disk_write_dma_alignment);
    virtual ~posix_file_impl() override;
    future<> flush(void) noexcept override;
    future<struct stat> stat(void) noexcept override;
    future<> truncate(uint64_t length) noexcept override;
    future<> discard(uint64_t offset, uint64_t length) noexcept override;
    virtual future<> allocate(uint64_t position, uint64_t length) noexcept override;
    future<uint64_t> size() noexcept override;
    virtual future<> close() noexcept override;
    virtual std::unique_ptr<seastar::file_handle_impl> dup() override;
    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)> next) override;

    open_flags flags() const {
        return _open_flags;
    }
private:
    void query_dma_alignment(uint32_t block_size);

    /**
     * Try to read from the given position where the previous short read has
     * stopped. Check the EOF condition.
     *
     * The below code assumes the following: short reads due to I/O errors
     * always end at address aligned to HW block boundary. Therefore if we issue
     * a new read operation from the next position we are promised to get an
     * error (different from EINVAL). If we've got a short read because we have
     * reached EOF then the above read would either return a zero-length success
     * (if the file size is aligned to HW block size) or an EINVAL error (if
     * file length is not aligned to HW block size).
     *
     * @param pos offset to read from
     * @param len number of bytes to read
     * @param pc the IO priority class under which to queue this operation
     *
     * @return temporary buffer with read data or zero-sized temporary buffer if
     *         pos is at or beyond EOF.
     * @throw appropriate exception in case of I/O error.
     */
    future<temporary_buffer<uint8_t>>
    read_maybe_eof(uint64_t pos, size_t len, const io_priority_class& pc);

protected:
    future<size_t> do_write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) noexcept;
    future<size_t> do_write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) noexcept;
    future<size_t> do_read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) noexcept;
    future<size_t> do_read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) noexcept;
    future<temporary_buffer<uint8_t>> do_dma_read_bulk(uint64_t offset, size_t range_size, const io_priority_class& pc) noexcept;
};

class posix_file_real_impl final : public posix_file_impl {
public:
    posix_file_real_impl(int fd, open_flags of, file_open_options options, dev_t device_id, uint32_t block_size)
        : posix_file_impl(fd, of, std::move(options), device_id, block_size) {}
    posix_file_real_impl(int fd, open_flags of, std::atomic<unsigned>* refcount, dev_t device_id,
            uint32_t memory_dma_alignment, uint32_t disk_read_dma_alignment, uint32_t disk_write_dma_alignment)
        : posix_file_impl(fd, of, refcount, device_id, memory_dma_alignment, disk_read_dma_alignment, disk_write_dma_alignment) {}
    virtual future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) noexcept override;
    virtual future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) noexcept override;
    virtual future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) noexcept override;
    virtual future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) noexcept override;
    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t offset, size_t range_size, const io_priority_class& pc) noexcept override;
};

// The Linux XFS implementation is challenged wrt. append: a write that changes
// eof will be blocked by any other concurrent AIO operation to the same file, whether
// it changes file size or not. Furthermore, ftruncate() will also block and be blocked
// by AIO, so attempts to game the system and call ftruncate() have to be done very carefully.
//
// Other Linux filesystems may have different locking rules, so this may need to be
// adjusted for them.
class append_challenged_posix_file_impl final : public posix_file_impl, public enable_shared_from_this<append_challenged_posix_file_impl> {
    // File size as a result of completed kernel operations (writes and truncates)
    uint64_t _committed_size;
    // File size as a result of seastar API calls
    uint64_t _logical_size;
    // Pending operations
    enum class opcode {
        invalid,
        read,
        write,
        truncate,
        flush,
    };
    struct op {
        opcode type;
        uint64_t pos;
        size_t len;
        noncopyable_function<future<> ()> run;
    };
    // Queue of pending operations; processed from front to end to avoid
    // starvation, but can issue concurrent operations.
    std::deque<op> _q;
    unsigned _max_size_changing_ops = 0;
    unsigned _current_non_size_changing_ops = 0;
    unsigned _current_size_changing_ops = 0;
    bool _fsync_is_exclusive = true;

    // Set when the user is closing the file
    enum class state { open, draining, closing, closed };
    state _closing_state = state::open;

    bool _sloppy_size = false;
    uint64_t _sloppy_size_hint;
    // Fulfiled when _done and I/O is complete
    promise<> _completed;
private:
    void commit_size(uint64_t size) noexcept;
    bool must_run_alone(const op& candidate) const noexcept;
    bool size_changing(const op& candidate) const noexcept;
    bool may_dispatch(const op& candidate) const noexcept;
    void dispatch(op& candidate) noexcept;
    void optimize_queue() noexcept;
    void process_queue() noexcept;
    bool may_quit() const noexcept;
    void enqueue_op(op&& op);
    template <typename... T, typename Func>
    future<T...> enqueue(opcode type, uint64_t pos, size_t len, Func&& func) noexcept {
        try {
            auto pr = make_lw_shared(promise<T...>());
            auto fut = pr->get_future();
            auto op_func = [func = std::move(func), pr = std::move(pr)] () mutable {
                return futurize_invoke(std::move(func)).then_wrapped([pr = std::move(pr)] (future<T...> f) mutable {
                    f.forward_to(std::move(*pr));
                });
            };
            enqueue_op({type, pos, len, std::move(op_func)});
            return fut;
        } catch (...) {
            return make_exception_future<T...>(std::current_exception());
        }
    }
public:
    append_challenged_posix_file_impl(int fd, open_flags, file_open_options options, unsigned max_size_changing_ops, bool fsync_is_exclusive, dev_t device_id, size_t block_size);
    ~append_challenged_posix_file_impl() override;
    virtual future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) noexcept override;
    virtual future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) noexcept override;
    virtual future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) noexcept override;
    virtual future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) noexcept override;
    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t offset, size_t range_size, const io_priority_class& pc) noexcept override;
    future<> flush() noexcept override;
    future<struct stat> stat() noexcept override;
    future<> truncate(uint64_t length) noexcept override;
    future<uint64_t> size() noexcept override;
    future<> close() noexcept override;
};

class blockdev_file_impl final : public posix_file_impl {
public:
    blockdev_file_impl(int fd, open_flags, file_open_options options, dev_t device_id, size_t block_size);
    future<> truncate(uint64_t length) noexcept override;
    future<> discard(uint64_t offset, uint64_t length) noexcept override;
    future<uint64_t> size() noexcept override;
    virtual future<> allocate(uint64_t position, uint64_t length) noexcept override;
    virtual future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) noexcept override;
    virtual future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) noexcept override;
    virtual future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) noexcept override;
    virtual future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) noexcept override;
    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t offset, size_t range_size, const io_priority_class& pc) noexcept override;
};

}
