#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mpmc_detail {

// Atomic storage for a payload-and-sequence entry. Use the standard atomic
// interface on targets where it is reliable. Supported x86 targets use the
// compiler's native CMPXCHG16B primitive because aggregate-atomic answers can
// differ across compiler/standard-library pairings.
template<typename Entry, typename Payload>
class alignas(16) atomic_entry
{
    static constexpr bool standard_backend_available =
        std::atomic<Entry>::is_always_lock_free;

#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__) && \
    defined(__SIZEOF_INT128__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
    using native_entry = unsigned __int128;
    static constexpr bool native_backend_available =
        sizeof(Entry) == sizeof(native_entry) && sizeof(std::size_t) == 8 &&
        sizeof(Payload) <= 8;
    static constexpr bool use_native_backend = native_backend_available;
    using storage_type = std::conditional_t<use_native_backend,
                                            native_entry, std::atomic<Entry>>;
#else
    static constexpr bool native_backend_available = false;
    static constexpr bool use_native_backend = false;
    using storage_type = std::atomic<Entry>;
#endif

    storage_type storage{};

#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__) && \
    defined(__SIZEOF_INT128__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
    static native_entry pack(const Entry& value) noexcept
    {
        std::uint64_t payload = 0;
        std::memcpy(&payload, &value.data, sizeof(Payload));
        return (static_cast<native_entry>(value.sequence) << 64U) | payload;
    }

    static Entry unpack(native_entry value) noexcept
    {
        Entry result{};
        const std::uint64_t payload = static_cast<std::uint64_t>(value);
        std::memcpy(&result.data, &payload, sizeof(Payload));
        result.sequence = static_cast<std::size_t>(value >> 64U);
        return result;
    }

    native_entry* native_storage() noexcept
    {
        return reinterpret_cast<native_entry*>(&storage);
    }

    const native_entry* native_storage() const noexcept
    {
        return reinterpret_cast<const native_entry*>(&storage);
    }

    native_entry native_load() const noexcept
    {
        // CMPXCHG16B with equal compare/desired values is an atomic load.
        return __sync_val_compare_and_swap(
            const_cast<native_entry*>(native_storage()),
            native_entry{0}, native_entry{0});
    }
#endif

public:
    static constexpr bool is_always_lock_free =
        standard_backend_available || native_backend_available;
    static constexpr bool uses_native_backend = use_native_backend;

    Entry load(std::memory_order order) const noexcept
    {
        if constexpr (use_native_backend) {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__) && \
    defined(__SIZEOF_INT128__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
            (void)order; // __sync builtins provide stronger seq_cst ordering.
            return unpack(native_load());
#else
            static_assert(!use_native_backend);
#endif
        } else {
            return storage.load(order);
        }
    }

    void store(Entry desired, std::memory_order order) noexcept
    {
        if constexpr (use_native_backend) {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__) && \
    defined(__SIZEOF_INT128__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
            (void)order;
            const native_entry desired_value = pack(desired);
            native_entry expected = native_load();
            while (true) {
                const native_entry observed = __sync_val_compare_and_swap(
                    native_storage(), expected, desired_value);
                if (observed == expected)
                    return;
                expected = observed;
            }
#else
            static_assert(!use_native_backend);
#endif
        } else {
            storage.store(desired, order);
        }
    }

    bool compare_exchange_strong(Entry& expected, Entry desired,
                                 std::memory_order order) noexcept
    {
        if constexpr (use_native_backend) {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__) && \
    defined(__SIZEOF_INT128__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
            (void)order;
            const native_entry expected_value = pack(expected);
            const native_entry observed = __sync_val_compare_and_swap(
                native_storage(), expected_value, pack(desired));
            if (observed == expected_value)
                return true;
            expected = unpack(observed);
            return false;
#else
            static_assert(!use_native_backend);
#endif
        } else {
            return storage.compare_exchange_strong(expected, desired, order);
        }
    }
};

} // namespace mpmc_detail
