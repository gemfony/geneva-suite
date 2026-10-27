/********************************************************************************
 *
 * This file is part of the Geneva library collection. The following license
 * applies to this file:
 *
 * ------------------------------------------------------------------------------
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ------------------------------------------------------------------------------
 *
 * Note that other files in the Geneva library collection may use a different
 * license. Please see the licensing information in each file.
 *
 ********************************************************************************
 *
 * See the NOTICE file in the top-level directory of the Geneva library
 * collection for a list of contributors and copyright information.
 *
 ********************************************************************************/

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <algorithm>
#include <concepts>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <list>
#include <memory>
#include <ranges>
#include <sstream>
#include <type_traits>
#include <vector>

// Boost header files go here

// Geneva headers go here
#include "common/GArchiveNamed.hpp" // archive_named (named-member emitter)
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GTypeTraitsT.hpp"

namespace Gem::Common {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/**
 * @brief Concept: the container provides contiguous storage accessible via data().
 *
 * Satisfied by std::vector and std::array but not by std::deque or std::list.
 *
 * @tparam C The container type to check.
 */
template <typename C>
concept HasContiguousStorage = requires(C c) { c.data(); };

/******************************************************************************/
/**
 * @brief Concept: the container supports capacity management.
 *
 * Satisfied by std::vector but not by std::deque or std::list.
 *
 * @tparam C The container type to check.
 */
template <typename C>
concept HasCapacity = requires(C c, typename C::size_type n) {
    c.reserve(n);
    c.capacity();
    c.shrink_to_fit();
};

/******************************************************************************/
/**
 * @brief Concept: the container's iterators satisfy the random-access requirement.
 *
 * Satisfied by std::vector and std::deque but not by std::list or
 * std::forward_list.
 *
 * @tparam C The container type to check.
 */
template <typename C>
concept HasRandomAccess = std::random_access_iterator<typename C::iterator>;

/******************************************************************************/
/**
 * @brief Concept: the container supports O(1) front insertion and removal.
 *
 * Satisfied by std::deque and std::list but not by std::vector.
 *
 * @tparam C The container type to check.
 */
template <typename C>
concept HasFrontInsertion = requires(C c, typename C::value_type v) {
    c.push_front(v);
    c.pop_front();
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/**
 * @brief Storage policy for elements held BY VALUE.
 *
 * Wraps a plain sequence container whose element type is @p T. A simple
 * value-copy is sufficient for deep-copy semantics because a value owns
 * whatever it owns through its own members -- there is no handle to share.
 *
 * @par What @p T has to be
 * Only that a value copy IS a deep copy, i.e. @c std::copyable. The policy's
 * every operation is an assignment; nothing here inspects an object
 * representation, so triviality and standard layout are not required (they
 * were originally demanded, which kept out perfectly well-behaved value types
 * such as a record holding a @c std::vector). The name says POD because the
 * POD case is the overwhelmingly common one and because renaming a public
 * type is a separate, deliberate decision -- read it as "held by value".
 *
 * @tparam T         The element type. Must be @c std::copyable.
 * @tparam Container The underlying sequence container. Defaults to
 *                   std::vector<T>. Any container with a compatible STL
 *                   sequence interface may be used (e.g., std::deque<T>).
 */
template <typename T, typename Container = std::vector<T>>
    requires std::copyable<T>
struct PodStorage {
    /** @brief The logical element type exposed by the container interface. */
    using ValueType = T;
    /** @brief The type actually stored in the container (identical to T for POD). */
    using StoredType = T;
    /** @brief The underlying sequence container type. */
    using ContainerType = Container;

    /**
     * @brief Performs a deep copy of the container.
     *
     * For POD types, a plain assignment is sufficient.
     *
     * @param src The source container.
     * @param dst The destination container; its previous contents are replaced.
     */
    static void deepCopy(const ContainerType &src, ContainerType &dst) {
        dst = src;
    }
};

/******************************************************************************/
/**
 * @brief Storage policy for polymorphic Geneva objects held via shared_ptr.
 *
 * Wraps a sequence container of std::shared_ptr<T>. Deep copies are performed
 * via the clone()/load() protocol required by the Gemfony common interface.
 *
 * @tparam T         The element type. Must expose the Gemfony common interface
 *                   (detected via has_gemfony_common_interface<T>::value).
 * @tparam Container The underlying sequence container. Defaults to
 *                   std::vector<std::shared_ptr<T>>.
 */
template <typename T, typename Container = std::vector<std::shared_ptr<T>>>
    requires Gem::Common::gemfony_common_interface<T>
struct SharedPtrStorage {
    /** @brief The logical element type exposed by the container interface. */
    using ValueType = T;
    /** @brief The type actually stored in the container (a shared_ptr to T). */
    using StoredType = std::shared_ptr<T>;
    /** @brief The underlying sequence container type. */
    using ContainerType = Container;

    /**
     * @brief Performs a deep copy of the container using the clone()/load() protocol.
     *
     * Each element in @p src is cloned; pre-existing elements in @p dst are
     * reused via load() when sizes match to minimise allocations.
     *
     * @param src The source container.
     * @param dst The destination container; its previous contents are replaced.
     */
    static void deepCopy(const ContainerType &src, ContainerType &dst) {
        Gem::Common::copyCloneableSmartPointerContainer(src, dst);
    }
};

/******************************************************************************/
/**
 * @brief Storage policy: each element is uniquely owned via std::unique_ptr.
 *
 * The unique_ptr counterpart of SharedPtrStorage: same clone()/load() deep-copy protocol, but the
 * stored handles carry sole ownership, so copying/moving/destroying the container costs no atomic
 * reference counting. Deep copy goes through the unique_ptr overload of
 * copyCloneableSmartPointerContainer() (which uses clone()/load()).
 *
 * @tparam T         The element type (must expose the Gemfony common interface).
 * @tparam Container The underlying sequence container; defaults to std::vector<std::unique_ptr<T>>.
 */
template <typename T, typename Container = std::vector<std::unique_ptr<T>>>
    requires Gem::Common::gemfony_common_interface<T>
struct UniquePtrStorage {
    /** @brief The logical element type exposed by the container interface. */
    using ValueType = T;
    /** @brief The type actually stored in the container (a unique_ptr to T). */
    using StoredType = std::unique_ptr<T>;
    /** @brief The underlying sequence container type. */
    using ContainerType = Container;

    /**
     * @brief Performs a deep copy of the container using the clone()/load() protocol.
     *
     * @param src The source container.
     * @param dst The destination container; its previous contents are replaced.
     */
    static void deepCopy(const ContainerType &src, ContainerType &dst) {
        Gem::Common::copyCloneableSmartPointerContainer(src, dst);
    }
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/**
 * @brief A unified generic container base class combining POD and smart-pointer
 *        vector semantics under a single, policy-based interface.
 *
 * GContainerT is an abstract base class (pure-virtual destructor) that wraps
 * a configurable sequence container and provides the full STL sequence-container
 * API together with Geneva-specific extensions such as crossOver(), compare_base(),
 * clone-aware push/insert/resize operations (for SharedPtrStorage), and a
 * type-filtered range view.
 *
 * The underlying storage strategy is chosen at compile time via the
 * @p StoragePolicy parameter:
 *  - PodStorage<T>         — for POD element types (equivalent to GPODVectorT)
 *  - SharedPtrStorage<T>   — for polymorphic Geneva objects (equivalent to
 *                             GPtrVectorT)
 *
 * Container-capability concepts (HasContiguousStorage, HasCapacity,
 * HasRandomAccess, HasFrontInsertion) gate methods that are not universally
 * available, providing clear compile-time diagnostics when an unsupported
 * operation is attempted.
 *
 * @tparam T             The logical element type.
 * @tparam StoragePolicy The storage policy. Must be either PodStorage<T,...>
 *                       or SharedPtrStorage<T,...>.
 */
template <typename T, typename StoragePolicy = PodStorage<T>>
class GContainerT {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /**
     * @brief (De)serialises the underlying data container, against a Boost archive or a GArchive codec.
     *
     * The trailing unsigned int is the class version; it is intentionally unnamed and unused.
     *
     * @tparam Archive The GArchive codec type.
     * @param ar The archive to read from or write to.
     */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named(ar, "data_cnt_", data_cnt_);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    // ------------------------------------------------------------------
    // Type aliases
    // ------------------------------------------------------------------

    /** @brief The logical element type. */
    using ValueType = StoragePolicy::ValueType;
    /** @brief The type physically stored in the container. */
    using StoredType = StoragePolicy::StoredType;
    /** @brief The underlying sequence container type. */
    using ContainerType = StoragePolicy::ContainerType;

    /** @brief Standard STL alias: value_type */
    using value_type = ContainerType::value_type;
    /** @brief Standard STL alias: reference */
    using reference = ContainerType::reference;
    /** @brief Standard STL alias: const_reference */
    using const_reference = ContainerType::const_reference;
    /** @brief Standard STL alias: iterator */
    using iterator = ContainerType::iterator;
    /** @brief Standard STL alias: const_iterator */
    using const_iterator = ContainerType::const_iterator;
    /** @brief Standard STL alias: reverse_iterator */
    using reverse_iterator = ContainerType::reverse_iterator;
    /** @brief Standard STL alias: const_reverse_iterator */
    using const_reverse_iterator = ContainerType::const_reverse_iterator;
    /** @brief Standard STL alias: size_type */
    using size_type = ContainerType::size_type;
    /** @brief Standard STL alias: difference_type */
    using difference_type = ContainerType::difference_type;

    // ------------------------------------------------------------------
    // Constructors, destructor, assignment
    // ------------------------------------------------------------------

    /** @brief Default constructor — creates an empty container. */
    GContainerT() = default;

    /**
     * @brief Constructs the container with @p n_val copies of @p val.
     *
     * Only participates in overload resolution when StoragePolicy is
     * PodStorage (i.e., StoredType == ValueType).
     *
     * @param n_val The number of elements to create.
     * @param val  The value assigned to every element.
     */
    explicit GContainerT(size_type n_val, const StoredType &val)
        requires std::same_as<StoredType, ValueType>
      : data_cnt_(n_val, val) {
    }

    /**
     * @brief Copy constructor — performs a deep copy via the storage policy.
     *
     * For PodStorage a plain container copy suffices. For SharedPtrStorage
     * every element is cloned via the Gemfony clone()/load() protocol.
     *
     * @param cp The source object.
     */
    GContainerT(const GContainerT &cp) {
        StoragePolicy::deepCopy(cp.data_cnt_, data_cnt_);
    }

    /** @brief Move constructor. Leaves the source in a valid but unspecified state. */
    GContainerT(GContainerT &&) noexcept = default;

    /**
     * @brief Pure-virtual destructor — makes GContainerT an abstract base class.
     *
     * @note noexcept: the destructor itself never throws; derived destructors
     *       must uphold the same guarantee.
     */
    virtual ~GContainerT() = 0;

    /**
     * @brief Copy-assignment operator — performs a deep copy.
     *
     * Self-assignment is handled safely.
     *
     * @param cp The source object.
     * @return A reference to this object.
     */
    GContainerT &operator=(const GContainerT &cp) {
        if(this == &cp) {
            return *this;
        }
        StoragePolicy::deepCopy(cp.data_cnt_, data_cnt_);
        return *this;
    }

    /** @brief Move-assignment operator. */
    GContainerT &operator=(GContainerT &&) noexcept = default;

    /**
     * @brief Assignment from the raw underlying container (PodStorage only).
     *
     * Enables `podContainer = someStdVector;` which was supported by GPODVectorT.
     *
     * @param cont The source container.
     * @return A reference to this object.
     */
    GContainerT &operator=(const ContainerType &cont)
        requires std::same_as<StoredType, ValueType>
    {
        data_cnt_ = cont;
        return *this;
    }

    /** @brief Move-assign from raw underlying container (PodStorage only). */
    GContainerT &operator=(ContainerType &&cont)
        requires std::same_as<StoredType, ValueType>
    {
        data_cnt_ = std::move(cont);
        return *this;
    }

    // Deleted comparison operators (use compare_base() instead).
    bool operator==(const GContainerT &) const = delete;
    bool operator!=(const GContainerT &) const = delete;

    // ------------------------------------------------------------------
    // Capacity
    // ------------------------------------------------------------------

    /**
     * @brief Returns the number of elements in the container.
     *
     * @return The element count.
     * @note noexcept: this function never throws.
     */
    [[nodiscard]] size_type size() const noexcept {
        return data_cnt_.size();
    }

    /**
     * @brief Returns true if the container contains no elements.
     *
     * @return true when empty, false otherwise.
     * @note noexcept: this function never throws.
     */
    [[nodiscard]] bool empty() const noexcept {
        return data_cnt_.empty();
    }

    /**
     * @brief Returns the maximum number of elements the container can hold.
     *
     * @return The theoretical maximum size.
     */
    [[nodiscard]] size_type max_size() const {
        return data_cnt_.max_size();
    }

    /**
     * @brief Returns the number of elements the container has allocated space for.
     *
     * @note Only available when ContainerType satisfies HasCapacity
     *       (e.g., std::vector, but not std::deque).
     * @return The capacity.
     */
    [[nodiscard]] size_type capacity() const
        requires HasCapacity<ContainerType>
    {
        return data_cnt_.capacity();
    }

    /**
     * @brief Requests that the container increase its capacity to at least @p amount.
     *
     * @note Only available when ContainerType satisfies HasCapacity.
     * @param amount The minimum desired capacity.
     */
    void reserve(size_type amount)
        requires HasCapacity<ContainerType>
    {
        data_cnt_.reserve(amount);
    }

    /**
     * @brief Requests that unused capacity be released to the system.
     *
     * @note Only available when ContainerType satisfies HasCapacity.
     */
    void shrink_to_fit()
        requires HasCapacity<ContainerType>
    {
        data_cnt_.shrink_to_fit();
    }

    // ------------------------------------------------------------------
    // Element access
    // ------------------------------------------------------------------

    /**
     * @brief Returns a reference to the element at position @p pos.
     *
     * No bounds checking is performed.
     *
     * @param pos The zero-based position.
     * @return A reference to the element.
     */
    reference operator[](size_type pos) {
        return data_cnt_[pos];
    }

    /**
     * @brief Returns a const reference to the element at position @p pos.
     *
     * No bounds checking is performed.
     *
     * @param pos The zero-based position.
     * @return A const reference to the element.
     */
    [[nodiscard]] const_reference operator[](size_type pos) const {
        return data_cnt_[pos];
    }

    /**
     * @brief Returns a reference to the element at position @p pos.
     *
     * @param pos The zero-based position.
     * @return A reference to the element.
     * @throws std::out_of_range when @p pos >= size().
     */
    reference at(size_type pos) {
        return data_cnt_.at(pos);
    }

    /**
     * @brief Returns a const reference to the element at position @p pos.
     *
     * @param pos The zero-based position.
     * @return A const reference to the element.
     * @throws std::out_of_range when @p pos >= size().
     */
    [[nodiscard]] const_reference at(size_type pos) const {
        return data_cnt_.at(pos);
    }

    /**
     * @brief Returns a reference to the first element.
     *
     * @return A reference to the first element.
     * @note Calling this on an empty container is undefined behaviour.
     */
    reference front() {
        return data_cnt_.front();
    }

    /**
     * @brief Returns a const reference to the first element.
     *
     * @return A const reference to the first element.
     * @note Calling this on an empty container is undefined behaviour.
     */
    [[nodiscard]] const_reference front() const {
        return data_cnt_.front();
    }

    /**
     * @brief Returns a reference to the last element.
     *
     * @return A reference to the last element.
     * @note Calling this on an empty container is undefined behaviour.
     */
    reference back() {
        return data_cnt_.back();
    }

    /**
     * @brief Returns a const reference to the last element.
     *
     * @return A const reference to the last element.
     * @note Calling this on an empty container is undefined behaviour.
     */
    [[nodiscard]] const_reference back() const {
        return data_cnt_.back();
    }

    /**
     * @brief Returns a raw pointer to the underlying contiguous element array.
     *
     * @note Only available when ContainerType satisfies HasContiguousStorage
     *       (e.g., std::vector, but not std::deque).
     * @return A pointer to the first element.
     */
    [[nodiscard]] StoredType *data() noexcept
        requires HasContiguousStorage<ContainerType>
    {
        return data_cnt_.data();
    }

    /**
     * @brief Returns a const raw pointer to the underlying contiguous element array.
     *
     * @note Only available when ContainerType satisfies HasContiguousStorage.
     * @return A const pointer to the first element.
     */
    [[nodiscard]] const StoredType *data() const noexcept
        requires HasContiguousStorage<ContainerType>
    {
        return data_cnt_.data();
    }

    // ------------------------------------------------------------------
    // Iterators
    // ------------------------------------------------------------------

    /**
     * @brief Returns an iterator to the first element.
     * @note noexcept: iterator construction never throws.
     */
    iterator begin() noexcept {
        return data_cnt_.begin();
    }

    /**
     * @brief Returns a const iterator to the first element.
     * @note noexcept: iterator construction never throws.
     */
    [[nodiscard]] const_iterator begin() const noexcept {
        return data_cnt_.begin();
    }

    /**
     * @brief Returns a past-the-end iterator.
     * @note noexcept: iterator construction never throws.
     */
    iterator end() noexcept {
        return data_cnt_.end();
    }

    /**
     * @brief Returns a const past-the-end iterator.
     * @note noexcept: iterator construction never throws.
     */
    [[nodiscard]] const_iterator end() const noexcept {
        return data_cnt_.end();
    }

    /**
     * @brief Returns a const iterator to the first element.
     * @note noexcept: iterator construction never throws.
     */
    [[nodiscard]] const_iterator cbegin() const noexcept {
        return data_cnt_.cbegin();
    }

    /**
     * @brief Returns a const past-the-end iterator.
     * @note noexcept: iterator construction never throws.
     */
    [[nodiscard]] const_iterator cend() const noexcept {
        return data_cnt_.cend();
    }

    /**
     * @brief Returns a reverse iterator to the last element.
     * @note noexcept: iterator construction never throws.
     */
    reverse_iterator rbegin() noexcept {
        return data_cnt_.rbegin();
    }

    /**
     * @brief Returns a const reverse iterator to the last element.
     * @note noexcept: iterator construction never throws.
     */
    [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
        return data_cnt_.rbegin();
    }

    /**
     * @brief Returns a reverse past-the-end iterator.
     * @note noexcept: iterator construction never throws.
     */
    reverse_iterator rend() noexcept {
        return data_cnt_.rend();
    }

    /**
     * @brief Returns a const reverse past-the-end iterator.
     * @note noexcept: iterator construction never throws.
     */
    [[nodiscard]] const_reverse_iterator rend() const noexcept {
        return data_cnt_.rend();
    }

    /**
     * @brief Returns a const reverse iterator to the last element.
     * @note noexcept: iterator construction never throws.
     */
    [[nodiscard]] const_reverse_iterator crbegin() const noexcept {
        return data_cnt_.crbegin();
    }

    /**
     * @brief Returns a const reverse past-the-end iterator.
     * @note noexcept: iterator construction never throws.
     */
    [[nodiscard]] const_reverse_iterator crend() const noexcept {
        return data_cnt_.crend();
    }

    // ------------------------------------------------------------------
    // Modifiers — clear / resize
    // ------------------------------------------------------------------

    /**
     * @brief Removes all elements from the container.
     *
     * The capacity (if applicable) is left unchanged.
     */
    void clear() {
        data_cnt_.clear();
    }

    /**
     * @brief Resizes the container to @p amount elements (POD storage only).
     *
     * For POD storage, new elements are zero-initialised.
     * For SharedPtrStorage, use resize_clone/resize_noclone/resize_empty instead.
     *
     * @param amount The desired number of elements.
     */
    void resize(size_type amount)
        requires std::same_as<StoredType, ValueType>
    {
        data_cnt_.resize(amount);
    }

    /**
     * @brief Resizes the container to @p amount elements (SharedPtrStorage).
     *
     * Shrinking is allowed; growing throws because it would create null shared_ptrs.
     * Use resize_clone, resize_noclone, or resize_empty to grow SharedPtrStorage containers.
     *
     * @param amount The desired number of elements.
     * @throws geneva_exception when growing (would insert null shared_ptrs).
     */
    void resize(size_type amount)
        requires(!std::same_as<StoredType, ValueType>)
    {
        if(amount > data_cnt_.size()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GContainerT::resize(): "
                << "Cannot grow a SharedPtrStorage container without a prototype. "
                << "Use resize_clone(), resize_noclone(), or resize_empty() instead." << '\n'
            );
        }
        data_cnt_.resize(amount);
    }

    /**
     * @brief Resizes the container to @p amount elements, initialising new ones
     *        with copies of @p item.
     *
     * @param amount The desired number of elements.
     * @param item   The value used for new elements.
     */
    void resize(size_type amount, const StoredType &item) {
        data_cnt_.resize(amount, item);
    }

    // ------------------------------------------------------------------
    // Modifiers — assign
    // ------------------------------------------------------------------

    /**
     * @brief Replaces the contents with @p count copies of @p value.
     *
     * @param count The number of copies.
     * @param value The value to assign.
     */
    void assign(size_type count, const StoredType &value) {
        data_cnt_.assign(count, value);
    }

    /**
     * @brief Replaces the contents with elements from the range [@p first, @p last).
     *
     * @tparam InputIt An input iterator type.
     * @param first The beginning of the source range.
     * @param last  The past-the-end of the source range.
     */
    template <std::input_iterator InputIt>
    void assign(const InputIt& first, const InputIt& last) {
        data_cnt_.assign(first, last);
    }

    /**
     * @brief Replaces the contents with elements from an initializer list.
     *
     * @param il The initializer list.
     */
    void assign(std::initializer_list<StoredType> il) {
        data_cnt_.assign(il);
    }

    // ------------------------------------------------------------------
    // Modifiers — push / emplace / pop (back)
    // ------------------------------------------------------------------

    /**
     * @brief Appends a copy of @p item to the back of the container.
     *
     * For SharedPtrStorage: performs a no-clone insert (shared ownership).
     * Use push_back_clone() to insert an independent copy.
     *
     * @param item The item to append.
     */
    void push_back(const StoredType &item) {
        data_cnt_.push_back(item);
    }

    /**
     * @brief Appends a moved item to the back of the container.
     *
     * @param item The item to move-append.
     */
    void push_back(StoredType &&item) {
        data_cnt_.push_back(std::move(item));
    }

    /**
     * @brief Transitional overload for unique_ptr storage: appends an INDEPENDENT deep clone of a
     * shared_ptr-held object. This lets code that still builds elements as shared_ptr (e.g. a genome
     * built with push_back(std::make_shared<...>())) keep working while the container owns its elements
     * by unique_ptr. The shared_ptr is only read; the container stores a clone. New code can instead
     * push a std::unique_ptr (moved in) to avoid the clone.
     *
     * @tparam U The (derived-of-ValueType) pointee type of the supplied shared_ptr.
     * @param item A shared_ptr to a (derived-of-ValueType) object to clone into the container.
     */
    template <typename U>
        requires(std::same_as<StoredType, std::unique_ptr<ValueType>> &&
                 std::derived_from<U, ValueType>)
    void push_back(const std::shared_ptr<U> &item) {
        data_cnt_.push_back(item->template clone<ValueType>());
    }

    /**
     * @brief Constructs an element in-place at the back of the container.
     *
     * Only available for PodStorage (emplace_back on shared_ptr would require
     * passing constructor arguments for the pointee, which is not meaningful
     * in this context).
     *
     * @tparam Args Argument types forwarded to the element constructor.
     * @param args Arguments forwarded to the element constructor.
     * @return A reference to the newly constructed element.
     */
    template <typename... Args>
    reference emplace_back(Args &&...args)
        requires std::same_as<StoredType, ValueType>
    {
        return data_cnt_.emplace_back(std::forward<Args>(args)...);
    }

    /**
     * @brief Removes the last element from the container.
     *
     * Calling this on an empty container is undefined behaviour.
     */
    void pop_back() {
        data_cnt_.pop_back();
    }

    // ------------------------------------------------------------------
    // Modifiers — push / emplace / pop (front) — conditional
    // ------------------------------------------------------------------

    /**
     * @brief Prepends a copy of @p item to the front of the container.
     *
     * @note Only available when ContainerType satisfies HasFrontInsertion
     *       (e.g., std::deque, but not std::vector).
     * @param item The item to prepend.
     */
    void push_front(const StoredType &item)
        requires HasFrontInsertion<ContainerType>
    {
        data_cnt_.push_front(item);
    }

    /**
     * @brief Prepends a moved item to the front of the container.
     *
     * @note Only available when ContainerType satisfies HasFrontInsertion.
     * @param item The item to move-prepend.
     */
    void push_front(StoredType &&item)
        requires HasFrontInsertion<ContainerType>
    {
        data_cnt_.push_front(std::move(item));
    }

    /**
     * @brief Constructs an element in-place at the front of the container.
     *
     * @note Only available when ContainerType satisfies HasFrontInsertion
     *       and StoredType == ValueType (PodStorage).
     * @tparam Args Argument types forwarded to the element constructor.
     * @param args Arguments forwarded to the element constructor.
     */
    template <typename... Args>
    void emplace_front(Args &&...args)
        requires HasFrontInsertion<ContainerType> && std::same_as<StoredType, ValueType>
    {
        data_cnt_.emplace_front(std::forward<Args>(args)...);
    }

    /**
     * @brief Removes the first element from the container.
     *
     * @note Only available when ContainerType satisfies HasFrontInsertion.
     *       Calling this on an empty container is undefined behaviour.
     */
    void pop_front()
        requires HasFrontInsertion<ContainerType>
    {
        data_cnt_.pop_front();
    }

    // ------------------------------------------------------------------
    // Modifiers — insert / emplace
    // ------------------------------------------------------------------

    /**
     * @brief Inserts @p item before the element at @p pos.
     *
     * @param pos  An iterator pointing to the insertion position.
     * @param item The item to insert.
     * @return An iterator to the inserted element.
     */
    iterator insert(const_iterator pos, const StoredType &item) {
        return data_cnt_.insert(pos, item);
    }

    /**
     * @brief Inserts a moved @p item before the element at @p pos.
     *
     * @param pos  An iterator pointing to the insertion position.
     * @param item The item to move-insert.
     * @return An iterator to the inserted element.
     */
    iterator insert(const_iterator pos, StoredType &&item) {
        return data_cnt_.insert(pos, std::move(item));
    }

    /**
     * @brief Inserts @p count copies of @p item before @p pos.
     *
     * @param pos   An iterator pointing to the insertion position.
     * @param count The number of copies to insert.
     * @param item  The value to insert.
     * @return An iterator to the first inserted element.
     */
    iterator insert(const_iterator pos, size_type count, const StoredType &item) {
        return data_cnt_.insert(pos, count, item);
    }

    /**
     * @brief Inserts elements from the range [@p first, @p last) before @p pos.
     *
     * @tparam InputIt An input iterator type.
     * @param pos   An iterator pointing to the insertion position.
     * @param first The beginning of the source range.
     * @param last  The past-the-end of the source range.
     * @return An iterator to the first inserted element.
     */
    template <std::input_iterator InputIt>
    iterator insert(const_iterator pos, InputIt first, InputIt last) {
        return data_cnt_.insert(pos, first, last);
    }

    /**
     * @brief Inserts elements from an initializer list before @p pos.
     *
     * @param pos An iterator pointing to the insertion position.
     * @param il  The initializer list.
     * @return An iterator to the first inserted element.
     */
    iterator insert(const_iterator pos, std::initializer_list<StoredType> il) {
        return data_cnt_.insert(pos, il);
    }

    /**
     * @brief Constructs an element in-place before @p pos.
     *
     * Only available for PodStorage (same rationale as emplace_back()).
     *
     * @tparam Args Argument types forwarded to the element constructor.
     * @param pos  An iterator pointing to the insertion position.
     * @param args Arguments forwarded to the element constructor.
     * @return An iterator to the newly constructed element.
     */
    template <typename... Args>
    iterator emplace(const_iterator pos, Args &&...args)
        requires std::same_as<StoredType, ValueType>
    {
        return data_cnt_.emplace(pos, std::forward<Args>(args)...);
    }

    // ------------------------------------------------------------------
    // Modifiers — erase
    // ------------------------------------------------------------------

    /**
     * @brief Erases the element at @p pos.
     *
     * @param pos An iterator to the element to remove.
     * @return An iterator following the removed element.
     */
    iterator erase(const_iterator pos) {
        return data_cnt_.erase(pos);
    }

    /**
     * @brief Erases elements in the range [@p first, @p last).
     *
     * @param first An iterator to the first element to remove.
     * @param last  A past-the-end iterator of the range to remove.
     * @return An iterator following the last removed element.
     */
    iterator erase(const_iterator first, const_iterator last) {
        return data_cnt_.erase(first, last);
    }

    // ------------------------------------------------------------------
    // Search
    // ------------------------------------------------------------------

    /**
     * @brief Counts the number of elements equal to @p item.
     *
     * For PodStorage, equality is checked by value. For SharedPtrStorage the
     * function is overloaded below with the appropriate null-pointer guard.
     *
     * @param item The value to search for.
     * @return The number of matching elements.
     */
    [[nodiscard]] size_type count(const StoredType &item) const
        requires std::same_as<StoredType, ValueType>
    {
        return Gem::Common::narrow<size_type>(
            std::ranges::count(data_cnt_, item)
        );
    }

    /**
     * @brief Returns an iterator to the first element equal to @p item.
     *
     * For PodStorage only; SharedPtrStorage uses its own overload below.
     *
     * @param item The value to search for.
     * @return An iterator to the found element, or end() if not found.
     */
    [[nodiscard]] const_iterator find(const StoredType &item) const
        requires std::same_as<StoredType, ValueType>
    {
        return std::ranges::find(data_cnt_, item);
    }

    // ------------------------------------------------------------------
    // SharedPtrStorage-specific: count / find
    // ------------------------------------------------------------------

    /**
     * @brief Counts elements whose dereferenced value equals @p item.
     *
     * Compares via operator== on the pointed-to objects, not on pointer identity.
     * A null pointer in @p item is not accepted.
     *
     * @tparam ItemType A type derived from or equal to ValueType.
     * @param item A shared_ptr to the reference value.
     * @return The number of matching elements.
     * @throws geneva_exception when @p item is null.
     * @note Only available for SharedPtrStorage.
     */
    template <typename ItemType>
        requires(!std::same_as<StoredType, ValueType> && std::derived_from<ItemType, ValueType>)
    [[nodiscard]] size_type count(const std::shared_ptr<ItemType> &item) const {
        if(not item) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GContainerT::count(): "
                << "Tried to count with an empty smart pointer." << '\n'
            );
        }
        return Gem::Common::narrow<size_type>(std::ranges::count_if(
            data_cnt_,
            [&item](const StoredType &cont_item) -> bool {
                auto cast = std::dynamic_pointer_cast<ItemType>(cont_item);
                return cast && (*item == *cast);
            }
        ));
    }

    /**
     * @brief Returns an iterator to the first element whose dereferenced value
     *        equals @p item.
     *
     * @tparam ItemType A type derived from or equal to ValueType.
     * @param item A shared_ptr to the reference value.
     * @return A const_iterator to the found element, or end() if not found.
     * @throws geneva_exception when @p item is null.
     * @note Only available for SharedPtrStorage.
     */
    template <typename ItemType>
        requires(!std::same_as<StoredType, ValueType> && std::derived_from<ItemType, ValueType>)
    [[nodiscard]] const_iterator find(const std::shared_ptr<ItemType> &item) const {
        if(not item) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GContainerT::find(): "
                << "Tried to find an empty smart pointer." << '\n'
            );
        }
        return std::ranges::find_if(
            data_cnt_,
            [&item](const StoredType &cont_item) -> bool {
                auto cast = std::dynamic_pointer_cast<ItemType>(cont_item);
                return cast && (*item == *cast);
            }
        );
    }

    // ------------------------------------------------------------------
    // Pointer-storage operations (SharedPtrStorage or UniquePtrStorage)
    // ------------------------------------------------------------------

    /**
     * @brief Clones the pointee of a stored handle into a fresh StoredType.
     *
     * Policy-aware deep clone used by the clone-based pointer operations: shared_ptr storage clones
     * via clone() (an independent, separately-owned copy), unique_ptr storage via clone()
     * (sole ownership). Caller must ensure @p p is non-null.
     *
     * @param p A non-null stored handle whose pointee is to be cloned.
     * @return A fresh StoredType handle owning the cloned object.
     * @note Only available for pointer storage (SharedPtrStorage / UniquePtrStorage).
     */
    static StoredType clone_into_stored(const StoredType &p)
        requires (!std::same_as<StoredType, ValueType>)
    {
        if constexpr(std::same_as<StoredType, std::unique_ptr<ValueType>>) {
            return p->template clone<ValueType>();
        }
        else {
            return p->template clone<ValueType>();
        }
    }

    /**
     * @brief Appends a cloned copy of @p item_ptr to the container.
     *
     * Changes to the original object after this call do not affect the stored copy.
     *
     * @param item_ptr A shared_ptr to the object to clone and append.
     * @throws geneva_exception when @p item_ptr is null.
     * @note Only available for SharedPtrStorage.
     */
    void push_back_clone(const StoredType &item_ptr)
        requires (!std::same_as<StoredType, ValueType>)
    {
        if(not item_ptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GContainerT::push_back_clone(): "
                << "Tried to clone an empty smart pointer." << '\n'
            );
        }
        data_cnt_.push_back(clone_into_stored(item_ptr));
    }

    /**
     * @brief Appends @p item_ptr itself (shared ownership) to the container.
     *
     * Changes to the original object after this call will also be visible
     * through the stored pointer.
     *
     * @param item_ptr A shared_ptr to the object to append.
     * @throws geneva_exception when @p item_ptr is null.
     * @note Only available for SharedPtrStorage.
     */
    void push_back_noclone(StoredType item_ptr)
        requires (!std::same_as<StoredType, ValueType>)
    {
        if(not item_ptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GContainerT::push_back_noclone(): "
                << "Tried to insert an empty smart pointer." << '\n'
            );
        }
        data_cnt_.push_back(std::move(item_ptr));
    }

    // ------------------------------------------------------------------
    // SharedPtrStorage-specific: insert variants
    // ------------------------------------------------------------------

    /**
     * @brief Inserts a cloned copy of @p item_ptr before @p pos.
     *
     * @param pos     An iterator pointing to the insertion position.
     * @param item_ptr The object to clone and insert.
     * @return An iterator to the inserted element.
     * @throws geneva_exception when @p item_ptr is null.
     * @note Only available for SharedPtrStorage.
     */
    iterator insert_clone(const_iterator pos, const StoredType &item_ptr)
        requires (!std::same_as<StoredType, ValueType>)
    {
        if(not item_ptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GContainerT::insert_clone(): "
                << "Tried to clone an empty smart pointer." << '\n'
            );
        }
        return data_cnt_.insert(pos, clone_into_stored(item_ptr));
    }

    /**
     * @brief Inserts @p count cloned copies of @p item_ptr before @p pos.
     *
     * @param pos     An iterator pointing to the insertion position.
     * @param count   The number of clones to insert.
     * @param item_ptr The object to clone and insert.
     * @throws geneva_exception when @p item_ptr is null.
     * @note Only available for SharedPtrStorage.
     */
    void insert_clone(const_iterator pos, size_type count, const StoredType &item_ptr)
        requires (!std::same_as<StoredType, ValueType>)
    {
        if(not item_ptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GContainerT::insert_clone(count): "
                << "Tried to clone an empty smart pointer." << '\n'
            );
        }
        if(count == 0) return;

        // Pre-build the clones into a local buffer, then perform a single range
        // insert. This avoids re-inserting at a stale iterPos across iterations
        // (each vector::insert invalidates iterators) and turns an O(count * N)
        // pattern into an O(count + N) operation.
        std::vector<StoredType> clones;
        clones.reserve(count);
        for(std::size_t i = 0; i < count; ++i) {
            clones.push_back(clone_into_stored(item_ptr));
        }
        data_cnt_.insert_range(pos, clones | std::views::as_rvalue);
    }

    /**
     * @brief Inserts @p item_ptr itself (shared ownership) before @p pos.
     *
     * @param pos     An iterator pointing to the insertion position.
     * @param item_ptr The object to insert (not cloned).
     * @return An iterator to the inserted element.
     * @throws geneva_exception when @p item_ptr is null.
     * @note Only available for SharedPtrStorage.
     */
    iterator insert_noclone(const_iterator pos, StoredType item_ptr)
        requires (!std::same_as<StoredType, ValueType>)
    {
        if(not item_ptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GContainerT::insert_noclone(): "
                << "Tried to insert an empty smart pointer." << '\n'
            );
        }
        return data_cnt_.insert(pos, std::move(item_ptr));
    }

    /**
     * @brief Inserts @p count references to @p item_ptr (clones all but the first)
     *        before @p pos.
     *
     * Inserts @p item_ptr itself followed by (count-1) clones. This matches the
     * semantics of GPtrVectorT::insert_noclone(pos, amount, item).
     *
     * @param pos     An iterator pointing to the insertion position.
     * @param count   The number of elements to insert.
     * @param item_ptr The object to insert.
     * @throws geneva_exception when @p item_ptr is null.
     * @note Only available for SharedPtrStorage or UniquePtrStorage (not PodStorage).
     */
    void insert_noclone(const_iterator pos, size_type count, StoredType item_ptr)
        requires (!std::same_as<StoredType, ValueType>)
    {
        if(not item_ptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GContainerT::insert_noclone(count): "
                << "Tried to insert an empty smart pointer." << '\n'
            );
        }
        // Guard against unsigned underflow of `count - 1` below when count == 0.
        if(count == 0) return;

        // Pre-build the inserted range: the original item_ptr first, followed by
        // (count-1) independent clones. The single range insert that follows
        // preserves the existing test contract that the original item_ptr ends up
        // at exactly `pos` (see GTestIndividual1 "Test insert_clone, insert_noclone").
        std::vector<StoredType> to_insert;
        to_insert.reserve(count);
        to_insert.push_back(std::move(item_ptr));
        for(std::size_t i = 0; i < count - 1; ++i) {
            to_insert.push_back(clone_into_stored(to_insert.front()));
        }
        data_cnt_.insert_range(pos, to_insert | std::views::as_rvalue);
    }

    // ------------------------------------------------------------------
    // SharedPtrStorage-specific: resize variants
    // ------------------------------------------------------------------

    /**
     * @brief Resizes the container, filling new slots with clones of @p item_ptr.
     *
     * If the container shrinks, excess elements are removed. If it grows,
     * clones of @p item_ptr are appended.
     *
     * @param amount  The desired number of elements.
     * @param item_ptr The prototype object used to fill new slots.
     * @throws geneva_exception when growing and @p item_ptr is null.
     * @note Only available for SharedPtrStorage.
     */
    void resize_clone(size_type amount, const StoredType &item_ptr)
        requires (!std::same_as<StoredType, ValueType>)
    {
        std::size_t const data_size = data_cnt_.size();
        if(amount < data_size) {
            data_cnt_.resize(amount);
        }
        else if(amount > data_size) {
            if(not item_ptr) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GContainerT::resize_clone(): "
                    << "Tried to clone an empty smart pointer." << '\n'
                );
            }
            // Clone the prototype into an independent local FIRST: item_ptr frequently aliases an element
            // of this very container (e.g. resize_clone(n, data_cnt_[0])), and the reserve()/push_back()
            // below may reallocate the container, which would leave that reference dangling. The local
            // owning copy is immune to the reallocation. (The by-value parameter used to provide this
            // safety implicitly; a unique_ptr cannot be passed by value, hence the explicit local.)
            StoredType const prototype = clone_into_stored(item_ptr);
            data_cnt_.reserve(amount);
            for(std::size_t i = data_size; i < amount; ++i) {
                data_cnt_.push_back(clone_into_stored(prototype));
            }
        }
    }

    /**
     * @brief Resizes the container; the last new slot gets @p item_ptr itself,
     *        all others get clones.
     *
     * @param amount  The desired number of elements.
     * @param item_ptr The prototype object used to fill new slots.
     * @throws geneva_exception when growing and @p item_ptr is null.
     * @note Only available for SharedPtrStorage.
     */
    void resize_noclone(size_type amount, StoredType item_ptr)
        requires (!std::same_as<StoredType, ValueType>)
    {
        std::size_t const data_size = data_cnt_.size();
        if(amount < data_size) {
            data_cnt_.resize(amount);
        }
        else if(amount > data_size) {
            if(not item_ptr) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GContainerT::resize_noclone(): "
                    << "Tried to insert an empty smart pointer." << '\n'
                );
            }
            data_cnt_.reserve(amount);
            for(std::size_t i = data_size; i < amount - 1; ++i) {
                data_cnt_.push_back(clone_into_stored(item_ptr));
            }
            data_cnt_.push_back(std::move(item_ptr));
        }
    }

    /**
     * @brief Resizes the container, filling new slots with empty (null) shared_ptrs.
     *
     * Intended for pre-allocation followed by manual assignment to each slot.
     *
     * @param amount The desired number of elements.
     * @note Only available for SharedPtrStorage.
     */
    void resize_empty(size_type amount)
        requires(!std::same_as<StoredType, ValueType>)
    {
        std::size_t const data_size = data_cnt_.size();
        if(amount < data_size) {
            data_cnt_.resize(amount);
        }
        else {
            for(std::size_t i = data_size; i < amount; ++i) {
                data_cnt_.push_back(StoredType{});
            }
        }
    }

    // ------------------------------------------------------------------
    // SharedPtrStorage-specific: clone_at / attachViewTo / filteredView
    // ------------------------------------------------------------------

    /**
     * @brief Attempts a polymorphic down-cast of a stored pointer to @p DerivedType,
     *        returning the result as a std::shared_ptr<DerivedType> (or null on mismatch).
     *
     * For SharedPtrStorage this is a co-owning std::dynamic_pointer_cast. For UniquePtrStorage
     * the element is owned uniquely by this container, so a NON-OWNING shared_ptr view (no-op
     * deleter) is returned instead -- it must not outlive the container. This keeps the
     * shared_ptr-returning convenience views (attachViewTo / filteredView) usable on both storage
     * policies without transferring ownership out of a unique container.
     *
     * @tparam DerivedType The target type of the polymorphic down-cast.
     * @param ptr The stored handle whose pointee is to be cast.
     * @return A std::shared_ptr<DerivedType> (co-owning for shared storage, non-owning for unique
     *         storage), or a null shared_ptr when the dynamic cast fails.
     */
    template <typename DerivedType>
    [[nodiscard]] static std::shared_ptr<DerivedType> viewCast_(const StoredType &ptr) {
        if constexpr(std::same_as<StoredType, std::shared_ptr<ValueType>>) {
            return std::dynamic_pointer_cast<DerivedType>(ptr);
        }
        else {
            auto *raw = dynamic_cast<DerivedType *>(ptr.get());
            return raw ? std::shared_ptr<DerivedType>(raw, [](DerivedType *) { /* non-owning */ })
                       : std::shared_ptr<DerivedType>();
        }
    }

    /**
     * @brief Clones the element at index @p pos and returns it as a shared_ptr
     *        to @p TargetType.
     *
     * @tparam TargetType The desired result type (must be derived from ValueType).
     * @param pos The zero-based index.
     * @return A shared_ptr<TargetType> owning an independent clone.
     * @throws std::out_of_range when @p pos >= size().
     * @note Only available for SharedPtrStorage.
     */
    template <typename TargetType = ValueType>
    [[nodiscard]] std::shared_ptr<TargetType> clone_at(std::size_t pos) const
        requires(!std::same_as<StoredType, ValueType>)
    {
        return data_cnt_.at(pos)->template clone<TargetType>();
    }

    /**
     * @brief Appends shared_ptrs to all elements that are dynamically castable
     *        to @p DerivedType to the provided target vector.
     *
     * Elements that cannot be cast are silently skipped.
     *
     * @tparam DerivedType The derived type to filter for.
     * @param target A vector that receives the filtered pointers.
     * @note Only available for SharedPtrStorage.
     */
    template <typename DerivedType>
    void attachViewTo(std::vector<std::shared_ptr<DerivedType>> &target)
        requires(!std::same_as<StoredType, ValueType>)
    {
        for(auto &item_ptr : data_cnt_) {
            if(std::shared_ptr<DerivedType> cast = viewCast_<DerivedType>(item_ptr)) {
                target.push_back(std::move(cast));
            }
        }
    }

    /**
     * @brief Returns a lazy C++20 range view that yields only elements
     *        dynamically castable to @p DerivedType.
     *
     * The returned view is a composition of std::views::transform (attempting
     * the dynamic_cast) and std::views::filter (dropping null results).
     * A stateful iterator class was considered but the range-view approach is
     * preferred here because it composes naturally with other std::views
     * adaptors and avoids manual iterator bookkeeping.
     *
     * @tparam DerivedType The derived type to filter for.
     * @return A range of std::shared_ptr<DerivedType> (nulls excluded).
     * @note Only available for SharedPtrStorage.
     */
    template <typename DerivedType>
    [[nodiscard]] auto filteredView()
        requires(!std::same_as<StoredType, ValueType>)
    {
        return data_cnt_ | std::views::transform([](const StoredType &ptr) {
            return viewCast_<DerivedType>(ptr);
        }) | std::views::filter([](const std::shared_ptr<DerivedType> &ptr) {
            return static_cast<bool>(ptr);
        });
    }

    /**
     * @brief Returns a lazy const C++20 range view that yields only elements
     *        dynamically castable to @p DerivedType.
     *
     * @tparam DerivedType The derived type to filter for.
     * @return A const range of std::shared_ptr<DerivedType> (nulls excluded).
     * @note Only available for SharedPtrStorage.
     */
    template <typename DerivedType>
    [[nodiscard]] auto filteredView() const
        requires(!std::same_as<StoredType, ValueType>)
    {
        return data_cnt_ | std::views::transform([](const StoredType &ptr) {
            return viewCast_<DerivedType>(ptr);
        }) | std::views::filter([](const std::shared_ptr<DerivedType> &ptr) {
            return static_cast<bool>(ptr);
        });
    }

    // ------------------------------------------------------------------
    // SharedPtrStorage-specific: getDataCopy (deep clone)
    // ------------------------------------------------------------------

    /**
     * @brief Creates a deep copy of the internal data container into @p cp.
     *
     * For PodStorage, a plain assignment suffices.
     * For SharedPtrStorage, every element is cloned independently.
     *
     * @param cp The destination container. Its previous content is discarded.
     */
    void getDataCopy(ContainerType &cp) const
        requires std::same_as<StoredType, ValueType>
    {
        cp = data_cnt_;
    }

    /**
     * @brief Creates a deep copy of the internal data container into @p cp.
     *
     * Every element is individually cloned so that @p cp has no shared
     * ownership with this container.
     *
     * @param cp The destination container. Its previous content is discarded.
     * @note Only available for SharedPtrStorage.
     */
    void getDataCopy(ContainerType &cp) const
        requires(!std::same_as<StoredType, ValueType>)
    {
        cp.clear();
        for(const auto &item : data_cnt_) {
            cp.push_back(clone_into_stored(item));
        }
    }

    // ------------------------------------------------------------------
    // Geneva-specific operations
    // ------------------------------------------------------------------

    /**
     * @brief Performs a genetic cross-over operation at index @p pos.
     *
     * Elements from index @p pos up to min(size(), cp.size())-1 are swapped
     * between the two containers. If the containers differ in length, the
     * excess elements of the longer container are moved to the shorter one,
     * making both containers change their sizes to reflect the exchange.
     *
     * @param cp  The partner container.
     * @param pos The starting position of the cross-over.
     * @note Only available when ContainerType satisfies HasRandomAccess.
     * @note In debug builds: throws when @p pos >= min(size(), cp.size()).
     */
    void crossOver(GContainerT &cp, const std::size_t &pos)
        requires HasRandomAccess<ContainerType>
    {
        std::size_t const min_size = std::min(this->size(), cp.size());

#ifdef DEBUG
        if(pos >= min_size) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GContainerT::crossOver(): Error!" << '\n'
                << "Invalid position " << pos << " / " << this->size() << " / " << cp.size()
                << '\n'
            );
        }
#endif /* DEBUG */

        for(std::size_t i = pos; i < min_size; ++i) {
            std::swap(data_cnt_[i], cp.data_cnt_[i]);
        }

        if(this->size() > cp.size()) {
            for(std::size_t i = cp.size(); i < this->size(); ++i) {
                cp.data_cnt_.push_back(std::move(data_cnt_[i]));
            }
            data_cnt_.erase(
                data_cnt_.begin() + static_cast<difference_type>(min_size),
                data_cnt_.end()
            );
        }
        else if(cp.size() > this->size()) {
            for(std::size_t i = this->size(); i < cp.size(); ++i) {
                data_cnt_.push_back(std::move(cp.data_cnt_[i]));
            }
            cp.data_cnt_.erase(
                cp.data_cnt_.begin() + static_cast<difference_type>(min_size),
                cp.data_cnt_.end()
            );
        }
    }

    /**
     * @brief Compares this container against @p cp according to expectation @p e.
     *
     * Uses the Geneva GToken / compare_t infrastructure so that results
     * participate in the standard reporting chain.
     *
     * @param cp    The container to compare against.
     * @param e     The expected outcome (EQUALITY, FP_SIMILARITY, INEQUALITY).
     * @param limit The tolerance used for floating-point similarity checks.
     * @throws g_expectation_violation when the comparison result violates @p e.
     */
    virtual void compare_base(
        const GContainerT &cp,
        Gem::Common::expectation e,
        [[maybe_unused]] double limit
    ) const {
        Gem::Common::GToken token("GContainerT", e);
        Gem::Common::compare_t(Gem::Common::getIdentity(this->data_cnt_, cp.data_cnt_, "this->data_cnt_", "cp.data_cnt_"), token);
        token.evaluate();
    }

    // ------------------------------------------------------------------
    // Comparison operator<=> (conditional)
    // ------------------------------------------------------------------

    /**
     * @brief Three-way lexicographic comparison.
     *
     * @note Only available when ContainerType satisfies HasRandomAccess and
     *       StoredType is three-way comparable.
     * @param other The container to compare against.
     * @return The result of std::lexicographical_compare_three_way.
     */
    [[nodiscard]] auto operator<=>(const GContainerT &other) const
        requires HasRandomAccess<ContainerType>
              && std::three_way_comparable<StoredType>
    {
        return std::lexicographical_compare_three_way(
            data_cnt_.begin(),
            data_cnt_.end(),
            other.data_cnt_.begin(),
            other.data_cnt_.end()
        );
    }

protected:
    // ------------------------------------------------------------------
    // Test hooks (protected virtual)
    // ------------------------------------------------------------------

    ContainerType data_cnt_; ///< The underlying sequence container holding the data.
};

/******************************************************************************/
/**
 * @brief Out-of-line definition of the pure-virtual destructor.
 *
 * Clears the data container so that derived-class destructors run cleanly
 * even if the base destructor is invoked directly.
 */
template <typename T, typename StoragePolicy>
inline GContainerT<T, StoragePolicy>::~GContainerT() {
    data_cnt_.clear();
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/**
 * @brief Convenience alias: a GContainerT for POD types backed by @p Container.
 *
 * Equivalent to the old GPODVectorT but with an exchangeable backend.
 *
 * @tparam T         The POD element type.
 * @tparam Container The underlying sequence container. Defaults to std::vector<T>.
 */
template <typename T, typename Container = std::vector<T>>
using GPodContainerT = GContainerT<T, PodStorage<T, Container>>;

/**
 * @brief Convenience alias: a GContainerT for polymorphic Geneva objects
 *        held via shared_ptr, backed by @p Container.
 *
 * Equivalent to the old GPtrVectorT but with an exchangeable backend.
 *
 * @tparam T         The base element type (must expose the Gemfony common interface).
 * @tparam Container The underlying sequence container.
 *                   Defaults to std::vector<std::shared_ptr<T>>.
 */
template <typename T, typename Container = std::vector<std::shared_ptr<T>>>
using GPtrContainerT = GContainerT<T, SharedPtrStorage<T, Container>>;

/******************************************************************************/
/**
 * @brief A container of uniquely-owned (std::unique_ptr) cloneable elements.
 *
 * The unique_ptr counterpart of GPtrContainerT: same policy-based interface, but elements are
 * sole-owned, so copy/move/destroy incur no atomic reference counting. Deep copy clones via
 * clone()/load(). Boost serialises std::unique_ptr, so serialisation works unchanged.
 *
 * @tparam T         The base element type (must expose the Gemfony common interface).
 * @tparam Container The underlying sequence container.
 *                   Defaults to std::vector<std::unique_ptr<T>>.
 */
template <typename T, typename Container = std::vector<std::unique_ptr<T>>>
using GUniquePtrContainerT = GContainerT<T, UniquePtrStorage<T, Container>>;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Common */


