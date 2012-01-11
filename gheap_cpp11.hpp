#ifndef GHEAP_H
#define GHEAP_H

// Generalized heap implementation for C++11.
// The implementation requires the following C++11 features:
// - <cstdint> must contain SIZE_MAX definition.
// - std::move() support. The implementation relies on move constructors
//   and move assignment operators, so define them for classes with expensive
//   copy constructors and copy assignment operators.
// See http://en.wikipedia.org/wiki/C%2B%2B11 for details.
//
// Use gheap_cpp03.hpp instead if your compiler doesn't support these features.
// The implementation for C++11 is usually faster than the implementation
// for C++03.
//
// Don't forget passing -DNDEBUG option to the compiler when creating optimized
// builds. This significantly speeds up gheap code by removing debug assertions.
//
// Author: Aliaksandr Valialkin <valyala@gmail.com>.

#include <cassert>     // for assert
#include <cstddef>     // for size_t
#include <cstdint>     // for SIZE_MAX
#include <iterator>    // for std::iterator_traits
#include <utility>     // for std::move(), std::swap(), std::pair

template <size_t Fanout, size_t PageChunks = 1>
class gheap
{
public:

  // Returns parent index for the given child index.
  // Child index must be greater than 0.
  // Returns 0 if the parent is root.
  static size_t get_parent_index(size_t u)
  {
    assert(u > 0);

    --u;
    if (PageChunks == 1) {
      return u / Fanout;
    }

    if (u < Fanout) {
      // Parent is root.
      return 0;
    }

    assert(PageChunks <= SIZE_MAX / Fanout);
    const size_t page_size = Fanout * PageChunks;
    size_t v = u % page_size;
    if (v >= Fanout) {
      // Fast path. Parent is on the same page as the child.
      return u - v + v / Fanout;
    }

    // Slow path. Parent is on another page.
    v = u / page_size - 1;
    const size_t page_leaves = (Fanout - 1) * PageChunks + 1;
    u = v / page_leaves + 1;
    return u * page_size + v % page_leaves - page_leaves + 1;
  }

  // Returns the index of the first child for the given parent index.
  // Parent index must be less than SIZE_MAX.
  // Returns SIZE_MAX if the index of the first child for the given parent
  // cannot fit size_t.
  static size_t get_child_index(size_t u)
  {
    assert(u < SIZE_MAX);

    if (PageChunks == 1) {
      if (u > (SIZE_MAX - 1) / Fanout) {
        // Child overflow.
        return SIZE_MAX;
      }
      return u * Fanout + 1;
    }

    if (u == 0) {
      // Root's child is always 1.
      return 1;
    }

    assert(PageChunks <= SIZE_MAX / Fanout);
    const size_t page_size = Fanout * PageChunks;
    --u;
    size_t v = u % page_size + 1;
    if (v < page_size / Fanout) {
      // Fast path. Child is on the same page as the parent.
      v *= Fanout - 1;
      if (u > SIZE_MAX - 2 - v) {
        // Child overflow.
        return SIZE_MAX;
      }
      return u + v + 2;
    }

    // Slow path. Child is on another page.
    const size_t page_leaves = (Fanout - 1) * PageChunks + 1;
    v += (u / page_size + 1) * page_leaves - page_size;
    if (v > (SIZE_MAX - 1) / page_size) {
      // Child overflow.
      return SIZE_MAX;
    }
    return v * page_size + 1;
  }

private:

  // moves the value from src to dst.
  template <class T>
  static void _move(T &dst, const T &src)
  {
    // src is const for optimization purposes only. This hints compiler
    // that the value referenced by src cannot be modified by somebody else,
    // so it is safe reading the value from a register instead of reading it
    // from slow memory on each read access.
    //
    // Of course, this optimization works only for values small enough to fit
    // CPU registers.
    dst = std::move(const_cast<T &>(src));
  }

  // Sifts the item up in the given sub-heap with the given root_index
  // starting from the hole_index.
  template <class RandomAccessIterator, class LessComparer>
  static void _sift_up(const RandomAccessIterator &first,
      const LessComparer &less_comparer,
      const size_t root_index, size_t hole_index,
      const typename std::iterator_traits<RandomAccessIterator>::value_type
          &item)
  {
    assert(hole_index >= root_index);

    typedef typename std::iterator_traits<RandomAccessIterator>::value_type
        value_type;

    while (hole_index > root_index) {
      const size_t parent_index = get_parent_index(hole_index);
      assert(parent_index >= root_index);
      const value_type &parent = first[parent_index];
      if (!less_comparer(parent, item)) {
        break;
      }
      _move(first[hole_index], parent);
      hole_index = parent_index;
    }
    _move(first[hole_index], item);
  }

  // Moves the max child into the given hole and returns index
  // of the new hole.
  template <class RandomAccessIterator, class LessComparer>
  static size_t _move_up_max_child(const RandomAccessIterator &first,
      const LessComparer &less_comparer, const size_t children_count,
      const size_t hole_index, const size_t child_index)
  {
    assert(child_index == get_child_index(hole_index));

    typedef typename std::iterator_traits<RandomAccessIterator>::value_type
        value_type;

    const value_type *max_child = &first[child_index];
    size_t j = 0;
    for (size_t i = 1; i < children_count; ++i) {
      const value_type &tmp = first[child_index + i];
      if (!less_comparer(tmp, *max_child)) {
        j = i;
        max_child = &tmp;
      }
    }
    _move(first[hole_index], *max_child);
    return child_index + j;
  }

  // Sifts the given item down in the heap of the given size starting
  // from the hole_index.
  template <class RandomAccessIterator, class LessComparer>
  static void _sift_down(const RandomAccessIterator &first,
      const LessComparer &less_comparer,
      const size_t heap_size, size_t hole_index,
      const typename std::iterator_traits<RandomAccessIterator>::value_type
          &item)
  {
    assert(heap_size > 0);
    assert(hole_index < heap_size);

    const size_t root_index = hole_index;
    const size_t remaining_items = (heap_size - 1) % Fanout;
    while (true) {
      const size_t child_index = get_child_index(hole_index);
      if (child_index >= heap_size - remaining_items) {
        if (child_index < heap_size) {
          assert(heap_size - child_index == remaining_items);
          hole_index = _move_up_max_child(first, less_comparer, remaining_items,
              hole_index, child_index);
        }
        break;
      }
      assert(heap_size - child_index >= Fanout);
      hole_index = _move_up_max_child(first, less_comparer, Fanout,
          hole_index, child_index);
    }
    _sift_up(first, less_comparer, root_index, hole_index, item);
  }

  // Pops the maximum item from the heap into first[item_index].
  template <class RandomAccessIterator, class LessComparer>
  static void _pop_heap(const RandomAccessIterator &first,
      const LessComparer &less_comparer, const size_t heap_size)
  {
      assert(heap_size > 1);

      typedef typename std::iterator_traits<RandomAccessIterator>::value_type
          value_type;

      const size_t hole_index = heap_size - 1;
      value_type item = std::move(first[hole_index]);
      _move(first[hole_index], first[0]);
      _sift_down(first, less_comparer, hole_index, 0, item);
  }

  // Standard less comparer.
  template <class InputIterator>
  static bool _std_less_comparer(
      const typename std::iterator_traits<InputIterator>::value_type &a,
      const typename std::iterator_traits<InputIterator>::value_type &b)
  {
    return (a < b);
  }

  // Less comparer for nway_merge().
  template <class LessComparer>
  class _nway_merge_less_comparer
  {
  private:
    const LessComparer &_less_comparer;

  public:
    _nway_merge_less_comparer(const LessComparer &less_comparer) :
        _less_comparer(less_comparer) {}

    template <class InputIterator>
    bool operator() (
      const std::pair<InputIterator, InputIterator> &input_range_a,
      const std::pair<InputIterator, InputIterator> &input_range_b) const
    {
      assert(input_range_a.first != input_range_a.second);
      assert(input_range_b.first != input_range_b.second);

      return _less_comparer(*(input_range_b.first), *(input_range_a.first));
    }
  };

public:

  // Returns an iterator for the first non-heap item in the range
  // [first ... last) using less_comparer for items' comparison.
  // Returns last if the range contains valid max heap.
  template <class RandomAccessIterator, class LessComparer>
  static RandomAccessIterator is_heap_until(
      const RandomAccessIterator &first, const RandomAccessIterator &last,
      const LessComparer &less_comparer)
  {
    assert(last >= first);

    const size_t heap_size = last - first;
    for (size_t u = 1; u < heap_size; ++u) {
      const size_t v = get_parent_index(u);
      if (less_comparer(first[v], first[u])) {
        return first + u;
      }
    }
    return last;
  }

  // Returns an iterator for the first non-heap item in the range
  // [first ... last) using operator< for items' comparison.
  // Returns last if the range contains valid max heap.
  template <class RandomAccessIterator>
  static RandomAccessIterator is_heap_until(
    const RandomAccessIterator &first, const RandomAccessIterator &last)
  {
    return is_heap_until(first, last, _std_less_comparer<RandomAccessIterator>);
  }

  // Returns true if the range [first ... last) contains valid max heap.
  // Returns false otherwise.
  // Uses less_comparer for items' comparison.
  template <class RandomAccessIterator, class LessComparer>
  static bool is_heap(const RandomAccessIterator &first,
      const RandomAccessIterator &last, const LessComparer &less_comparer)
  {
    return (is_heap_until(first, last, less_comparer) == last);
  }

  // Returns true if the range [first ... last) contains valid max heap.
  // Returns false otherwise.
  // Uses operator< for items' comparison.
  template <class RandomAccessIterator>
  static bool is_heap(const RandomAccessIterator &first,
    const RandomAccessIterator &last)
  {
    return is_heap(first, last, _std_less_comparer<RandomAccessIterator>);
  }

  // Makes max heap from items [first ... last) using the given less_comparer
  // for items' comparison.
  template <class RandomAccessIterator, class LessComparer>
  static void make_heap(const RandomAccessIterator &first,
      const RandomAccessIterator &last, const LessComparer &less_comparer)
  {
    assert(last >= first);

    typedef typename std::iterator_traits<RandomAccessIterator>::value_type
        value_type;

    const size_t heap_size = last - first;
    if (heap_size > 1) {
      // Skip leaf nodes without children. This is easy to do for non-paged
      // heap, i.e. when page_chunks = 1, but it is difficult for paged heaps.
      // So leaf nodes in paged heaps are visited anyway.
      size_t i = (PageChunks == 1) ? ((heap_size - 2) / Fanout) :
          (heap_size - 2);
      do {
        value_type item = std::move(first[i]);
        _sift_down(first, less_comparer, heap_size, i, item);
      } while (i-- > 0);
    }

    assert(is_heap(first, last, less_comparer));
  }

  // Makes max heap from items [first ... last) using operator< for items'
  // comparison.
  template <class RandomAccessIterator>
  static void make_heap(const RandomAccessIterator &first,
    const RandomAccessIterator &last)
  {
    make_heap(first, last, _std_less_comparer<RandomAccessIterator>);
  }

  // Pushes the item *(last - 1) into max heap [first ... last - 1)
  // using the given less_comparer for items' comparison.
  template <class RandomAccessIterator, class LessComparer>
  static void push_heap(const RandomAccessIterator &first,
      const RandomAccessIterator &last, const LessComparer &less_comparer)
  {
    assert(last > first);
    assert(is_heap(first, last - 1, less_comparer));

    typedef typename std::iterator_traits<RandomAccessIterator>::value_type
        value_type;

    const size_t heap_size = last - first;
    if (heap_size > 1) {
      const size_t u = heap_size - 1;
      value_type item = std::move(first[u]);
      _sift_up(first, less_comparer, 0, u, item);
    }

    assert(is_heap(first, last, less_comparer));
  }

  // Pushes the item *(last - 1) into max heap [first ... last - 1)
  // using operator< for items' comparison.
  template <class RandomAccessIterator>
  static void push_heap(const RandomAccessIterator &first,
      const RandomAccessIterator &last)
  {
    push_heap(first, last, _std_less_comparer<RandomAccessIterator>);
  }

  // Pops the maximum item from max heap [first ... last) into
  // *(last - 1) using the given less_comparer for items' comparison.
  template <class RandomAccessIterator, class LessComparer>
  static void pop_heap(const RandomAccessIterator &first,
      const RandomAccessIterator &last, const LessComparer &less_comparer)
  {
    assert(last > first);
    assert(is_heap(first, last, less_comparer));

    const size_t heap_size = last - first;
    if (heap_size > 1) {
      _pop_heap(first, less_comparer, heap_size);
    }

    assert(is_heap(first, last - 1, less_comparer));
  }

  // Pops the maximum item from max heap [first ... last) into
  // *(last - 1) using operator< for items' comparison.
  template <class RandomAccessIterator>
  static void pop_heap(const RandomAccessIterator &first,
      const RandomAccessIterator &last)
  {
    pop_heap(first, last, _std_less_comparer<RandomAccessIterator>);
  }

  // Sorts max heap [first ... last) using the given less_comparer
  // for items' comparison.
  // Items are sorted in ascending order.
  template <class RandomAccessIterator, class LessComparer>
  static void sort_heap(const RandomAccessIterator &first,
      const RandomAccessIterator &last, const LessComparer &less_comparer)
  {
    assert(last >= first);

    const size_t heap_size = last - first;
    for (size_t i = heap_size; i > 1; --i) {
      _pop_heap(first, less_comparer, i);
    }
  }

  // Sorts max heap [first ... last) using operator< for items' comparison.
  // Items are sorted in ascending order.
  template <class RandomAccessIterator>
  static void sort_heap(const RandomAccessIterator &first,
    const RandomAccessIterator &last)
  {
    sort_heap(first, last, _std_less_comparer<RandomAccessIterator>);
  }

  // Restores max heap invariant after item's value has been increased,
  // i.e. less_comparer(old_item, new_item) == true.
  template <class RandomAccessIterator, class LessComparer>
  static void restore_heap_after_item_increase(
      const RandomAccessIterator &first, const RandomAccessIterator &item,
      const LessComparer &less_comparer)
  {
    assert(item >= first);
    assert(is_heap(first, item, less_comparer));

    typedef typename std::iterator_traits<RandomAccessIterator>::value_type
        value_type;

    const size_t hole_index = item - first;
    if (hole_index > 0) {
      value_type tmp = std::move(*item);
      _sift_up(first, less_comparer, 0, hole_index, tmp);
    }

    assert(is_heap(first, item + 1, less_comparer));
  }

  // Restores max heap invariant after item's value has been increased,
  // i.e. old_item < new_item.
  template <class RandomAccessIterator>
  static void restore_heap_after_item_increase(
      const RandomAccessIterator &first, const RandomAccessIterator &item)
  {
    restore_heap_after_item_increase(first, item,
        _std_less_comparer<RandomAccessIterator>);
  }

  // Restores max heap invariant after item's value has been decreased,
  // i.e. less_comparer(new_item, old_item) == true.
  template <class RandomAccessIterator, class LessComparer>
  static void restore_heap_after_item_decrease(
      const RandomAccessIterator &first, const RandomAccessIterator &item,
      const RandomAccessIterator &last, const LessComparer &less_comparer)
  {
    assert(last > first);
    assert(item >= first);
    assert(item < last);
    assert(is_heap(first, item, less_comparer));

    typedef typename std::iterator_traits<RandomAccessIterator>::value_type
        value_type;

    const size_t heap_size = last - first;
    const size_t hole_index = item - first;
    value_type tmp = std::move(*item);
    _sift_down(first, less_comparer, heap_size, hole_index, tmp);

    assert(is_heap(first, last, less_comparer));
  }

  // Restores max heap invariant after item's value has been decreased,
  // i.e. new_item < old_item.
  template <class RandomAccessIterator>
  static void restore_heap_after_item_decrease(
      const RandomAccessIterator &first, const RandomAccessIterator &item,
      const RandomAccessIterator &last)
  {
    restore_heap_after_item_decrease(first, item, last,
        _std_less_comparer<RandomAccessIterator>);
  }

  // Removes the given item from the heap and puts it into *(last - 1).
  // less_comparer is used for items' comparison.
  template <class RandomAccessIterator, class LessComparer>
  static void remove_from_heap(const RandomAccessIterator &first,
      const RandomAccessIterator &item, const RandomAccessIterator &last,
      const LessComparer &less_comparer)
  {
    assert(last > first);
    assert(item >= first);
    assert(item < last);
    assert(is_heap(first, last, less_comparer));

    typedef typename std::iterator_traits<RandomAccessIterator>::value_type
        value_type;

    const size_t new_heap_size = last - first - 1;
    const size_t hole_index = item - first;
    if (hole_index < new_heap_size) {
      value_type tmp = std::move(first[new_heap_size]);
      _move(first[new_heap_size], *item);
      if (less_comparer(tmp, first[new_heap_size])) {
        _sift_down(first, less_comparer, new_heap_size, hole_index, tmp);
      }
      else {
        _sift_up(first, less_comparer, 0, hole_index, tmp);
      }
    }

    assert(is_heap(first, last - 1, less_comparer));
  }

  // Removes the given item from the heap and puts it into *(last - 1).
  // operator< is used for items' comparison.
  template <class RandomAccessIterator>
  static void remove_from_heap(const RandomAccessIterator &first,
      const RandomAccessIterator &item, const RandomAccessIterator &last)
  {
    remove_from_heap(first, item, last,
        _std_less_comparer<RandomAccessIterator>);
  }

  // Performs N-way merging of the given input ranges into the result sorted
  // in ascending order, using less_comparer for items' comparison.
  //
  // Each input range must hold non-zero number of items sorted
  // in ascending order. Each range is defined as a std::pair containing
  // input iterators, where the first iterator points to the beginning
  // of the range, while the second iterator points to the end of the range.
  //
  // As a side effect the function shuffles input ranges between
  // [input_ranges_first ... input_ranges_last) and sets the first iterator
  // for each input range to the end of the corresponding range.
  template <class RandomAccessIterator, class OutputIterator,
      class LessComparer>
  static void nway_merge(const RandomAccessIterator &input_ranges_first,
      const RandomAccessIterator &input_ranges_last,
      const OutputIterator &result, const LessComparer &less_comparer)
  {
    assert(input_ranges_first < input_ranges_last);

    typedef typename std::iterator_traits<RandomAccessIterator>::value_type
        input_range_iterator;

    const RandomAccessIterator &first = input_ranges_first;
    RandomAccessIterator last = input_ranges_last;
    OutputIterator output = result;

    const _nway_merge_less_comparer<LessComparer> less(less_comparer);

    make_heap(first, last, less);
    while (true) {
      input_range_iterator &input_range = first[0];
      assert(input_range.first != input_range.second);
      *output = *(input_range.first);
      ++output;
      ++(input_range.first);
      if (input_range.first == input_range.second) {
        --last;
        if (first == last) {
          break;
        }
        std::swap(*first, *last);
      }
      restore_heap_after_item_decrease(first, first, last, less);
    }
  }

  // Performs N-way merging of the given input ranges into the result sorted
  // in ascending order, using operator< for items' comparison.
  //
  // Each input range must hold non-zero number of items sorted
  // in ascending order. Each range is defined as a std::pair containing
  // input iterators, where the first iterator points to the beginning
  // of the range, while the second iterator points to the end of the range.
  //
  // As a side effect the function shuffles input ranges between
  // [input_ranges_first ... input_ranges_last) and sets the first iterator
  // for each input range to the end of the corresponding range.
  template <class RandomAccessIterator, class OutputIterator>
  static void nway_merge(const RandomAccessIterator &input_ranges_first,
      const RandomAccessIterator &input_ranges_last,
      const OutputIterator &result)
  {
    typedef typename std::iterator_traits<RandomAccessIterator
        >::value_type::first_type input_iterator;

    nway_merge(input_ranges_first, input_ranges_last, result,
        _std_less_comparer<input_iterator>);
  }
};
#endif
