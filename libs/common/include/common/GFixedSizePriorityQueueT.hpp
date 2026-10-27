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

// Standard headers go here
#include <algorithm>
#include <concepts>
#include <iterator>
#include <memory>
#include <unordered_set>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GContainerT.hpp" // PodStorage / SharedPtrStorage / UniquePtrStorage
#include "common/GReflectiveInterfaceT.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include <common/GExpectationChecksT.hpp>

namespace Gem::Common {
// Some default values
constexpr std::size_t GFSPQ_DEF_MAX_SIZE = 10;
constexpr auto GFSPQ_DEF_SORT_ORDER = Gem::Common::sortOrder::LOWERISBETTER;

/******************************************************************************/
/**
 * @brief A fixed-size priority queue holding the best items seen so far.
 *
 * The queue keeps at most `max_size_` items (0 means unlimited), ordered by the
 * priority the derived class assigns through evaluation(). Insertion, ordering
 * and eviction of the worst surplus item happen in one step: this is a bounded
 * "keep the N best" archive, not a FIFO. It is a plain container — it performs no
 * locking of its own, because the object that owns an archive is what defines the
 * span over which a read must be consistent (a `best()` plus the clone taken from
 * it is one such span, and no per-operation lock could make it atomic).
 *
 * @par Storage policies
 * What the queue physically stores is chosen by the @p StoragePolicy template
 * argument, reusing the very policies GContainerT is built on, so container and
 * archive can never drift apart in their notion of "deep copy":
 *
 *  - `PodStorage<T, std::vector<T>>` — items are held **by value**.
 *  - `SharedPtrStorage<T, std::vector<std::shared_ptr<T>>>` — items are held by
 *    shared handle (the default, so `GFixedSizePriorityQueueT<T>` keeps its meaning).
 *  - `UniquePtrStorage<T, std::vector<std::unique_ptr<T>>>` — items are **solely
 *    owned** by the queue.
 *
 * The convenience aliases GPodFixedSizePriorityQueueT / GPtrFixedSizePriorityQueueT /
 * GUniquePtrFixedSizePriorityQueueT below name the three instantiations.
 *
 * @par Insertion: two semantics, spelled identically for every holder
 * There is deliberately no `do_clone` flag. A boolean cannot express what a
 * `unique_ptr` holder needs (there is no "share it" option), and a flag hides the
 * ownership decision at the call site. Instead:
 *
 *  - `add(StoredType &&)` — the queue **takes the handle over**. For unique storage
 *    that is a transfer of sole ownership; for shared storage the queue becomes a
 *    co-owner; for value storage the value is moved in.
 *  - `addClone(StoredType const &)` — the queue stores an **independent copy** and
 *    the caller keeps its handle untouched (clone() for the pointer policies, a
 *    value copy for POD).
 *
 * A caller that wants the old "share the handle, do not clone" behaviour with shared
 * storage writes `q.add(StoredType{ptr})`, which says "here is a second owner" in
 * the code rather than in a flag.
 *
 * @par Validity and duplicates
 * Every candidate passes isValid() first; an item that fails is skipped. The
 * default rejects an empty handle (and accepts every value for POD storage), and a
 * derived class narrows it further (e.g. "only already-evaluated individuals").
 * For the pointer policies the queue additionally drops items that are the *same
 * object* inserted twice; value storage has no object identity, so two equal values
 * are two legitimate entries there.
 *
 * @tparam T The logical type of the work items stored in the queue
 * @tparam StoragePolicy How an item is physically held (see above)
 */
template <typename T, typename StoragePolicy = SharedPtrStorage<T, std::vector<std::shared_ptr<T>>>>
class GFixedSizePriorityQueueT
  : public GReflectiveInterfaceBaseT<
        GFixedSizePriorityQueueT<T, StoragePolicy>,
        GCommonInterfaceT<GFixedSizePriorityQueueT<T, StoragePolicy>>> {
    ///////////////////////////////////////////////////////////////////////
    // GReflectiveInterfaceAccess lets the mixin reach localMembers_(); this abstract root is
    // never reconstructed on load.
    friend struct GReflectiveInterfaceAccess;

public:
    /** @brief The logical element type. */
    using ValueType = typename StoragePolicy::ValueType;
    /** @brief The type physically stored (a value, a shared_ptr or a unique_ptr). */
    using StoredType = typename StoragePolicy::StoredType;
    /** @brief The underlying sequence container. */
    using ContainerType = typename StoragePolicy::ContainerType;
    /** @brief Read-only traversal of the stored handles, best first. */
    using const_iterator = typename ContainerType::const_iterator;

    /** @brief True when items are held by value (PodStorage), false for the pointer policies. */
    static constexpr bool holds_values = std::same_as<StoredType, ValueType>;

private:
    /***************************************************************************/
    /**
     * @brief Single declaration of this class'es local data members
     *
     * The data container is described by the kind its storage policy calls for: a
     * pointer container is deep-cloned element-wise on load, a value container is
     * plain-assigned. Both are serialized and value-compared.
     *
     * @return A tuple of named member references driving the GReflectiveInterfaceBaseT-generated
     *         serialize(), load_() and compare_()
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        if constexpr(holds_values) {
            return std::make_tuple(
                Gem::Common::make_member("max_size_", self.max_size_),
                Gem::Common::make_member("sort_order_", self.sort_order_),
                Gem::Common::make_member("data_cnt_", self.data_cnt_)
            );
        }
        else {
            return std::make_tuple(
                Gem::Common::make_member("max_size_", self.max_size_),
                Gem::Common::make_member("sort_order_", self.sort_order_),
                Gem::Common::make_cloneable_container_member("data_cnt_", self.data_cnt_)
            );
        }
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GFixedSizePriorityQueueT<T>";

    /***************************************************************************/
    /**
     * @brief Initialization with the maximum number of entries
     *
     * @param max_size The maximum size of the queue (0 means unlimited)
     */
    explicit GFixedSizePriorityQueueT(const std::size_t &max_size)
      : max_size_(max_size) {
        /* nothing */
    }

    /***************************************************************************/
    /**
     * @brief Initialization with the maximum number of entries and the information,
     * whether higher or lower evaluations are better.
     *
     * @param max_size The maximum size of the queue (0 means unlimited)
     * @param sort_order Indicates whether the queue should minimize or maximize
     */
    GFixedSizePriorityQueueT(const std::size_t &max_size, const sortOrder &sort_order)
      : max_size_(max_size)
      , sort_order_(sort_order) {
        /* nothing */
    }

    /***************************************************************************/
    /**
     * @brief The copy constructor
     *
     * The stored items are deep-copied through the storage policy, so the copy shares
     * nothing with @p cp -- also for the shared_ptr policy, whose entries are cloned
     * rather than co-owned.
     *
     * @param cp A constant reference to another object of the same type that is copied
     */
    GFixedSizePriorityQueueT(GFixedSizePriorityQueueT const &cp)
      : max_size_(cp.max_size_)
      , sort_order_(cp.sort_order_) {
        StoragePolicy::deepCopy(cp.data_cnt_, data_cnt_);
    }

    /***************************************************************************/
    /**
     * @brief The move constructor
     *
     * @param cp An rvalue reference to another object of the same type whose content is moved in
     *           (and which is reset to default values afterwards)
     */
    GFixedSizePriorityQueueT(GFixedSizePriorityQueueT &&cp) noexcept
      : max_size_(cp.max_size_)
      , sort_order_(cp.sort_order_)
      , data_cnt_(std::move(cp.data_cnt_)) {
        // Reset cp to default values
        cp.max_size_ = GFSPQ_DEF_MAX_SIZE;
        cp.sort_order_ = GFSPQ_DEF_SORT_ORDER;
        cp.data_cnt_.clear();
    }

    /***************************************************************************/
    // Defaulted functions

    GFixedSizePriorityQueueT() = default;
    ~GFixedSizePriorityQueueT() override = default;

    /***************************************************************************/
    /**
     * @brief Copy assignment operator
     *
     * @param cp A constant reference to another object of the same type that is copied
     * @return A reference to this object
     */
    GFixedSizePriorityQueueT &operator=(GFixedSizePriorityQueueT const &cp) {
        if(this == &cp) {
            return *this;
        }
        max_size_ = cp.max_size_;
        sort_order_ = cp.sort_order_;

        StoragePolicy::deepCopy(cp.data_cnt_, data_cnt_);

        return *this;
    }

    /***************************************************************************/
    /**
     * @brief Move assignment operator
     *
     * @param cp An rvalue reference to another object of the same type whose content is moved in
     *           (and which is reset to default values afterwards)
     * @return A reference to this object
     */
    GFixedSizePriorityQueueT &operator=(GFixedSizePriorityQueueT &&cp) noexcept {
        // Move data over, then set remote object to default values
        max_size_ = cp.max_size_;
        cp.max_size_ = GFSPQ_DEF_MAX_SIZE;

        sort_order_ = cp.sort_order_;
        cp.sort_order_ = GFSPQ_DEF_SORT_ORDER;

        data_cnt_ = std::move(cp.data_cnt_);
        cp.data_cnt_.clear();

        return *this;
    }

    /***************************************************************************/
    // Insertion. Two semantics -- take the handle over, or store an independent
    // copy -- available in a single-item and a bulk form, identical for all three
    // storage policies.

    /**
     * @brief Adds an item to the queue, taking the handle over.
     *
     * For unique storage this transfers sole ownership; for shared storage the queue
     * becomes a co-owner of the pointee; for value storage the value is moved in. The
     * item is dropped when isValid() rejects it, or when the queue is full and it is
     * not strictly better than the current worst entry.
     *
     * @param item The item whose handle the queue takes over
     */
    void add(StoredType &&item) {
        if(isValid(item)) {
            insert_(std::move(item));
        }
        finalize_();
    }

    /**
     * @brief Adds an independent copy of an item to the queue.
     *
     * The caller keeps its own handle. Pointer policies deep-clone the pointee via
     * clone(); value storage copies the value.
     *
     * @param item The item to be copied into the queue
     */
    void addClone(StoredType const &item) {
        if(isValid(item)) {
            insert_(cloneStored_(item));
        }
        finalize_();
    }

    /**
     * @brief Adds the items of a range to the queue, taking every handle over.
     *
     * @param begin An iterator to the first item of the range to be moved in
     * @param end An iterator one past the last item of the range to be moved in
     * @param replace If true, the queue is emptied before the new items are added
     */
    template <std::input_iterator It>
        requires std::same_as<std::iter_value_t<It>, StoredType>
    void add(It begin, It end, bool replace) {
        prepareBulk_(replace);
        std::size_t accepted = 0;
        std::size_t offered = 0;
        for(auto it = begin; it != end; ++it) {
            ++offered;
            if(isValid(*it)) {
                insert_(std::move(*it));
                ++accepted;
            }
        }
        checkBulkYield_(offered, accepted, "add(range)");
        finalize_();
    }

    /**
     * @brief Adds the items of a container to the queue, taking every handle over.
     *
     * @param items_cnt The container whose items are moved into the queue
     * @param replace If true, the queue is emptied before the new items are added
     */
    void add(ContainerType &&items_cnt, bool replace) {
        this->add(items_cnt.begin(), items_cnt.end(), replace);
    }

    /**
     * @brief Adds independent copies of the items of a range to the queue.
     *
     * @param begin An iterator to the first item of the range to be copied in
     * @param end An iterator one past the last item of the range to be copied in
     * @param replace If true, the queue is emptied before the new items are added
     */
    template <std::input_iterator It>
        requires std::same_as<std::iter_value_t<It>, StoredType>
    void addClone(It begin, It end, bool replace) {
        prepareBulk_(replace);
        std::size_t accepted = 0;
        std::size_t offered = 0;
        for(auto it = begin; it != end; ++it) {
            ++offered;
            if(isValid(*it)) {
                insert_(cloneStored_(*it));
                ++accepted;
            }
        }
        checkBulkYield_(offered, accepted, "addClone(range)");
        finalize_();
    }

    /**
     * @brief Adds independent copies of the items of a container to the queue.
     *
     * @param items_cnt The container whose items are copied into the queue
     * @param replace If true, the queue is emptied before the new items are added
     */
    void addClone(ContainerType const &items_cnt, bool replace) {
        this->addClone(items_cnt.begin(), items_cnt.end(), replace);
    }

    /***************************************************************************/
    /**
     * @brief Gives non-owning access to the best item (the front of the queue)
     *
     * The reference stays valid until the queue is next modified. Ownership is not
     * transferred -- a caller that wants to keep the item takes a copy of the handle
     * (shared storage) or clones the pointee.
     *
     * @return A constant reference to the stored handle of the best item
     * @throw geneva_exception if the queue is empty
     */
    [[nodiscard]] StoredType const &best() const {
        if(data_cnt_.empty()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFixedSizePriorityQueueT<T>::best(): Error!" << '\n'
                << "Priority queue is empty." << '\n'
            );
        }
        return data_cnt_.front();
    }

    /***************************************************************************/
    /**
     * @brief Gives non-owning access to the worst item (the back of the queue)
     *
     * @return A constant reference to the stored handle of the worst item
     * @throw geneva_exception if the queue is empty
     */
    [[nodiscard]] StoredType const &worst() const {
        if(data_cnt_.empty()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFixedSizePriorityQueueT<T>::worst(): Error!" << '\n'
                << "Priority queue is empty." << '\n'
            );
        }
        return data_cnt_.back();
    }

    /***************************************************************************/
    /**
     * @brief Gives non-owning access to the item at a given RANK in the ordered sequence
     *
     * Rank 0 is the best item and rank size()-1 the worst, so this is the general form of which
     * best() and worst() are the two ends. It exists because a ranked archive is read by rank --
     * a rank-weighted roulette selection over the elite set names a rank and then needs the
     * item behind it -- and iterating to it by hand at every such site would state the queue's
     * "best first" ordering once per caller.
     *
     * Read-only by construction: handing out a mutable item would let a caller change the
     * evaluation the ordering rests on and silently break the queue's invariant.
     *
     * @param rank The zero-based position in the ordered sequence (0 == best)
     * @return A constant reference to the stored handle at that rank
     * @throw geneva_exception if rank is out of range
     */
    [[nodiscard]] StoredType const &at(std::size_t rank) const {
        if(rank >= data_cnt_.size()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFixedSizePriorityQueueT<T>::at(): Error!" << '\n'
                << "Requested rank " << rank << " but the queue holds " << data_cnt_.size()
                << " items." << '\n'
            );
        }
        return data_cnt_[rank];
    }

    /***************************************************************************/
    /**
     * @brief Allows to set the priority mode. A value of "HIGHERISBETTER" means that higher
     * values are considered better, "LOWERISBETTER" means that lower values are
     * considered to be better.
     *
     * @param sort_order The new sort order to be used for prioritizing items
     */
    void setSortOrder(const sortOrder &sort_order) {
        sort_order_ = sort_order;
    }

    /***************************************************************************/
    /**
     * @brief Allows to retrieve the current value of sort_order_
     *
     * @return The currently configured sort order
     */
    [[nodiscard]] sortOrder getSortOrder() const {
        return sort_order_;
    }

    /***************************************************************************/
    /**
     * @brief Removes the best item from the queue and hands it out
     *
     * The queue gives up its hold on the item: unique storage transfers sole
     * ownership to the caller, shared storage hands over its co-ownership, value
     * storage moves the value out.
     *
     * @return The stored handle of the (now removed) best item
     * @throw geneva_exception if the queue is empty
     */
    StoredType pop() {
        if(data_cnt_.empty()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFixedSizePriorityQueueT<T>::pop(): Error!" << '\n'
                << "Priority queue is empty." << '\n'
            );
        }
        StoredType item = std::move(data_cnt_.front());
        data_cnt_.erase(data_cnt_.begin());
        return item;
    }

    /***************************************************************************/
    /**
     * @brief Returns independent copies of the stored items, best first
     *
     * Copies rather than handles, so the result is usable for every storage policy
     * (a container of uniquely-owned items cannot be copied out any other way) and so
     * the caller can never accidentally alias the archive's own entries.
     *
     * @return A std::vector of deep copies of the stored items, in priority order
     */
    [[nodiscard]] std::vector<StoredType> cloneToVector() const {
        std::vector<StoredType> result;
        result.reserve(data_cnt_.size());
        for(auto const &item : data_cnt_) {
            result.push_back(cloneStored_(item));
        }
        return result;
    }

    /***************************************************************************/
    /** @brief Read-only iteration over the stored handles, best first. @return An iterator to the best item */
    [[nodiscard]] const_iterator begin() const noexcept {
        return data_cnt_.begin();
    }
    /** @brief Read-only iteration over the stored handles. @return An iterator one past the worst item */
    [[nodiscard]] const_iterator end() const noexcept {
        return data_cnt_.end();
    }
    /** @brief Read-only iteration over the stored handles, best first. @return An iterator to the best item */
    [[nodiscard]] const_iterator cbegin() const noexcept {
        return data_cnt_.cbegin();
    }
    /** @brief Read-only iteration over the stored handles. @return An iterator one past the worst item */
    [[nodiscard]] const_iterator cend() const noexcept {
        return data_cnt_.cend();
    }

    /***************************************************************************/
    /**
     * @brief Returns the current size of the queue
     *
     * @return The number of items currently held in the queue
     */
    [[nodiscard]] std::size_t size() const {
        return data_cnt_.size();
    }

    /***************************************************************************/
    /**
     * @brief Checks whether the data is empty
     *
     * @return A boolean indicating whether the queue holds no items
     */
    [[nodiscard]] bool empty() const {
        return data_cnt_.empty();
    }

    /***************************************************************************/
    /**
     * @brief Allows to clear the queue, removing all stored items
     */
    void clear() {
        data_cnt_.clear();
    }

    /***************************************************************************/
    /**
     * @brief Sets the maximum size of the priority queue
     *
     * If the queue currently holds more items than max_size, surplus
     * (worst) items are dropped.
     *
     * @param max_size The new maximum number of items the queue may hold (0 means unlimited)
     */
    void setMaxSize(std::size_t max_size) {
        // Make sure the current size of data_cnt_ complies with max_size
        if(max_size && data_cnt_.size() > max_size) {
            data_cnt_.resize(max_size);
        }

        max_size_ = max_size;
    }

    /***************************************************************************/
    /**
     * @brief Retrieves the maximum size of the priority queue
     *
     * @return The maximum number of items the queue may hold (0 means unlimited)
     */
    [[nodiscard]] std::size_t getMaxSize() const {
        return max_size_;
    }

    /***************************************************************************/
    /**
     * @brief Prints the evaluations of all stored items to std::cout. This is for debugging purposes.
     */
    void printEvaluations() const {
        std::cout << "==================== printEvaluations =====================" << '\n';
        for(auto const &item : data_cnt_) {
            std::cout << this->evaluation(item) << '\n';
        }
    }

protected:
    // load_(), compare_(), name_() and clone_() are generated by the GReflectiveInterfaceBaseT
    // base (clone_ stays pure -- this is the abstract root) from class_name and localMembers_().

    /***************************************************************************/
    /**
     * @brief Checks whether the evaluation of new_item is better than that of old_item
     *
     * @param new_item The candidate item whose evaluation is being judged
     * @param old_item The reference item to compare against
     * @return A boolean indicating whether new_item is strictly better than old_item
     * @note Only present for the pointer policies; with value storage a stored item IS its own
     *       evaluation type, so this overload would collide with the plain-value one below.
     */
    [[nodiscard]] bool isBetter(StoredType const &new_item, StoredType const &old_item) const
        requires(not holds_values)
    {
        return this->isBetter(this->evaluation(new_item), this->evaluation(old_item));
    }

    /***************************************************************************/
    /**
     * @brief Checks whether the evaluation of new_item is better than the value old_item_val
     *
     * @param new_item The candidate item whose evaluation is being judged
     * @param old_item_val The reference evaluation value to compare against
     * @return A boolean indicating whether new_item is strictly better than old_item_val
     * @note Pointer policies only -- see the note above.
     */
    [[nodiscard]] bool isBetter(StoredType const &new_item, double old_item_val) const
        requires(not holds_values)
    {
        return this->isBetter(this->evaluation(new_item), old_item_val);
    }

    /***************************************************************************/
    /**
     * @brief Checks whether the value new_item_val is better than the evaluation of old_item
     *
     * @param new_item_val The candidate evaluation value being judged
     * @param old_item The reference item to compare against
     * @return A boolean indicating whether new_item_val is strictly better than old_item
     * @note Pointer policies only -- see the note above.
     */
    [[nodiscard]] bool isBetter(double new_item_val, StoredType const &old_item) const
        requires(not holds_values)
    {
        return this->isBetter(new_item_val, this->evaluation(old_item));
    }

    /***************************************************************************/
    /**
     * @brief Checks whether value new_item_val is *strictly* better than value old_item_val.
     *
     * Both branches use a strict comparison (`<` / `>`) so the ordering is
     * symmetric: equal values are never reported as "better", regardless of
     * sort direction. The previous LOWERISBETTER branch used `<=`, which
     * treated equality as "better" only for one direction — making
     * incumbent-replacement behave differently for the two sort orders.
     *
     * @param new_item_val The candidate evaluation value being judged
     * @param old_item_val The reference evaluation value to compare against
     * @return A boolean that is true if new_item_val is strictly better than old_item_val
     *         under the current sort order
     */
    [[nodiscard]] bool isBetter(double new_item_val, double old_item_val) const {
        return (sort_order_ == sortOrder::LOWERISBETTER) ? (new_item_val < old_item_val)
                                                         : (new_item_val > old_item_val);
    }

    /***************************************************************************/
    /**
     * @brief Decides whether an item may enter the queue at all
     *
     * This is the queue's single admission gate: every insertion path consults it, so
     * a derived class states its admission rule exactly once. The default accepts any
     * value (POD storage) and any non-empty handle (pointer storage); a derived class
     * narrows it further and chains to this implementation for the emptiness check.
     *
     * @param item The stored handle to be checked
     * @return A boolean indicating whether the item may be added
     */
    [[nodiscard]] virtual bool isValid(StoredType const &item) const {
        if constexpr(holds_values) {
            return true;
        }
        else {
            return static_cast<bool>(item);
        }
    }

    /***************************************************************************/
    /**
     * @brief Evaluates a single work item, so that it can be sorted
     *
     * @param item The stored handle of the work item to be evaluated
     * @return The evaluation (priority) value associated with the item
     */
    [[nodiscard]] virtual double evaluation(StoredType const &item) const = 0;

    /***************************************************************************/

    std::size_t max_size_{GFSPQ_DEF_MAX_SIZE}; ///< The maximum number of work-items
    sortOrder sort_order_{sortOrder::LOWERISBETTER};
    ///< Indicates whether higher evaluations of items indicate a higher priority

    ContainerType data_cnt_{}; ///< Holds the actual data. Empty at the beginning.

private:
    /***************************************************************************/
    /**
     * @brief Produces an independent copy of a stored handle
     *
     * @param item The handle whose item is to be copied
     * @return A fresh handle owning a deep copy (a plain value copy for POD storage)
     */
    static StoredType cloneStored_(StoredType const &item) {
        if constexpr(holds_values) {
            return item;
        }
        else {
            return item->template clone<ValueType>();
        }
    }

    /***************************************************************************/
    /**
     * @brief Appends an already-admitted item if it can still improve the queue
     *
     * The candidate is kept when the queue is unlimited, not yet full, or the item is
     * strictly better than the current worst entry. Ordering and trimming happen once
     * per add() call in finalize_(), not per item.
     *
     * @param item The handle to be stored (moved in)
     */
    void insert_(StoredType &&item) {
        if(0 == max_size_ || data_cnt_.size() < max_size_ ||
           isBetter(this->evaluation(item), worst_admitted_)) {
            data_cnt_.push_back(std::move(item));
        }
    }

    /***************************************************************************/
    /**
     * @brief Sets up a bulk insertion: optionally clears the queue and records the
     * evaluation every candidate of this batch has to beat.
     *
     * @param replace If true, the queue's existing content is dropped first
     */
    void prepareBulk_(bool replace) {
        if(replace) {
            data_cnt_.clear();
        }
        refreshWorstAdmitted_();
    }

    /***************************************************************************/
    /**
     * @brief Recomputes the bar a candidate has to clear to displace the incumbents
     *
     * An empty queue admits everything (the worst case of the sort order); otherwise
     * the current worst entry sets the bar. Held as a member so a bulk insertion
     * compares against one stable value rather than against a queue that is changing
     * underneath it.
     */
    void refreshWorstAdmitted_() {
        worst_admitted_ = data_cnt_.empty() ? Gem::Common::getWorstCase<double>(sort_order_)
                                            : this->evaluation(data_cnt_.back());
    }

    /***************************************************************************/
    /**
     * @brief In DEBUG builds, reports a bulk insertion that admitted nothing
     *
     * A caller that hands over a non-empty batch of which not one item is admissible
     * has a problem of its own (typically a population handed to the archive before it
     * was evaluated). Release builds simply skip the items.
     *
     * @param offered The number of items the caller supplied
     * @param accepted The number of items that passed isValid()
     * @param where The name of the calling overload, for the error message
     */
    void checkBulkYield_(
        [[maybe_unused]] std::size_t offered,
        [[maybe_unused]] std::size_t accepted,
        [[maybe_unused]] char const *where
    ) const {
#ifdef DEBUG
        if(offered > 0 && 0 == accepted) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFixedSizePriorityQueueT<T>::" << where << ": Error!" << '\n'
                << "None of the " << offered << " supplied items was admissible." << '\n'
            );
        }
#endif /* DEBUG */
    }

    /***************************************************************************/
    /**
     * @brief Restores the queue's invariants after an insertion: no duplicates,
     * sorted best-first, and no more than max_size_ entries.
     */
    void finalize_() {
        removeDuplicates_();

        // Sort the data according to the evaluation, so the worst items end up at the back
        std::ranges::sort(data_cnt_, [this](StoredType const &x, StoredType const &y) -> bool {
            return this->isBetter(this->evaluation(x), this->evaluation(y));
        });

        // Remove surplus work items. As the worst items are at the end of the queue, they
        // are the ones dropped. Only has an effect if max_size_ is != 0.
        if(max_size_ && data_cnt_.size() > max_size_) {
            data_cnt_.resize(max_size_);
        }

        refreshWorstAdmitted_();
    }

    /***************************************************************************/
    /**
     * @brief Drops entries that refer to the same object as an earlier entry
     *
     * Only meaningful for the pointer policies: it catches the same object being handed
     * to the queue twice. Values have no object identity, so for POD storage two equal
     * values are two legitimate entries and nothing is removed.
     */
    void removeDuplicates_() {
        if constexpr(not holds_values) {
            // Stores addresses we have already encountered
            std::unordered_set<ValueType const *> known_addresses;

            auto it = data_cnt_.begin();
            while(it != data_cnt_.end()) {
                if(ValueType const *raw_ptr = it->get(); known_addresses.contains(raw_ptr)) {
                    // Found a duplicate -- remove it. "it" will then point to the next element.
                    it = data_cnt_.erase(it);
                }
                else {
                    known_addresses.insert(raw_ptr);
                    ++it;
                }
            }
        }
    }

    /***************************************************************************/

    /** @brief The evaluation a candidate must beat once the queue is full; a transient
     *  scratch value derived from data_cnt_, so it is neither serialized nor compared. */
    double worst_admitted_{Gem::Common::getWorstCase<double>(GFSPQ_DEF_SORT_ORDER)};
};

/******************************************************************************/
/**
 * @brief Convenience alias: a fixed-size priority queue holding items by value.
 *
 * @tparam T The (POD) item type
 * @tparam Container The underlying sequence container
 */
template <typename T, typename Container = std::vector<T>>
using GPodFixedSizePriorityQueueT = GFixedSizePriorityQueueT<T, PodStorage<T, Container>>;

/**
 * @brief Convenience alias: a fixed-size priority queue holding items by shared handle.
 *
 * @tparam T The item type (must expose the Gemfony common interface)
 * @tparam Container The underlying sequence container
 */
template <typename T, typename Container = std::vector<std::shared_ptr<T>>>
using GPtrFixedSizePriorityQueueT = GFixedSizePriorityQueueT<T, SharedPtrStorage<T, Container>>;

/**
 * @brief Convenience alias: a fixed-size priority queue that solely owns its items.
 *
 * @tparam T The item type (must expose the Gemfony common interface)
 * @tparam Container The underlying sequence container
 */
template <typename T, typename Container = std::vector<std::unique_ptr<T>>>
using GUniquePtrFixedSizePriorityQueueT =
    GFixedSizePriorityQueueT<T, UniquePtrStorage<T, Container>>;

/******************************************************************************/
} /* namespace Gem::Common */
