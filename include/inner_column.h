#pragma once
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include <cstdint>

#include "attribute.h"
#include "statement.h"

struct FilterThreadPool {
    size_t num_threads;

    explicit FilterThreadPool(unsigned num_threads)
    : num_threads(num_threads ? num_threads : 1) {}

    size_t begin_idx(size_t thread_id, size_t tasks) const {
        const size_t base = tasks / num_threads;
        const size_t rem  = tasks % num_threads;
        return thread_id * base + std::min(thread_id, rem);
    }

    void run(std::function<void(size_t, size_t)> function, size_t tasks) {
        if (tasks == 0) return;

        const size_t nt = num_threads ? num_threads : 1;
        std::vector<std::thread> threads;
        threads.reserve(nt ? nt - 1 : 0);

        for (size_t t = 0; t + 1 < nt; ++t) {
            threads.emplace_back([&, t]() {
                const size_t begin = begin_idx(t, tasks);
                const size_t end   = begin_idx(t + 1, tasks);
                if (begin < end) function(begin, end);
            });
        }

        // Execute last shard on calling thread to avoid extra join/creation overhead.
        const size_t begin_last = begin_idx(nt - 1, tasks);
        const size_t end_last   = begin_idx(nt, tasks);
        if (begin_last < end_last) function(begin_last, end_last);

        for (auto& th : threads) th.join();
    }
};

inline FilterThreadPool filter_tp(12);

struct InnerColumnBase {
    DataType type;

    InnerColumnBase(DataType type)
    : type(type) {}

    virtual ~InnerColumnBase() {}
};

template <class T>
struct InnerColumn: InnerColumnBase {
    static constexpr DataType data_type() {
        if constexpr (std::is_same_v<T, int32_t>) {
            return DataType::INT32;
        } else if constexpr (std::is_same_v<T, int64_t>) {
            return DataType::INT64;
        } else if constexpr (std::is_same_v<T, double>) {
            return DataType::FP64;
        }
    }

    InnerColumn()
    : InnerColumnBase(data_type()) {}

    std::vector<T>       data;
    std::vector<uint8_t> bitmap;

    void bitmap_push_back(bool not_null) {
        if ((data.size() + 7) / 8 > bitmap.size()) {
            if (not_null) {
                bitmap.push_back(0x01);
            } else {
                bitmap.push_back(0x00);
            }
        } else {
            size_t byte_idx = (data.size() - 1) / 8;
            size_t bit_idx  = (data.size() - 1) % 8;
            if (not_null) {
                bitmap[byte_idx] |= (0x1 << bit_idx);
            } else {
                bitmap[byte_idx] &= ~(0x1 << bit_idx);
            }
        }
    }

    void push_back(T value) {
        data.emplace_back(value);
        bitmap_push_back(true);
    }

    void push_back_null() {
        data.emplace_back();
        bitmap_push_back(false);
    }

    bool is_not_null(size_t idx) const {
        size_t byte_idx = idx / 8;
        size_t bit_idx  = idx % 8;
        return bitmap[byte_idx] & (0x1 << bit_idx);
    }

    T get(size_t idx) const { return data[idx]; }

    std::vector<uint8_t> less(T rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, rhs, &ret](size_t byte_begin, size_t byte_end) {
            less(data.data() + byte_begin * 8,
                bitmap.data() + byte_begin,
                ret.data() + byte_begin,
                std::min(byte_end * 8, data.size()) - byte_begin * 8,
                rhs);
        };
        filter_tp.run(task, (data.size() + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> greater(T rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, rhs, &ret](size_t byte_begin, size_t byte_end) {
            greater(data.data() + byte_begin * 8,
                bitmap.data() + byte_begin,
                ret.data() + byte_begin,
                std::min(byte_end * 8, data.size()) - byte_begin * 8,
                rhs);
        };
        filter_tp.run(task, (data.size() + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> less_equal(T rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, rhs, &ret](size_t byte_begin, size_t byte_end) {
            less_equal(data.data() + byte_begin * 8,
                bitmap.data() + byte_begin,
                ret.data() + byte_begin,
                std::min(byte_end * 8, data.size()) - byte_begin * 8,
                rhs);
        };
        filter_tp.run(task, (data.size() + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> greater_equal(T rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, rhs, &ret](size_t byte_begin, size_t byte_end) {
            greater_equal(data.data() + byte_begin * 8,
                bitmap.data() + byte_begin,
                ret.data() + byte_begin,
                std::min(byte_end * 8, data.size()) - byte_begin * 8,
                rhs);
        };
        filter_tp.run(task, (data.size() + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> equal(T rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, rhs, &ret](size_t byte_begin, size_t byte_end) {
            equal(data.data() + byte_begin * 8,
                bitmap.data() + byte_begin,
                ret.data() + byte_begin,
                std::min(byte_end * 8, data.size()) - byte_begin * 8,
                rhs);
        };
        filter_tp.run(task, (data.size() + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> not_equal(T rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, rhs, &ret](size_t byte_begin, size_t byte_end) {
            not_equal(data.data() + byte_begin * 8,
                bitmap.data() + byte_begin,
                ret.data() + byte_begin,
                std::min(byte_end * 8, data.size()) - byte_begin * 8,
                rhs);
        };
        filter_tp.run(task, (data.size() + 7) / 8);
        return ret;
    }

    static void less(const T* __restrict__ data,
        const uint8_t* __restrict__ bitmap,
        uint8_t* __restrict__ output,
        size_t size,
        T      rhs) {
        for (size_t i = 0; i < size; ++i) {
            size_t byte_idx   = i / 8;
            size_t bit_idx    = i % 8;
            output[byte_idx] |= (bitmap[byte_idx] & (0x1 << bit_idx)
                                 & (static_cast<uint8_t>(data[i] < rhs) << bit_idx));
        }
    }

    static void greater(const T* __restrict__ data,
        const uint8_t* __restrict__ bitmap,
        uint8_t* __restrict__ output,
        size_t size,
        T      rhs) {
        for (size_t i = 0; i < size; ++i) {
            size_t byte_idx   = i / 8;
            size_t bit_idx    = i % 8;
            output[byte_idx] |= (bitmap[byte_idx] & (0x1 << bit_idx)
                                 & (static_cast<uint8_t>(data[i] > rhs) << bit_idx));
        }
    }

    static void less_equal(const T* __restrict__ data,
        const uint8_t* __restrict__ bitmap,
        uint8_t* __restrict__ output,
        size_t size,
        T      rhs) {
        for (size_t i = 0; i < size; ++i) {
            size_t byte_idx   = i / 8;
            size_t bit_idx    = i % 8;
            output[byte_idx] |= (bitmap[byte_idx] & (0x1 << bit_idx)
                                 & (static_cast<uint8_t>(data[i] <= rhs) << bit_idx));
        }
    }

    static void greater_equal(const T* __restrict__ data,
        const uint8_t* __restrict__ bitmap,
        uint8_t* __restrict__ output,
        size_t size,
        T      rhs) {
        for (size_t i = 0; i < size; ++i) {
            size_t byte_idx   = i / 8;
            size_t bit_idx    = i % 8;
            output[byte_idx] |= (bitmap[byte_idx] & (0x1 << bit_idx)
                                 & (static_cast<uint8_t>(data[i] >= rhs) << bit_idx));
        }
    }

    static void equal(const T* __restrict__ data,
        const uint8_t* __restrict__ bitmap,
        uint8_t* __restrict__ output,
        size_t size,
        T      rhs) {
        for (size_t i = 0; i < size; ++i) {
            size_t byte_idx   = i / 8;
            size_t bit_idx    = i % 8;
            output[byte_idx] |= (bitmap[byte_idx] & (0x1 << bit_idx)
                                 & (static_cast<uint8_t>(data[i] == rhs) << bit_idx));
        }
    }

    static void not_equal(const T* __restrict__ data,
        const uint8_t* __restrict__ bitmap,
        uint8_t* __restrict__ output,
        size_t size,
        T      rhs) {
        for (size_t i = 0; i < size; ++i) {
            size_t byte_idx   = i / 8;
            size_t bit_idx    = i % 8;
            output[byte_idx] |= (bitmap[byte_idx] & (0x1 << bit_idx)
                                 & (static_cast<uint8_t>(data[i] != rhs) << bit_idx));
        }
    }
};

template <>
struct InnerColumn<std::string>: InnerColumnBase {
    static constexpr DataType data_type() { return DataType::VARCHAR; }

    InnerColumn()
    : InnerColumnBase(data_type()) {}

    std::vector<char>    data;
    std::vector<size_t>  offsets;
    std::vector<uint8_t> bitmap;
    size_t               row = 0;

    void bitmap_push_back(bool not_null) {
        if (row / 8 + 1 > bitmap.size()) {
            if (not_null) {
                bitmap.push_back(0x01);
            } else {
                bitmap.push_back(0x00);
            }
        } else {
            size_t byte_idx = row / 8;
            size_t bit_idx  = row % 8;
            if (not_null) {
                bitmap[byte_idx] |= (0x1 << bit_idx);
            } else {
                bitmap[byte_idx] &= ~(0x1 << bit_idx);
            }
        }
        row += 1;
    }

    void push_back(std::string_view value) {
        data.insert(data.end(), value.begin(), value.end());
        offsets.emplace_back(data.size());
        bitmap_push_back(true);
    }

    void push_back_null() {
        offsets.emplace_back(data.size());
        bitmap_push_back(false);
    }

    bool is_not_null(size_t idx) const {
        size_t byte_idx = idx / 8;
        size_t bit_idx  = idx % 8;
        return bitmap[byte_idx] & (0x1 << bit_idx);
    }

    std::string_view get(size_t idx) const {
        size_t begin;
        if (idx == 0) [[unlikely]] {
            begin = 0;
        } else {
            begin = offsets[idx - 1];
        }
        size_t end = offsets[idx];
        return std::string_view{data.data() + begin, end - begin};
    }

    std::vector<uint8_t> less(std::string_view rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, &ret, &rhs](size_t byte_begin, size_t byte_end) {
            for (size_t byte_idx = byte_begin; byte_idx < byte_end; ++byte_idx) {
                for (size_t bit_idx = 0; bit_idx < 8; ++bit_idx) {
                    size_t i = byte_idx * 8 + bit_idx;
                    if (i >= row) {
                        break;
                    }
                    bool             non_null = bitmap[byte_idx] & (0x1 << bit_idx);
                    size_t           begin = i == 0 ? (size_t)0 : offsets[i - 1];
                    size_t           end = offsets[i];
                    std::string_view value{data.data() + begin, end - begin};
                    ret[byte_idx] |=
                        (static_cast<uint8_t>(non_null and value < rhs) << bit_idx);
                }
            }
        };
        filter_tp.run(task, (row + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> greater(std::string_view rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, &ret, &rhs](size_t byte_begin, size_t byte_end) {
            for (size_t byte_idx = byte_begin; byte_idx < byte_end; ++byte_idx) {
                for (size_t bit_idx = 0; bit_idx < 8; ++bit_idx) {
                    size_t i = byte_idx * 8 + bit_idx;
                    if (i >= row) {
                        break;
                    }
                    bool             non_null = bitmap[byte_idx] & (0x1 << bit_idx);
                    size_t           begin = i == 0 ? (size_t)0 : offsets[i - 1];
                    size_t           end = offsets[i];
                    std::string_view value{data.data() + begin, end - begin};
                    ret[byte_idx] |=
                        (static_cast<uint8_t>(non_null and value > rhs) << bit_idx);
                }
            }
        };
        filter_tp.run(task, (row + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> less_equal(std::string_view rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, &ret, &rhs](size_t byte_begin, size_t byte_end) {
            for (size_t byte_idx = byte_begin; byte_idx < byte_end; ++byte_idx) {
                for (size_t bit_idx = 0; bit_idx < 8; ++bit_idx) {
                    size_t i = byte_idx * 8 + bit_idx;
                    if (i >= row) {
                        break;
                    }
                    bool             non_null = bitmap[byte_idx] & (0x1 << bit_idx);
                    size_t           begin = i == 0 ? (size_t)0 : offsets[i - 1];
                    size_t           end = offsets[i];
                    std::string_view value{data.data() + begin, end - begin};
                    ret[byte_idx] |=
                        (static_cast<uint8_t>(non_null and value <= rhs) << bit_idx);
                }
            }
        };
        filter_tp.run(task, (row + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> greater_equal(std::string_view rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, &ret, &rhs](size_t byte_begin, size_t byte_end) {
            for (size_t byte_idx = byte_begin; byte_idx < byte_end; ++byte_idx) {
                for (size_t bit_idx = 0; bit_idx < 8; ++bit_idx) {
                    size_t i = byte_idx * 8 + bit_idx;
                    if (i >= row) {
                        break;
                    }
                    bool             non_null = bitmap[byte_idx] & (0x1 << bit_idx);
                    size_t           begin = i == 0 ? (size_t)0 : offsets[i - 1];
                    size_t           end = offsets[i];
                    std::string_view value{data.data() + begin, end - begin};
                    ret[byte_idx] |=
                        (static_cast<uint8_t>(non_null and value >= rhs) << bit_idx);
                }
            }
        };
        filter_tp.run(task, (row + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> equal(std::string_view rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, &ret, &rhs](size_t byte_begin, size_t byte_end) {
            for (size_t byte_idx = byte_begin; byte_idx < byte_end; ++byte_idx) {
                for (size_t bit_idx = 0; bit_idx < 8; ++bit_idx) {
                    size_t i = byte_idx * 8 + bit_idx;
                    if (i >= row) {
                        break;
                    }
                    bool             non_null = bitmap[byte_idx] & (0x1 << bit_idx);
                    size_t           begin = i == 0 ? (size_t)0 : offsets[i - 1];
                    size_t           end = offsets[i];
                    std::string_view value{data.data() + begin, end - begin};
                    ret[byte_idx] |=
                        (static_cast<uint8_t>(non_null and value == rhs) << bit_idx);
                }
            }
        };
        filter_tp.run(task, (row + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> not_equal(std::string_view rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, &ret, &rhs](size_t byte_begin, size_t byte_end) {
            for (size_t byte_idx = byte_begin; byte_idx < byte_end; ++byte_idx) {
                for (size_t bit_idx = 0; bit_idx < 8; ++bit_idx) {
                    size_t i = byte_idx * 8 + bit_idx;
                    if (i >= row) {
                        break;
                    }
                    bool             non_null = bitmap[byte_idx] & (0x1 << bit_idx);
                    size_t           begin = i == 0 ? (size_t)0 : offsets[i - 1];
                    size_t           end = offsets[i];
                    std::string_view value{data.data() + begin, end - begin};
                    ret[byte_idx] |=
                        (static_cast<uint8_t>(non_null and value != rhs) << bit_idx);
                }
            }
        };
        filter_tp.run(task, (row + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> like(const std::string& rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, &ret, &rhs](size_t byte_begin, size_t byte_end) {
            for (size_t byte_idx = byte_begin; byte_idx < byte_end; ++byte_idx) {
                for (size_t bit_idx = 0; bit_idx < 8; ++bit_idx) {
                    size_t i = byte_idx * 8 + bit_idx;
                    if (i >= row) {
                        break;
                    }
                    bool             non_null = bitmap[byte_idx] & (0x1 << bit_idx);
                    size_t           begin = i == 0 ? (size_t)0 : offsets[i - 1];
                    size_t           end = offsets[i];
                    std::string_view value{data.data() + begin, end - begin};
                    ret[byte_idx] |=
                        (static_cast<uint8_t>(non_null and Comparison::like_match(value, rhs))
                            << bit_idx);
                }
            }
        };
        filter_tp.run(task, (row + 7) / 8);
        return ret;
    }

    std::vector<uint8_t> not_like(const std::string& rhs) const {
        std::vector<uint8_t> ret(bitmap.size());
        auto                 task = [this, &ret, &rhs](size_t byte_begin, size_t byte_end) {
            for (size_t byte_idx = byte_begin; byte_idx < byte_end; ++byte_idx) {
                for (size_t bit_idx = 0; bit_idx < 8; ++bit_idx) {
                    size_t i = byte_idx * 8 + bit_idx;
                    if (i >= row) {
                        break;
                    }
                    bool             non_null = bitmap[byte_idx] & (0x1 << bit_idx);
                    size_t           begin = i == 0 ? (size_t)0 : offsets[i - 1];
                    size_t           end = offsets[i];
                    std::string_view value{data.data() + begin, end - begin};
                    ret[byte_idx] |= (static_cast<uint8_t>(
                                          non_null and not Comparison::like_match(value, rhs))
                                      << bit_idx);
                }
            }
        };
        filter_tp.run(task, (row + 7) / 8);
        return ret;
    }
};

struct InnerTable {
    size_t                                        rows;
    std::vector<std::unique_ptr<InnerColumnBase>> columns;
};

struct InnerTableView {
    size_t                              rows;
    std::vector<const InnerColumnBase*> columns;

    InnerTableView() = default;

    InnerTableView(const InnerTable& table)
    : rows(table.rows) {
        for (auto& c: table.columns) {
            columns.push_back(c.get());
        }
    }
};
