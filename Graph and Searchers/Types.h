#pragma once

// Standard includes required by downstream code
#include <cstdint>    // uint64_t
#include <cstddef>    // std::size_t
#include <functional> // std::hash
#include <type_traits>
#include <utility>    // std::move if needed

// StrongId: opaque, typed ID wrapper
template <typename Tag>
class StrongId
{
public:
    using value_type = uint64_t;

    // default constructs to zero (empty id)
    constexpr StrongId() noexcept : v_(0) {}

    // explicit from integral value
    explicit constexpr StrongId(value_type v) noexcept : v_(v) {}

    // copy / move defaulted
    constexpr StrongId(const StrongId&) noexcept = default;
    constexpr StrongId(StrongId&&) noexcept = default;
    StrongId& operator=(const StrongId&) noexcept = default;
    StrongId& operator=(StrongId&&) noexcept = default;

    // access
    constexpr value_type value() const noexcept { return v_; }

    // conversions only when explicitly requested
    explicit constexpr operator value_type() const noexcept { return v_; }

    // comparisons
    constexpr bool operator==(const StrongId& o) const noexcept { return v_ == o.v_; }
    constexpr bool operator!=(const StrongId& o) const noexcept { return v_ != o.v_; }
    constexpr bool operator<(const StrongId& o) const noexcept { return v_ < o.v_; }
    constexpr bool operator<=(const StrongId& o) const noexcept { return v_ <= o.v_; }
    constexpr bool operator>(const StrongId& o) const noexcept { return v_ > o.v_; }
    constexpr bool operator>=(const StrongId& o) const noexcept { return v_ >= o.v_; }

    // helper for empty check
    constexpr bool IsEmpty() const noexcept { return v_ == 0; }

    // prefix increment (++id)
    StrongId& operator++() noexcept {
        ++v_;
        return *this;
    }

    // postfix increment (id++)
    StrongId operator++(int) noexcept {
        StrongId tmp = *this;
        ++v_;
        return tmp;
    }

private:
    value_type v_;
};

// Provide std::hash specialization so StrongId can be used in unordered containers
namespace std
{
    template <typename Tag>
    struct hash<StrongId<Tag>>
    {
        std::size_t operator()(const StrongId<Tag>& id) const noexcept
        {
            // hash the underlying integral value
            return std::hash<typename StrongId<Tag>::value_type>()(id.value());
        }
    };
}

// graph nodes and edges
using NodeId = StrongId<struct NodeTag>;
using EdgeId = StrongId<struct EdgeTag>;

// contiguous CSR indices
using CSR_ID = StrongId<struct CSR_Tag>;

// invalid nodes and edges
static const NodeId NodeIdInvalid(0);
static const EdgeId EdgeIdInvalid(0);

// invalid CSR Key (starts from index 1)
static const CSR_ID CSR_ID_INVALID(0);

// tracks direction of edges (used for queries)
enum EdgeDirection {
    OUTGOING,
    INCOMING,
    BOTH
};

// tracks query type for CSR
enum class CSR_Mode {
    SHORTEST_PATH,
    REACHABLE,
    KHOP
};